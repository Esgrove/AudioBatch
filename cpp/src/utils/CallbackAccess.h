/// Coordinates exclusive access between a non-waiting audio callback and nestable control-thread operations.
/// Callback admission uses one atomic attempt and releases without native mutex calls or wakeups.
/// Control callers close admission before waiting for an in-flight callback, so continuous audio cannot starve them.
/// The coordinator does not own protected resources. Device shutdown must disconnect every caller before destruction.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace audiobatch::utils
{
class CallbackAccess;

/// Holds callback access only when its single admission attempt succeeded.
/// The coordinator and protected resources must outlive this scope.
class CallbackAccessGuard final
{
public:
    explicit CallbackAccessGuard(CallbackAccess& coordinator) noexcept;
    ~CallbackAccessGuard();

    CallbackAccessGuard(const CallbackAccessGuard&) = delete;
    CallbackAccessGuard& operator=(const CallbackAccessGuard&) = delete;
    CallbackAccessGuard(CallbackAccessGuard&&) = delete;
    CallbackAccessGuard& operator=(CallbackAccessGuard&&) = delete;

    /// Reports admission without retrying or changing the coordinator.
    [[nodiscard]] bool isAcquired() const noexcept
    {
        return acquired;
    }

private:
    CallbackAccess& access;
    bool acquired;
};

/// Closes callback admission and waits only on the control side, preserving nested lifecycle operations.
/// The coordinator must outlive this scope. Never construct this guard from an audio callback.
class CallbackControlGuard final
{
public:
    explicit CallbackControlGuard(CallbackAccess& coordinator);
    ~CallbackControlGuard();

    CallbackControlGuard(const CallbackControlGuard&) = delete;
    CallbackControlGuard& operator=(const CallbackControlGuard&) = delete;
    CallbackControlGuard(CallbackControlGuard&&) = delete;
    CallbackControlGuard& operator=(CallbackControlGuard&&) = delete;

private:
    CallbackAccess& access;
};

/// Synchronizes callback-owned mutable resources with control-side lifecycle work.
/// The recursive native mutex serializes control callers only. The audio callback never touches it.
/// Destroy only after the device has disconnected every callback and all guards have finished.
class CallbackAccess final
{
public:
    /// Starts with callback admission open and no control owner.
    CallbackAccess() = default;

    ~CallbackAccess() = default;

    CallbackAccess(const CallbackAccess&) = delete;
    CallbackAccess& operator=(const CallbackAccess&) = delete;
    CallbackAccess(CallbackAccess&&) = delete;
    CallbackAccess& operator=(CallbackAccess&&) = delete;

#ifdef AUDIOBATCH_TESTING
    /// Observes admission closure for deterministic tests. Never grants access to protected data.
    [[nodiscard]] bool isControlPendingForTesting() const noexcept
    {
        return (state.load(std::memory_order_acquire) & CLOSED) != 0;
    }
#endif

private:
    friend class CallbackAccessGuard;
    friend class CallbackControlGuard;

    /// Makes one bounded admission attempt. Failure leaves the protected resources untouched.
    /// The acquire observes the previous callback's release or the control owner's reopened state.
    /// ACTIVE and CLOSED share one modification order, so acquisition cannot bypass an earlier control close.
    [[nodiscard]] bool tryEnterCallback() noexcept
    {
        unsigned int expected = 0;
        return state.compare_exchange_strong(expected, ACTIVE, std::memory_order_acquire, std::memory_order_relaxed);
    }

    /// Preserves a concurrent close request while publishing the completed callback's writes.
    /// A release RMW is necessary here: a plain store could erase CLOSED and admit a second callback
    /// before the waiting controller has obtained exclusivity. This operation never retries or notifies.
    void leaveCallback() noexcept
    {
        state.fetch_and(~ACTIVE, std::memory_order_release);
    }

    /// Serializes control owners, closes admission, then observes completion before allowing mutation.
    /// No timeout permits unsafe mutation. Device shutdown remains responsible for ending callbacks.
    void enterControl()
    {
        controlLock.enter();
        if (controlDepth++ == 0) {
            state.fetch_or(CLOSED, std::memory_order_acq_rel);
            while ((state.load(std::memory_order_acquire) & ACTIVE) != 0) {
                std::this_thread::sleep_for(CONTROL_POLL_INTERVAL);
            }
        }
    }

    /// Publishes completed changes before reopening admission, only when the outermost owner exits.
    void leaveControl() noexcept
    {
        if (--controlDepth == 0) {
            state.store(0, std::memory_order_release);
        }
        controlLock.exit();
    }

    static constexpr unsigned int ACTIVE {1};
    static constexpr unsigned int CLOSED {2};
    static constexpr auto CONTROL_POLL_INTERVAL {std::chrono::microseconds(50)};
    static_assert(std::atomic<unsigned int>::is_always_lock_free);

    juce::CriticalSection controlLock;
    std::atomic<unsigned int> state {0};
    int controlDepth {0};
};

/// Attempts admission without blocking, allocating, or entering a native synchronization primitive.
inline CallbackAccessGuard::CallbackAccessGuard(CallbackAccess& coordinator) noexcept :
    access(coordinator),
    acquired(access.tryEnterCallback())
{}

/// Publishes callback completion without clearing a pending control-side exclusion.
inline CallbackAccessGuard::~CallbackAccessGuard()
{
    if (acquired) {
        access.leaveCallback();
    }
}

/// Obtains exclusive control access. Only the control side may wait for in-flight processing.
inline CallbackControlGuard::CallbackControlGuard(CallbackAccess& coordinator) : access(coordinator)
{
    access.enterControl();
}

/// Reopens callback admission after the outermost control scope has finished.
inline CallbackControlGuard::~CallbackControlGuard()
{
    access.leaveControl();
}
}  // namespace audiobatch::utils
