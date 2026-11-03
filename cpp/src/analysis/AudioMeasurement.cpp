/// Implements fixed-interval libebur128 metering over arbitrary planar chunks.
/// Collects signed sample peaks, continuous true peaks, short-term loudness, and bounded overload histories,
/// and attaches the committed file identity to measurements made during output rendering.

#include "analysis/AudioMeasurement.h"

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/NativeWaveformBuilder.h"
#include "analysis/OverloadHistory.h"
#include "analysis/SpectrogramBuilder.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

extern "C" {
#include <ebur128.h>
}

namespace audiobatch
{
namespace
{
/// Stream limits accepted by libebur128.
constexpr int maximumLoudnessChannels {64};
constexpr double minimumLoudnessSampleRate {16.0};
constexpr double maximumLoudnessSampleRate {2'822'400.0};

/// Owns a libebur128 state through its C destruction API.
struct EbuR128StateDeleter {
    /// Releases the state and its internal buffers.
    void operator()(ebur128_state* state) const noexcept
    {
        ebur128_destroy(&state);
    }
};

/// Creates integrated, short-term, and true-peak measurement, treating mono as dual mono.
std::unique_ptr<ebur128_state, EbuR128StateDeleter> createLoudnessState(const int channelCount, const double sampleRate)
{
    constexpr auto mode = static_cast<int>(
        static_cast<unsigned int>(EBUR128_MODE_I) | static_cast<unsigned int>(EBUR128_MODE_S)
        | static_cast<unsigned int>(EBUR128_MODE_TRUE_PEAK)
    );
    std::unique_ptr<ebur128_state, EbuR128StateDeleter> state(ebur128_init(
        static_cast<unsigned int>(channelCount), static_cast<unsigned long>(juce::roundToInt(sampleRate)), mode
    ));
    if (state != nullptr && channelCount == 1
        && ebur128_set_channel(state.get(), 0, EBUR128_DUAL_MONO) != EBUR128_SUCCESS)
    {
        return nullptr;
    }
    return state;
}

/// Maps silence and non-finite loudness to the record sentinel.
double normalizeLoudness(const double value)
{
    return std::isfinite(value) && value > AudioAnalysisRecord::negativeInfinityLoudness
        ? value
        : AudioAnalysisRecord::negativeInfinityLoudness;
}
}  // namespace

/// Internal state whose histories share the declared stream length and measurement interval.
struct AudioMeasurementState {
    /// Allocates scratch storage once for the validated stream.
    AudioMeasurementState(const int channels, const double rate, const std::int64_t length) :
        partial(channels, overloadBlockSize(rate)),
        overloads(channels, rate, length, partial.getNumSamples()),
        meter(createLoudnessState(channels, rate)),
        interleaved(static_cast<std::size_t>(channels) * static_cast<std::size_t>(partial.getNumSamples()), 0.0f),
        maximums(static_cast<std::size_t>(channels), 0.0f),
        minimums(static_cast<std::size_t>(channels), 0.0f),
        flags(static_cast<std::size_t>(channels), 0)
    {
        // Optional preview allocation must never prevent the loudness measurement from starting.
        try {
            spectrogram = std::make_unique<SpectrogramBuilder>(channels, juce::roundToInt(rate));
        } catch (...) {
            utils::logWarn("Spectrogram initialization failed: {}", utils::describeCurrentException());
        }
        try {
            waveform = std::make_unique<NativeWaveformBuilder>(channels, juce::roundToInt(rate), length);
        } catch (...) {
            utils::logWarn("Waveform initialization failed: {}", utils::describeCurrentException());
        }
        history->sampleRate = rate;
        history->lengthInSamples = length;
        history->samplesPerPoint = partial.getNumSamples();
        const auto interval = static_cast<std::int64_t>(history->samplesPerPoint);
        const auto points = length / interval + (length % interval != 0 ? 1 : 0);
        history->values.reserve(static_cast<std::size_t>(points));
    }

