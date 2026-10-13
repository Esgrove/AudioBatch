/// Implementation of the worker thread helpers.
/// callOnMessageThread uses a shared call record that decides exactly once whether the queued message runs the function
/// or the waiting worker abandons it, so the function never starts after the worker stopped waiting.

#include "ThreadUtils.h"

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace audiobatch::utils
{
namespace
{
/// How long a worker waits for the message thread between cancellation checks, in milliseconds.
constexpr int messageThreadPollIntervalMs = 20;

/// Lifecycle of a MessageThreadCall.
enum class MessageThreadCallState : std::uint8_t { pending, running, abandoned };

/// A function posted from a worker thread to the message thread,
/// shared between the waiting worker and the queued message.
/// The state moves from pending to either running or abandoned exactly once, and whichever side gets there first wins.
/// This guarantees that the function never starts after the worker has stopped waiting for it,
/// so it can safely refer to the worker's stack and to the plugin chain the worker holds.
struct MessageThreadCall {
    std::function<void()> function;
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

        call->function();
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

    return true;
}

}  // namespace audiobatch::utils