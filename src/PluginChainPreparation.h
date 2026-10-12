/// Per-file preparation of a hosted plugin chain for offline processing.
/// Declares prepareChain, which configures the buses, offline mode, saved state,
/// and processing setup of every plugin for one file, and releasePluginResources, which undoes it.

#pragma once

#include "PluginProcessing.h"

#include <JuceHeader.h>

#include <vector>

namespace audiobatch::plugin_processing
{
/// Block size in samples the chain is prepared for and processed in.
constexpr int processingBlockSize = 1024;

/// Values gathered while preparing a plugin chain for one file that rendering needs.
struct ChainPreparation {
    /// Empty when every plugin in the chain was prepared.
    juce::String errorMessage;
    /// Widest channel count of any plugin in the chain, at least the file's channel count.
    int processChannelCount = 0;
    /// Sum of the individually clamped per-plugin tails.
    juce::int64 tailSamples = 0;
    /// Sum of the latencies the plugins report after preparation.
    juce::int64 latencySamples = 0;
};

/// Configures the buses, enables offline rendering, restores the saved state,
/// prepares, and resets every plugin in the chain for one file,
/// and collects the channel count, tail, and latency of the chain.
/// Must run on the message thread:
/// the VST3 specification restricts bus arrangement, setupProcessing, setActive, setState,
/// and the tail and latency queries to the UI thread,
/// and JUCE's VST3 host asserts the message thread in prepareToPlay and setStateInformation.
/// When a plugin rejects the file's channel layout,
/// the plugins prepared before it are released again so the chain stays usable for the next file.
ChainPreparation prepareChain(
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const PluginProcessingOptions& options,
    int numChannels,
    double sampleRate
);

/// Releases the resources of every plugin in the chain, leaving the chain inactive for the next file.
/// Must run on the message thread, because releasing a VST3 plugin deactivates it through setActive.
/// Plugins that are not prepared ignore the call.
void releasePluginResources(const std::vector<juce::AudioPluginInstance*>& chainInstances);
}  // namespace audiobatch::plugin_processing
