/// Implementation of PluginProcessingCoordinator.
/// Covers queuing one thread-pool job per file,
/// the per-run free pool of pre-instantiated plugin chains that workers acquire and release between files,
/// run-id based cancellation, and publishing results and completion to the message thread.
/// Cancellation also releases workers waiting for plugin setup on the message thread,
/// so cancelAndWait can block the message thread without deadlocking.
/// Plugin instances are always destroyed on the message thread to satisfy VST3 and Audio Unit hosting requirements.

#include "plugins/PluginProcessingCoordinator.h"

#include "normalization/AudioNormalization.h"
#include "plugins/PluginProcessing.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// How long cancelAndWait waits for running plugin processing jobs to finish before giving up, in milliseconds.
constexpr int jobShutdownTimeoutMilliseconds {60'000};

/// How long a worker waits for a plugin chain to be released before checking for cancellation again, in milliseconds.
constexpr int chainWaitIntervalMilliseconds {100};

/// Returns whether every chain aligns index-for-index with options.plugins and contains no null instances.
/// At least one chain is required.
bool areChainsUsable(const std::vector<PluginChainInstances>& chains, const PluginProcessingOptions& options)
{
    const auto isUsableChain = [&options](const PluginChainInstances& chain) {
        return !chain.empty() && chain.size() == options.plugins.size()
            && std::ranges::none_of(chain, [](const auto& instance) { return instance == nullptr; });
    };

    return !chains.empty() && std::ranges::all_of(chains, isUsableChain);
}

/// Returns existing records in caller order, retaining the first occurrence of each full path.
std::vector<AudioAnalysisRecord> collectQueuedRecords(const std::vector<AudioAnalysisRecord>& records)
{
    std::set<juce::String> seenPaths;
    std::vector<AudioAnalysisRecord> queuedRecords;
    queuedRecords.reserve(records.size());
    for (const auto& record : records) {
        if (record.file.existsAsFile() && seenPaths.insert(record.fullPath).second) {
            queuedRecords.push_back(record);
        }
    }

    return queuedRecords;
}
}  // namespace

/// Reserves one physical core for the UI and other work, with a half-logical-core fallback.
/// At least one worker is available even when the machine has few cores or reports invalid counts.
int getPluginProcessingWorkerCount(const int physicalCoreCount, const int logicalCoreCount)
{
    const auto processingCores = physicalCoreCount > 0 ? physicalCoreCount : juce::jmax(0, logicalCoreCount) / 2;
    return juce::jmax(1, processingCores - 1);
}

/// Creates a coordinator with a thread pool sized for the current machine.
PluginProcessingCoordinator::PluginProcessingCoordinator(const int workerCount) : threadPool(juce::jmax(1, workerCount))
{}

/// Cancels any in-flight work and destroys the worker pool.
/// Waits without a timeout so workers cannot access destroyed coordinator state.
PluginProcessingCoordinator::~PluginProcessingCoordinator()
{
    cancelAndWait();
    threadPool.removeAllJobs(true, -1);
    currentChains.reset();
}

/// Takes ownership of the run's chains, with all of them free when `usable` is true.
PluginRunChains::PluginRunChains(std::vector<PluginChainInstances> chainInstances, const bool usable) :
    chains(std::move(chainInstances))
{
    if (usable) {
        freeIndices.reserve(chains.size());

        for (int chainIndex = 0; std::cmp_less(chainIndex, chains.size()); ++chainIndex) {
            freeIndices.push_back(chainIndex);
        }
    }
}

/// Destroys the plugin instances on the message thread, as VST3 and Audio Unit hosting require.
/// When the last owner is a worker, the instances are handed to the message thread instead.
PluginRunChains::~PluginRunChains()
{
    if (chains.empty() || juce::MessageManager::existsAndIsCurrentThread()) {
        return;
    }

    juce::MessageManager::callAsync([chainsCaptured = std::make_shared<decltype(chains)>(std::move(chains))] {
        chainsCaptured->clear();
    });
}

/// Returns the index of a free chain, or -1 when none are available.
int PluginRunChains::acquire()
{
    const juce::ScopedLock scopedLock(lock);

    if (freeIndices.empty()) {
        return -1;
    }

    const auto chainIndex = freeIndices.back();
    freeIndices.pop_back();
    return chainIndex;
}

