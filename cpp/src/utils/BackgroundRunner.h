/// Shared pieces of the analysis, normalization, and plugin processing coordinators.
/// Declares RunCallback, a callback that worker threads invoke while the message thread may replace it,
/// and BackgroundRunner, which runs the jobs of one run at a time on a thread pool
/// and tells each job whether its run is still the current one.

#pragma once

#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <utility>

namespace audiobatch
{
/// A callback that can be replaced on one thread while worker threads invoke it.
template<typename... Args>
class RunCallback
{
public:
    /// Replaces the callback, or stops further calls when the function is empty.
    void set(std::function<void(Args...)> newCallback)
    {
        const juce::ScopedLock lock(callbackLock);
        callback = std::move(newCallback);
    }

    /// Invokes the callback, if one is set.
    /// The callback is copied under the lock and invoked without it, so it may call back into its owner.
    void operator()(Args... arguments) const
    {
        std::function<void(Args...)> callbackCopy;

        {
            const juce::ScopedLock lock(callbackLock);
            callbackCopy = callback;
        }

        if (callbackCopy != nullptr) {
            callbackCopy(std::forward<Args>(arguments)...);
        }
    }

private:
    /// The callback, or empty when none is set.
    std::function<void(Args...)> callback;
    /// Guards the callback, which a worker copies while the message thread replaces it.
    juce::CriticalSection callbackLock;
};

/// Runs the jobs of one run at a time on a thread pool.
/// Every cancellation starts a new run id, so the jobs of the old run stop at their next check
/// and their callbacks are no longer published.
/// A stop request instead keeps the run current: its jobs stop at their next check,
/// but the results they still publish, such as a file committed just before, and the completion still arrive.
/// The owner must call waitForAllJobs in its destructor before the state the jobs use is destroyed.
class BackgroundRunner
{
public:
    BackgroundRunner(int workerCount, int jobShutdownTimeoutMilliseconds);

    BackgroundRunner(const BackgroundRunner&) = delete;
    BackgroundRunner& operator=(const BackgroundRunner&) = delete;
    BackgroundRunner(BackgroundRunner&&) = delete;
    BackgroundRunner& operator=(BackgroundRunner&&) = delete;

    ~BackgroundRunner() = default;

    [[nodiscard]] int getCurrentRunId() const noexcept;
    [[nodiscard]] int getNumThreads() const noexcept;
    [[nodiscard]] bool hasJobs() const;
    [[nodiscard]] bool isActive(int runId) const noexcept;
    [[nodiscard]] bool isCurrent(int runId) const noexcept;
    [[nodiscard]] bool isStopRequested(int runId) const noexcept;
    [[nodiscard]] utils::CancellationCheck makeCancellationCheck(int runId) const;

    /// Invokes the callback with the run id appended to the values, while the run is still current.
    template<typename... Args, typename... Values>
    void publish(const RunCallback<Args...>& callback, const int runId, Values&&... values) const
    {
        if (isCurrent(runId)) {
            callback(std::forward<Values>(values)..., runId);
        }
    }

    void addJob(std::function<void()> job);
    void cancel();
    void cancelAndWait();
    void requestStop();
    void waitForAllJobs();
    void waitForRunningJobs();

private:
    /// Runs the jobs.
    juce::ThreadPool threadPool;
    /// Id of the current run, raised by every cancellation.
    std::atomic<int> currentRunId {0};
    /// The run asked to stop, or -1 when none was.
    std::atomic<int> stoppedRunId {-1};
    /// True after a wait for the running jobs timed out, until the pool is empty again,
    /// so later waits for the same hung job return at once instead of waiting the whole timeout again.
    std::atomic<bool> waitTimedOut {false};
    /// How long cancelAndWait waits for the running jobs to finish.
    int shutdownTimeoutMilliseconds;
};
}  // namespace audiobatch
