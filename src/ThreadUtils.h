/// Helpers for worker threads of the long-running services.
/// Declares the CancellationCheck callback that analysis, normalization, and plugin processing poll,
/// the helper that evaluates an optional check,
/// and callOnMessageThread, the blocking call from a worker to the JUCE message thread that can be abandoned on
/// cancellation. VST3 restricts plugin setup and release to the message thread, which plugin processing relies on.

#pragma once

#include <functional>

namespace audiobatch::utils
{
/// Returns true when the caller wants the work to stop as soon as possible.
/// Polled from worker threads, so it must be thread-safe and cheap.
using CancellationCheck = std::function<bool()>;

/// Returns true when a cancellation check is set and reports that cancellation was requested.
[[nodiscard]] inline bool isCancellationRequested(const CancellationCheck& isCancelled)
{
    return isCancelled != nullptr && isCancelled();
}

bool callOnMessageThread(std::function<void()> function, const CancellationCheck& isCancelled);
}  // namespace audiobatch::utils