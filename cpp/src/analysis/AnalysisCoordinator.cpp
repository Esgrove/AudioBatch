/// Implementation of AnalysisCoordinator.
/// Plans each run on a worker thread: expands the input paths, publishes cached records,
/// and queues one thread pool job per stale file.
/// Stores fresh results back into the cache, and publishes through the shared BackgroundRunner,
/// so cancelled runs stop publishing results.
/// Also provides the blocking analysis entry point used by the CLI.

#include "analysis/AnalysisCoordinator.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// How long cancelAndWait waits for running analysis jobs to finish before giving up, in milliseconds.
constexpr int jobShutdownTimeoutMilliseconds {30'000};
}  // namespace

/// Creates a coordinator with a thread pool sized for the current machine.
AnalysisCoordinator::AnalysisCoordinator(AnalysisCache& analysisCache, const int workerCount) :
    cache(analysisCache),
    runner(workerCount, jobShutdownTimeoutMilliseconds)
{}

/// Cancels any in-flight work and waits for every job without a timeout,
/// so workers cannot access destroyed coordinator state.
/// A planning job can queue analysis jobs while the runner waits, which waitForAllJobs also covers.
AnalysisCoordinator::~AnalysisCoordinator()
{
    runner.cancelAndWait();
    runner.waitForAllJobs();
}

/// Sets the callback invoked when a run completes.
void AnalysisCoordinator::setCompletionCallback(CompletionCallback callback)
{
    completionCallback.set(std::move(callback));
}

/// Sets the callback invoked for each published result.
void AnalysisCoordinator::setResultCallback(ResultCallback callback)
{
    resultCallback.set(std::move(callback));
}

/// Sets the callback invoked when a file's analysis is about to start.
void AnalysisCoordinator::setStartingCallback(StartingCallback callback)
{
    startingCallback.set(std::move(callback));
}

/// Sets the callback invoked with the analyzed fraction of each file.
void AnalysisCoordinator::setProgressCallback(ProgressCallback callback)
{
    progressCallback.set(std::move(callback));
}

/// Sets the callback invoked with the file list of each run.
void AnalysisCoordinator::setPlanCallback(PlanCallback callback)
{
    planCallback.set(std::move(callback));
}

/// Cancels queued work and waits for active jobs to finish.
void AnalysisCoordinator::cancelAndWait()
{
    runner.cancelAndWait();
}

/// Cancels the current run without waiting for its jobs, so the message thread never blocks on a slow file.
/// Queued jobs are dropped, and running jobs stop at their next run id check without caching or publishing anything.
void AnalysisCoordinator::cancel()
{
    runner.cancel();
}

/// Starts a background analysis run and returns its run id.
/// Expanding the input paths and the cache lookups also run in the background,
/// so the caller never waits for the file system.
/// The plan callback comes first, then cached results, then fresh results, and finally the completion callback,
/// which is published even when no files are found.
/// A run still in progress is cancelled without waiting, since its jobs exit on their own run id checks.
int AnalysisCoordinator::start(const AudioAnalysisOptions& options)
{
    cancel();

    const auto runId = runner.getCurrentRunId();
    runner.addJob([this, options, runId] { planRun(options, runId); });
    return runId;
}

/// Expands the input paths, publishes the plan and the cached results, and queues one job per stale file.
/// Runs on a worker thread and stops early when the run is cancelled.
void AnalysisCoordinator::planRun(const AudioAnalysisOptions& options, const int runId)
{
    const auto isCancelled = runner.makeCancellationCheck(runId);

    AnalysisPlan plan;
    plan.runId = runId;
    plan.files = collectInputFiles(options.inputPaths, options.recursive, isCancelled);
    std::vector<AudioAnalysisRecord> cachedRecords;

    for (const auto& file : plan.files) {
        if (isCancelled()) {
            return;
        }

        if (auto cachedRecord = options.refresh ? std::nullopt : cache.getAnalysis(file)) {
            cachedRecords.push_back(std::move(*cachedRecord));
        } else {
            plan.staleFiles.add(file);
        }
    }

    if (runner.isCurrent(runId)) {
        planCallback(plan);
    }

    for (const auto& record : cachedRecords) {
        runner.publish(resultCallback, runId, record);
    }

    if (plan.staleFiles.isEmpty()) {
        runner.publish(completionCallback, runId, plan.files.size());
        return;
    }

    const auto pendingFiles = std::make_shared<std::atomic<int>>(plan.staleFiles.size());

    for (const auto& file : plan.staleFiles) {
        runner.addJob([this, file, runId, totalFiles = plan.files.size(), pendingFiles] {
            analyzeStaleFile(file, runId, totalFiles, pendingFiles);
        });
    }
}

