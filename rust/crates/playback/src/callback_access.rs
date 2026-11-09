//! Exclusive renderer access with bounded callback admission and control-only waiting.
//!
//! One atomic state contains callback ownership and a control-side close request.
//! Closing admission cannot miss an in-flight callback, and callback exit preserves a concurrent close request.
//! The control mutex serializes control operations only. No callback operation touches it or wakes a waiter.

#![expect(
    unsafe_code,
    reason = "scoped atomic admission protects one renderer without callback-side native locks"
)]

use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::thread;
use std::time::Duration;

/// The callback holds exclusive access to the value.
const ACTIVE: u8 = 1;
/// Control has closed admission, including while waiting for an earlier callback.
const CLOSED: u8 = 2;
/// Control sleeps between observations rather than consuming a core while audio finishes.
const CONTROL_POLL_INTERVAL: Duration = Duration::from_micros(50);

/// Keeps a renderer alive until its owner explicitly retires it after closing callback admission.
/// Shared references expose the value only through exclusive scoped admission.
pub struct CallbackAccess<T> {
    state: AtomicU8,
    control: Mutex<()>,
    value: UnsafeCell<Option<T>>,
}

/// Clears only callback ownership, preserving a close request published during rendering.
struct CallbackGuard<'a>(&'a AtomicU8);

/// Holds control-side serialization and keeps admission closed for the complete mutation scope.
struct ControlGuard<'a> {
    state: &'a AtomicU8,
    _serialization: MutexGuard<'a, ()>,
    reopen: bool,
}

// SAFETY: moving the coordinator moves its value, which is permitted only when T is Send.
unsafe impl<T: Send> Send for CallbackAccess<T> {}

// SAFETY: every access to value is protected by the atomic protocol, and callbacks cannot overlap control.
// Only one callback can change the state from zero to ACTIVE. Control serializes through its private mutex.
unsafe impl<T: Send> Sync for CallbackAccess<T> {}

impl<T> CallbackAccess<T> {
    /// Creates a coordinator whose renderer is available to the first callback.
    pub const fn new(value: T) -> Self {
        Self {
            state: AtomicU8::new(0),
            control: Mutex::new(()),
            value: UnsafeCell::new(Some(value)),
        }
    }

    /// Makes exactly one admission attempt and never blocks, retries, allocates, or notifies.
    /// The closure cannot retain a reference to the renderer after its guard publishes completion.
    #[cfg(test)]
    #[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
    pub fn try_with<R>(&self, action: impl FnOnce(&mut T) -> R) -> Option<R> {
        self.try_with_if(|| true, action)
    }

    /// Checks the stream identity after admission but before even constructing a reference to the renderer.
    /// A rejected identity still releases only atomic ownership and never touches the protected value.
    #[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
    pub fn try_with_if<R>(&self, current: impl FnOnce() -> bool, action: impl FnOnce(&mut T) -> R) -> Option<R> {
        self.state
            .compare_exchange(0, ACTIVE, Ordering::Acquire, Ordering::Relaxed)
            .ok()?;
        let _guard = CallbackGuard(&self.state);
        if !current() {
            return None;
        }
        // SAFETY: the successful zero-to-ACTIVE transition excludes all other callbacks and control access.
        // The guard releases ownership after the closure returns, including an unwind outside production callbacks.
        let value = unsafe { &mut *self.value.get() };
        value.as_mut().map(action)
    }

    /// Observes whether control has requested exclusion, for deterministic concurrency tests only.
    #[cfg(all(test, not(audiobatch_rtsan)))]
    pub fn control_requested(&self) -> bool {
        self.state.load(Ordering::Acquire) & CLOSED != 0
    }

    /// Runs a control operation only after admission closes and every earlier callback has exited.
    /// This function may sleep and must never be called by a render callback.
    pub fn with_control<R>(&self, action: impl FnOnce(&mut T) -> R) -> Option<R> {
        let _guard = self.enter_control(true);
        // SAFETY: control owns CLOSED with ACTIVE cleared, and its private mutex excludes every other controller.
        let value = unsafe { &mut *self.value.get() };
        value.as_mut().map(action)
    }

    /// Permanently closes admission and transfers final renderer disposal to the caller's thread.
    /// Late callbacks can still hold the coordinator, but they can never reach the retired value.
    pub fn retire(&self) -> Option<T> {
        let _guard = self.enter_control(false);
        // SAFETY: admission remains CLOSED permanently and all earlier callback writes have been acquired.
        unsafe { &mut *self.value.get() }.take()
    }

    /// Closes admission before waiting so continuous callbacks cannot starve a control request.
    /// Control waiting is intentionally unbounded, it must never mutate or reclaim an active value.
    fn enter_control(&self, reopen: bool) -> ControlGuard<'_> {
        let serialization = self.control.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        let previous = self.state.fetch_or(CLOSED, Ordering::AcqRel);
        while self.state.load(Ordering::Acquire) & ACTIVE != 0 {
            thread::sleep(CONTROL_POLL_INTERVAL);
        }
        ControlGuard {
            state: &self.state,
            _serialization: serialization,
            reopen: reopen && previous & CLOSED == 0,
        }
    }
}

impl Drop for CallbackGuard<'_> {
    fn drop(&mut self) {
        self.0.fetch_and(!ACTIVE, Ordering::Release);
    }
}

impl Drop for ControlGuard<'_> {
    fn drop(&mut self) {
        if self.reopen {
            self.state.store(0, Ordering::Release);
        }
    }
}

#[cfg(test)]
mod test_callback_access {
    use std::cell::Cell;
    #[cfg(not(audiobatch_rtsan))]
    use std::panic::{AssertUnwindSafe, catch_unwind};
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::mpsc;
    use std::thread;

