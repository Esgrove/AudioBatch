/// Centered, zero-padded Hann STFTs with channel-power averaging and bounded max reduction.
/// Quantization is monotone, so quantize(max(power)) equals max(quantize(power)).
/// Keeping quantized maxima therefore preserves the exact output while bounding storage to 4096 * 512 bytes.

#include "analysis/SpectrogramBuilder.h"

#include "analysis/SpectrogramData.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <utility>

// The shared contract rounds every multiplication and addition separately, as the Rust implementation does.
// CMakeLists.txt disables contraction for Clang, and this pragma does it for MSVC, which has no undoing option.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma fp_contract(off)
#endif

namespace audiobatch
{
namespace
{
constexpr int FFT_ORDER {12};
constexpr int HALF_WINDOW {SpectrogramData::fftSize / 2};
constexpr int FREQUENCY_BINS {HALF_WINDOW + 1};
constexpr int MAXIMUM_CHANNELS {2};
constexpr double DECIBEL_STEPS {255.0};
constexpr double POWER_DECIBEL_FACTOR {10.0};
constexpr double MINIMUM_POWER {1.0e-12};

/// Inclusive FFT bin range whose half-bin frequency cells overlap a mel row.
/// Sharing the nearest bin for sub-bin rows avoids artificial empty low-frequency stripes.
struct SpectrogramRowSupport {
    int firstBin {0};
    int lastBin {0};
};

/// Immutable symmetric Hann coefficients and their double-precision sum, shared by every builder.
struct SpectrogramWindow {
    SpectrogramWindow();

    /// Float coefficients used directly by the source-rate FFT input multiplication.
    std::array<float, SpectrogramData::fftSize> symmetricHann {};
    /// Stored float coefficients accumulated in ascending sample order in double precision.
    double sum {0.0};
};

/// Calculates the explicit Hann formula without substituting precomputed or native FFT window tables.
/// Casting each coefficient to float before its double accumulation preserves the algorithm's operation order.
SpectrogramWindow::SpectrogramWindow()
{
    for (int frame = 0; frame < SpectrogramData::fftSize; ++frame) {
        const auto coefficient
            = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * frame / (SpectrogramData::fftSize - 1)));
        symmetricHann[static_cast<std::size_t>(frame)] = coefficient;
        sum += static_cast<double>(coefficient);
    }
}

/// Initializes the immutable calculation data once, safely across concurrent decoding workers.
const SpectrogramWindow& spectrogramWindow()
{
    static const SpectrogramWindow WINDOW;
    return WINDOW;
}

/// Clips power to the fixed dB profile and rounds to nearest byte, with half-way values rounded upward.
/// Parity exception: this uses juce::dsp::FFT and the Rust application realfft.
/// Their last-bit FFT rounding can move a power across a byte boundary,
/// so cached grids may differ by one intensity step in at most one cell per thousand.
/// The Rust shared-cache tests measure that bound against this executable on the shared spectral fixtures.
std::uint8_t quantizePower(const double power)
{
    const auto decibels = POWER_DECIBEL_FACTOR * std::log10(std::max(power, MINIMUM_POWER));
    const auto normalized = (std::clamp(decibels, SpectrogramData::minimumDecibels, SpectrogramData::maximumDecibels)
                             - SpectrogramData::minimumDecibels)
        / (SpectrogramData::maximumDecibels - SpectrogramData::minimumDecibels);
    return static_cast<std::uint8_t>(std::floor(normalized * DECIBEL_STEPS + 0.5));
}
}  // namespace

/// Single-threaded implementation with fixed PCM/FFT scratch and one bounded byte vector.
struct SpectrogramBuilderState {
    SpectrogramBuilderState(int channels, int sampleRate);

    void appendColumn();
    void mergePairs();

    /// Bounded column-major artifact and its actual decoded source geometry.
    SpectrogramData data;
    /// Reusable FFT plan, private to this decoding worker.
    juce::dsp::FFT transform {FFT_ORDER};
    /// Published immutable artifact retained for idempotent completion.
    std::shared_ptr<const SpectrogramData> result;

    /// Fixed source-frame ring storage for the supported mono or stereo channels.
    std::array<std::array<float, SpectrogramData::fftSize>, MAXIMUM_CHANNELS> pcm {};
    /// Independent channel powers accumulated for the current source window.
    std::array<double, FREQUENCY_BINS> powers {};
    /// Per-source-rate mel row supports calculated once for this builder.
    std::array<SpectrogramRowSupport, SpectrogramData::frequencyRows> rowSupports {};
    /// Fixed input/output work array reused for every channel FFT.
    std::array<float, static_cast<std::size_t>(SpectrogramData::fftSize) * 2> scratch {};

