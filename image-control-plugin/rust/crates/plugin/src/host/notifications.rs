//! Coalesced VST3 latency notifications dispatched only from SDK-defined UI lifecycle callbacks.
//!
//! No editor, construction-thread identity, or mutable DSP access participates in delivery.

#![expect(
    unsafe_code,
    reason = "calls the retained VST3 host handler from SDK-defined UI lifecycle callbacks"
)]

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use vst3::ComPtr;
use vst3::Steinberg::Vst::{IComponentHandler, IComponentHandlerTrait as _, RestartFlags_};

use crate::host::enums::to_i32;

/// One notification and one coalesced follow-up bound host-induced reconfiguration during delivery.
/// Further changes remain pending for the next successful setup or handler installation.
const MAXIMUM_RESTARTS_PER_FLUSH: usize = 2;

/// Detached latency dirtiness survives an absent handler, while exclusive delivery prevents recursive callbacks.
pub(super) struct LatencyNotifications {
    /// A successful setup changed the published latency since the last notification was taken.
    pending: AtomicBool,
    /// The outer UI callback owns delivery, including reference-count callbacks while releasing its handler.
    flushing: AtomicBool,
}

impl LatencyNotifications {
    /// Starts without unpublished latency or an active host callback, independently of the construction thread.
    pub(super) const fn new() -> Self {
        Self {
            pending: AtomicBool::new(false),
            flushing: AtomicBool::new(false),
        }
    }

    /// Coalesces successful latency publications without invoking any host code or entering DSP admission.
    pub(super) fn request(&self) {
        self.pending.store(true, Ordering::Release);
    }

    /// Discards notifications when the component disconnects from its host during termination.
    pub(super) fn clear(&self) {
        self.pending.store(false, Ordering::Release);
    }

    /// Delivers pending latency changes outside every plugin lock and mutable DSP admission.
    /// Reentrant setup and handler replacement queue work for the outer delivery instead of recursing.
    /// A failing host result is not retried automatically, avoiding a host rejection loop.
    ///
    /// # Safety
    /// The caller must be in the host UI-thread context, as required by `IComponentHandler::restartComponent`.
    /// The SDK annotates both `IAudioProcessor::setupProcessing` and `IEditController::setComponentHandler`
    /// as UI-thread entry points. The plugin relies on that host contract, not the construction thread.
    pub(super) unsafe fn flush(&self, handlers: &Mutex<Option<Arc<ComPtr<IComponentHandler>>>>) {
        if self
            .flushing
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            return;
        }
        let _guard = FlushGuard(&self.flushing);
        for _ in 0..MAXIMUM_RESTARTS_PER_FLUSH {
            if !self.pending.load(Ordering::Acquire) {
                break;
            }
            // Arc cloning cannot call host AddRef. The handler lock ends before host code executes.
            let handler = handlers
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .as_ref()
                .map(Arc::clone);
            let Some(handler) = handler else {
                break;
            };
            if !self.pending.swap(false, Ordering::AcqRel) {
                break;
            }
            // SAFETY: the caller guarantees the SDK's UI-thread context, and the Arc retains a live COM owner.
            unsafe { handler.restartComponent(to_i32(RestartFlags_::kLatencyChanged)) };
            // Keep delivery admitted through Release, which may replace the handler or prepare another latency.
            drop(handler);
        }
    }
}

/// Releases notification admission on ordinary returns and Rust unwinding without touching host references.
struct FlushGuard<'a>(&'a AtomicBool);

impl Drop for FlushGuard<'_> {
    /// Allows the next UI lifecycle callback to flush any remaining pending notification.
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}
