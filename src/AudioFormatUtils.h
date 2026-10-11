/// Audio format helpers shared by analysis, normalization, and plugin processing.
/// Declares the per-thread format manager, the AIFF writer lookup,
/// the conversion of reader metadata for writer options, and peak magnitude math.

#pragma once

#include <unordered_map>

#include <JuceHeader.h>

#include <cmath>

namespace utils
{
/// Returns a format manager private to the calling thread, with the basic formats registered.
/// Thread-local storage lets worker threads read files concurrently without sharing a manager or locking.
juce::AudioFormatManager& threadLocalFormatManager();

/// Returns the AIFF format registered in the manager, or nullptr when this build has none.
juce::AudioFormat* findAiffFormat(const juce::AudioFormatManager& formatManager);

/// Copies JUCE reader metadata into the map type expected by juce::AudioFormatWriterOptions,
/// so written files keep the metadata JUCE read from the source.
std::unordered_map<juce::String, juce::String> copyMetadata(const juce::StringPairArray& metadata);

/// Returns the magnitude of a peak value, since analysis records store signed peaks.
template<typename Value>
[[nodiscard]] Value peakMagnitude(const Value peak)
{
    return std::abs(peak);
}
}  // namespace utils
