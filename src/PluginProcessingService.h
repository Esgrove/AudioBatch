#pragma once

#include "AudioAnalysisService.h"
#include "PluginProcessing.h"

#include <JuceHeader.h>

#include <memory>

/// Stateless audio-file processing through a single plugin instance.
/// Output is always written as 16-bit AIFF next to the original file.
class PluginProcessingService
{
public:
    /// Returns true when the file's format can be read for processing.
    static bool canProcessFile(const juce::File& file);

    /// Returns the on-disk output path that processing would produce for the given input.
    static juce::File getProcessingOutputFile(const juce::File& input);

    /// Processes a single file through the given plugin instance. The plugin must already
    /// have been prepared with the appropriate sample rate and block size. The function
    /// resets the plugin internally before processing.
    /// The caller owns the plugin instance and is responsible for thread-safety: a single
    /// plugin instance must only be in use by one thread at a time.
    static PluginProcessingResult processFile(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        juce::AudioPluginInstance* plugin
    );

private:
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};
