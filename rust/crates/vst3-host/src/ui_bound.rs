//! A value that only the UI thread may use, but that any thread may hold.
//!
//! Plugin instances must stay on the UI thread, yet a processing worker needs to own the chain it renders with
//! and hand it to the UI thread for each setup call.
//! `UiBound` makes that possible: it can be sent to and shared with any thread,
//! but its value is only reached through a `&UiThread`, and it is dropped on the UI thread.

#![expect(
    unsafe_code,
    reason = "lets a value that is not Send be held by other threads without being used there"
)]

use std::mem::ManuallyDrop;

use crate::thread::{UiDispatcher, UiThread};

/// A value that is only used and dropped on the UI thread.
pub struct UiBound<T: 'static> {
    /// The value, taken out exactly once, when the wrapper is dropped.
    value: ManuallyDrop<T>,
    /// Runs the drop on the UI thread when the wrapper is dropped elsewhere.
    dispatcher: UiDispatcher,
}

/// Carries a value to the UI thread to be dropped there.
struct DropOnUiThread<T: 'static>(ManuallyDrop<T>);

#[expect(
    clippy::non_send_fields_in_send_ty,
    reason = "the value is never used on the thread that holds the wrapper"
)]
// SAFETY: the value is only reached through methods that take a `&UiThread`,
// which exists only on the UI thread, and it is dropped there or leaked, so it is never used on another thread.
unsafe impl<T: 'static> Send for UiBound<T> {}

// SAFETY: shared references give access to the value only together with a `&UiThread`,
// so every access happens on the UI thread.
unsafe impl<T: 'static> Sync for UiBound<T> {}

#[expect(
    clippy::non_send_fields_in_send_ty,
    reason = "the value is only moved to the UI thread to be dropped there"
)]
// SAFETY: the value is only moved, never used, until it is dropped on the UI thread or leaked.
unsafe impl<T: 'static> Send for DropOnUiThread<T> {}

impl<T: 'static> UiBound<T> {
    /// Wraps a value created on the UI thread.
    #[must_use]
    pub const fn new(_ui: &UiThread, dispatcher: UiDispatcher, value: T) -> Self {
        Self {
            value: ManuallyDrop::new(value),
            dispatcher,
        }
    }

    /// Returns the value.
    #[must_use]
    pub fn get(&self, _ui: &UiThread) -> &T {
        &self.value
    }

    /// Returns the value for changing it.
    pub fn get_mut(&mut self, _ui: &UiThread) -> &mut T {
        &mut self.value
    }

    /// Returns the dispatcher the wrapper drops its value through.
    #[must_use]
    pub const fn dispatcher(&self) -> &UiDispatcher {
        &self.dispatcher
    }
}

impl<T: 'static> Drop for UiBound<T> {
    fn drop(&mut self) {
        // SAFETY: the value is taken exactly once, here, and the wrapper is not used afterwards.
        let value = unsafe { ManuallyDrop::take(&mut self.value) };
        if UiThread::is_current() {
            drop(value);
        } else {
            let carrier = DropOnUiThread(ManuallyDrop::new(value));
            self.dispatcher.post(move |_| drop(carrier));
        }
    }
}

impl<T: 'static> std::fmt::Debug for UiBound<T> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_struct("UiBound").finish_non_exhaustive()
    }
}

impl<T: 'static> Drop for DropOnUiThread<T> {
    /// Drops the value on the UI thread.
    /// When the UI thread no longer runs jobs, the process is exiting, and the value is leaked instead.
    fn drop(&mut self) {
        if UiThread::is_current() {
            // SAFETY: the value is taken exactly once, here, and the carrier is not used afterwards.
            drop(unsafe { ManuallyDrop::take(&mut self.0) });
        }
    }
}

#[cfg(test)]
mod test_ui_bound {
    use std::rc::Rc;
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, Ordering};

    use super::*;
    use crate::test_helpers::dispatcher;

    /// Records whether it was dropped on the UI thread.
    struct DropProbe {
        /// Set when dropped on the UI thread.
        dropped_on_ui: Arc<AtomicBool>,
        /// Makes the probe neither `Send` nor `Sync`, like a plugin instance.
        thread_bound: Rc<()>,
    }

    impl Drop for DropProbe {
        fn drop(&mut self) {
            self.dropped_on_ui.store(UiThread::is_current(), Ordering::SeqCst);
        }
    }

    #[test]
    fn a_value_held_by_another_thread_is_used_and_dropped_on_the_ui_thread() {
        let ui_dispatcher = dispatcher();
        let dropped_on_ui = Arc::new(AtomicBool::new(false));
        let flag = Arc::clone(&dropped_on_ui);
        let bound = ui_dispatcher
            .call(
                move |ui| {
                    let probe = DropProbe {
                        dropped_on_ui: flag,
                        thread_bound: Rc::new(()),
                    };
                    UiBound::new(ui, dispatcher().clone(), probe)
                },
                &|| false,
            )
            .expect("UI thread runs the job");
        let bound = Arc::new(bound);
        let shared = Arc::clone(&bound);
        let strong_count = ui_dispatcher
            .call(move |ui| Rc::strong_count(&shared.get(ui).thread_bound), &|| false)
            .expect("UI thread runs the job");
        assert_eq!(strong_count, 1);
        drop(bound);
        ui_dispatcher
            .call(|_| (), &|| false)
            .expect("UI thread drains the drop job");
        assert!(dropped_on_ui.load(Ordering::SeqCst));
    }
}
