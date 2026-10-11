/// Implementation of the shared audio format helpers.

#include "AudioFormatUtils.h"

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
