/// Implementation of AsyncCallback, the non-template message-thread trigger behind MessageThreadBatcher.
/// Keeping the juce::AsyncUpdater overrides out of the class template defines its virtual functions once.

#include "utils/MessageThreadBatcher.h"

#include <functional>
#include <utility>

namespace audiobatch::utils
{
/// Creates a callback that runs the function on the message thread when triggered.
AsyncCallback::AsyncCallback(std::function<void()> callback) : function(std::move(callback)) {}

/// Drops a call that has not run yet.
AsyncCallback::~AsyncCallback()
{
    cancelPendingUpdate();
}

/// Schedules the function to run on the message thread, unless a call is already pending.
/// Safe to call from any thread.
void AsyncCallback::trigger()
{
    triggerAsyncUpdate();
}

/// Runs a pending call right away instead of on the next message loop turn. Message thread only.
void AsyncCallback::runNowIfPending()
{
    handleUpdateNowIfNeeded();
}

/// Runs the function on the message thread.
void AsyncCallback::handleAsyncUpdate()
{
    if (function != nullptr) {
        function();
    }
}
}  // namespace audiobatch::utils
