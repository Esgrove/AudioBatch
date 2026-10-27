/// Background job coordination for batch plugin processing.
/// Declares PluginProcessingCoordinator,
/// which runs files through pre-instantiated plugin chains on a worker thread pool
/// and marshals per-file starts, progress, results, completion, and start
/// errors back to the message thread through callbacks.

#pragma once

#include "plugins/PluginProcessing.h"
#include "utils/BackgroundRunner.h"

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
    /// Guards freeIndices.
    juce::CriticalSection lock;
    /// Indices of the chains no worker is using.
    std::vector<int> freeIndices;
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
    /// The run whose pool the chain goes back to.
    PluginRunChains& runChains;
    /// Index of the leased chain in the run.
    int chainIndex;
};

/// Coordinates background plugin-processing jobs and marshals results back to the UI layer.
class PluginProcessingCoordinator
{
public:
    // Every callback of a run carries the id of the run it belongs to,
    // so a receiver that handles them asynchronously can drop messages from a run that ended meanwhile.

    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles, int runId)>;

    /// Called from the worker thread with the processed fraction of a file, throttled to a few updates per second.
    using ProgressCallback = std::function<void(const juce::File& file, float progress, int runId)>;

    /// Called for each processed file as plugin-processing results become available.
    using ResultCallback = std::function<void(const PluginProcessingResult& result, int runId)>;

    /// Called from the worker thread once a file has a plugin chain and its processing begins.
    using StartingCallback = std::function<void(const juce::File& file, int runId)>;

    /// Called on the message thread when the coordinator could not start a run.
    /// Receives the error message, and no run id, because no run started.
    using StartErrorCallback = std::function<void(juce::String errorMessage)>;

    explicit PluginProcessingCoordinator(int workerCount = getPluginProcessingWorkerCount());

    ~PluginProcessingCoordinator();

    PluginProcessingCoordinator(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator& operator=(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator(PluginProcessingCoordinator&&) = delete;
    PluginProcessingCoordinator& operator=(PluginProcessingCoordinator&&) = delete;

    [[nodiscard]] int getCurrentRunId() const noexcept;

    void cancel();

    void cancelAndWait();

    void requestStop();

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
    void runFileJob(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        int runId,
        int totalFiles,
        PluginRunChains& runChains,
        std::atomic<int>& pendingFiles
    ) const;

    [[nodiscard]] PluginProcessingResult processLeasedFile(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        int runId,
        std::span<juce::AudioPluginInstance* const> chainView
    ) const;

    /// Receives the completion of each run.
    RunCallback<int, int> completionCallback;
    /// Receives the processed fraction of each file.
    RunCallback<const juce::File&, float, int> progressCallback;
    /// Receives the result of each file.
    RunCallback<const PluginProcessingResult&, int> resultCallback;
    /// Receives the reason a run could not start its workers.
    RunCallback<juce::String> startErrorCallback;
    /// Receives each file a worker starts on.
    RunCallback<const juce::File&, int> startingCallback;

    /// Chains of the current run. Only accessed on the message thread.
    std::shared_ptr<PluginRunChains> currentChains;

    /// Declared last, so the callbacks and chains the jobs use outlive it.
    BackgroundRunner runner;
};
}  // namespace audiobatch
