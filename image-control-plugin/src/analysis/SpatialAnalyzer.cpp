/// Implements SpatialAnalyzer: the Hann-windowed STFT of both channels, the per-bin position and coherence,
/// the mapping of bins onto logarithmic columns and position rows, and the exponential decay of the image.

#include "analysis/SpatialAnalyzer.h"

#include "Parameters.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace esgrove::image_control
{
namespace
{
/// Number of samples in each analysis frame.
constexpr int FRAME_SIZE {static_cast<int>(1U << static_cast<unsigned>(ANALYSIS_FFT_ORDER))};
/// Number of columns, as an index type.
constexpr auto COLUMNS {static_cast<std::size_t>(SPECTROGRAM_COLUMNS)};
/// Number of cells in the spectrogram image.
constexpr auto CELL_COUNT {static_cast<std::size_t>(SPECTROGRAM_COLUMNS) * SPECTROGRAM_ROWS};
/// Bins whose normalized energy is below this, about -120 dB, are skipped.
constexpr float SILENCE_ENERGY {1.0e-12F};
/// Scale from atan2 of the channel magnitudes, which spans 0 to π/2, to the position range of two.
constexpr float POSITION_SCALE {4.0F / std::numbers::pi_v<float>};
}  // namespace

/// Creates an analyzer for 48 kHz audio with the default persistence.
SpatialAnalyzer::SpatialAnalyzer() :
    fft(ANALYSIS_FFT_ORDER),
    energy(CELL_COUNT, 0.0F),
    coherentEnergy(CELL_COUNT, 0.0F),
    columnBinShare(static_cast<std::size_t>(SPECTROGRAM_COLUMNS), 0.0F),
    leftFrame(static_cast<std::size_t>(FRAME_SIZE), 0.0F),
    rightFrame(static_cast<std::size_t>(FRAME_SIZE), 0.0F),
    leftSpectrum(static_cast<std::size_t>(FRAME_SIZE) * 2, 0.0F),
    rightSpectrum(static_cast<std::size_t>(FRAME_SIZE) * 2, 0.0F),
    window(static_cast<std::size_t>(FRAME_SIZE), 0.0F)
{
    juce::dsp::WindowingFunction<float>::fillWindowingTables(
        window.data(), window.size(), juce::dsp::WindowingFunction<float>::hann, false
    );
    double windowSum = 0.0;
    for (const float value : window) {
        windowSum += static_cast<double>(value);
    }
    // A sine of amplitude A peaks at A times half the window sum in its bin.
    const double fullScaleMagnitude = windowSum * 0.5;
    energyScale = static_cast<float>(1.0 / (fullScaleMagnitude * fullScaleMagnitude));
    prepare(sampleRate);
}

/// Returns the frequency in Hz at a fractional column, the inverse of frequencyToColumn.
double SpatialAnalyzer::columnToFrequency(const double column)
{
    const double ratio = static_cast<double>(MAX_FREQUENCY) / static_cast<double>(MIN_FREQUENCY);
    return static_cast<double>(MIN_FREQUENCY) * std::pow(ratio, column / static_cast<double>(SPECTROGRAM_COLUMNS));
}

/// Returns the fractional column of a frequency on the logarithmic axis.
/// MIN_FREQUENCY is the left edge of column zero, and MAX_FREQUENCY the right edge of the last column.
double SpatialAnalyzer::frequencyToColumn(const double frequency)
{
    const double ratio = static_cast<double>(MAX_FREQUENCY) / static_cast<double>(MIN_FREQUENCY);
    return std::log(frequency / static_cast<double>(MIN_FREQUENCY)) / std::log(ratio)
        * static_cast<double>(SPECTROGRAM_COLUMNS);
}

/// Returns the fractional row of a stereo position: row zero is full left (+1), the last row full right (−1).
double SpatialAnalyzer::positionToRow(const double position)
{
    return (1.0 - position) * 0.5 * static_cast<double>(SPECTROGRAM_ROWS - 1);
}

/// Returns the stereo position of a fractional row, the inverse of positionToRow.
double SpatialAnalyzer::rowToPosition(const double row)
{
    return 1.0 - 2.0 * row / static_cast<double>(SPECTROGRAM_ROWS - 1);
}

/// Returns the accumulated energy times coherence of every cell, row by row.
std::span<const float> SpatialAnalyzer::getCoherentEnergy() const
{
    return coherentEnergy;
}

/// Returns the accumulated energy of every cell, row by row, each row holding SPECTROGRAM_COLUMNS cells.
std::span<const float> SpatialAnalyzer::getEnergy() const
{
    return energy;
}

/// Returns how many frames have been analyzed and clears made, which changes whenever the image does.
std::uint64_t SpatialAnalyzer::getFrameCount() const
{
    return frameCount;
}

/// Clears the image and the partly filled frame.
void SpatialAnalyzer::clear()
{
    ++frameCount;
    std::ranges::fill(energy, 0.0F);
    std::ranges::fill(coherentEnergy, 0.0F);
    std::ranges::fill(leftFrame, 0.0F);
    std::ranges::fill(rightFrame, 0.0F);
    framePosition = 0;
}

/// Sets the sample rate, recomputes the column shares and the decay, and clears the image.
void SpatialAnalyzer::prepare(const double newSampleRate)
{
    sampleRate = newSampleRate;
    const double binWidth = sampleRate / static_cast<double>(FRAME_SIZE);
    for (int column = 0; column < SPECTROGRAM_COLUMNS; ++column) {
        const double columnWidth
            = columnToFrequency(static_cast<double>(column + 1)) - columnToFrequency(static_cast<double>(column));
        columnBinShare[static_cast<std::size_t>(column)] = static_cast<float>(std::min(1.0, columnWidth / binWidth));
    }
    setPersistence(persistenceSeconds);
    clear();
}

/// Adds samples of both channels, which must be the same length,
/// and analyzes a frame every ANALYSIS_HOP samples once a full frame has arrived.
void SpatialAnalyzer::pushSamples(const std::span<const float> left, const std::span<const float> right)
{
    jassert(left.size() == right.size());
    std::size_t consumed = 0;
    while (consumed < left.size()) {
        const auto space = static_cast<std::size_t>(FRAME_SIZE - framePosition);
        const std::size_t count = std::min(space, left.size() - consumed);
        const auto destination = static_cast<std::ptrdiff_t>(framePosition);
        std::copy_n(left.begin() + static_cast<std::ptrdiff_t>(consumed), count, leftFrame.begin() + destination);
        std::copy_n(right.begin() + static_cast<std::ptrdiff_t>(consumed), count, rightFrame.begin() + destination);
        consumed += count;
        framePosition += static_cast<int>(count);
        if (framePosition == FRAME_SIZE) {
            analyzeFrame();
            std::copy(leftFrame.begin() + ANALYSIS_HOP, leftFrame.end(), leftFrame.begin());
            std::copy(rightFrame.begin() + ANALYSIS_HOP, rightFrame.end(), rightFrame.begin());
            framePosition = FRAME_SIZE - ANALYSIS_HOP;
        }
    }
}

/// Sets the time constant of the image's exponential decay, clamped to the persistence range.
void SpatialAnalyzer::setPersistence(const double seconds)
{
    persistenceSeconds = std::clamp(seconds, MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS);
    decay = static_cast<float>(std::exp(-static_cast<double>(ANALYSIS_HOP) / (sampleRate * persistenceSeconds)));
}

/// Adds the energy of one bin to the columns its frequency span covers,
/// split between the two rows around its position.
/// Each contribution is scaled by one minus the decay, so a steady signal settles at its own energy.
void SpatialAnalyzer::accumulateBin(const int bin, const float binEnergy, const float position, const float coherence)
{
    const double binWidth = sampleRate / static_cast<double>(FRAME_SIZE);
    const double centre = static_cast<double>(bin) * binWidth;
    const double lowEdge = std::max(static_cast<double>(MIN_FREQUENCY), centre - binWidth * 0.5);
    const double highEdge = std::min(static_cast<double>(MAX_FREQUENCY), centre + binWidth * 0.5);
    const int firstColumn = std::clamp(static_cast<int>(frequencyToColumn(lowEdge)), 0, SPECTROGRAM_COLUMNS - 1);
    const int lastColumn = std::clamp(static_cast<int>(frequencyToColumn(highEdge)), 0, SPECTROGRAM_COLUMNS - 1);

    const double row = positionToRow(static_cast<double>(position));
    const int upperRow = std::clamp(static_cast<int>(row), 0, SPECTROGRAM_ROWS - 1);
    const int lowerRow = std::min(upperRow + 1, SPECTROGRAM_ROWS - 1);
    const auto lowerShare = static_cast<float>(row - static_cast<double>(upperRow));
    const float weight = binEnergy * (1.0F - decay);

    for (int column = firstColumn; column <= lastColumn; ++column) {
        const float share = weight * columnBinShare[static_cast<std::size_t>(column)];
        const auto upperCell = static_cast<std::size_t>(upperRow) * COLUMNS + static_cast<std::size_t>(column);
        const auto lowerCell = static_cast<std::size_t>(lowerRow) * COLUMNS + static_cast<std::size_t>(column);
        energy[upperCell] += share * (1.0F - lowerShare);
        energy[lowerCell] += share * lowerShare;
        coherentEnergy[upperCell] += share * (1.0F - lowerShare) * coherence;
        coherentEnergy[lowerCell] += share * lowerShare * coherence;
    }
}

/// Decays the image by one hop and adds the bins of the current frame.
void SpatialAnalyzer::analyzeFrame()
{
    ++frameCount;
    juce::FloatVectorOperations::multiply(energy.data(), decay, static_cast<int>(energy.size()));
    juce::FloatVectorOperations::multiply(coherentEnergy.data(), decay, static_cast<int>(coherentEnergy.size()));

    std::ranges::fill(leftSpectrum, 0.0F);
    std::ranges::fill(rightSpectrum, 0.0F);
    for (std::size_t sample = 0; sample < window.size(); ++sample) {
        leftSpectrum[sample] = leftFrame[sample] * window[sample];
        rightSpectrum[sample] = rightFrame[sample] * window[sample];
    }
    fft.performRealOnlyForwardTransform(leftSpectrum.data(), true);
    fft.performRealOnlyForwardTransform(rightSpectrum.data(), true);

    const double binWidth = sampleRate / static_cast<double>(FRAME_SIZE);
    const int firstBin = std::max(1, static_cast<int>(std::ceil(static_cast<double>(MIN_FREQUENCY) / binWidth)));
    const int lastBin = std::min(FRAME_SIZE / 2 - 1, static_cast<int>(static_cast<double>(MAX_FREQUENCY) / binWidth));
    for (int bin = firstBin; bin <= lastBin; ++bin) {
        const auto index = static_cast<std::size_t>(bin) * 2;
        const float leftReal = leftSpectrum[index];
        const float leftImaginary = leftSpectrum[index + 1];
        const float rightReal = rightSpectrum[index];
        const float rightImaginary = rightSpectrum[index + 1];
        const float leftMagnitude = std::hypot(leftReal, leftImaginary);
        const float rightMagnitude = std::hypot(rightReal, rightImaginary);
        const float binEnergy = (leftMagnitude * leftMagnitude + rightMagnitude * rightMagnitude) * energyScale;
        if (binEnergy < SILENCE_ENERGY) {
            continue;
        }
        const float position = POSITION_SCALE * std::atan2(leftMagnitude, rightMagnitude) - 1.0F;
        const float magnitudeProduct = leftMagnitude * rightMagnitude;
        const float coherence = magnitudeProduct > 0.0F
            ? (leftReal * rightReal + leftImaginary * rightImaginary) / magnitudeProduct
            : 1.0F;
        accumulateBin(bin, binEnergy, position, coherence);
    }
}
}  // namespace esgrove::image_control