    /// Channel-average and window-sum power normalization, constant throughout this stream.
    double inverseWindowPower {0.0};
    /// Source sample at the next centered hop, independent of output time reduction.
    std::int64_t nextCenter {0};
    /// Whether completion has frozen the artifact and prohibited further pushes.
    bool finished {false};
    /// Whether every consumed range and PCM sample remains suitable for an optional artifact.
    bool valid {true};
};

/// Prepares the reusable FFT, shared symmetric Hann normalization, and source-rate mel support.
/// The window is float(0.5 - 0.5 * cos(2 * pi * n / (N - 1))), with n in [0, N).
/// The normalization sums these stored float coefficients in double precision.
SpectrogramBuilderState::SpectrogramBuilderState(const int channels, const int sampleRate) :
    valid(channels >= 1 && channels <= MAXIMUM_CHANNELS && sampleRate > 0)
{
    data.channelCount = channels;
    data.sampleRate = sampleRate;

    if (!valid) {
        return;
    }
    data.intensities.reserve(
        static_cast<std::size_t>(SpectrogramData::maximumColumns) * SpectrogramData::frequencyRows
    );
    const auto windowSum = spectrogramWindow().sum;
    inverseWindowPower = 1.0 / (windowSum * windowSum * channels);
    const auto maximumMel = frequencyToMel(sampleRate * 0.5);
    const auto melStep = maximumMel / SpectrogramData::frequencyRows;
    const auto frequencyToBin = static_cast<double>(SpectrogramData::fftSize) / sampleRate;
    for (int row = 0; row < SpectrogramData::frequencyRows; ++row) {
        const auto lowBin = melToFrequency(row * melStep) * frequencyToBin;
        const auto highBin = melToFrequency((row + 1) * melStep) * frequencyToBin;
        auto& support = rowSupports[static_cast<std::size_t>(row)];
        support.firstBin = std::clamp(static_cast<int>(std::ceil(lowBin - 0.5)), 0, HALF_WINDOW);
        support.lastBin = std::clamp(static_cast<int>(std::floor(highBin + 0.5)), support.firstBin, HALF_WINDOW);
    }
}

/// Reduces adjacent byte columns in place without allocating another image or losing isolated peaks.
/// Destination column k reads source columns 2k and 2k + 1 before writing,
/// and never overlaps a source column that a later destination still needs.
void SpectrogramBuilderState::mergePairs()
{
    constexpr auto ROWS = static_cast<std::size_t>(SpectrogramData::frequencyRows);
    const auto columns = data.columnCount();
    const std::span intensities(data.intensities);
    for (std::size_t column = 0; column < columns / 2; ++column) {
        const auto first = intensities.subspan(2 * column * ROWS, ROWS);
        const auto second = intensities.subspan((2 * column + 1) * ROWS, ROWS);
        const auto destination = intensities.subspan(column * ROWS, ROWS);
        for (std::size_t row = 0; row < ROWS; ++row) {
            destination[row] = std::max(first[row], second[row]);
        }
    }
    data.intensities.resize((columns / 2) * SpectrogramData::frequencyRows);
    data.samplesPerColumn *= 2;
}