/// Returns a chain to the free pool and wakes one worker waiting for a chain.
void PluginRunChains::release(const int chainIndex)
{
    if (chainIndex < 0) {
        return;
    }

    {
        const juce::ScopedLock scopedLock(lock);
        freeIndices.push_back(chainIndex);
    }

    available.signal();
}

/// Returns a raw pointer view of the given chain for the service call.
std::vector<juce::AudioPluginInstance*> PluginRunChains::getChainView(const int chainIndex) const
{
    return chains[static_cast<std::size_t>(chainIndex)]
        | std::views::transform([](const auto& instance) { return instance.get(); }) | std::ranges::to<std::vector>();
}

/// Sets the callback that is invoked for each published result.
void PluginProcessingCoordinator::setResultCallback(ResultCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    resultCallback = std::move(callback);
}

/// Sets the callback that is invoked when a run completes.
void PluginProcessingCoordinator::setCompletionCallback(CompletionCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    completionCallback = std::move(callback);
}

/// Sets the callback that is invoked if starting fails.
void PluginProcessingCoordinator::setStartErrorCallback(StartErrorCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    startErrorCallback = std::move(callback);
}

/// Sets the callback that is invoked with the processed fraction of each file.
void PluginProcessingCoordinator::setProgressCallback(ProgressCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    progressCallback = std::move(callback);
}

/// Sets the callback that is invoked when a file's processing begins.
void PluginProcessingCoordinator::setStartingCallback(StartingCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    startingCallback = std::move(callback);
}

/// Invokes the progress callback for one file being processed, unless the run has been superseded or cancelled.
void PluginProcessingCoordinator::publishProgress(const juce::File& file, const float progress, const int runId) const
{
    if (runId != currentRunId.load()) {
        return;
    }

    ProgressCallback callbackCopy;
    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = progressCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(file, progress);
    }
}

/// Invokes the starting callback for a file whose processing begins, unless the run has been superseded or cancelled.
void PluginProcessingCoordinator::publishStarting(const juce::File& file, const int runId) const
{
    if (runId != currentRunId.load()) {
        return;
    }

    StartingCallback callbackCopy;
    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = startingCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(file);
    }
}

/// Invokes the result callback for one processed file, unless the run has been superseded or cancelled.
void PluginProcessingCoordinator::publishResult(const PluginProcessingResult& result, const int runId) const
{
    if (runId != currentRunId.load()) {
        return;
    }

    ResultCallback callbackCopy;
    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = resultCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(result);
    }
}

/// Invokes the completion callback, unless the run has been superseded or cancelled.
void PluginProcessingCoordinator::publishCompletion(const int totalFiles, const int runId) const
{
    if (runId != currentRunId.load()) {
        return;
    }

    CompletionCallback callbackCopy;
    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = completionCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(totalFiles);
    }
}

/// Cancels queued work and waits for active jobs to finish.
/// Safe to call on the message thread:
/// workers waiting for plugin setup on the message thread give up once the run is cancelled,
/// and workers that are rendering stop at the next block.
void PluginProcessingCoordinator::cancelAndWait()
{
    ++currentRunId;

    // Wake any workers blocked waiting for a chain.
    if (currentChains != nullptr) {
        currentChains->available.signal();
    }

    threadPool.removeAllJobs(true, jobShutdownTimeoutMilliseconds);
}

/// Reports that a run could not start to the start error callback, if one is set.
void PluginProcessingCoordinator::reportStartError(const juce::String& message) const
{
    StartErrorCallback errorCallback;
    {
        const juce::ScopedLock lock(callbackLock);
        errorCallback = startErrorCallback;
    }

    if (errorCallback != nullptr) {
        errorCallback(message);
    }
}

