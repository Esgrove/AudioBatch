/// Background job coordination for batch plugin processing.
/// Declares PluginProcessingCoordinator,
/// which runs files through pre-instantiated plugin chains on a worker thread pool
/// and marshals per-file results, completion, and start errors back to the message thread through callbacks.

#pragma once

#include "plugins/PluginProcessing.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace audiobatch
{
/// One pre-instantiated chain of plugin instances, in processing order.
using PluginChainInstances = std::vector<std::unique_ptr<juce::AudioPluginInstance>>;

/// The plugin chains of one run and the indices of the chains no worker is using.
/// Every job of the run shares ownership, so a job that outlives a cancellation timeout keeps its chain alive
/// and can never return a chain index to the free pool of a later run.
struct PluginRunChains {
    PluginRunChains(std::vector<PluginChainInstances> chainInstances, bool usable);

    ~PluginRunChains();

    PluginRunChains(const PluginRunChains&) = delete;
    PluginRunChains& operator=(const PluginRunChains&) = delete;
    PluginRunChains(PluginRunChains&&) = delete;
    PluginRunChains& operator=(PluginRunChains&&) = delete;

    int acquire();

    void release(int chainIndex);

    [[nodiscard]] std::vector<juce::AudioPluginInstance*> getChainView(int chainIndex) const;

    /// The plugin instances of every chain, owned for the whole run.
    std::vector<PluginChainInstances> chains;
    /// Guards freeIndices.
    juce::CriticalSection lock;
    /// Indices of the chains no worker is using.
    std::vector<int> freeIndices;
    /// Auto-reset event, signaled on release and on cancellation.
    juce::WaitableEvent available {false};
};

/// Coordinates background plugin-processing jobs and marshals results back to the UI layer.
class PluginProcessingCoordinator
{
public:
    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles)>;

    /// Called for each processed file as plugin-processing results become available.
    using ResultCallback = std::function<void(const PluginProcessingResult& result)>;

    /// Called on the message thread when the coordinator could not start a run.
    /// Receives the error message.
    using StartErrorCallback = std::function<void(juce::String errorMessage)>;

    explicit PluginProcessingCoordinator(int workerCount = juce::SystemStats::getNumCpus());

    ~PluginProcessingCoordinator();

    PluginProcessingCoordinator(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator& operator=(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator(PluginProcessingCoordinator&&) = delete;
    PluginProcessingCoordinator& operator=(PluginProcessingCoordinator&&) = delete;

    void cancelAndWait();

    void setCompletionCallback(CompletionCallback callback);

    void setResultCallback(ResultCallback callback);

    void setStartErrorCallback(StartErrorCallback callback);

    int start(
        const std::vector<AudioAnalysisRecord>& records,
        const PluginProcessingOptions& options,
        std::vector<PluginChainInstances> chainInstances
    );

private:
    void publishCompletion(int totalFiles, int runId) const;

    void publishResult(const PluginProcessingResult& result, int runId) const;

    void reportStartError(const juce::String& message) const;

    void runFileJob(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        int runId,
        int totalFiles,
        PluginRunChains& runChains,
        std::atomic<int>& pendingFiles
    ) const;

    juce::ThreadPool threadPool;
    juce::CriticalSection callbackLock;
    ResultCallback resultCallback;
    CompletionCallback completionCallback;
    StartErrorCallback startErrorCallback;

    /// Chains of the current run. Only accessed on the message thread.
    std::shared_ptr<PluginRunChains> currentChains;

    std::atomic<int> currentRunId {0};
};
}  // namespace audiobatch
