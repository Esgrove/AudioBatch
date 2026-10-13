/// Blocking calls from worker threads to the JUCE message thread that can be abandoned on cancellation.
/// Declares callOnMessageThread, used by plugin processing,
/// because VST3 restricts plugin setup and release to the message thread.

#pragma once

#include "Cancellation.h"

#include <functional>

namespace utils
{
bool callOnMessageThread(std::function<void()> function, const CancellationCheck& isCancelled);
}  // namespace utils
