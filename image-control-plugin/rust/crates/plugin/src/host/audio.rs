//! Safe single-attempt admission to exclusively borrowed DSP history.
//!
//! The processor is never shared immutably. Atomic borrowing admits one callback without a mutex or allocation.

use atomic_refcell::AtomicRefCell;
use image_control_dsp::Imager;
use sync_wrapper::SyncWrapper;

/// Only an admitted mutable borrow can access the processor, including lifecycle preparation and reset.
pub struct AudioState {
    /// `SyncWrapper` exposes only mutable access, so `Imager` needs `Send` but does not have to be `Sync`.
    imager: AtomicRefCell<SyncWrapper<Imager>>,
}

impl AudioState {
    /// Takes processor ownership before exposing the instance to host callbacks.
    pub(crate) const fn new(imager: Imager) -> Self {
        Self {
            imager: AtomicRefCell::new(SyncWrapper::new(imager)),
        }
    }

    /// Attempts one atomic exclusive borrow and rejects overlapping or reentrant host calls immediately.
    /// The guard releases admission on return or unwinding. No mutable reference can escape this closure.
    pub(crate) fn try_with<R>(&self, operation: impl FnOnce(&mut Imager) -> R) -> Option<R> {
        let mut imager = self.imager.try_borrow_mut().ok()?;
        Some(operation(imager.get_mut()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn processor_ownership_returns_after_nested_rejection() {
        let audio = AudioState::new(Imager::new());
        let shared = audio
            .try_with(|imager| {
                assert!(audio.try_with(|_| ()).is_none());
                imager.shared()
            })
            .expect("first admission");
        assert!(
            audio
                .try_with(|imager| std::sync::Arc::ptr_eq(&shared, &imager.shared()))
                .expect("returned ownership")
        );
    }
}
