#pragma once

#include "PluginProcessing.h"
#include "PluginProcessingService.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

/// Coordinates background plugin-processing jobs and marshals results back to the UI layer.
class PluginProcessingCoordinator
{
public:
    /// One pre-instantiated chain of plugin instances, in processing order.
    using PluginChainInstances = std::vector<std::unique_ptr<juce::AudioPluginInstance>>;

    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles)>;

    /// Called for each processed file as plugin-processing results become available.
    using ResultCallback = std::function<void(const PluginProcessingResult& result)>;

    /// Called on the message thread when the coordinator could not start a run; receives the error message.
    using StartErrorCallback = std::function<void(juce::String errorMessage)>;

    /// Creates a coordinator with a thread pool sized for the current machine.
    explicit PluginProcessingCoordinator(int workerCount = juce::SystemStats::getNumCpus());

    /// Cancels any in-flight work and destroys the worker pool.
    ~PluginProcessingCoordinator();

    /// Cancels queued work and waits for active jobs to finish.
    void cancelAndWait();

    /// Sets the callback that is invoked when a run completes.
    void setCompletionCallback(CompletionCallback callback);

    /// Sets the callback that is invoked for each published result.
    void setResultCallback(ResultCallback callback);

    /// Sets the callback that is invoked if starting fails.
    void setStartErrorCallback(StartErrorCallback callback);

    /// Starts a background processing run.
    /// Plugin chains must be pre-instantiated on the message thread by the caller
    /// and ownership is transferred to the coordinator.
    /// Each chain's instances must align index-for-index with options.plugins.
    /// One chain is consumed by each worker.
    /// Chains are returned to a free pool between files.
    /// Returns the number of queued files.
    int start(
        const std::vector<AudioAnalysisRecord>& records,
        const PluginProcessingOptions& options,
        std::vector<PluginChainInstances> chainInstances
    );

private:
    void publishCompletion(int totalFiles, int runId) const;
    void publishResult(const PluginProcessingResult& result, int runId) const;

    /// Returns the index of a free chain, or -1 when none are available.
    int acquireChain();
    void releaseChain(int chainIndex);

    juce::ThreadPool threadPool;
    juce::CriticalSection callbackLock;
    ResultCallback resultCallback;
    CompletionCallback completionCallback;
    StartErrorCallback startErrorCallback;

    juce::CriticalSection instanceLock;
    juce::WaitableEvent instanceAvailable {false};  ///< Auto-reset: signaled on releaseChain().
    std::vector<PluginChainInstances> ownedChains;
    std::vector<int> freeChainIndices;

    std::atomic<int> currentRunId {0};
    std::atomic<int> pendingJobs {0};
};
