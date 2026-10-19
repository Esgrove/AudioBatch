/// Implementation of NormalizeCoordinator.
/// Queues one thread pool job per file, runs normalizeFile on worker threads,
/// and guards callback publication with run id checks and a callback lock
/// so cancelled runs stop publishing results.

#include "normalization/NormalizeCoordinator.h"

#include <map>

namespace audiobatch
{
namespace
{
/// How long cancelAndWait waits for running normalization jobs to finish before giving up, in milliseconds.
constexpr int jobShutdownTimeoutMilliseconds {30000};
}  // namespace

/// Creates a coordinator with a thread pool sized for the current machine.
NormalizeCoordinator::NormalizeCoordinator(const int workerCount) : threadPool(juce::jmax(1, workerCount)) {}

/// Cancels any in-flight work and destroys the worker pool.
/// Waits without a timeout so workers cannot access destroyed coordinator state.
NormalizeCoordinator::~NormalizeCoordinator()
{
    cancelAndWait();
    threadPool.removeAllJobs(true, -1);
}

/// Sets the callback that is invoked for each published result.
void NormalizeCoordinator::setResultCallback(ResultCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    resultCallback = std::move(callback);
}

/// Sets the callback that is invoked when a run completes.
void NormalizeCoordinator::setCompletionCallback(CompletionCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    completionCallback = std::move(callback);
}

/// Sets the callback that is invoked with the normalized fraction of each file.
void NormalizeCoordinator::setProgressCallback(ProgressCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    progressCallback = std::move(callback);
}

/// Sets the callback that is invoked right before each file's normalization begins.
void NormalizeCoordinator::setStartingCallback(StartingCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    startingCallback = std::move(callback);
}

/// Invokes the progress callback when the given run id is still current.
/// Runs on the worker thread while a file is being normalized.
void NormalizeCoordinator::publishProgress(const juce::File& file, const float progress, const int runId) const
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

/// Invokes the starting callback when the given run id is still current.
/// Runs on the worker thread right before a file's normalization begins.
void NormalizeCoordinator::publishStarting(const juce::File& file, const int runId) const
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

/// Invokes the result callback when the given run id is still current.
/// Runs on the worker thread, so the callback must marshal to the message thread itself if needed.
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

/// Invokes the completion callback when the given run id is still current.
/// The callback is copied under the lock and invoked without it,
/// so the callback may safely call back into the coordinator.
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

/// Cancels queued work and waits for active jobs to finish.
void NormalizeCoordinator::cancelAndWait()
{
    ++currentRunId;
    threadPool.removeAllJobs(true, jobShutdownTimeoutMilliseconds);
}

/// Starts a background normalize run and returns the number of queued files.
/// Records that lose a batch output conflict are published as failures without being normalized.
/// Jobs of a run poll the run id, so cancelAndWait or a new run stops them before they commit any output.
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

    if (normalizedRecords.empty()) {
        publishCompletion(0, runId);
        return 0;
    }

    const auto conflicts = findBatchOutputConflicts(normalizedRecords);
    // The counter belongs to this run, so a job that outlives a cancellation timeout cannot change a later run's count.
    const auto pendingFiles = std::make_shared<std::atomic<int>>(static_cast<int>(normalizedRecords.size()));

    for (const auto& record : normalizedRecords) {
        const bool hasConflict = conflicts.contains(record.file.getFullPathName());

        threadPool.addJob([this,
                           record,
                           options,
                           hasConflict,
                           runId,
                           pendingFiles,
                           totalFiles = static_cast<int>(normalizedRecords.size())] {
            if (runId != currentRunId.load()) {
                return;
            }

            if (hasConflict) {
                publishResult(
                    AudioNormalizationResult::failure(record.file, batchOutputConflictMessage(record.file)), runId
                );
            } else {
                publishStarting(record.file, runId);
                const auto onProgress = utils::makeThrottledProgress([this, &record, runId](const float progress) {
                    publishProgress(record.file, progress, runId);
                });
                publishResult(
                    normalizeFile(
                        record, options, [this, runId] { return runId != currentRunId.load(); }, onProgress
                    ),
                    runId
                );
            }

            if (pendingFiles->fetch_sub(1) == 1) {
                publishCompletion(totalFiles, runId);
            }
        });
    }

    return static_cast<int>(normalizedRecords.size());
}
}  // namespace audiobatch
