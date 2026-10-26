/// Implementation of the worker thread helpers.
/// callOnMessageThread uses a shared call record that decides exactly once whether the queued message runs the function
/// or the waiting worker abandons it, so the function never starts after the worker stopped waiting.
/// The progress helpers map a phase into part of a file's progress and throttle how often progress is forwarded.

#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>

namespace audiobatch::utils
{
namespace
{
/// How long a worker waits for the message thread between cancellation checks, in milliseconds.
constexpr int messageThreadPollIntervalMs {20};

/// Lifecycle of a MessageThreadCall.
enum class MessageThreadCallState : std::uint8_t { pending, running, abandoned };

/// A function posted from a worker thread to the message thread,
/// shared between the waiting worker and the queued message.
/// The state moves from pending to either running or abandoned exactly once, and whichever side gets there first wins.
/// This guarantees that the function never starts after the worker has stopped waiting for it,
/// so it can safely refer to the worker's stack and to the plugin chain the worker holds.
struct MessageThreadCall {
    std::function<void()> function;
    /// Exception thrown by the function, rethrown on the waiting worker.
    /// Written before finished is signalled, so the worker reads it only after the wait succeeded.
    std::exception_ptr exception;
    std::atomic<MessageThreadCallState> state {MessageThreadCallState::pending};
    juce::WaitableEvent finished;
};
}  // namespace

/// Runs a function on the message thread and blocks the calling worker until it has finished.
/// Unlike MessageManager::callSync, the wait gives up when cancellation is requested while the call is still queued,
/// because the message thread may itself be blocked waiting for this worker,
/// for example in a coordinator's cancelAndWait when the app quits during processing.
/// The abandoned call is skipped when the message thread later dequeues it.
/// A call that has already started always runs to completion,
/// since the message thread is then busy running it and cannot be waiting for the worker.
/// An exception thrown by the function is caught on the message thread and rethrown here,
/// so the worker never waits forever for a call that ended early, and it handles the error as if it made the call.
/// Calls the function directly when already on the message thread.
/// Returns true when the function ran,
/// and false when it was cancelled before starting or could not be posted to the message thread.
bool callOnMessageThread(std::function<void()> function, const CancellationCheck& isCancelled)
{
    auto const* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (messageManager == nullptr) {
        return false;
    }

    if (messageManager->isThisTheMessageThread()) {
        function();
        return true;
    }

    if (isCancellationRequested(isCancelled)) {
        return false;
    }

    auto const call = std::make_shared<MessageThreadCall>();
    call->function = std::move(function);

    const bool posted = juce::MessageManager::callAsync([call] {
        auto expected = MessageThreadCallState::pending;
        if (!call->state.compare_exchange_strong(expected, MessageThreadCallState::running)) {
            return;
        }

        try {
            call->function();
        } catch (...) {
            call->exception = std::current_exception();
        }

        call->finished.signal();
    });

    // Posting fails once the app has started quitting and the message loop no longer dispatches.
    if (!posted) {
        return false;
    }

    while (!call->finished.wait(messageThreadPollIntervalMs)) {
        if (!isCancellationRequested(isCancelled)) {
            continue;
        }

        auto expected = MessageThreadCallState::pending;
        if (call->state.compare_exchange_strong(expected, MessageThreadCallState::abandoned)) {
            return false;
        }

        // The message thread is already running the function, so it finishes without waiting on this worker.
        call->finished.wait();
        break;
    }

    if (call->exception != nullptr) {
        std::rethrow_exception(call->exception);
    }

    return true;
}

/// Returns the message of the exception being handled, for a worker job that turns it into a failed result.
/// An exception escaping a thread pool job or a std::jthread terminates the process,
/// and a job that stops early never reports its file as finished, so the jobs catch everything.
/// Must be called from inside a catch block.
juce::String describeCurrentException()
{
    try {
        throw;
    } catch (const std::exception& exception) {
        return juce::String::fromUTF8(exception.what());
    } catch (...) {
        return "Unknown error";
    }
}

/// Returns a callback
/// that maps the 0 to 1 progress of one phase into the range from start to end of the whole operation
/// and reports it to onProgress, or an empty callback when onProgress is empty.
/// The returned callback refers to onProgress, so it must not outlive it.
ProgressCallback makeProgressRange(const ProgressCallback& onProgress, const float start, const float end)
{
    if (onProgress == nullptr) {
        return {};
    }

    return [&onProgress, start, end](const float progress) {
        onProgress(start + (end - start) * juce::jlimit(0.0f, 1.0f, progress));
    };
}

/// Returns a callback that forwards progress to onProgress at most once per percent and once per interval,
/// always forwarding completion,
/// so a worker reporting every block does not flood the message thread.
/// The throttling state lives in the returned callback, so create one per file and do not copy it.
/// Returns an empty callback when onProgress is empty.
ProgressCallback makeThrottledProgress(ProgressCallback onProgress)
{
    if (onProgress == nullptr) {
        return {};
    }

    /// Smallest progress change that is forwarded.
    constexpr float minimumStep = 0.01f;
    /// Shortest time between forwarded updates, in milliseconds.
    constexpr std::uint32_t minimumIntervalMs = 50;

    return [callback = std::move(onProgress),
            lastProgress = -1.0f,
            lastReportMs = std::uint32_t {0}](const float progress) mutable {
        const auto nowMs = juce::Time::getMillisecondCounter();
        const bool finished = progress >= 1.0f && lastProgress < 1.0f;

        if (!finished && (progress - lastProgress < minimumStep || nowMs - lastReportMs < minimumIntervalMs)) {
            return;
        }

        lastProgress = progress;
        lastReportMs = nowMs;
        callback(progress);
    };
}
}  // namespace audiobatch::utils
