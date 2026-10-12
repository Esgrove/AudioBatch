/// Implementation of the shared audio format helpers.

#include "AudioFormatUtils.h"

#include "StringFormat.h"

#include <memory>
#include <utility>

namespace utils
{
juce::AudioFormatManager& threadLocalFormatManager()
{
    thread_local juce::AudioFormatManager formatManager;
    thread_local const bool initialized = [] {
        formatManager.registerBasicFormats();
        return true;
    }();

    juce::ignoreUnused(initialized);
    return formatManager;
}

juce::AudioFormat* findAiffFormat(const juce::AudioFormatManager& formatManager)
{
    if (auto* format = formatManager.findFormatForFileExtension("aif"); format != nullptr) {
        return format;
    }

    return formatManager.findFormatForFileExtension("aiff");
}

bool readFloatSamples(
    juce::AudioFormatReader& reader,
    float* const* channels,
    const int numChannels,
    const juce::int64 startSample,
    const int numSamples
)
{
    if (reader.read(channels, numChannels, startSample, numSamples)) {
        return true;
    }

    if (!reader.usesFloatingPointData) {
        for (int channel = 0; channel < numChannels; ++channel) {
            juce::FloatVectorOperations::clear(channels[channel], numSamples);
        }
    }

    return false;
}

juce::String validateWrittenAudio(
    juce::AudioFormatManager& formatManager,
    const juce::File& file,
    const int expectedChannels,
    const double expectedSampleRate,
    const juce::int64 expectedLength
)
{
    const std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        return "the output file cannot be read";
    }

    if (std::cmp_not_equal(reader->numChannels, expectedChannels)) {
        return format("the output has {} channels instead of {}", reader->numChannels, expectedChannels);
    }

    if (!juce::approximatelyEqual(reader->sampleRate, expectedSampleRate)) {
        return format("the output sample rate is {} Hz instead of {} Hz", reader->sampleRate, expectedSampleRate);
    }

    if (reader->lengthInSamples != expectedLength) {
        return format("the output has {} samples instead of {}", reader->lengthInSamples, expectedLength);
    }

    return {};
}

std::unordered_map<juce::String, juce::String> copyMetadata(const juce::StringPairArray& metadata)
{
    std::unordered_map<juce::String, juce::String> copiedMetadata;
    const auto& keys = metadata.getAllKeys();
    const auto& values = metadata.getAllValues();

    for (int index = 0; index < keys.size(); ++index) {
        copiedMetadata.emplace(keys[index], values[index]);
    }

    return copiedMetadata;
}
}  // namespace utils
