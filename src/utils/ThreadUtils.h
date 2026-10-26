/// Helpers for worker threads of the long-running services.
/// Declares the CancellationCheck callback that analysis, normalization, and plugin processing poll,
/// the helper that evaluates an optional check,
/// the ProgressCallback they report per-file progress through with its range and throttling helpers,
/// and callOnMessageThread,
/// the blocking call from a worker to the JUCE message thread that can be abandoned on cancellation.
/// VST3 restricts plugin setup and release to the message thread, which plugin processing relies on.
/// describeCurrentException lets worker jobs turn an exception into a failed result,
/// and MessageThreadBatcher hands values from worker threads to the message thread in batches.

#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

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

bool callOnMessageThread(std::function<void()> function, const CancellationCheck& isCancelled);

juce::String describeCurrentException();

ProgressCallback makeProgressRange(const ProgressCallback& onProgress, float start, float end);
ProgressCallback makeThrottledProgress(ProgressCallback onProgress);

/// Collects values posted from any thread and hands them to a handler on the message thread in batches.
/// Values posted before a pending batch is delivered join it,
/// so a burst of worker results costs one delivery instead of one message each.
/// Batches go through the JUCE message queue, so a message posted after a value never arrives before it.
/// Destroy the batcher on the message thread, after every thread that posts to it has stopped.
template<typename Value>
class MessageThreadBatcher final : private juce::AsyncUpdater
{
public:
    /// Receives the values posted since the previous batch, in posting order. Runs on the message thread.
    using Handler = std::function<void(std::vector<Value> batch)>;

    /// Creates a batcher that delivers every batch to the handler.
    explicit MessageThreadBatcher(Handler batchHandler) : handler(std::move(batchHandler)) {}

    /// Drops a batch that has not been delivered yet.
    ~MessageThreadBatcher() override
    {
        cancelPendingUpdate();
    }

    MessageThreadBatcher(const MessageThreadBatcher&) = delete;
    MessageThreadBatcher& operator=(const MessageThreadBatcher&) = delete;
    MessageThreadBatcher(MessageThreadBatcher&&) = delete;
    MessageThreadBatcher& operator=(MessageThreadBatcher&&) = delete;

    /// Adds a value to the next batch and schedules its delivery. Safe to call from any thread.
    void post(Value value)
    {
        {
            const std::scoped_lock lock(mutex);
            pending.push_back(std::move(value));
        }

        triggerAsyncUpdate();
    }

    /// Delivers the values posted so far right away instead of on the next message loop turn.
    /// Call it on the message thread before handling something that must see every earlier value.
    void deliverPending()
    {
        handleUpdateNowIfNeeded();
    }

private:
    /// Delivers the values posted since the previous batch.
    /// The values are taken out under the lock and handled without it, so workers never wait for the handler.
    void handleAsyncUpdate() override
    {
        std::vector<Value> batch;

        {
            const std::scoped_lock lock(mutex);
            batch.swap(pending);
        }

        if (!batch.empty() && handler != nullptr) {
            handler(std::move(batch));
        }
    }

    Handler handler;
    std::mutex mutex;
    std::vector<Value> pending;
};
}  // namespace audiobatch::utils