/// Processes one queued file on a worker thread.
/// Waits for a free chain of the run, runs the file through it, publishes the result,
/// and publishes completion when it was the last pending file of the run, counted in pendingFiles.
/// The caller keeps the chains and the counter alive for as long as the job may run.
void PluginProcessingCoordinator::runFileJob(
    const AudioAnalysisRecord& record,
    const PluginProcessingOptions& options,
    const int runId,
    const int totalFiles,
    PluginRunChains& runChains,
    std::atomic<int>& pendingFiles
) const
{
    if (runId != currentRunId.load()) {
        return;
    }

    auto chainIndex = runChains.acquire();

    // Wait for a chain to become free (worker count > chain count is unusual).
    while (chainIndex < 0 && runId == currentRunId.load()) {
        runChains.available.wait(chainWaitIntervalMilliseconds);
        chainIndex = runChains.acquire();
    }

    if (runId != currentRunId.load()) {
        runChains.release(chainIndex);
        return;
    }

    publishStarting(record.file, runId);
    const auto onProgress = utils::makeThrottledProgress([this, &record, runId](const float progress) {
        publishProgress(record.file, progress, runId);
    });

    PluginProcessingResult result;

    try {
        // Cancellation lets the worker stop waiting for the message thread,
        // which cancelAndWait blocks while it waits for the workers to finish.
        result = processFileWithPlugins(
            record,
            options,
            runChains.getChainView(chainIndex),
            [this, runId] { return runId != currentRunId.load(); },
            onProgress
        );
    } catch (...) {
        // Plugins are third-party code, so an exception from one fails only this file
        // and its chain still goes back to the pool.
        const auto message = utils::format("Plugin processing failed: {}", utils::describeCurrentException());
        utils::logError("Plugin processing failed for {}: {}", record.fullPath.quoted(), message);
        result = PluginProcessingResult::failure(record.file, message);
    }

    runChains.release(chainIndex);

    publishResult(result, runId);

    if (pendingFiles.fetch_sub(1) == 1) {
        publishCompletion(totalFiles, runId);
    }
}

/// Starts a background processing run.
/// Plugin chains must be pre-instantiated on the message thread by the caller
/// and ownership is transferred to the coordinator.
/// Each chain's instances must align index-for-index with options.plugins.
/// One chain is consumed by each worker.
/// Chains are returned to a free pool between files.
/// Returns the number of queued files.
int PluginProcessingCoordinator::start(
    const std::vector<AudioAnalysisRecord>& records,
    const PluginProcessingOptions& options,
    std::vector<PluginChainInstances> chainInstances
)
{
    cancelAndWait();

    const auto chainsUsable = areChainsUsable(chainInstances, options);
    // Replacing the previous run's chains destroys them here on the message thread,
    // unless a job that outlived the cancellation timeout still holds them.
    currentChains = std::make_shared<PluginRunChains>(std::move(chainInstances), chainsUsable);

    if (!chainsUsable) {
        reportStartError("No plugin chain instances were available to start processing.");
        publishCompletion(0, currentRunId.load());
        return 0;
    }

    const auto runId = currentRunId.load();
    const auto queuedRecords = collectQueuedRecords(records);
    const auto totalFiles = static_cast<int>(queuedRecords.size());

    if (queuedRecords.empty()) {
        publishCompletion(0, runId);
        return 0;
    }

    const auto conflicts = findBatchOutputConflicts(queuedRecords);
    // The counter belongs to this run, so a job that outlives a cancellation timeout cannot change a later run's count.
    const auto pendingFiles = std::make_shared<std::atomic<int>>(totalFiles);
    // The options hold the saved state of every plugin, so the jobs share one copy instead of each holding their own.
    const auto runOptions = std::make_shared<const PluginProcessingOptions>(options);

    const auto concurrentFiles
        = juce::jmin(totalFiles, threadPool.getNumThreads(), static_cast<int>(currentChains->chains.size()));
    utils::logInfo("Plugin processing started: {} files, up to {} concurrent", totalFiles, concurrentFiles);

    for (const auto& record : queuedRecords) {
        if (conflicts.contains(record.file.getFullPathName())) {
            threadPool.addJob([this, record, runId, totalFiles, pendingFiles] {
                if (runId != currentRunId.load()) {
                    return;
                }

                publishResult(
                    PluginProcessingResult::failure(record.file, batchOutputConflictMessage(record.file)), runId
                );

                if (pendingFiles->fetch_sub(1) == 1) {
                    publishCompletion(totalFiles, runId);
                }
            });
            continue;
        }

        threadPool.addJob([this, record, runOptions, runId, totalFiles, runChains = currentChains, pendingFiles] {
            runFileJob(record, *runOptions, runId, totalFiles, *runChains, *pendingFiles);
        });
    }

    return totalFiles;
}
}  // namespace audiobatch
