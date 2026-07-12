#pragma once

#include "AudioAnalysisTypes.h"

#include <JuceHeader.h>

#include <vector>

/// Lightweight reference describing which plugin to instantiate and its saved state.
struct PluginDescriptorRef {
    juce::String pluginFormatName;  ///< "VST3", "AudioUnit", etc.
    juce::String identifierString;  ///< juce::PluginDescription::createIdentifierString()
    juce::String name;
    juce::String manufacturer;
    juce::MemoryBlock state;  ///< Result of AudioProcessor::getStateInformation()

    /// True when this descriptor identifies a plugin.
    [[nodiscard]] bool isValid() const noexcept
    {
        return identifierString.isNotEmpty();
    }
};

/// One entry in the plugin processing chain.
struct PluginChainSlot {
    PluginDescriptorRef plugin;
    bool enabled = true;
};

/// Options controlling a batch plugin processing run.
struct PluginProcessingOptions {
    /// Enabled plugins in chain order.
    /// Disabled slots are filtered out before a run starts,
    /// so entries here align index-for-index with the instantiated chain each worker holds.
    std::vector<PluginDescriptorRef> plugins;
    bool normalizeBeforePlugin = false;  ///< Apply peak normalization gain when a file has no custom gain.
};

/// Result payload for a single plugin-processing operation.
struct PluginProcessingResult {
    juce::File originalFile;  ///< Source file path before processing.
    juce::String originalFullPath;
    juce::File outputFile;  ///< Final on-disk file after processing (.aiff).
    juce::String fileName;
    juce::String errorMessage;
    AudioAnalysisRecord analysisRecord;
    bool succeeded = false;

    /// True when processing failed.
    [[nodiscard]] bool hasError() const noexcept
    {
        return !succeeded;
    }

    /// Creates a failure result for the given file and message.
    static PluginProcessingResult failure(const juce::File& file, const juce::String& message)
    {
        PluginProcessingResult result;
        result.originalFile = file;
        result.originalFullPath = file.getFullPathName();
        result.outputFile = file;
        result.fileName = file.getFileName();
        result.errorMessage = message;
        return result;
    }
};
