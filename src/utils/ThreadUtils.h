/// Helpers for worker threads of the long-running services.
/// Declares the CancellationCheck callback that analysis, normalization, and plugin processing poll,
/// the helper that evaluates an optional check,
/// the ProgressCallback they report per-file progress through with its range and throttling helpers,
/// and callOnMessageThread,
/// the blocking call from a worker to the JUCE message thread that can be abandoned on cancellation.
/// VST3 restricts plugin setup and release to the message thread, which plugin processing relies on.
/// describeCurrentException lets worker jobs turn an exception into a failed result.

#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>

namespace audiobatch::utils
{
/// Receives the completed fraction of a long-running operation on one file, in the range 0 to 1.
/// Called from worker threads, so it must be thread-safe and cheap.
using ProgressCallback = std::function<void(float progress)>;

/// Returns true when the caller wants the work to stop as soon as possible.
/// Polled from worker threads, so it must be thread-safe and cheap.
using CancellationCheck = std::function<bool()>;

/// Number of milliseconds in one second, shared by duration conversions and timing output.
inline constexpr double millisecondsPerSecond {1000.0};

/// Returns true when a cancellation check is set and reports that cancellation was requested.
[[nodiscard]] inline bool isCancellationRequested(const CancellationCheck& isCancelled)
{
    return isCancelled != nullptr && isCancelled();
}

/// Returns the done amount as a fraction of the total for progress reporting, or 1 when the total is not positive.
[[nodiscard]] constexpr float progressFraction(const std::int64_t done, const std::int64_t total)
{
    return total > 0 ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 1.0f;
}

/// Reports the completed fraction to the callback when one is set.
inline void reportProgress(const ProgressCallback& onProgress, const float progress)
{
    if (onProgress != nullptr) {
        onProgress(progress);
    }
}

[[nodiscard]] bool callOnMessageThread(std::function<void()> function, const CancellationCheck& isCancelled);

juce::String describeCurrentException();

ProgressCallback makeProgressRange(const ProgressCallback& onProgress, float start, float end);
/// Deleted, because the returned callback refers to onProgress, which a temporary would leave dangling.
ProgressCallback makeProgressRange(ProgressCallback&& onProgress, float start, float end) = delete;
ProgressCallback makeThrottledProgress(ProgressCallback onProgress);
}  // namespace audiobatch::utils
