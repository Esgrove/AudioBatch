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

ChainPreparation prepareChain(
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const PluginProcessingOptions& options,
    int numChannels,
    double sampleRate
);

void releasePluginResources(const std::vector<juce::AudioPluginInstance*>& chainInstances);
}  // namespace audiobatch::plugin_processing
