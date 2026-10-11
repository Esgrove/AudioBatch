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

void AnalysisCoordinator::publishResult(const AudioAnalysisRecord& result, const int runId) const
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

void AnalysisCoordinator::publishCompletion(const int totalFiles, const int runId) const
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

void AnalysisCoordinator::cancelAndWait()
{
    ++currentRunId;
    threadPool.removeAllJobs(true, jobShutdownTimeoutMilliseconds);
    pendingJobs.store(0);
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
    AnalysisPlan plan;
    plan.files = AudioAnalysisService::collectInputFiles(options.inputPaths, options.recursive);
    std::vector<AudioAnalysisRecord> cachedRecords;

    for (const auto& file : plan.files) {
        if (runId != currentRunId.load()) {
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

    pendingJobs.store(plan.staleFiles.size());

    if (plan.staleFiles.isEmpty()) {
        publishCompletion(plan.files.size(), runId);
        return;
    }

    for (const auto& file : plan.staleFiles) {
        threadPool.addJob([this, file, runId, totalFiles = plan.files.size()] {
            analyzeStaleFile(file, runId, totalFiles);
        });
    }
}

void AnalysisCoordinator::analyzeStaleFile(const juce::File& file, const int runId, const int totalFiles)
{
    if (runId != currentRunId.load()) {
        return;
    }

    publishStarting(file, runId);

    auto result = AudioAnalysisService::analyzeFile(file, [this, runId] { return runId != currentRunId.load(); });

    if (runId != currentRunId.load()) {
        return;
    }

    // A refresh re-analyzes unchanged files too, and those keep the custom gain the user set for them.
    if (float customGainDb = 0.0f; cache.getCustomGain(file, result.fileSize, result.modifiedTimeMs, customGainDb)) {
        result.customGainDb = customGainDb;
        result.hasCustomGain = true;
    }

    cache.storeAnalysis(result);
    publishResult(result, runId);

    if (pendingJobs.fetch_sub(1) == 1) {
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
    if (runId != currentRunId.load()) {
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

    setResultCallback([&results, &resultsMutex](const AudioAnalysisRecord& result) {
        const std::scoped_lock lock(resultsMutex);
        results.push_back(result);
    });

    setCompletionCallback([&finishedEvent](int) { finishedEvent.signal(); });

    start(options);
    finishedEvent.wait(-1);
    return results;
}
