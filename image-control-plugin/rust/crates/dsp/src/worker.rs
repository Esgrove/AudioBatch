//! Bounded spatial-analysis worker and latest-only publication, with deterministic shutdown outside processing.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::{SharedState, SpatialAnalyzer, SpectralPoint};

/// Maximum retained observations between display refreshes. Old display work may be discarded.
const MAXIMUM_POINTS: usize = 65_536;
/// Fallback wake interval, independent of a host's display refresh scheduling.
const POLL_INTERVAL: Duration = Duration::from_millis(16);

/// Worker-projected GPU instance values, independent of a graphics crate or platform API.
#[derive(Clone, Copy, Debug)]
pub struct SpatialParticle {
    position: [f32; 2],
    color: [f32; 3],
    emphasis: f32,
}

/// Owns one analysis consumer. Closing it joins the worker and releases capture ownership off the audio thread.
///
/// An external GUI owner may stop it through an independent shutdown handle before this value is dropped.
pub struct AnalysisWorker {
    shutdown: AnalysisShutdown,
    pending: Arc<Mutex<Vec<SpatialParticle>>>,
}

/// Cloneable editor-side shutdown ownership, independent of a retained view or an active GUI poll.
///
/// Keep this in the GUI bridge so closing the editor can synchronously stop analysis even while its state is borrowed.
/// Cloning and dropping a handle do not stop analysis. Dropping the worker or calling [`Self::shutdown`] does.
/// Although thread-safe, this handle belongs to non-realtime teardown and must never be used on the audio thread.
#[derive(Clone)]
pub struct AnalysisShutdown {
    state: Arc<AnalysisShutdownState>,
}

/// Shared only by GUI-side owners. The analysis thread never retains this state or its join handle.
struct AnalysisShutdownState {
    shared: Arc<SharedState>,
    stop: Arc<AtomicBool>,
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl SpatialParticle {
    /// Projects a spectral observation into normalized GPU coordinates on the analyzer worker.
    fn from_point(point: SpectralPoint) -> Self {
        let x = (point.frequency() / 20.0).log(1_000.0);
        let level = (10.0_f32.mul_add(point.power().log10(), 72.0) / 72.0).clamp(0.0, 1.0);
        Self {
            position: [x.clamp(0.0, 1.0), point.position()],
            color: [0.1_f32, 0.5, 0.9].map(|center| (x - center).abs().mul_add(-2.0, 1.0).max(0.0) * level),
            emphasis: level,
        }
    }

    /// Returns normalized log-frequency and top-to-bottom stereo position.
    #[must_use]
    pub const fn position(self) -> [f32; 2] {
        self.position
    }

    /// Returns linear frequency-palette energy, not a semantic UI color.
    #[must_use]
    pub const fn color(self) -> [f32; 3] {
        self.color
    }

    /// Returns the relative signal emphasis for Gaussian halo sizing.
    #[must_use]
    pub const fn emphasis(self) -> f32 {
        self.emphasis
    }
}

impl AnalysisShutdown {
    /// Stops and joins analysis synchronously before releasing its single-consumer claim.
    /// Calls through every clone are serialized and idempotent, including after a replacement worker starts.
    /// This may lock and wait for FFT work. Call only from non-realtime GUI teardown, never from audio or analysis.
    pub fn shutdown(&self) {
        self.state.stop.store(true, Ordering::Release);
        let mut thread = self
            .state
            .thread
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if let Some(worker) = thread.take() {
            worker.thread().unpark();
            let _ = worker.join();
            // Keep the lock through the join and claim release so every caller returns after shutdown completes.
            self.state.shared.analysis_enabled.store(false, Ordering::Release);
        }
    }
}

impl AnalysisWorker {
    /// Starts the worker without changing process-wide thread pools or installing an application logger.
    /// Allocate and destroy only off the audio thread.
    ///
    /// # Errors
    /// Returns the native thread creation error, releasing the analysis claim if startup fails.
    /// An existing consumer is left unchanged and reported as `WouldBlock`.
    pub fn new(shared: Arc<SharedState>) -> Result<Self, std::io::Error> {
        // Allocate GUI-side ownership before claiming capture so failed allocation cannot strand the claim.
        let mut stale = Vec::with_capacity(32_768);
        let stop = Arc::new(AtomicBool::new(false));
        let pending = Arc::new(Mutex::new(Vec::with_capacity(MAXIMUM_POINTS)));
        let worker_shared = Arc::clone(&shared);
        let shutdown = AnalysisShutdown {
            state: Arc::new(AnalysisShutdownState {
                shared,
                stop: Arc::clone(&stop),
                thread: Mutex::new(None),
            }),
        };
        let shared = &shutdown.state.shared;
        let worker_pending = Arc::clone(&pending);
        let builder = thread::Builder::new().name("imagecontrol-analysis".into());
        if shared
            .analysis_enabled
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Relaxed)
            .is_err()
        {
            return Err(std::io::Error::new(
                std::io::ErrorKind::WouldBlock,
                "an analyzer already owns this instance",
            ));
        }
        shared.drain_audio(&mut stale);
        // The worker owns only its inputs, never the GUI shutdown state containing its own join handle.
        let thread = builder
            .spawn(move || {
                run_worker(&worker_shared, &stop, &worker_pending);
            })
            .inspect_err(|_| shared.analysis_enabled.store(false, Ordering::Release))?;
        *shutdown
            .state
            .thread
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(thread);
        Ok(Self { shutdown, pending })
    }

