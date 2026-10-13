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
    /// The plugin chains of one run and the indices of the chains no worker is using.
    /// Every job of the run shares ownership, so a job that outlives a cancellation timeout keeps its chain alive
    /// and can never return a chain index to the free pool of a later run.
    struct RunChains {
        RunChains(std::vector<PluginChainInstances> chainInstances, bool usable);

        ~RunChains();

        RunChains(const RunChains&) = delete;
        RunChains& operator=(const RunChains&) = delete;
        RunChains(RunChains&&) = delete;
        RunChains& operator=(RunChains&&) = delete;

        int acquire();

        void release(int chainIndex);

        [[nodiscard]] std::vector<juce::AudioPluginInstance*> getChainView(int chainIndex) const;

        std::vector<PluginChainInstances> chains;
        juce::CriticalSection lock;
        std::vector<int> freeIndices;
        /// Auto-reset event, signaled on release and on cancellation.
        juce::WaitableEvent available {false};
    };

    void publishCompletion(int totalFiles, int runId) const;

    void publishResult(const PluginProcessingResult& result, int runId) const;

    void reportStartError(const juce::String& message) const;

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