    /// Interleaves an interval while collecting its extremes, then advances the continuous meter.
    std::expected<void, juce::String> measure(
        const juce::AudioBuffer<float>& samples,
        const int offset,
        const int frames
    )
    {
        if (spectrogram != nullptr) {
            try {
                spectrogram->push(samples, offset, frames);
            } catch (...) {
                spectrogram.reset();
                utils::logWarn("Spectrogram processing failed: {}", utils::describeCurrentException());
            }
        }
        if (waveform != nullptr) {
            try {
                waveform->push(samples, offset, frames);
            } catch (...) {
                waveform.reset();
                utils::logWarn("Waveform processing failed: {}", utils::describeCurrentException());
            }
        }
        const auto channels = minimums.size();
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const std::span plane(
                samples.getReadPointer(static_cast<int>(channel), offset), static_cast<std::size_t>(frames)
            );
            auto minimum = 0.0f;
            auto maximum = 0.0f;
            for (std::size_t frame = 0; frame < plane.size(); ++frame) {
                minimum = std::min(minimum, plane[frame]);
                maximum = std::max(maximum, plane[frame]);
                interleaved[frame * channels + channel] = plane[frame];
            }
            minimums[channel] = std::min(minimums[channel], minimum);
            maximums[channel] = std::max(maximums[channel], maximum);
            flags[channel] = minimum < -1.0f || maximum > 1.0f ? OverloadHistory::sampleOver : 0;
        }
        if (ebur128_add_frames_float(meter.get(), interleaved.data(), static_cast<std::size_t>(frames))
            != EBUR128_SUCCESS)
        {
            return std::unexpected(juce::String("Loudness analysis failed while processing audio"));
        }
        for (std::size_t channel = 0; channel < channels; ++channel) {
            double peak = 0.0;
            if (ebur128_prev_true_peak(meter.get(), static_cast<unsigned int>(channel), &peak) != EBUR128_SUCCESS) {
                return std::unexpected(juce::String("True peak analysis failed while processing audio"));
            }
            if (peak > 1.0) {
                flags[channel] |= OverloadHistory::truePeakOver;
            }
        }
        overloads.addBlock(flags);
        double loudness = AudioAnalysisRecord::negativeInfinityLoudness;
        if (ebur128_loudness_shortterm(meter.get(), &loudness) != EBUR128_SUCCESS) {
            return std::unexpected(juce::String("Short-term loudness analysis failed"));
        }
        const auto normalized = normalizeLoudness(loudness);
        maxShortTerm = std::max(maxShortTerm, normalized);
        history->values.push_back(static_cast<float>(normalized));
        return {};
    }

    /// Finalizes the optional preview independently of the required measurement result.
    std::shared_ptr<const SpectrogramData> finishSpectrogram()
    {
        if (spectrogram == nullptr) {
            return {};
        }
        std::shared_ptr<const SpectrogramData> result;
        try {
            result = spectrogram->finish();
        } catch (...) {
            utils::logWarn("Spectrogram completion failed: {}", utils::describeCurrentException());
        }
        spectrogram.reset();
        return result;
    }

    /// Finalizes native waveform bytes without allowing optional failures to invalidate measurements.
    std::optional<juce::MemoryBlock> finishWaveform()
    {
        if (waveform == nullptr) {
            return std::nullopt;
        }
        std::optional<juce::MemoryBlock> result;
        try {
            result = waveform->finish();
        } catch (...) {
            utils::logWarn("Waveform completion failed: {}", utils::describeCurrentException());
        }
        waveform.reset();
        return result;
    }

    /// Storage for an interval crossing input chunk boundaries.
    juce::AudioBuffer<float> partial;
    /// Bounded overload history builder. Its interval depends on partial's initialized buffer size.
    OverloadHistoryBuilder overloads;
    /// Error that invalidates the measurement after a failed push.
    juce::String error;

    /// Short-term readings and their timing properties.
    std::shared_ptr<ShortTermLoudnessHistory> history {std::make_shared<ShortTermLoudnessHistory>()};
    /// Continuous loudness and true-peak state.
    std::unique_ptr<ebur128_state, EbuR128StateDeleter> meter;
    /// Optional spectral preview fed the exact PCM intervals consumed by the loudness meter.
    std::unique_ptr<SpectrogramBuilder> spectrogram;
    /// Optional native waveform fed the same fixed PCM intervals as the spectral preview and meter.
    std::unique_ptr<NativeWaveformBuilder> waveform;

    /// Meter input reused for every interval.
    std::vector<float> interleaved;
    /// Per-channel positive sample extremes.
    std::vector<float> maximums;
    /// Per-channel negative sample extremes.
    std::vector<float> minimums;
    /// Per-channel overload flags of the current interval.
    std::vector<std::uint8_t> flags;

    /// Highest normalized short-term reading.
    double maxShortTerm {AudioAnalysisRecord::negativeInfinityLoudness};
    /// Frames buffered in an incomplete interval.
    int partialFrames {0};
    /// Prevents reading a moved-out history or extending a completed measurement.
    bool finished {false};
    /// Frames accepted from all chunks.
    std::int64_t framesReceived {0};
};