    /// Returns independent GUI teardown ownership without borrowing this worker or its retained editor state.
    /// Calling the handle's shutdown joins analysis even while this worker remains alive.
    #[must_use]
    pub fn shutdown_handle(&self) -> AnalysisShutdown {
        self.shutdown.clone()
    }

    /// Swaps completed observations into reusable UI-owned storage without running FFT work on the UI thread.
    pub fn drain(&self, destination: &mut Vec<SpatialParticle>) {
        let mut pending = self.pending.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        destination.clear();
        std::mem::swap(destination, &mut pending);
    }
}

impl Drop for AnalysisWorker {
    fn drop(&mut self) {
        self.shutdown.shutdown();
    }
}

/// Processes at most one audio queue capacity per iteration and publishes only bounded display work.
fn run_worker(shared: &SharedState, stop: &AtomicBool, pending: &Mutex<Vec<SpatialParticle>>) {
    let mut frames = Vec::with_capacity(32_768);
    let mut points = Vec::with_capacity(MAXIMUM_POINTS);
    let mut rate = shared.sample_rate();
    let Ok(mut analyzer) = SpatialAnalyzer::new(rate) else {
        return;
    };
    while !stop.load(Ordering::Acquire) {
        let next_rate = shared.sample_rate();
        if next_rate.to_bits() != rate.to_bits() {
            let Ok(next) = SpatialAnalyzer::new(next_rate) else {
                break;
            };
            analyzer = next;
            rate = next_rate;
        }
        shared.drain_audio(&mut frames);
        points.clear();
        if analyzer.push(&frames, &mut points).is_err() {
            analyzer.reset();
        }
        if !points.is_empty() {
            let mut destination = pending.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            if destination.len() + points.len() > MAXIMUM_POINTS {
                destination.clear();
            }
            let available = MAXIMUM_POINTS - destination.len();
            destination.extend(points.iter().take(available).copied().map(SpatialParticle::from_point));
        }
        thread::park_timeout(POLL_INTERVAL);
    }
}

#[cfg(test)]
mod test_worker {
    use std::sync::Arc;
    use std::sync::atomic::Ordering;

    use super::AnalysisWorker;
    use crate::Imager;

    #[test]
    fn shutdown_handle_joins_live_owner_without_disabling_a_replacement() {
        let imager = Imager::new();
        let shared = imager.shared();
        let worker = AnalysisWorker::new(Arc::clone(&shared)).expect("first consumer");
        let shutdown = worker.shutdown_handle();
        shutdown.shutdown();
        assert_eq!(
            Arc::strong_count(&worker.pending),
            1,
            "joined worker released its inputs"
        );
        assert!(!shared.analysis_enabled.load(Ordering::Acquire));
        let replacement = AnalysisWorker::new(Arc::clone(&shared)).expect("joined consumer released ownership");
        shutdown.shutdown();
        drop(worker);
        assert!(shared.analysis_enabled.load(Ordering::Acquire));
        assert!(AnalysisWorker::new(Arc::clone(&shared)).is_err());
        drop(replacement);
        assert!(!shared.analysis_enabled.load(Ordering::Acquire));
    }

    #[test]
    fn shutdown_clones_are_concurrently_idempotent() {
        let imager = Imager::new();
        let shared = imager.shared();
        let worker = AnalysisWorker::new(Arc::clone(&shared)).expect("first consumer");
        let shutdown = worker.shutdown_handle();
        let handles = [shutdown.clone(), shutdown.clone(), shutdown.clone()];
        std::thread::scope(|scope| {
            for handle in &handles {
                let pending = &worker.pending;
                scope.spawn(move || {
                    handle.shutdown();
                    assert_eq!(Arc::strong_count(pending), 1, "shutdown clone waited for the join");
                });
            }
            shutdown.shutdown();
            assert_eq!(Arc::strong_count(&worker.pending), 1, "shutdown waited for the join");
        });
        assert_eq!(
            Arc::strong_count(&worker.pending),
            1,
            "every shutdown waits for the worker"
        );
        let replacement = AnalysisWorker::new(Arc::clone(&shared)).expect("shutdown released ownership");
        for handle in handles {
            handle.shutdown();
        }
        drop(worker);
        assert!(shared.analysis_enabled.load(Ordering::Acquire));
        drop(replacement);
        assert!(!shared.analysis_enabled.load(Ordering::Acquire));
    }

    #[test]
    fn second_consumer_is_rejected_and_reopen_is_allowed_after_join() {
        let imager = Imager::new();
        let shared = imager.shared();
        let worker = AnalysisWorker::new(std::sync::Arc::clone(&shared)).expect("first consumer");
        assert!(AnalysisWorker::new(std::sync::Arc::clone(&shared)).is_err());
        drop(worker);
        assert!(AnalysisWorker::new(shared).is_ok());
    }

    #[test]
    fn worker_shutdown_requires_no_processing_or_display_callbacks() {
        let imager = Imager::new();
        let worker = AnalysisWorker::new(imager.shared()).expect("thread creation");
        drop(worker);
    }
}
