/// Implementation of NormalizeCoordinator.
/// Queues one thread pool job per file, runs AudioNormalizationService on worker threads,
/// and guards callback publication with run id checks and a callback lock
/// so cancelled runs stop publishing results.

#include "NormalizeCoordinator.h"

#include <map>

/// How long cancelAndWait waits for running normalization jobs to finish before giving up, in milliseconds.
static constexpr int jobShutdownTimeoutMilliseconds = 30000;

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

void NormalizeCoordinator::publishResult(const AudioNormalizationResult& result, const int runId) const
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

void NormalizeCoordinator::publishCompletion(const int totalFiles, const int runId) const
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
    threadPool.removeAllJobs(true, jobShutdownTimeoutMilliseconds);
    pendingJobs.store(0);
}

int NormalizeCoordinator::start(
    const std::vector<AudioAnalysisRecord>& records,
    const AudioNormalizationOptions& options
)
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

    const auto conflicts = AudioNormalizationService::findBatchOutputConflicts(normalizedRecords);

    for (const auto& record : normalizedRecords) {
        const bool hasConflict = conflicts.contains(record.file.getFullPathName());

        threadPool.addJob(
            [this, record, options, hasConflict, runId, totalFiles = static_cast<int>(normalizedRecords.size())] {
                if (runId != currentRunId.load()) {
                    return;
                }

                if (hasConflict) {
                    publishResult(
                        AudioNormalizationResult::failure(
                            record.file, AudioNormalizationService::batchOutputConflictMessage(record.file)
                        ),
                        runId
                    );
                } else {
                    publishResult(
                        AudioNormalizationService::normalizeFile(
                            record, options, [this, runId] { return runId != currentRunId.load(); }
                        ),
                        runId
                    );
                }

                if (pendingJobs.fetch_sub(1) == 1) {
                    publishCompletion(totalFiles, runId);
                }
            }
        );
    }

    return static_cast<int>(normalizedRecords.size());
}
