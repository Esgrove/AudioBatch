#pragma once

#include "AudioAnalysisService.h"
#include "PluginProcessing.h"

#include <JuceHeader.h>

#include <memory>
#include <vector>

/// Stateless audio-file processing through a chain of plugin instances.
/// Output is always written as 16-bit AIFF next to the original file.
class PluginProcessingService
{
public:
    /// Returns true when the file's format can be read for processing.
    static bool canProcessFile(const juce::File& file);

    /// Returns the on-disk output path that processing would produce for the given input.
    static juce::File getProcessingOutputFile(const juce::File& input);

    /// Processes a single file through the given plugin instances in order.
    /// The instances must align index-for-index with options.plugins.
    /// The function prepares, resets, and restores state on each plugin internally before processing,
    /// and releases their resources afterwards.
    /// The caller owns the plugin instances and is responsible for thread-safety:
    /// a chain of instances must only be in use by one thread at a time.
    /// Note that a mono file running through a stereo-only plugin mid-chain
    /// keeps only the first channel in the written output,
    /// matching the single-plugin mono-through-stereo fallback behavior.
    static PluginProcessingResult processFile(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        const std::vector<juce::AudioPluginInstance*>& chainInstances
    );

private:
    /// Returns a format manager private to the calling thread, with the basic formats registered.
    /// Thread-local storage lets worker threads read files concurrently
    /// without sharing a manager or locking.
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};
