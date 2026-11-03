/// Streaming audio measurement shared by file analysis and PCM output rendering.
/// AudioMeasurement retains a continuous libebur128 meter and regroups planar chunks into fixed short-term
/// and overload intervals without depending on a decoder or writer.
/// Optional bounded waveform and spectrogram builders consume the same PCM and finish as transient worker products.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "utils/FileUtils.h"

#include <expected>
#include <memory>
#include <optional>

namespace audiobatch
{
/// Private storage for the meter, reusable sample buffers, and unfinished histories.
struct AudioMeasurementState;

/// Measures consecutive planar chunks with the same interval boundaries regardless of chunk size.
class AudioMeasurement
{
public:
    AudioMeasurement(int channelCount, double sampleRate, std::int64_t lengthInSamples);

    ~AudioMeasurement();

    AudioMeasurement(const AudioMeasurement&) = delete;
    AudioMeasurement& operator=(const AudioMeasurement&) = delete;
    AudioMeasurement(AudioMeasurement&&) = delete;
    AudioMeasurement& operator=(AudioMeasurement&&) = delete;

    [[nodiscard]] std::expected<AudioAnalysisRecord, juce::String> finish();
    [[nodiscard]] std::optional<juce::MemoryBlock> waveformSnapshot() const;

    [[nodiscard]] std::expected<void, juce::String> checkInitialization() const;
    [[nodiscard]] std::expected<void, juce::String> push(
        const juce::AudioBuffer<float>& samples,
        int firstSample,
        int frames
    );

private:
    /// Initialization failure reported by every operation instead of returning partial measurements.
    juce::String initializationError;
    /// Owns the meter and its scratch storage, absent when initialization failed.
    std::unique_ptr<AudioMeasurementState> state;
};

[[nodiscard]] AudioAnalysisRecord completeRenderedAnalysis(
    AudioAnalysisRecord measurements,
    const juce::File& file,
    int bitsPerSample,
    const utils::FileIdentity& renderedIdentity
);
[[nodiscard]] bool isSupportedMeasurementStream(int channelCount, double sampleRate) noexcept;
}  // namespace audiobatch