/// Checks stream bounds before allocating buffers or rounding its sample rate.
bool isSupportedMeasurementStream(const int channelCount, const double sampleRate) noexcept
{
    return channelCount > 0 && channelCount <= maximumLoudnessChannels && std::isfinite(sampleRate)
        && sampleRate >= minimumLoudnessSampleRate && sampleRate <= maximumLoudnessSampleRate;
}

/// Initializes a validated stream, retaining initialization failures for callers to report.
AudioMeasurement::AudioMeasurement(const int channelCount, const double sampleRate, const std::int64_t lengthInSamples)
{
    if (!isSupportedMeasurementStream(channelCount, sampleRate) || lengthInSamples < 0) {
        initializationError = "Unsupported audio stream parameters";
        return;
    }
    state = std::make_unique<AudioMeasurementState>(channelCount, sampleRate, lengthInSamples);
    if (state->meter == nullptr) {
        initializationError = "Could not initialize loudness analyzer";
    }
}

/// Destroys the private C meter and all scratch storage.
AudioMeasurement::~AudioMeasurement() = default;

/// Returns an initialization failure before decoding or writing begins.
std::expected<void, juce::String> AudioMeasurement::checkInitialization() const
{
    if (initializationError.isNotEmpty()) {
        return std::unexpected(initializationError);
    }
    return {};
}

/// Borrows complete intervals and copies only intervals that span chunk boundaries.
/// Invalid chunks and measurement failures permanently invalidate this stream.
std::expected<void, juce::String> AudioMeasurement::push(
    const juce::AudioBuffer<float>& samples,
    const int firstSample,
    const int frames
)
{
    if (const auto initialized = checkInitialization(); !initialized) {
        return initialized;
    }
    auto& measurement = *state;
    if (measurement.finished) {
        return std::unexpected(juce::String("Audio measurement has already finished"));
    }
    if (measurement.error.isNotEmpty()) {
        return std::unexpected(measurement.error);
    }
    if (samples.getNumChannels() != measurement.partial.getNumChannels() || firstSample < 0 || frames < 0
        || firstSample > samples.getNumSamples() || frames > samples.getNumSamples() - firstSample
        || frames > measurement.history->lengthInSamples - measurement.framesReceived)
    {
        measurement.error = "Invalid audio measurement chunk";
        return std::unexpected(measurement.error);
    }
    const auto interval = measurement.partial.getNumSamples();
    for (int offset = 0; offset < frames;) {
        const auto take = std::min(interval - measurement.partialFrames, frames - offset);
        std::expected<void, juce::String> measured;
        if (measurement.partialFrames == 0 && take == interval) {
            measured = measurement.measure(samples, firstSample + offset, take);
        } else {
            for (int channel = 0; channel < samples.getNumChannels(); ++channel) {
                measurement.partial.copyFrom(
                    channel, measurement.partialFrames, samples, channel, firstSample + offset, take
                );
            }
            measurement.partialFrames += take;
            if (measurement.partialFrames == interval) {
                measured = measurement.measure(measurement.partial, 0, interval);
                measurement.partialFrames = 0;
            }
        }
        if (!measured) {
            measurement.error = measured.error();
            return measured;
        }
        offset += take;
    }
    measurement.framesReceived += frames;
    return {};
}

/// Copies a bounded waveform snapshot on the measurement's worker thread.
/// Only completed measurement intervals are exposed, and optional serialization failures remain nonfatal.
/// These sparse provisional bytes are display-only and must never be persisted.
std::optional<juce::MemoryBlock> AudioMeasurement::waveformSnapshot() const
{
    if (state == nullptr || state->finished || state->error.isNotEmpty() || state->waveform == nullptr) {
        return std::nullopt;
    }
    try {
        return state->waveform->snapshot();
    } catch (...) {
        utils::logWarn("Waveform snapshot failed: {}", utils::describeCurrentException());
        return std::nullopt;
    }
}

