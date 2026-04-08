#include "NormalizeCoordinator.h"

#include <map>

/// Thread-pool orchestration for publishing normalize work back to the caller.
NormalizeCoordinator::NormalizeCoordinator(const int workerCount) : threadPool(juce::jmax(1, workerCount)) { }

NormalizeCoordinator::~NormalizeCoordinator()
{
    cancelAndWait();
}

void NormalizeCoordinator::setResultCallback(ResultCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    resultCallback = std::move(callback);
}

void NormalizeCoordinator::setCompletionCallback(CompletionCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    completionCallback = std::move(callback);
}

void NormalizeCoordinator::publishResult(const AudioNormalizationResult& result, const int runId)
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

void NormalizeCoordinator::publishCompletion(const int totalFiles, const int runId)
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

void NormalizeCoordinator::cancelAndWait()
{
    ++currentRunId;
    threadPool.removeAllJobs(true, 30000);
    pendingJobs.store(0);
}

int NormalizeCoordinator::start(const std::vector<AudioAnalysisRecord>& records)
{
    cancelAndWait();

    const auto runId = currentRunId.load();
    std::vector<AudioAnalysisRecord> normalizedRecords;
    std::map<juce::String, AudioAnalysisRecord> recordsByPath;

    for (const auto& record : records) {
        if (!record.file.existsAsFile()) {
            continue;
        }

        recordsByPath.insert_or_assign(record.fullPath, record);
    }

    normalizedRecords.reserve(recordsByPath.size());

    for (const auto& [path, record] : recordsByPath) {
        juce::ignoreUnused(path);
        normalizedRecords.push_back(record);
    }

    pendingJobs.store(static_cast<int>(normalizedRecords.size()));

    if (normalizedRecords.empty()) {
        publishCompletion(0, runId);
        return 0;
    }

    for (const auto& record : normalizedRecords) {
        threadPool.addJob([this, record, runId, totalFiles = static_cast<int>(normalizedRecords.size())] {
            if (runId != currentRunId.load()) {
                return;
            }

            publishResult(AudioNormalizationService::normalizeFile(record), runId);

            if (pendingJobs.fetch_sub(1) == 1) {
                publishCompletion(totalFiles, runId);
            }
        });
    }

    return static_cast<int>(normalizedRecords.size());
}
