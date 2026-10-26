/// Audio format helpers shared by analysis, normalization, and plugin processing.
/// Declares the format registration that decodes MP3 files with JUCE's gapless reader, the per-thread format manager,
/// readers with exact unindexed MP3 lengths, the AIFF writer lookup,
/// the float sample read shared by the decode loops, the validation of written output,
/// the conversion of AIFF reader metadata for the AIFF writer, and peak magnitude math.

#pragma once

#include <unordered_map>

#include <JuceHeader.h>

#include <cmath>
#include <expected>
#include <memory>
#include <span>
#include <tfile.h>

namespace audiobatch::utils
{
/// Fallback sample rate for preview and plugin preparation before an audio device is available.
constexpr double defaultSampleRate {48'000.0};
/// Returns the magnitude of a peak value, since analysis records store signed peaks.
template<typename Value>
[[nodiscard]] Value peakMagnitude(const Value peak)
{
    return std::abs(peak);
}

juce::AudioFormat* findAiffFormat(const juce::AudioFormatManager& formatManager);
[[nodiscard]] TagLib::FileName tagLibFileName(const juce::String& fullPath);
void registerAudioFormats(juce::AudioFormatManager& formatManager);
juce::AudioFormatManager& threadLocalFormatManager();
std::unique_ptr<juce::AudioFormatReader> createAudioReader(const juce::File& file);

[[nodiscard]] std::expected<void, juce::String> validateWrittenAudio(
    juce::AudioFormatManager& formatManager,
    const juce::File& file,
    int expectedChannels,
    double expectedSampleRate,
    juce::int64 expectedLength
);

std::unordered_map<juce::String, juce::String> aiffWriterMetadata(const juce::StringPairArray& metadata);

[[nodiscard]] bool readFloatSamples(
    juce::AudioFormatReader& reader,
    std::span<float* const> channels,
    juce::int64 startSample,
    int numSamples
);

std::span<float* const> writePointers(juce::AudioBuffer<float>& buffer);
}  // namespace audiobatch::utils
