/// Audio format helpers shared by analysis, normalization, and plugin processing.
/// Declares the per-thread format manager, the AIFF writer lookup,
/// the float sample read shared by the decode loops, the validation of written output,
/// the conversion of AIFF reader metadata for the AIFF writer, and peak magnitude math.

#pragma once

#include <unordered_map>

#include <JuceHeader.h>

#include <cmath>

namespace audiobatch::utils
{
/// Returns the magnitude of a peak value, since analysis records store signed peaks.
template<typename Value>
[[nodiscard]] Value peakMagnitude(const Value peak)
{
    return std::abs(peak);
}

juce::AudioFormat* findAiffFormat(const juce::AudioFormatManager& formatManager);
juce::AudioFormatManager& threadLocalFormatManager();

juce::String validateWrittenAudio(
    juce::AudioFormatManager& formatManager,
    const juce::File& file,
    int expectedChannels,
    double expectedSampleRate,
    juce::int64 expectedLength
);

std::unordered_map<juce::String, juce::String> aiffWriterMetadata(const juce::StringPairArray& metadata);

bool readFloatSamples(
    juce::AudioFormatReader& reader,
    float* const* channels,
    int numChannels,
    juce::int64 startSample,
    int numSamples
);
}  // namespace audiobatch::utils
