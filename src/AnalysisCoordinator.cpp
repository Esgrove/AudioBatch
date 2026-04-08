#include "AnalysisCoordinator.h"

#include <mutex>

/// Thread-pool orchestration for publishing analysis work back to the caller.
AnalysisCoordinator::AnalysisCoordinator(AnalysisCache& analysisCache, const int workerCount) :
    cache(analysisCache),
    threadPool(juce::jmax(1, workerCount))
{ }

AnalysisCoordinator::~AnalysisCoordinator()
{
    cancelAndWait();
}

void AnalysisCoordinator::setResultCallback(ResultCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    resultCallback = std::move(callback);
}

void AnalysisCoordinator::setCompletionCallback(CompletionCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    completionCallback = std::move(callback);
}

void AnalysisCoordinator::publishResult(const AudioAnalysisRecord& result, const int runId)
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
    threadPool.removeAllJobs(true, 30000);
    pendingJobs.store(0);
}

int AnalysisCoordinator::start(const AudioAnalysisOptions& options)
{
    return start(options, AudioAnalysisService::collectInputFiles(options.inputPaths, options.recursive));
}

int AnalysisCoordinator::start(const AudioAnalysisOptions& options, const juce::Array<juce::File>& files)
{
    cancelAndWait();

    const auto runId = currentRunId.load();
    juce::Array<juce::File> staleFiles;

    for (const auto& file : files) {
        if (AudioAnalysisRecord cachedRecord; !options.refresh && cache.getAnalysis(file, cachedRecord)) {
            publishResult(cachedRecord, runId);
        } else {
            staleFiles.add(file);
        }
    }

    pendingJobs.store(staleFiles.size());

    if (files.isEmpty()) {
        publishCompletion(0, runId);
        return 0;
    }

    for (const auto& file : staleFiles) {
        threadPool.addJob([this, file, runId, totalFiles = files.size()] {
            if (runId != currentRunId.load()) {
                return;
            }

            const auto result = AudioAnalysisService::analyzeFile(file);
            cache.storeAnalysis(result);
            publishResult(result, runId);

            if (pendingJobs.fetch_sub(1) == 1) {
                publishCompletion(totalFiles, runId);
            }
        });
    }

    if (staleFiles.isEmpty()) {
        publishCompletion(files.size(), runId);
    }

    return files.size();
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

    if (const auto totalFiles = start(options); totalFiles == 0) {
        return results;
    }

    finishedEvent.wait(-1);
    return results;
}