/// Analyzes one file, caches the result, and publishes it.
/// The stored custom gain is kept while the file is unchanged on disk.
/// A file whose analysis is cancelled is neither cached nor published.
/// pendingFiles counts the run's unfinished files, and the job that finishes the last one publishes completion.
/// It belongs to the run, so a job that outlives a cancellation timeout cannot change a later run's count.
void AnalysisCoordinator::analyzeStaleFile(
    const juce::File& file,
    const int runId,
    const int totalFiles,
    const std::shared_ptr<std::atomic<int>>& pendingFiles
) const
{
    if (!runner.isCurrent(runId)) {
        return;
    }

    runner.publish(startingCallback, runId, file);

    const auto onProgress = utils::makeThrottledProgress([this, &file, runId](const float progress) {
        runner.publish(progressCallback, runId, file, progress);
    });

    AudioAnalysisRecord result;

    try {
        result = analyzeFile(file, runner.makeCancellationCheck(runId), onProgress);
    } catch (...) {
        result = AudioAnalysisRecord::fromFile(file);
        result.status = AudioAnalysisStatus::failed;
        result.error = AnalysisError::exception;
        result.errorMessage = utils::format("Analysis failed: {}", utils::describeCurrentException());
        utils::logError("Analysis failed for {}: {}", result.fullPath.quoted(), result.errorMessage);
    }

    if (!runner.isCurrent(runId)) {
        return;
    }

    // A refresh re-analyzes unchanged files too, and those keep the custom gain the user set for them.
    if (!result.hasError()) {
        result.customGain = cache.getCustomGain(file, result.fileSize, result.modifiedTimeMs);
    }

    // The cache keeps only failures that repeat for the same bytes, so the next run retries the others.
    // The cache logs its own failures, and the result is still published, so the next run only reads it again.
    static_cast<void>(cache.storeAnalysis(result));

    runner.publish(resultCallback, runId, result);

    if (pendingFiles->fetch_sub(1) == 1) {
        runner.publish(completionCallback, runId, totalFiles);
    }
}

/// Runs a full analysis synchronously and returns all collected results.
/// Each result is also passed to onResult as it arrives, from a worker thread, when it is set.
/// Replaces the result and completion callbacks for the run and clears them before returning.
std::vector<AudioAnalysisRecord> AnalysisCoordinator::analyzeBlocking(
    const AudioAnalysisOptions& options,
    const ResultObserver& onResult
)
{
    std::vector<AudioAnalysisRecord> results;
    std::mutex resultsMutex;
    juce::WaitableEvent finishedEvent;

    setResultCallback([&results, &resultsMutex, &onResult](const AudioAnalysisRecord& result, int /*runId*/) {
        if (onResult != nullptr) {
            onResult(result);
        }
        const std::scoped_lock lock(resultsMutex);
        results.push_back(result);
    });

    setCompletionCallback([&finishedEvent](int /*totalFiles*/, int /*runId*/) { finishedEvent.signal(); });

    // The completion callback above stays in place for the whole run, so the run id is not needed.
    static_cast<void>(start(options));
    finishedEvent.wait(-1);

    // The callbacks refer to this function's locals, so they must not outlive it.
    setResultCallback({});
    setCompletionCallback({});
    return results;
}
}  // namespace audiobatch
