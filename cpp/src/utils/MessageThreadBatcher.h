/// Delivery of work from worker threads to the JUCE message thread in coalesced bursts.
/// Declares AsyncCallback, which runs a function on the message thread once for each burst of triggers,
/// and MessageThreadBatcher, which collects values posted from any thread
/// and hands them to a message-thread handler in batches.

#pragma once

#include <JuceHeader.h>

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace audiobatch::utils
{
/// Runs a function on the message thread once for each burst of triggers.
/// Triggers that arrive before the pending call runs are coalesced into it.
/// Destroy it on the message thread, after every thread that triggers it has stopped.
class AsyncCallback final : private juce::AsyncUpdater
{
public:
    explicit AsyncCallback(std::function<void()> callback);

    ~AsyncCallback() override;

    AsyncCallback(const AsyncCallback&) = delete;
    AsyncCallback& operator=(const AsyncCallback&) = delete;
    AsyncCallback(AsyncCallback&&) = delete;
    AsyncCallback& operator=(AsyncCallback&&) = delete;

    void trigger();
    void runNowIfPending();

private:
    void handleAsyncUpdate() override;

    std::function<void()> function;
};

/// Collects values posted from any thread and hands them to a handler on the message thread in batches.
/// Values posted before a pending batch is delivered join it,
/// so a burst of worker results costs one delivery instead of one message each.
/// Batches go through the JUCE message queue, so a message posted after a value never arrives before it.
/// Destroy the batcher on the message thread, after every thread that posts to it has stopped.
template<typename Value>
class MessageThreadBatcher final
{
public:
    /// Receives the values posted since the previous batch, in posting order. Runs on the message thread.
    using Handler = std::function<void(std::vector<Value> batch)>;

    /// Creates a batcher that delivers every batch to the handler.
    explicit MessageThreadBatcher(Handler batchHandler) : handler(std::move(batchHandler)) {}

    /// Adds a value to the next batch and schedules its delivery. Safe to call from any thread.
    void post(Value value)
    {
        {
            const std::scoped_lock lock(mutex);
            pending.push_back(std::move(value));
        }

        delivery.trigger();
    }

    /// Delivers the values posted so far right away instead of on the next message loop turn.
    /// Call it on the message thread before handling something that must see every earlier value.
    void deliverPending()
    {
        delivery.runNowIfPending();
    }

private:
    /// Delivers the values posted since the previous batch.
    /// The values are taken out under the lock and handled without it, so workers never wait for the handler.
    void deliver()
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

    /// Receives each batch on the message thread.
    Handler handler;
    /// Guards the pending values, which workers post while the message thread takes them.
    std::mutex mutex;
    /// The values posted since the last delivery, in posting order.
    std::vector<Value> pending;

    /// Declared last, so it is destroyed first and a pending delivery never reaches the destroyed values.
    AsyncCallback delivery {[this] { deliver(); }};
};
}  // namespace audiobatch::utils
