/// Per-file preparation of a hosted plugin chain for offline processing.
/// Declares prepareChain, which configures the buses, offline mode, saved state,
/// and processing setup of every plugin for one file, and releasePluginResources, which undoes it.

#pragma once

#include "plugins/PluginProcessing.h"

#include <JuceHeader.h>

#include <span>

namespace audiobatch::plugin_processing
{
/// Block size in samples the chain is prepared for and processed in.
constexpr int processingBlockSize {1024};
/// Per-plugin bound shared by offline rendering and live preview.
constexpr double maximumTailSeconds {20.0};

/// Values gathered while preparing a plugin chain for one file that rendering needs.
struct ChainPreparation {
    /// Empty when every plugin in the chain was prepared.
    juce::String errorMessage;
    /// Widest channel count of any plugin in the chain, at least the file's channel count.
    int processChannelCount {0};
    /// Sum of the individually clamped per-plugin tails.
    juce::int64 tailSamples {0};
    /// Sum of the latencies the plugins report after preparation.
    juce::int64 latencySamples {0};
};

ChainPreparation prepareChain(
    std::span<juce::AudioPluginInstance* const> chainInstances,
    const PluginProcessingOptions& options,
    int numChannels,
    double sampleRate
);

[[nodiscard]] juce::int64 getPluginTailSamples(const juce::AudioPluginInstance& plugin, double sampleRate);
[[nodiscard]] juce::String formatPluginLatency(juce::int64 samples, double sampleRate);

bool configurePluginChannels(juce::AudioPluginInstance& plugin, int numChannels, double sampleRate, int blockSize);

void releasePluginResources(std::span<juce::AudioPluginInstance* const> chainInstances);
void restoreStateAndPrepare(
    juce::AudioPluginInstance& plugin,
    const juce::MemoryBlock& state,
    double sampleRate,
    int blockSize
);
}  // namespace audiobatch::plugin_processing
