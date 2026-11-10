//! Single-attempt admission to audio-owned mutable history without allocation, locking, or thread-local state.

#![expect(unsafe_code, reason = "atomic admission guards audio-owned UnsafeCell data")]

use std::cell::UnsafeCell;
use std::sync::atomic::{AtomicBool, Ordering};

use image_control_dsp::Imager;

/// Exclusive DSP storage guarded by one bounded compare-exchange attempt.
pub(super) struct AudioState {
    /// Only the successful admission owner accesses the processor.
    admitted: AtomicBool,
    /// Prepared processor state, never accessed by state or editor callbacks.
    imager: UnsafeCell<Imager>,
}

/// Releases admission on every Rust return path.
struct Admission<'a>(&'a AtomicBool);

// SAFETY: successful Acquire admission excludes all other accesses until the guard's Release store.
// No reference into the imager can escape the closure's temporary borrow.
unsafe impl Sync for AudioState {}

impl AudioState {
    /// Takes sole ownership of a processor before publishing its COM object.
    pub(super) const fn new(imager: Imager) -> Self {
        Self {
            admitted: AtomicBool::new(false),
            imager: UnsafeCell::new(imager),
        }
    }

    /// Runs an operation with exclusive DSP access, rejecting concurrent calls immediately.
    pub(super) fn try_with<R>(&self, operation: impl FnOnce(&mut Imager) -> R) -> Option<R> {
        self.admitted
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .ok()?;
        let admission = Admission(&self.admitted);
        // SAFETY: admission excludes every other mutable or shared access until guard destruction.
        let result = operation(unsafe { &mut *self.imager.get() });
        drop(admission);
        Some(result)
    }
}

impl Drop for Admission<'_> {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}
