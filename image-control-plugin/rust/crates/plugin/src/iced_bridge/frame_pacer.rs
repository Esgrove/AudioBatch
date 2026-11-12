//! Display-paced Windows redraw requests for the embedded editor child.
//!
//! Baseview 0.4 drives Windows frames from `WM_TIMER`, whose system granularity is about 15.6 ms.
//! Its fixed 15 ms redraw timer and the editor's poll timer
//! therefore cap rendering far below high-refresh displays.
//! This worker waits for each desktop composition with `DwmFlush`
//! and only invalidates the child window.
//! All rendering still happens in baseview's own `WM_PAINT` handler on the owning UI thread,
//! so no Iced, wgpu, or host state is touched from this thread.
//! Windows does not deliver `WM_PAINT` to hidden or minimized windows, so hidden editors stay idle.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::HWND;
use windows_sys::Win32::Graphics::Dwm::DwmFlush;
use windows_sys::Win32::Graphics::Gdi::InvalidateRect;
use windows_sys::Win32::UI::WindowsAndMessaging::{IsWindow, IsWindowVisible};

/// A composition wait shorter than this did not wait for a display refresh.
const MINIMUM_COMPOSITION_WAIT: Duration = Duration::from_millis(1);

/// Paces redraws when composition is unavailable, such as a locked or sleeping display.
const FALLBACK_INTERVAL: Duration = Duration::from_millis(8);

/// Bounds editor close if composition stalls. A later wake still observes the stop flag.
const STOP_TIMEOUT: Duration = Duration::from_millis(250);

/// Owns the redraw worker. Stopping it before the child is destroyed prevents invalidating a stale handle.
pub(super) struct FramePacer {
    /// The child HWND as an address, zero until attachment identifies it.
    target: Arc<AtomicUsize>,
    /// Requests worker exit before its next invalidation.
    stop: Arc<AtomicBool>,
    /// Signals worker exit so stopping can be bounded without a timed join.
    exited: mpsc::Receiver<()>,
    /// Joined when the worker exits within the stop bound.
    worker: Option<JoinHandle<()>>,
}

impl FramePacer {
    /// Starts an idle worker. It invalidates nothing until `attach` supplies the child.
    pub(super) fn start() -> std::io::Result<Self> {
        let target = Arc::new(AtomicUsize::new(0));
        let stop = Arc::new(AtomicBool::new(false));
        let (exit_sender, exited) = mpsc::sync_channel(1);
        let worker_target = Arc::clone(&target);
        let worker_stop = Arc::clone(&stop);
        let worker = std::thread::Builder::new()
            .name("ImageControl frame pacer".into())
            .spawn(move || {
                pace(&worker_target, &worker_stop);
                let _ = exit_sender.send(());
            })?;
        Ok(Self {
            target,
            stop,
            exited,
            worker: Some(worker),
        })
    }

    /// Begins pacing the editor child, which must remain alive until `stop` returns.
    pub(super) fn attach(&self, child: HWND) {
        self.target.store(child.expose_provenance(), Ordering::Release);
    }

    /// Stops and joins the worker, or abandons it after a bounded wait if composition stalls.
    pub(super) fn stop(&mut self) {
        self.stop.store(true, Ordering::Release);
        let Some(worker) = self.worker.take() else {
            return;
        };
        if self.exited.recv_timeout(STOP_TIMEOUT).is_ok() {
            let _ = worker.join();
        }
    }
}

impl Drop for FramePacer {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Requests one paint per desktop composition until stopped or the child is destroyed.
#[expect(
    unsafe_code,
    reason = "Waits for composition and invalidates the editor child, which close keeps alive until this exits"
)]
fn pace(target: &AtomicUsize, stop: &AtomicBool) {
    while !stop.load(Ordering::Acquire) {
        let started = Instant::now();
        // SAFETY: DwmFlush has no arguments and only blocks this worker until the next composition.
        let composed = unsafe { DwmFlush() } >= 0;
        let waited = started.elapsed();
        if !composed || waited < MINIMUM_COMPOSITION_WAIT {
            std::thread::sleep(FALLBACK_INTERVAL.saturating_sub(waited));
        }
        if stop.load(Ordering::Acquire) {
            break;
        }
        let address = target.load(Ordering::Acquire);
        if address == 0 {
            continue;
        }
        let child: HWND = std::ptr::with_exposed_provenance_mut(address);
        // SAFETY: The editor stops this worker before destroying the child.
        // Invalidation only marks the update region.
        // Windows then generates WM_PAINT on the child's own UI thread.
        unsafe {
            if IsWindow(child) == 0 {
                break;
            }
            if IsWindowVisible(child) != 0 {
                InvalidateRect(child, std::ptr::null(), 0);
            }
        }
    }
}
