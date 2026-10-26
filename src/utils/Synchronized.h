/// Synchronized, which keeps a value together with the mutex that guards it, like a Mutex<T> in Rust.
/// The value can only be reached through with(), which holds the lock while a callable uses the value,
/// so code can neither forget to lock nor keep using the value after unlocking.

#pragma once

#include <mutex>
#include <utility>

namespace audiobatch::utils
{
/// Holds a value that is only reachable while its mutex is locked.
/// Neither copyable nor movable, since the mutex is not.
template<typename Value>
class Synchronized
{
public:
    /// Value-initializes the value, so a scalar starts at zero.
    Synchronized() = default;

    /// Constructs the value from the given arguments.
    template<typename... Arguments>
    explicit Synchronized(std::in_place_t /*inPlace*/, Arguments&&... arguments) :
        value(std::forward<Arguments>(arguments)...)
    {}

    /// Runs the callable with the value while holding the lock and returns what the callable returns.
    /// The callable must not return a reference or pointer into the value, which would outlive the lock.
    template<typename Callable>
    decltype(auto) with(Callable&& callable)
    {
        const std::scoped_lock lock(mutex);
        return std::forward<Callable>(callable)(value);
    }

    /// Runs the callable with the value as const while holding the lock and returns what the callable returns.
    /// The callable must not return a reference or pointer into the value, which would outlive the lock.
    template<typename Callable>
    decltype(auto) with(Callable&& callable) const
    {
        const std::scoped_lock lock(mutex);
        return std::forward<Callable>(callable)(std::as_const(value));
    }

private:
    mutable std::mutex mutex;
    Value value {};
};
}  // namespace audiobatch::utils
