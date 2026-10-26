/// Background job coordination for batch plugin processing.
/// Declares PluginProcessingCoordinator,
/// which runs files through pre-instantiated plugin chains on a worker thread pool
/// and marshals per-file starts, progress, results, completion, and start
/// errors back to the message thread through callbacks.

#pragma once

#include "plugins/PluginProcessing.h"
#include "utils/Synchronized.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch
{
int getPluginProcessingWorkerCount(
    int physicalCoreCount = juce::SystemStats::getNumPhysicalCpus(),
    int logicalCoreCount = juce::SystemStats::getNumCpus()
);

/// One pre-instantiated chain of plugin instances, in processing order.
using PluginChainInstances = std::vector<std::unique_ptr<juce::AudioPluginInstance>>;

/// The callbacks a PluginProcessingCoordinator publishes to, each of which may be empty.
struct PluginProcessingCallbacks {
    /// Called once after a run has published all queued results.
    std::function<void(int totalFiles)> completion;
    /// Called from the worker thread with the processed fraction of a file, throttled to a few updates per second.
    std::function<void(const juce::File& file, float progress)> progress;
    /// Called for each processed file as plugin-processing results become available.
    std::function<void(const PluginProcessingResult& result)> result;
    /// Called from the worker thread once a file has a plugin chain and its processing begins.
    std::function<void(const juce::File& file)> starting;
    /// Called on the message thread when the coordinator could not start a run.
    /// Receives the error message.
    std::function<void(juce::String errorMessage)> startError;
};

class ChainLease;

/// The plugin chains of one run and the pool of the chains no worker is using.
/// Every job of the run shares ownership, so a job that outlives a cancellation timeout keeps its chain alive
/// and can never return a chain to the free pool of a later run.
class PluginRunChains
{
public:
    PluginRunChains(std::vector<PluginChainInstances> chainInstances, bool usable);

    ~PluginRunChains();

    PluginRunChains(const PluginRunChains&) = delete;
    PluginRunChains& operator=(const PluginRunChains&) = delete;
    PluginRunChains(PluginRunChains&&) = delete;
    PluginRunChains& operator=(PluginRunChains&&) = delete;

    [[nodiscard]] std::optional<ChainLease> acquire(const std::function<bool()>& keepWaiting);

    [[nodiscard]] int getNumChains() const noexcept;

    void wakeWaitingWorkers();

private:
    friend class ChainLease;

    void release(int chainIndex);

    /// The plugin instances of every chain, owned for the whole run.
    std::vector<PluginChainInstances> chains;
    /// Indices of the chains no worker is using.
    utils::Synchronized<std::vector<int>> freeIndices;
    /// Auto-reset event, signaled when a chain goes back to the pool and on cancellation.
    juce::WaitableEvent available {false};
};

/// A chain a worker took from the free pool of a run, which goes back to the pool when the lease is destroyed.
/// The run must outlive the lease.
class ChainLease
{
public:
    ChainLease(PluginRunChains& chains, int index) noexcept;

    ~ChainLease();

    ChainLease(const ChainLease&) = delete;
    ChainLease& operator=(const ChainLease&) = delete;
    ChainLease(ChainLease&&) = delete;
    ChainLease& operator=(ChainLease&&) = delete;

    [[nodiscard]] std::vector<juce::AudioPluginInstance*> getChainView() const;

private:
    PluginRunChains& runChains;
    int chainIndex;
};

/// Coordinates background plugin-processing jobs and marshals results back to the UI layer.
class PluginProcessingCoordinator
{
public:
    /// Type of the completion callback in PluginProcessingCallbacks.
    using CompletionCallback = decltype(PluginProcessingCallbacks::completion);

    /// Type of the progress callback in PluginProcessingCallbacks.
    using ProgressCallback = decltype(PluginProcessingCallbacks::progress);

    /// Type of the result callback in PluginProcessingCallbacks.
    using ResultCallback = decltype(PluginProcessingCallbacks::result);

    /// Type of the starting callback in PluginProcessingCallbacks.
    using StartingCallback = decltype(PluginProcessingCallbacks::starting);

    /// Type of the startError callback in PluginProcessingCallbacks.
    using StartErrorCallback = decltype(PluginProcessingCallbacks::startError);

    explicit PluginProcessingCoordinator(int workerCount = getPluginProcessingWorkerCount());

    ~PluginProcessingCoordinator();

    PluginProcessingCoordinator(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator& operator=(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator(PluginProcessingCoordinator&&) = delete;
    PluginProcessingCoordinator& operator=(PluginProcessingCoordinator&&) = delete;

    void cancelAndWait();

    void setCompletionCallback(CompletionCallback callback);

    void setProgressCallback(ProgressCallback callback);

    void setResultCallback(ResultCallback callback);

    void setStartErrorCallback(StartErrorCallback callback);

    void setStartingCallback(StartingCallback callback);

    [[nodiscard]] int start(
        const std::vector<AudioAnalysisRecord>& records,
        const PluginProcessingOptions& options,
        std::vector<PluginChainInstances> chainInstances
    );

private:
    void publishCompletion(int totalFiles, int runId) const;
    void publishProgress(const juce::File& file, float progress, int runId) const;
    void publishResult(const PluginProcessingResult& result, int runId) const;
    void publishStarting(const juce::File& file, int runId) const;
    void reportStartError(const juce::String& message) const;

    void runFileJob(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        int runId,
        int totalFiles,
        PluginRunChains& runChains,
        std::atomic<int>& pendingFiles
    ) const;

    CompletionCallback completionCallback;
    ProgressCallback progressCallback;
    ResultCallback resultCallback;
    StartErrorCallback startErrorCallback;
    utils::Synchronized<PluginProcessingCallbacks> callbacks;
    juce::ThreadPool threadPool;

    /// Chains of the current run. Only accessed on the message thread.
    std::shared_ptr<PluginRunChains> currentChains;

    std::atomic<int> currentRunId {0};
};
}  // namespace audiobatch
