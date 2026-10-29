/// Implementation of PluginProcessingCoordinator.
/// Covers queuing one thread-pool job per file,
/// the per-run free pool of pre-instantiated plugin chains that workers acquire and release between files,
/// run-id based cancellation, and publishing results and completion to the message thread.
/// Cancellation also releases workers waiting for plugin setup on the message thread,
/// so cancelAndWait can block the message thread without deadlocking.
/// Plugin instances are always destroyed on the message thread to satisfy VST3 and Audio Unit hosting requirements.

#include "plugins/PluginProcessingCoordinator.h"

#include "analysis/AudioAnalysisTypes.h"
#include "normalization/AudioNormalization.h"
#include "plugins/PluginProcessing.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <utility>
#include <variant>
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

/// Returns the records in caller order, retaining the first occurrence of each full path.
/// A file that has gone missing is kept, so its processing fails with a message instead of being left out silently.
std::vector<AudioAnalysisRecord> collectQueuedRecords(const std::vector<AudioAnalysisRecord>& records)
{
    std::set<juce::String> seenPaths;
    std::vector<AudioAnalysisRecord> queuedRecords;
    queuedRecords.reserve(records.size());
    for (const auto& record : records) {
        if (seenPaths.insert(record.fullPath).second) {
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
PluginProcessingCoordinator::PluginProcessingCoordinator(const int workerCount) :
    runner(workerCount, jobShutdownTimeoutMilliseconds)
{}

/// Cancels any in-flight work and waits for every job, so workers cannot access destroyed coordinator state.
/// A plugin's processBlock never checks for cancellation, so a plugin that hangs would keep the app from quitting.
/// When a job is still running after the shutdown timeout, the process ends instead of waiting forever.
/// Originals are only replaced by finished outputs, so ending the process leaves them untouched.
PluginProcessingCoordinator::~PluginProcessingCoordinator()
{
    cancelAndWait();
    if (runner.hasJobs()) {
        utils::logError(
            "A plugin did not stop within {:.0f} s, so AudioBatch ends without waiting for it",
            jobShutdownTimeoutMilliseconds / utils::millisecondsPerSecond
        );
        juce::Process::terminate();
    }
    runner.waitForAllJobs();
    currentChains.reset();
}

/// Returns the id of the latest run, which its callbacks carry.
int PluginProcessingCoordinator::getCurrentRunId() const noexcept
{
    return runner.getCurrentRunId();
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

    auto chainsCaptured = std::make_shared<decltype(chains)>(std::move(chains));
    if (juce::MessageManager::callAsync([chainsCaptured] { chainsCaptured->clear(); })) {
        return;
    }

    // Posting fails once the app is quitting and the message loop no longer runs.
    // Destroying the instances here would release plugins off the message thread,
    // so they are left for the operating system to reclaim when the process exits.
    auto leaked = std::make_unique<std::shared_ptr<decltype(chains)>>(std::move(chainsCaptured));
    juce::ignoreUnused(leaked.release());
}

/// Takes a free chain from the pool, waiting for one while keepWaiting returns true.
/// Returns a lease that puts the chain back when it is destroyed,
/// or nothing when keepWaiting returned false before a chain became free.
std::optional<ChainLease> PluginRunChains::acquire(const std::function<bool()>& keepWaiting)
{
    while (keepWaiting()) {
        {
            const juce::ScopedLock scopedLock(lock);

            if (!freeIndices.empty()) {
                const auto chainIndex = freeIndices.back();
                freeIndices.pop_back();
                return std::optional<ChainLease>(std::in_place, *this, chainIndex);
            }
        }

        // More workers than chains is unusual, so polling while waiting costs little.
        available.wait(chainWaitIntervalMilliseconds);
    }

    return std::nullopt;
}

/// Returns how many plugin chains the run owns, whether or not they are in use.
int PluginRunChains::getNumChains() const noexcept
{
    return static_cast<int>(chains.size());
}

/// Wakes a worker waiting for a chain, so it notices that its run was cancelled.
void PluginRunChains::wakeWaitingWorkers()
{
    available.signal();
}

/// Returns a chain to the free pool and wakes one worker waiting for a chain.
void PluginRunChains::release(const int chainIndex)
{
    {
        const juce::ScopedLock scopedLock(lock);
        freeIndices.push_back(chainIndex);
    }

    available.signal();
}

/// Takes over a chain that PluginRunChains::acquire removed from the free pool.
ChainLease::ChainLease(PluginRunChains& chains, const int index) noexcept : runChains(chains), chainIndex(index) {}

/// Returns the chain to the free pool of its run.
ChainLease::~ChainLease()
{
    runChains.release(chainIndex);
}

/// Returns a raw pointer view of the leased chain for the service call, which is valid while the lease lives.
std::vector<juce::AudioPluginInstance*> ChainLease::getChainView() const
{
    return runChains.chains[static_cast<std::size_t>(chainIndex)]
        | std::views::transform([](const auto& instance) { return instance.get(); }) | std::ranges::to<std::vector>();
}

/// Sets the callback that is invoked for each published result.
void PluginProcessingCoordinator::setResultCallback(ResultCallback callback)
{
    resultCallback.set(std::move(callback));
}

/// Sets the callback that is invoked when a run completes.
void PluginProcessingCoordinator::setCompletionCallback(CompletionCallback callback)
{
    completionCallback.set(std::move(callback));
}

/// Sets the callback that is invoked if starting fails.
void PluginProcessingCoordinator::setStartErrorCallback(StartErrorCallback callback)
{
    startErrorCallback.set(std::move(callback));
}

/// Sets the callback that is invoked with the processed fraction of each file.
void PluginProcessingCoordinator::setProgressCallback(ProgressCallback callback)
{
    progressCallback.set(std::move(callback));
}

/// Sets the callback that is invoked when a file's processing begins.
void PluginProcessingCoordinator::setStartingCallback(StartingCallback callback)
{
    startingCallback.set(std::move(callback));
}

/// Cancels queued work and waits for active jobs to finish.
/// Safe to call on the message thread:
/// workers waiting for plugin setup on the message thread give up once the run is cancelled,
/// and workers that are rendering stop at the next block.
void PluginProcessingCoordinator::cancelAndWait()
{
    cancel();
    runner.waitForRunningJobs();
}

/// Ends the run without waiting, and without publishing anything more from it.
void PluginProcessingCoordinator::cancel()
{
    runner.cancel();

    // Wake any workers blocked waiting for a chain.
    if (currentChains != nullptr) {
        currentChains->wakeWaitingWorkers();
    }
}

/// Asks the run to stop without waiting.
/// Files that have not been committed are published as cancelled, files already committed with their result,
/// and the completion of the run still arrives.
void PluginProcessingCoordinator::requestStop()
{
    runner.requestStop();

    if (currentChains != nullptr) {
        currentChains->wakeWaitingWorkers();
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
    if (!runner.isCurrent(runId)) {
        return;
    }

    auto lease = runChains.acquire([this, runId] { return runner.isActive(runId); });

    if (!runner.isCurrent(runId)) {
        return;
    }

    const auto result = lease && !runner.isStopRequested(runId)
        ? processLeasedFile(record, options, runId, lease->getChainView())
        : PluginProcessingResult::cancelled(record.file);

    // The chain goes back to the pool before the result is published, so a waiting worker can start right away.
    lease.reset();
    runner.publish(resultCallback, runId, result);

    if (pendingFiles.fetch_sub(1) == 1) {
        runner.publish(completionCallback, runId, totalFiles);
    }
}

/// Runs one file through a leased chain on a worker thread and returns its result.
/// A failure after a stop request counts as cancelled, since the file was left unchanged.
PluginProcessingResult PluginProcessingCoordinator::processLeasedFile(
    const AudioAnalysisRecord& record,
    const PluginProcessingOptions& options,
    const int runId,
    const std::span<juce::AudioPluginInstance* const> chainView
) const
{
    runner.publish(startingCallback, runId, record.file);
    const auto onProgress = utils::makeThrottledProgress([this, &record, runId](const float progress) {
        runner.publish(progressCallback, runId, record.file, progress);
    });

    PluginProcessingResult result;

    try {
        // Cancellation lets the worker stop waiting for the message thread,
        // which cancelAndWait blocks while it waits for the workers to finish.
        result = processFileWithPlugins(record, options, chainView, runner.makeCancellationCheck(runId), onProgress);
    } catch (...) {
        // Plugins are third-party code, so an exception from one fails only this file
        // and its chain still goes back to the pool.
        const auto message = utils::format("Plugin processing failed: {}", utils::describeCurrentException());
        utils::logError("Plugin processing failed for {}: {}", record.fullPath.quoted(), message);
        result = PluginProcessingResult::failure(record.file, message);
    }

    if (runner.isStopRequested(runId) && std::holds_alternative<OperationFailed>(result.outcome)) {
        return PluginProcessingResult::cancelled(record.file);
    }
    return result;
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

    const auto runId = runner.getCurrentRunId();

    if (!chainsUsable) {
        startErrorCallback("No plugin chain instances were available to start processing.");
        runner.publish(completionCallback, runId, 0);
        return 0;
    }

    const auto queuedRecords = collectQueuedRecords(records);
    const auto totalFiles = static_cast<int>(queuedRecords.size());

    if (queuedRecords.empty()) {
        runner.publish(completionCallback, runId, 0);
        return 0;
    }

    const auto conflicts = findBatchOutputConflicts(queuedRecords);
    // The counter belongs to this run, so a job that outlives a cancellation timeout cannot change a later run's count.
    const auto pendingFiles = std::make_shared<std::atomic<int>>(totalFiles);
    // The options hold the saved state of every plugin, so the jobs share one copy instead of each holding their own.
    const auto runOptions = std::make_shared<const PluginProcessingOptions>(options);

    const auto concurrentFiles = juce::jmin(totalFiles, runner.getNumThreads(), currentChains->getNumChains());
    utils::logInfo("Plugin processing started: {} files, up to {} concurrent", totalFiles, concurrentFiles);

    for (const auto& record : queuedRecords) {
        if (conflicts.contains(record.file.getFullPathName())) {
            runner.addJob([this, record, runId, totalFiles, pendingFiles] {
                if (!runner.isCurrent(runId)) {
                    return;
                }

                runner.publish(
                    resultCallback,
                    runId,
                    PluginProcessingResult::failure(record.file, batchOutputConflictMessage(record.file))
                );

                if (pendingFiles->fetch_sub(1) == 1) {
                    runner.publish(completionCallback, runId, totalFiles);
                }
            });
            continue;
        }

        runner.addJob([this, record, runOptions, runId, totalFiles, runChains = currentChains, pendingFiles] {
            runFileJob(record, *runOptions, runId, totalFiles, *runChains, *pendingFiles);
        });
    }

    return totalFiles;
}
}  // namespace audiobatch
