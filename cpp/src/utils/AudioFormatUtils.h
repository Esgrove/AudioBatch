/// Audio format helpers shared by analysis, normalization, and plugin processing.
/// Declares the format registration that decodes MP3 files with JUCE's gapless reader, the per-thread format manager,
/// readers with exact unindexed MP3 lengths, the AIFF writer lookup,
/// the float sample read shared by the decode loops, the validation of written output,
/// the conversion of AIFF reader metadata for the AIFF writer, and peak magnitude math.

#pragma once

#include "utils/OptionalRef.h"
#include <unordered_map>

#include <JuceHeader.h>

#include <bit>
#include <cmath>
#include <concepts>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <tfile.h>

namespace audiobatch::utils
{
/// Fallback sample rate for preview and plugin preparation before an audio device is available.
constexpr double DEFAULT_SAMPLE_RATE {48'000.0};
OptionalRef<juce::AudioFormat> findAiffFormat(const juce::AudioFormatManager& formatManager);
[[nodiscard]] TagLib::FileName tagLibFileName(const juce::String& fullPath);
juce::AudioFormatManager& threadLocalFormatManager();

std::span<float* const> writePointers(juce::AudioBuffer<float>& buffer);
std::unique_ptr<juce::AudioFormatReader> createAudioReader(const juce::File& file);
std::unordered_map<juce::String, juce::String> aiffWriterMetadata(const juce::StringPairArray& metadata);

[[nodiscard]] std::expected<void, juce::String> quantizeNormalizedSamples(
    std::span<const float> input,
    std::span<int> pcm,
    std::span<float> observed,
    float gain,
    int bitsPerSample
);
[[nodiscard]] std::expected<void, juce::String> validateWrittenAudio(
    juce::AudioFormatManager& formatManager,
    const juce::File& file,
    int expectedChannels,
    double expectedSampleRate,
    juce::int64 expectedLength
);

/// Returns the magnitude of a peak value, since analysis records store signed peaks.
template<std::floating_point Value>
[[nodiscard]] Value peakMagnitude(const Value peak)
{
    return std::abs(peak);
}

[[nodiscard]] bool readFloatSamples(
    juce::AudioFormatReader& reader,
    std::span<float* const> channels,
    juce::int64 startSample,
    int numSamples
);

/// Reconstructs the float a JUCE integer AIFF reader returns after discarding unwritten low bits.
/// The input is aligned to the most significant bits of a 32-bit PCM value.
/// Masks the low bits without changing the sign, so negative samples round toward negative infinity.
[[nodiscard]] inline float pcmSampleToFloat(const int sample, const int bitsPerSample) noexcept
{
    const auto shift = static_cast<unsigned int>(32 - bitsPerSample);
    const auto mask = std::numeric_limits<unsigned int>::max() << shift;
    const auto aligned = std::bit_cast<int>(static_cast<unsigned int>(sample) & mask);
    constexpr auto scale = 1.0f / static_cast<float>(std::numeric_limits<int>::max());
    return static_cast<float>(aligned) * scale;
}

void registerAudioFormats(juce::AudioFormatManager& formatManager);
}  // namespace audiobatch::utils
