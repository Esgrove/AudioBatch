/// Per-file preparation of a hosted plugin chain for offline processing.
/// Declares prepareChain, which configures the buses, offline mode, saved state,
/// and processing setup of every plugin for one file, and releasePluginResources, which undoes it.
/// PreparedChain pairs the two on the message thread, so a prepared chain is always released again.

#pragma once

#include "plugins/PluginProcessing.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <optional>
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

/// Prepares a plugin chain for one file on the message thread and releases it again when destroyed,
/// so no path leaves the chain active for the next file, including an exception thrown by a plugin.
/// After cancellation the release is skipped, because the message thread may be blocked waiting for the worker,
/// and the plugin instances release their resources when they are destroyed instead.
/// Refers to the chain instances and the cancellation check, which must outlive it.
class PreparedChain
{
public:
    PreparedChain(
        std::span<juce::AudioPluginInstance* const> instances,
        const utils::CancellationCheck& cancellationCheck
    );
    ~PreparedChain();
    PreparedChain(const PreparedChain&) = delete;
    PreparedChain(PreparedChain&&) = delete;
    PreparedChain& operator=(const PreparedChain&) = delete;
    PreparedChain& operator=(PreparedChain&&) = delete;

    [[nodiscard]] std::optional<ChainPreparation> prepare(
        const PluginProcessingOptions& options,
        int numChannels,
        double sampleRate
    );

private:
    /// The instances of the chain in chain order, which the caller owns.
    std::span<juce::AudioPluginInstance* const> chainInstances;
    /// The cancellation check of the run, which skips the release on the message thread once it returns true.
    const utils::CancellationCheck& isCancelled;
    /// True after a preparation succeeded, so the destructor releases the chain again.
    bool prepared {false};
};

ChainPreparation prepareChain(
    std::span<juce::AudioPluginInstance* const> chainInstances,
    const PluginProcessingOptions& options,
    int numChannels,
    double sampleRate
);

[[nodiscard]] juce::int64 getPluginTailSamples(const juce::AudioPluginInstance& plugin, double sampleRate);
[[nodiscard]] juce::String formatPluginLatency(juce::int64 samples, double sampleRate);

void addToPreparation(ChainPreparation& preparation, const juce::AudioPluginInstance& plugin, double sampleRate);

bool configurePluginChannels(juce::AudioPluginInstance& plugin, int numChannels);
bool configurePreviewChannels(juce::AudioPluginInstance& plugin);

void releasePluginResources(std::span<juce::AudioPluginInstance* const> chainInstances);
void restoreStateAndPrepare(
    juce::AudioPluginInstance& plugin,
    const juce::MemoryBlock& state,
    double sampleRate,
    int blockSize
);
}  // namespace audiobatch::plugin_processing
