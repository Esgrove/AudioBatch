/// Implementation of BackgroundRunner:
/// the run id that cancellation advances, and the thread pool calls that drop, signal, and wait for jobs.

#include "utils/BackgroundRunner.h"

#include "utils/ThreadUtils.h"

#include <functional>
#include <utility>

namespace audiobatch
{
/// Creates a thread pool with at least one worker.
/// Cancelling and waiting gives running jobs the given time to finish before giving up.
BackgroundRunner::BackgroundRunner(const int workerCount, const int jobShutdownTimeoutMilliseconds) :
    threadPool(juce::jmax(1, workerCount)),
    shutdownTimeoutMilliseconds(jobShutdownTimeoutMilliseconds)
{}

/// Returns the id of the current run, which a job captures when it is queued.
int BackgroundRunner::getCurrentRunId() const noexcept
{
    return currentRunId.load();
}

/// Returns the number of worker threads.
int BackgroundRunner::getNumThreads() const noexcept
{
    return threadPool.getNumThreads();
}

/// Returns true while the run has not been cancelled or replaced by a newer one.
bool BackgroundRunner::isCurrent(const int runId) const noexcept
{
    return runId == currentRunId.load();
}

/// Returns true while the run is current and not asked to stop, so its jobs should keep working.
bool BackgroundRunner::isActive(const int runId) const noexcept
{
    return isCurrent(runId) && !isStopRequested(runId);
}

/// Returns true once requestStop was called during the run.
bool BackgroundRunner::isStopRequested(const int runId) const noexcept
{
    return runId == stoppedRunId.load();
}

/// Returns a check that reports cancellation once the run is no longer current or was asked to stop,
/// for the services to poll.
utils::CancellationCheck BackgroundRunner::makeCancellationCheck(const int runId) const
{
    return [this, runId] { return !isActive(runId); };
}

/// Asks the jobs of the current run to stop at their next check, without waiting and without ending the run,
/// so the results they still publish and the completion of the run arrive.
void BackgroundRunner::requestStop()
{
    stoppedRunId.store(currentRunId.load());
}

/// Queues a job on the thread pool.
void BackgroundRunner::addJob(std::function<void()> job)
{
    threadPool.addJob(std::move(job));
}

/// Returns true while any job is queued or running.
bool BackgroundRunner::hasJobs() const
{
    return threadPool.getNumJobs() > 0;
}

/// Ends the current run without waiting, so the calling thread never blocks on a slow file.
/// Queued jobs are dropped, and running jobs are signalled and stop at their next run id check.
void BackgroundRunner::cancel()
{
    ++currentRunId;
    threadPool.removeAllJobs(true, 0);
}

/// Ends the current run and waits for its running jobs to finish, up to the shutdown timeout.
void BackgroundRunner::cancelAndWait()
{
    cancel();
    waitForRunningJobs();
}

/// Waits without a timeout until no job is queued or running.
/// A job may queue more jobs while the pool waits for it, so this waits again until the pool is empty.
void BackgroundRunner::waitForAllJobs()
{
    while (threadPool.getNumJobs() != 0) {
        threadPool.removeAllJobs(true, -1);
    }
}

/// Signals the running jobs and waits for them to finish, up to the shutdown timeout.
/// After a wait timed out, later waits return at once while the same jobs still run,
/// so the owners that wait one after another during shutdown do not each wait the whole timeout.
void BackgroundRunner::waitForRunningJobs()
{
    if (waitTimedOut && threadPool.getNumJobs() != 0) {
        return;
    }
    waitTimedOut = !threadPool.removeAllJobs(true, shutdownTimeoutMilliseconds);
}
}  // namespace audiobatch
