/// Implementation of NormalizeCoordinator.
/// Queues one thread pool job per file, runs normalizeFile on worker threads,
/// and publishes through the shared BackgroundRunner,
/// so cancelled runs stop publishing results.

#include "normalization/NormalizeCoordinator.h"

#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <map>
#include <ranges>
#include <vector>

namespace audiobatch
{
namespace
{
/// How long cancelAndWait waits for running normalization jobs to finish before giving up, in milliseconds.
constexpr int jobShutdownTimeoutMilliseconds {30'000};
}  // namespace

/// Creates a coordinator with a thread pool sized for the current machine.
NormalizeCoordinator::NormalizeCoordinator(const int workerCount) : runner(workerCount, jobShutdownTimeoutMilliseconds)
{}

/// Cancels any in-flight work and waits for every job without a timeout,
/// so workers cannot access destroyed coordinator state.
NormalizeCoordinator::~NormalizeCoordinator()
{
    runner.cancelAndWait();
    runner.waitForAllJobs();
}

/// Returns the id of the latest run, which its callbacks carry.
int NormalizeCoordinator::getCurrentRunId() const noexcept
{
    return runner.getCurrentRunId();
}

/// Sets the callback that is invoked for each published result.
void NormalizeCoordinator::setResultCallback(ResultCallback callback)
{
    resultCallback.set(std::move(callback));
}

/// Sets the callback that is invoked when a run completes.
void NormalizeCoordinator::setCompletionCallback(CompletionCallback callback)
{
    completionCallback.set(std::move(callback));
}

/// Sets the callback that is invoked with the normalized fraction of each file.
void NormalizeCoordinator::setProgressCallback(ProgressCallback callback)
{
    progressCallback.set(std::move(callback));
}

/// Sets the callback that is invoked right before each file's normalization begins.
void NormalizeCoordinator::setStartingCallback(StartingCallback callback)
{
    startingCallback.set(std::move(callback));
}

/// Cancels queued work and waits for active jobs to finish.
void NormalizeCoordinator::cancelAndWait()
{
    runner.cancelAndWait();
}

/// Ends the run without waiting, and without publishing anything more from it.
void NormalizeCoordinator::cancel()
{
    runner.cancel();
}

/// Asks the run to stop without waiting.
/// Files that have not been committed are published as cancelled, files already committed with their result,
/// and the completion of the run still arrives.
void NormalizeCoordinator::requestStop()
{
    runner.requestStop();
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

    const auto runId = runner.getCurrentRunId();
    std::map<juce::String, AudioAnalysisRecord> recordsByPath;

    // A file that has gone missing is queued too, so it is reported as a failure instead of being left out silently.
    for (const auto& record : records) {
        recordsByPath.insert_or_assign(record.fullPath, record);
    }

    const auto normalizedRecords = recordsByPath | std::views::values | std::ranges::to<std::vector>();

    if (normalizedRecords.empty()) {
        runner.publish(completionCallback, runId, 0);
        return 0;
    }

    const auto conflicts = findBatchOutputConflicts(normalizedRecords);
    // The counter belongs to this run, so a job that outlives a cancellation timeout cannot change a later run's count.
    const auto pendingFiles = std::make_shared<std::atomic<int>>(static_cast<int>(normalizedRecords.size()));

    for (const auto& record : normalizedRecords) {
        const bool hasConflict = conflicts.contains(record.file.getFullPathName());

        runner.addJob([this,
                       record,
                       options,
                       hasConflict,
                       runId,
                       pendingFiles,
                       totalFiles = static_cast<int>(normalizedRecords.size())] {
            if (!runner.isCurrent(runId)) {
                return;
            }

            if (runner.isStopRequested(runId)) {
                runner.publish(resultCallback, runId, AudioNormalizationResult::cancelled(record.file));
            } else if (hasConflict) {
                runner.publish(
                    resultCallback,
                    runId,
                    AudioNormalizationResult::failure(record.file, batchOutputConflictMessage(record.file))
                );
            } else {
                runner.publish(startingCallback, runId, record.file);
                const auto onProgress = utils::makeThrottledProgress([this, &record, runId](const float progress) {
                    runner.publish(progressCallback, runId, record.file, progress);
                });

                AudioNormalizationResult result;

                try {
                    result = normalizeFile(record, options, runner.makeCancellationCheck(runId), onProgress);
                } catch (...) {
                    const auto message = utils::format("Normalization failed: {}", utils::describeCurrentException());
                    utils::logError("Normalization failed for {}: {}", record.fullPath.quoted(), message);
                    result = AudioNormalizationResult::failure(record.file, message);
                }

                // A failure is the file left unchanged, so after a stop request it counts as cancelled.
                if (runner.isStopRequested(runId) && std::holds_alternative<OperationFailed>(result.outcome)) {
                    result = AudioNormalizationResult::cancelled(record.file);
                }

                runner.publish(resultCallback, runId, result);
            }

            if (pendingFiles->fetch_sub(1) == 1) {
                runner.publish(completionCallback, runId, totalFiles);
            }
        });
    }

    return static_cast<int>(normalizedRecords.size());
}
}  // namespace audiobatch
