/// Streaming native JUCE thumbnail construction shared by analysis and GUI artifact decoding.
/// Retains only bounded serialized peaks and one partial min/max bucket per channel, never source PCM.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <optional>

namespace audiobatch
{
/// Single-thread-owned mono/stereo thumbnail accumulator with chunk-independent complete buckets.
/// Only finish() produces cacheable data after every source frame has been received.
/// snapshot() is display-only sparse provisional data and MUST NEVER be persisted or cached.
/// Snapshot headers deliberately claim complete geometry for the strict native loader, not decoded progress.
/// Track decoded-frame progress separately, using framesReceived() rather than the serialized header.
class NativeWaveformBuilder final
{
public:
    NativeWaveformBuilder(int channels, int sampleRate, juce::int64 totalLength);

    [[nodiscard]] std::optional<juce::MemoryBlock> finish();
    [[nodiscard]] std::optional<juce::MemoryBlock> snapshot() const;
    [[nodiscard]] juce::int64 framesReceived() const;
    void push(const juce::AudioBuffer<float>& buffer, int first, int frames);

private:
    juce::MemoryBlock data;
    std::array<float, 2> maximum {};
    std::array<float, 2> minimum {};

    juce::int64 received {0};
    juce::int64 totalLength {0};
    int channels {0};
    int detail {0};
    int partialFrames {0};
    bool valid {false};
};

[[nodiscard]] int samplesPerThumbnailPoint(juce::int64 lengthInSamples);
}  // namespace audiobatch
