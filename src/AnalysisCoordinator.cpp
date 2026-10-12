/// Implementation of AnalysisCoordinator.
/// Plans each run on a worker thread: expands the input paths, publishes cached records,
/// and queues one thread pool job per stale file.
/// Stores fresh results back into the cache,
/// and guards callback publication with run id checks and a callback lock.
/// Also provides the blocking analysis entry point used by the CLI.

#include "AnalysisCoordinator.h"

#include <mutex>

/// How long cancelAndWait waits for running analysis jobs to finish before giving up, in milliseconds.
static constexpr int jobShutdownTimeoutMilliseconds = 30000;

AnalysisCoordinator::AnalysisCoordinator(AnalysisCache& analysisCache, const int workerCount) :
    cache(analysisCache),
    threadPool(juce::jmax(1, workerCount))
{ }

AnalysisCoordinator::~AnalysisCoordinator()
{
    cancelAndWait();

    // A planning job can queue analysis jobs after removeAllJobs has collected the jobs it waits for,
    // so wait again until the pool is empty.
    while (threadPool.getNumJobs() != 0) {
        threadPool.removeAllJobs(true, -1);
    }
}

void AnalysisCoordinator::setCompletionCallback(CompletionCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    completionCallback = std::move(callback);
}

void AnalysisCoordinator::setResultCallback(ResultCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    resultCallback = std::move(callback);
}

void AnalysisCoordinator::setStartingCallback(StartingCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    startingCallback = std::move(callback);
}

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

void AnalysisCoordinator::cancelAndWait()
{
    ++currentRunId;
    threadPool.removeAllJobs(true, jobShutdownTimeoutMilliseconds);
}

bool AnalysisCoordinator::isRunCancelled(const int runId) const noexcept
{
    return runId != currentRunId.load();
}

int AnalysisCoordinator::start(const AudioAnalysisOptions& options)
{
    cancelAndWait();

    const auto runId = currentRunId.load();
    threadPool.addJob([this, options, runId] { planRun(options, runId); });
    return runId;
}

void AnalysisCoordinator::planRun(const AudioAnalysisOptions& options, const int runId)
{
    const auto isCancelled = [this, runId] { return isRunCancelled(runId); };

    AnalysisPlan plan;
    plan.runId = runId;
    plan.files = AudioAnalysisService::collectInputFiles(options.inputPaths, options.recursive, isCancelled);
    std::vector<AudioAnalysisRecord> cachedRecords;

    for (const auto& file : plan.files) {
        if (isCancelled()) {
            return;
        }

        if (AudioAnalysisRecord cachedRecord; !options.refresh && cache.getAnalysis(file, cachedRecord)) {
            cachedRecords.push_back(std::move(cachedRecord));
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

void AnalysisCoordinator::analyzeStaleFile(
    const juce::File& file,
    const int runId,
    const int totalFiles,
    const std::shared_ptr<std::atomic<int>>& pendingFiles
)
{
    if (isRunCancelled(runId)) {
        return;
    }

    publishStarting(file, runId);

    auto result = AudioAnalysisService::analyzeFile(file, [this, runId] { return isRunCancelled(runId); });

    if (isRunCancelled(runId)) {
        return;
    }

    // A refresh re-analyzes unchanged files too, and those keep the custom gain the user set for them.
    if (float customGainDb = 0.0f; cache.getCustomGain(file, result.fileSize, result.modifiedTimeMs, customGainDb)) {
        result.customGainDb = customGainDb;
        result.hasCustomGain = true;
    }

    cache.storeAnalysis(result);
    publishResult(result, runId);

    if (pendingFiles->fetch_sub(1) == 1) {
        publishCompletion(totalFiles, runId);
    }
}

void AnalysisCoordinator::setPlanCallback(PlanCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    planCallback = std::move(callback);
}

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

    start(options);
    finishedEvent.wait(-1);

    // The callbacks refer to this function's locals, so they must not outlive it.
    setResultCallback({});
    setCompletionCallback({});
    return results;
}
