/// Implementation of callOnMessageThread.
/// A shared call record decides exactly once whether the queued message runs the function
/// or the waiting worker abandons it, so the function never starts after the worker stopped waiting.

#include "MessageThreadCall.h"

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace utils
{
/// How long a worker waits for the message thread between cancellation checks, in milliseconds.
constexpr int messageThreadPollIntervalMs = 20;

/// A function posted from a worker thread to the message thread,
/// shared between the waiting worker and the queued message.
/// The state moves from pending to either running or abandoned exactly once,
/// and whichever side gets there first wins.
/// This guarantees that the function never starts after the worker has stopped waiting for it,
/// so it can safely refer to the worker's stack and to the plugin chain the worker holds.
struct MessageThreadCall {
    enum class State : std::uint8_t { pending, running, abandoned };

    std::function<void()> function;
    std::atomic<State> state {State::pending};
    juce::WaitableEvent finished;
};

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
        auto expected = MessageThreadCall::State::pending;
        if (!call->state.compare_exchange_strong(expected, MessageThreadCall::State::running)) {
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

        auto expected = MessageThreadCall::State::pending;
        if (call->state.compare_exchange_strong(expected, MessageThreadCall::State::abandoned)) {
            return false;
        }

        // The message thread is already running the function, so it finishes without waiting on this worker.
        call->finished.wait();
        break;
    }

    return true;
}

}  // namespace utils
