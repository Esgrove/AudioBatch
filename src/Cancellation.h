/// Cooperative cancellation shared by the long-running services.
/// Declares the CancellationCheck callback that analysis, normalization, and plugin processing poll,
/// and the helper that evaluates an optional check.

#pragma once

#include <functional>

namespace utils
{
/// Returns true when the caller wants the work to stop as soon as possible.
/// Polled from worker threads, so it must be thread-safe and cheap.
using CancellationCheck = std::function<bool()>;

/// Returns true when a cancellation check is set and reports that cancellation was requested.
[[nodiscard]] inline bool isCancellationRequested(const CancellationCheck& isCancelled)
{
    return isCancelled != nullptr && isCancelled();
}
}  // namespace utils
