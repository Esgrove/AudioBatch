/// Audio format helpers shared by analysis, normalization, and plugin processing.
/// Declares the per-thread format manager, the AIFF writer lookup,
/// the float sample read shared by the decode loops, the validation of written output,
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

/// Reads `numSamples` frames starting at `startSample` into the start of each channel as floats,
/// and returns false when the reader reports a failure.
/// JUCE converts the samples of a fixed-point reader to floats only after a successful read,
/// so after a failed read the block would hold raw integers reinterpreted as floats, which play as full-scale noise.
/// The block is zeroed in that case.
/// A floating point reader keeps the samples it decoded before the error.
bool readFloatSamples(
    juce::AudioFormatReader& reader,
    float* const* channels,
    int numChannels,
    juce::int64 startSample,
    int numSamples
);

/// Reads back a freshly written audio file and checks that it has the expected channel count, sample rate,
/// and length, which also catches a short write the writer could not report.
/// Returns an empty string when it matches, or a user-facing description of the problem.
/// The services run this before the source is replaced or moved to the trash.
juce::String validateWrittenAudio(
    juce::AudioFormatManager& formatManager,
    const juce::File& file,
    int expectedChannels,
    double expectedSampleRate,
    juce::int64 expectedLength
);

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
