/// Blocking calls from worker threads to the JUCE message thread that can be abandoned on cancellation.
/// Declares callOnMessageThread, used by plugin processing,
/// because VST3 restricts plugin setup and release to the message thread.

#pragma once

#include "Cancellation.h"

#include <functional>

namespace utils
{
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
bool callOnMessageThread(std::function<void()> function, const CancellationCheck& isCancelled);
}  // namespace utils
