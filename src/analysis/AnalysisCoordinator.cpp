/// Implementation of AnalysisCoordinator.
/// Plans each run on a worker thread: expands the input paths, publishes cached records,
/// and queues one thread pool job per stale file.
/// Stores fresh results back into the cache, and guards callback publication with run id checks and a callback lock.
/// Also provides the blocking analysis entry point used by the CLI.

#include "analysis/AnalysisCoordinator.h"

#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <mutex>

namespace audiobatch
{
/// How long cancelAndWait waits for running analysis jobs to finish before giving up, in milliseconds.
constexpr int jobShutdownTimeoutMilliseconds {30'000};

/// Creates a coordinator with a thread pool sized for the current machine.
AnalysisCoordinator::AnalysisCoordinator(AnalysisCache& analysisCache, const int workerCount) :
    cache(analysisCache),
    threadPool(juce::jmax(1, workerCount))
{}

/// Cancels any in-flight work and destroys the worker pool.
/// Waits without a timeout so workers cannot access destroyed coordinator state.
AnalysisCoordinator::~AnalysisCoordinator()
{
    cancelAndWait();

    // A planning job can queue analysis jobs after removeAllJobs has collected the jobs it waits for,
    // so wait again until the pool is empty.
    while (threadPool.getNumJobs() != 0) {
        threadPool.removeAllJobs(true, -1);
    }
}

/// Sets the callback invoked when a run completes.
void AnalysisCoordinator::setCompletionCallback(CompletionCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    completionCallback = std::move(callback);
}

/// Sets the callback invoked for each published result.
void AnalysisCoordinator::setResultCallback(ResultCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    resultCallback = std::move(callback);
}

/// Sets the callback invoked when a file's analysis is about to start.
void AnalysisCoordinator::setStartingCallback(StartingCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    startingCallback = std::move(callback);
}

/// Invokes the starting callback when the given run id is still current.
/// Runs on the worker thread right before a file's analysis begins.
void AnalysisCoordinator::publishStarting(const juce::File& file, const int runId) const
{
    if (isRunCancelled(runId)) {
        return;
    }

    StartingCallback callbackCopy;

    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = startingCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(file, runId);
    }
}

/// Sets the callback invoked with the analyzed fraction of each file.
void AnalysisCoordinator::setProgressCallback(ProgressCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    progressCallback = std::move(callback);
}

/// Invokes the progress callback when the given run id is still current.
/// Runs on the worker thread while a file is being decoded.
void AnalysisCoordinator::publishProgress(const juce::File& file, const float progress, const int runId) const
{
    if (isRunCancelled(runId)) {
        return;
    }

    ProgressCallback callbackCopy;

    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = progressCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(file, progress, runId);
    }
}

/// Invokes the result callback when the given run id is still current.
/// Runs on the worker thread, so the callback must marshal to the message thread itself if needed.
void AnalysisCoordinator::publishResult(const AudioAnalysisRecord& result, const int runId) const
{
    if (isRunCancelled(runId)) {
        return;
    }

    ResultCallback callbackCopy;

    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = resultCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(result, runId);
    }
}

/// Invokes the completion callback when the given run id is still current.
/// The callback is copied under the lock and invoked without it,
/// so the callback may safely call back into the coordinator.
void AnalysisCoordinator::publishCompletion(const int totalFiles, const int runId) const
{
    if (isRunCancelled(runId)) {
        return;
    }

    CompletionCallback callbackCopy;

    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = completionCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(totalFiles, runId);
    }
}

/// Cancels queued work and waits for active jobs to finish.
void AnalysisCoordinator::cancelAndWait()
{
    ++currentRunId;
    threadPool.removeAllJobs(true, jobShutdownTimeoutMilliseconds);
}

/// Cancels the current run without waiting for its jobs, so the message thread never blocks on a slow file.
/// Queued jobs are dropped, and running jobs stop at their next run id check without caching or publishing anything.
void AnalysisCoordinator::cancel()
{
    ++currentRunId;
    threadPool.removeAllJobs(true, 0);
}

/// Returns true when the run has been cancelled or replaced by a newer one.
bool AnalysisCoordinator::isRunCancelled(const int runId) const noexcept
{
    return runId != currentRunId.load();
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

    const auto runId = currentRunId.load();
    threadPool.addJob([this, options, runId] { planRun(options, runId); });
    return runId;
}

/// Expands the input paths, publishes the plan and the cached results, and queues one job per stale file.
/// Runs on a worker thread and stops early when the run is cancelled.
void AnalysisCoordinator::planRun(const AudioAnalysisOptions& options, const int runId)
{
    const auto isCancelled = [this, runId] { return isRunCancelled(runId); };

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

    publishPlan(plan, runId);

    for (const auto& record : cachedRecords) {
        publishResult(record, runId);
    }

    if (plan.staleFiles.isEmpty()) {
        publishCompletion(plan.files.size(), runId);
        return;
    }

    const auto pendingFiles = std::make_shared<std::atomic<int>>(plan.staleFiles.size());

    for (const auto& file : plan.staleFiles) {
        threadPool.addJob([this, file, runId, totalFiles = plan.files.size(), pendingFiles] {
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
    if (isRunCancelled(runId)) {
        return;
    }

    publishStarting(file, runId);

    const auto onProgress = utils::makeThrottledProgress([this, &file, runId](const float progress) {
        publishProgress(file, progress, runId);
    });

    AudioAnalysisRecord result;

    try {
        result = analyzeFile(file, [this, runId] { return isRunCancelled(runId); }, onProgress);
    } catch (...) {
        result = AudioAnalysisRecord::fromFile(file);
        result.status = AudioAnalysisStatus::failed;
        result.error = AnalysisError::exception;
        result.errorMessage = utils::format("Analysis failed: {}", utils::describeCurrentException());
        utils::logError("Analysis failed for {}: {}", result.fullPath.quoted(), result.errorMessage);
    }

    if (isRunCancelled(runId)) {
        return;
    }

    // A refresh re-analyzes unchanged files too, and those keep the custom gain the user set for them.
    if (!result.hasError()) {
        result.customGain = cache.getCustomGain(file, result.fileSize, result.modifiedTimeMs);
    }

    // The cache keeps only failures that repeat for the same bytes, so the next run retries the others.
    // The cache logs its own failures, and the result is still published, so the next run only reads it again.
    static_cast<void>(cache.storeAnalysis(result));

    publishResult(result, runId);

    if (pendingFiles->fetch_sub(1) == 1) {
        publishCompletion(totalFiles, runId);
    }
}

/// Sets the callback invoked with the file list of each run.
void AnalysisCoordinator::setPlanCallback(PlanCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    planCallback = std::move(callback);
}

/// Invokes the plan callback when the given run id is still current.
void AnalysisCoordinator::publishPlan(const AnalysisPlan& plan, const int runId) const
{
    if (isRunCancelled(runId)) {
        return;
    }

    PlanCallback callbackCopy;

    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = planCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(plan);
    }
}

/// Runs a full analysis synchronously and returns all collected results.
/// Replaces the result and completion callbacks for the run and clears them before returning.
std::vector<AudioAnalysisRecord> AnalysisCoordinator::analyzeBlocking(const AudioAnalysisOptions& options)
{
    std::vector<AudioAnalysisRecord> results;
    std::mutex resultsMutex;
    juce::WaitableEvent finishedEvent;

    setResultCallback([&results, &resultsMutex](const AudioAnalysisRecord& result, int /*runId*/) {
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