/// Measures the final partial interval and reduces every channel to a complete analysis record.
std::expected<AudioAnalysisRecord, juce::String> AudioMeasurement::finish()
{
    if (const auto initialized = checkInitialization(); !initialized) {
        return std::unexpected(initialized.error());
    }
    auto& measurement = *state;
    if (measurement.finished || measurement.error.isNotEmpty()
        || measurement.framesReceived != measurement.history->lengthInSamples)
    {
        return std::unexpected(
            measurement.error.isNotEmpty() ? measurement.error
                                           : juce::String("Audio measurement is incomplete or already finished")
        );
    }
    measurement.finished = true;
    if (measurement.partialFrames > 0) {
        if (const auto measured = measurement.measure(measurement.partial, 0, measurement.partialFrames); !measured) {
            return std::unexpected(measured.error());
        }
    }
    AudioAnalysisRecord record;
    record.channels = measurement.partial.getNumChannels();
    record.sampleRate = juce::roundToInt(measurement.history->sampleRate);
    record.lengthInSamples = measurement.framesReceived;
    record.durationSeconds = static_cast<double>(record.lengthInSamples) / measurement.history->sampleRate;
    for (int channel = 0; channel < record.channels; ++channel) {
        const auto index = static_cast<std::size_t>(channel);
        const auto minimum = measurement.minimums[index];
        const auto maximum = measurement.maximums[index];
        const auto peak = utils::peakMagnitude(minimum) > utils::peakMagnitude(maximum) ? minimum : maximum;
        double truePeak = 0.0;
        if (ebur128_true_peak(measurement.meter.get(), static_cast<unsigned int>(channel), &truePeak)
            != EBUR128_SUCCESS)
        {
            return std::unexpected(juce::String("True peak analysis failed"));
        }
        if (channel == 0) {
            record.peakLeft = record.peakRight = record.overallPeak = peak;
            record.truePeakLeft = record.truePeakRight = record.overallTruePeak = truePeak;
        } else {
            if (channel == 1) {
                record.peakRight = peak;
                record.truePeakRight = truePeak;
            }
            if (utils::peakMagnitude(record.overallPeak) <= utils::peakMagnitude(peak)) {
                record.overallPeak = peak;
            }
            record.overallTruePeak = std::max(record.overallTruePeak, truePeak);
        }
    }
    double integrated = AudioAnalysisRecord::negativeInfinityLoudness;
    if (ebur128_loudness_global(measurement.meter.get(), &integrated) != EBUR128_SUCCESS) {
        return std::unexpected(juce::String("Integrated loudness analysis failed"));
    }
    record.integratedLufs = normalizeLoudness(integrated);
    record.maxShortTermLufs = measurement.maxShortTerm;
    record.shortTermLoudness = std::move(measurement.history);
    record.overloads = measurement.overloads.finish();
    record.status = AudioAnalysisStatus::analyzed;
    record.spectrogram = measurement.finishSpectrogram();
    record.nativeWaveform = measurement.finishWaveform();
    return record;
}

/// Attaches the committed AIFF identity without decoding its audio again.
/// Missing or inaccessible output remains a committed-but-unanalyzed failure for the service to report.
AudioAnalysisRecord completeRenderedAnalysis(
    AudioAnalysisRecord measurements,
    const juce::File& file,
    const int bitsPerSample,
    const utils::FileIdentity& renderedIdentity
)
{
    const auto identity = AudioAnalysisRecord::fromFile(file);
    measurements.file = identity.file;
    measurements.fileName = identity.fileName;
    measurements.fullPath = identity.fullPath;
    measurements.fileSize = identity.fileSize;
    measurements.modifiedTimeMs = identity.modifiedTimeMs;
    measurements.formatName = "AIFF file";
    measurements.bitsPerSample = bitsPerSample;
    // Retain the pre-commit inode receipt, rather than adopting potentially replaced bytes at persistence time.
    measurements.renderedIdentity.reset();
    try {
        measurements.renderedIdentity = std::make_shared<const utils::FileIdentity>(renderedIdentity);
    } catch (...) {
        measurements.nativeWaveform.reset();
        measurements.spectrogram.reset();
        utils::logWarn("Rendered preview identity capture failed: {}", utils::describeCurrentException());
    }
    const auto unchanged = renderedIdentity.owns(file);
    const auto readable = file.existsAsFile() && file.createInputStream() != nullptr;
    if (!unchanged || !readable || !renderedIdentity.owns(file)) {
        measurements.nativeWaveform.reset();
        measurements.spectrogram.reset();
        measurements.renderedIdentity.reset();
        measurements.status = AudioAnalysisStatus::failed;
        measurements.error = AnalysisError::unreadable;
        measurements.errorMessage = !readable
            ? "The rendered output could not be opened for reading"
            : "The rendered output changed before its analysis record could be completed";
        utils::logError("Analysis failed for {}: {}", measurements.fullPath.quoted(), measurements.errorMessage);
    }
    return measurements;
}
}  // namespace audiobatch