/// Measures one source hop center using leading/trailing zero padding and mean independent channel powers.
/// Interior FFT bins use 4 * magnitudeSquared / windowSumSquared for sinusoid amplitude calibration.
/// DC and Nyquist use factor 1, since their real sinusoids have no distinct negative-frequency partner.
void SpectrogramBuilderState::appendColumn()
{
    const auto& window = spectrogramWindow().symmetricHann;
    powers.fill(0.0);
    // The window covers source frames [center - HALF_WINDOW, center + HALF_WINDOW).
    // Frames before zero or at EOF stay zero, and the valid span maps to at most two contiguous ring segments.
    const auto firstFrame = static_cast<std::size_t>(std::max<std::int64_t>(HALF_WINDOW - nextCenter, 0));
    const auto firstSource = std::max<std::int64_t>(nextCenter - HALF_WINDOW, 0);
    const auto lastSource = std::min<std::int64_t>(nextCenter + HALF_WINDOW, data.lengthInSamples);
    const auto validFrames = static_cast<std::size_t>(std::max<std::int64_t>(lastSource - firstSource, 0));
    const auto ringStart = static_cast<std::size_t>(firstSource % SpectrogramData::fftSize);
    const auto leading = std::min(validFrames, static_cast<std::size_t>(SpectrogramData::fftSize) - ringStart);
    for (int channel = 0; channel < data.channelCount; ++channel) {
        scratch.fill(0.0f);
        const auto& ring = pcm[static_cast<std::size_t>(channel)];
        for (std::size_t index = 0; index < leading; ++index) {
            scratch[firstFrame + index] = ring[ringStart + index] * window[firstFrame + index];
        }
        for (std::size_t index = leading; index < validFrames; ++index) {
            scratch[firstFrame + index] = ring[index - leading] * window[firstFrame + index];
        }
        transform.performRealOnlyForwardTransform(scratch.data(), true);
        for (int bin = 0; bin < FREQUENCY_BINS; ++bin) {
            const auto real = static_cast<double>(scratch[2 * static_cast<std::size_t>(bin)]);
            const auto imaginary = static_cast<double>(scratch[2 * static_cast<std::size_t>(bin) + 1]);
            const auto factor = bin == 0 || bin == HALF_WINDOW ? 1.0 : 4.0;
            powers[static_cast<std::size_t>(bin)]
                += (real * real + imaginary * imaginary) * factor * inverseWindowPower;
        }
    }
    // Finite PCM may overflow a float FFT. Treat that as a missing artifact rather than quantizing NaNs.
    if (std::ranges::any_of(powers, [](const auto power) { return !std::isfinite(power); })) {
        valid = false;
        return;
    }
    if (nextCenter / data.samplesPerColumn >= SpectrogramData::maximumColumns) {
        mergePairs();
    }
    const auto column = static_cast<std::size_t>(nextCenter / data.samplesPerColumn);
    if (column == data.columnCount()) {
        data.intensities.resize(data.intensities.size() + SpectrogramData::frequencyRows, 0);
    }
    for (std::size_t row = 0; row < rowSupports.size(); ++row) {
        const auto support = rowSupports[row];
        double maximumPower = 0.0;
        for (int bin = support.firstBin; bin <= support.lastBin; ++bin) {
            maximumPower = std::max(maximumPower, powers[static_cast<std::size_t>(bin)]);
        }
        auto& intensity = data.intensities[column * SpectrogramData::frequencyRows + row];
        intensity = std::max(intensity, quantizePower(maximumPower));
    }
    nextCenter += SpectrogramData::hopSize;
}

/// Creates an isolated optional-artifact builder, without holding or changing caller PCM.
SpectrogramBuilder::SpectrogramBuilder(const int channels, const int sampleRate) :
    state(std::make_unique<SpectrogramBuilderState>(channels, sampleRate))
{}

/// Releases FFT resources and bounded working memory on the owner's decoding thread.
SpectrogramBuilder::~SpectrogramBuilder() = default;

/// Consumes a checked range of consecutive frames, scheduling a center as soon as its right half is available.
/// A zero-frame range is harmless. Malformed ranges and nonfinite samples invalidate only this builder.
void SpectrogramBuilder::push(const juce::AudioBuffer<float>& samples, const int firstSample, const int frames)
{
    auto& working = *state;
    if (!working.valid || working.finished) {
        return;
    }
    if (firstSample < 0 || frames < 0 || firstSample > samples.getNumSamples()
        || frames > samples.getNumSamples() - firstSample || samples.getNumChannels() != working.data.channelCount
        || working.data.lengthInSamples > std::numeric_limits<std::int64_t>::max() - SpectrogramData::fftSize - frames)
    {
        working.valid = false;
        return;
    }
    for (int frame = 0; frame < frames; ++frame) {
        const auto position = static_cast<std::size_t>(working.data.lengthInSamples % SpectrogramData::fftSize);
        for (int channel = 0; channel < working.data.channelCount; ++channel) {
            const auto sample = samples.getSample(channel, firstSample + frame);
            if (!std::isfinite(sample)) {
                working.valid = false;
                return;
            }
            working.pcm[static_cast<std::size_t>(channel)][position] = sample;
        }
        ++working.data.lengthInSamples;
        if (working.data.lengthInSamples == working.nextCenter + HALF_WINDOW) {
            working.appendColumn();
            if (!working.valid) {
                return;
            }
        }
    }
}

/// Flushes just the remaining centers before actual EOF, padding only their missing PCM with zero.
std::shared_ptr<const SpectrogramData> SpectrogramBuilder::finish()
{
    auto& working = *state;
    if (working.finished) {
        return working.result;
    }
    working.finished = true;
    while (working.valid && working.nextCenter < working.data.lengthInSamples) {
        working.appendColumn();
    }
    if (working.valid && isValidSpectrogramData(working.data)) {
        working.result = std::make_shared<SpectrogramData>(std::move(working.data));
    }
    return working.result;
}
}  // namespace audiobatch
