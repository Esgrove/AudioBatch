/// Offline rendering of audio files through a plugin chain.
/// Declares PluginProcessingService, a stateless service
/// that processes one file at a time through a caller-owned sequence of plugin instances
/// and writes the result as `<name>.aif` next to the original file.

#pragma once

#include "Cancellation.h"
#include "PluginProcessing.h"

#include <JuceHeader.h>

#include <functional>
#include <vector>

/// Stateless audio-file processing through a chain of plugin instances.
/// Output is always written as `<name>.aif` next to the original file, at the source bit depth up to 24 bits.
class PluginProcessingService
{
public:
    /// Returns true when the caller wants processing to stop as soon as possible.
    using CancellationCheck = utils::CancellationCheck;

    static bool canProcessFile(const juce::File& file);

    static juce::File getProcessingOutputFile(const juce::File& input);

    static PluginProcessingResult processFile(
        const AudioAnalysisRecord& sourceRecord,
        const PluginProcessingOptions& options,
        const std::vector<juce::AudioPluginInstance*>& chainInstances,
        const CancellationCheck& isCancelled = {}
    );
};
