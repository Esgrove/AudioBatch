/// Background job coordination for batch plugin processing.
/// Declares PluginProcessingCoordinator,
/// which runs files through pre-instantiated plugin chains on a worker thread pool
/// and marshals per-file results, completion, and start errors back to the message thread through callbacks.

#pragma once

#include "PluginProcessing.h"

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

    /// Called on the message thread when the coordinator could not start a run.
    /// Receives the error message.
    using StartErrorCallback = std::function<void(juce::String errorMessage)>;

    /// Creates a coordinator with a thread pool sized for the current machine.
    explicit PluginProcessingCoordinator(int workerCount = juce::SystemStats::getNumCpus());

    /// Cancels any in-flight work and destroys the worker pool.
    /// Waits without a timeout so workers cannot access destroyed coordinator state.
    ~PluginProcessingCoordinator();

    PluginProcessingCoordinator(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator& operator=(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator(PluginProcessingCoordinator&&) = delete;
    PluginProcessingCoordinator& operator=(PluginProcessingCoordinator&&) = delete;

    /// Cancels queued work and waits for active jobs to finish.
    /// Safe to call on the message thread:
    /// workers waiting for plugin setup on the message thread give up once the run is cancelled,
    /// and workers that are rendering stop at the next block.
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
    /// The plugin chains of one run and the indices of the chains no worker is using.
    /// Every job of the run shares ownership,
    /// so a job that outlives a cancellation timeout keeps its chain alive
    /// and can never return a chain index to the free pool of a later run.
    struct RunChains {
        /// Takes ownership of the run's chains, with all of them free when `usable` is true.
        RunChains(std::vector<PluginChainInstances> chainInstances, bool usable);

        /// Destroys the plugin instances on the message thread, as VST3 and Audio Unit hosting require.
        /// When the last owner is a worker, the instances are handed to the message thread instead.
        ~RunChains();

        RunChains(const RunChains&) = delete;
        RunChains& operator=(const RunChains&) = delete;
        RunChains(RunChains&&) = delete;
        RunChains& operator=(RunChains&&) = delete;

        /// Returns the index of a free chain, or -1 when none are available.
        int acquire();

        /// Returns a chain to the free pool and wakes one worker waiting for a chain.
        void release(int chainIndex);

        /// Returns a raw pointer view of the given chain for the service call.
        [[nodiscard]] std::vector<juce::AudioPluginInstance*> getChainView(int chainIndex) const;

        std::vector<PluginChainInstances> chains;
        juce::CriticalSection lock;
        std::vector<int> freeIndices;
        /// Auto-reset event, signaled on release and on cancellation.
        juce::WaitableEvent available {false};
    };

    /// Invokes the completion callback, unless the run has been superseded or cancelled.
    void publishCompletion(int totalFiles, int runId) const;

    /// Invokes the result callback for one processed file,
    /// unless the run has been superseded or cancelled.
    void publishResult(const PluginProcessingResult& result, int runId) const;

    /// Reports that a run could not start to the start error callback, if one is set.
    void reportStartError(const juce::String& message) const;

    /// Processes one queued file on a worker thread.
    /// Waits for a free chain of the run, runs the file through it, publishes the result,
    /// and publishes completion when it was the last pending file of the run, counted in pendingFiles.
    /// The caller keeps the chains and the counter alive for as long as the job may run.
    void runFileJob(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        int runId,
        int totalFiles,
        RunChains& runChains,
        std::atomic<int>& pendingFiles
    ) const;

    juce::ThreadPool threadPool;
    juce::CriticalSection callbackLock;
    ResultCallback resultCallback;
    CompletionCallback completionCallback;
    StartErrorCallback startErrorCallback;

    /// Chains of the current run. Only accessed on the message thread.
    std::shared_ptr<RunChains> currentChains;

    std::atomic<int> currentRunId {0};
};