    use super::{CLOSED, CallbackAccess};

    /// Records final disposal on the thread that created the control owner.
    struct DropProbe {
        dropped: Arc<AtomicBool>,
        owner: thread::ThreadId,
    }

    impl Drop for DropProbe {
        fn drop(&mut self) {
            self.dropped
                .store(thread::current().id() == self.owner, Ordering::Relaxed);
        }
    }

    #[test]
    fn callback_admission_is_exclusive_and_publishes_mutations() {
        let access = CallbackAccess::new(0);
        assert_eq!(
            access.try_with(|value| {
                *value = 7;
                assert_eq!(access.try_with(|_| ()), None);
                *value
            }),
            Some(7)
        );
        assert_eq!(access.with_control(|value| *value), Some(7));
        access.with_control(|value| {
            *value = 9;
            assert_eq!(access.try_with(|_| ()), None);
        });
        assert_eq!(access.try_with(|value| *value), Some(9));
    }

    #[test]
    fn control_waits_for_the_running_callback_then_excludes_new_callbacks() {
        let access = Arc::new(CallbackAccess::new(0));
        let (entered, entry) = mpsc::channel();
        let (release, released) = mpsc::channel();
        let (closed, closing) = mpsc::channel();
        thread::scope(|scope| {
            let callback_access = Arc::clone(&access);
            scope.spawn(move || {
                callback_access.try_with(|value| {
                    entered.send(()).expect("callback entered");
                    released.recv().expect("controller releases callback");
                    *value = 42;
                });
            });
            entry.recv().expect("callback owns access");
            let control_access = Arc::clone(&access);
            scope.spawn(move || {
                closed.send(()).expect("controller starts");
                control_access.with_control(|value| {
                    assert_eq!(*value, 42);
                    assert_eq!(control_access.try_with(|_| ()), None);
                    *value += 1;
                });
            });
            closing.recv().expect("control request starts");
            while access.state.load(Ordering::Acquire) & CLOSED == 0 {
                thread::yield_now();
            }
            assert_eq!(access.try_with(|_| ()), None);
            release.send(()).expect("finish in-flight callback");
        });
        assert_eq!(access.try_with(|value| *value), Some(43));
    }

    #[test]
    fn epoch_predicate_runs_inside_admission_before_value_access() {
        let access = CallbackAccess::new(7);
        assert_eq!(access.try_with_if(|| false, |_| 99), None);
        assert_eq!(access.try_with(|value| *value), Some(7));
        let observed = AtomicBool::new(false);
        access.with_control(|_| {
            assert_eq!(
                access.try_with_if(
                    || {
                        observed.store(true, Ordering::Relaxed);
                        true
                    },
                    |_| ()
                ),
                None
            );
        });
        assert!(!observed.load(Ordering::Relaxed));
    }

    #[test]
    fn retirement_keeps_late_callbacks_silent_and_drops_on_the_control_thread() {
        let dropped = Arc::new(AtomicBool::new(false));
        let access = Arc::new(CallbackAccess::new(DropProbe {
            dropped: Arc::clone(&dropped),
            owner: thread::current().id(),
        }));
        drop(access.retire());
        assert_eq!(access.state.load(Ordering::Acquire), CLOSED);
        assert!(dropped.load(Ordering::Relaxed));
        assert_eq!(access.try_with(|_| ()), None);
        assert_eq!(access.state.load(Ordering::Acquire), CLOSED);
        assert!(access.retire().is_none());
        assert_eq!(access.state.load(Ordering::Acquire), CLOSED);
        assert!(access.with_control(|_| ()).is_none());
        assert_eq!(access.state.load(Ordering::Acquire), CLOSED);
        assert_eq!(access.try_with(|_| ()), None);
        assert_eq!(access.state.load(Ordering::Acquire), CLOSED);
    }

    #[test]
    fn send_but_not_sync_values_are_shared_only_through_exclusive_access() {
        let access = Arc::new(CallbackAccess::new(Cell::new(0)));
        thread::scope(|scope| {
            let callback_access = Arc::clone(&access);
            scope.spawn(move || {
                callback_access.try_with(|value| value.set(42));
            });
        });
        assert_eq!(access.with_control(|value| value.get()), Some(42));
    }

    #[cfg(not(audiobatch_rtsan))]
    #[test]
    fn unwinding_closures_release_admission_and_recover_control_poisoning() {
        let access = CallbackAccess::new(0);
        let callback_unwind = catch_unwind(AssertUnwindSafe(|| {
            access.try_with::<()>(|_| std::panic::resume_unwind(Box::new(7_u8)));
        }));
        assert!(callback_unwind.is_err());
        assert_eq!(access.state.load(Ordering::Acquire), 0);
        assert_eq!(access.try_with(|value| *value), Some(0));
        let control_unwind = catch_unwind(AssertUnwindSafe(|| {
            access.with_control::<()>(|value| {
                *value = 42;
                std::panic::resume_unwind(Box::new(7_u8));
            });
        }));
        assert!(control_unwind.is_err());
        assert_eq!(access.state.load(Ordering::Acquire), 0);
        assert_eq!(access.with_control(|value| *value), Some(42));
        assert_eq!(access.try_with(|value| *value), Some(42));
    }

    #[test]
    fn concurrent_control_owners_never_overlap_mutable_access() {
        let access = Arc::new(CallbackAccess::new(0));
        thread::scope(|scope| {
            for _ in 0..4 {
                let access = Arc::clone(&access);
                scope.spawn(move || {
                    for _ in 0..8 {
                        access.with_control(|value| *value += 1);
                    }
                });
            }
        });
        assert_eq!(access.try_with(|value| *value), Some(32));
    }
}
