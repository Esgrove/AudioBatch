//! Bounded generation-aware spatial worker with exclusive consumer ownership and joined editor shutdown.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::monitoring::{AnalysisSample, capture_enabled};
use crate::particles::ParticleBuilder;
use crate::{AnalysisStatus, SharedState, SpatialAnalyzer, SpatialParticle};

/// Hard upper bound on pending instances independently of producer or refresh speed.
const MAXIMUM_POINTS: usize = 65_536;
/// Worker polling never requires an audio-side notification or a GUI callback.
const POLL_INTERVAL: Duration = Duration::from_millis(16);
/// Bounded chunk between cancellation and stream-generation checks.
const CHUNK_FRAMES: usize = 4096;
/// Maximum queue storage examined per pass, even with a continuously refilling producer.
const CHUNKS_PER_PASS: usize = 8;

/// Metadata accompanies empty batches too, allowing GPU history to clear during silence.
#[derive(Clone, Copy, Debug)]
pub struct AnalysisUpdate {
    /// Exact stream identity whose observations are in this batch.
    pub generation: u64,
    /// Explicit availability for editor status text.
    pub status: AnalysisStatus,
}

/// One bounded completed batch, shared only between worker and editor.
struct PendingBatch {
    points: Vec<SpatialParticle>,
    update: AnalysisUpdate,
}

/// Owns the sole queue consumer. Construction and destruction belong off the audio thread.
pub struct AnalysisWorker {
    shutdown: AnalysisShutdown,
    pending: Arc<Mutex<PendingBatch>>,
}

/// Independent editor-close capability that joins even while retained GUI state is borrowed.
#[derive(Clone)]
pub struct AnalysisShutdown {
    state: Arc<AnalysisShutdownState>,
}

/// GUI-side owner. The worker never retains its own join handle or shutdown owner.
struct AnalysisShutdownState {
    shared: Arc<SharedState>,
    stop: Arc<AtomicBool>,
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl AnalysisShutdown {
    /// Cancels and joins synchronously, then releases ownership exactly once.
    /// Never call from a realtime callback or from the analysis worker itself.
    pub fn shutdown(&self) {
        self.state.stop.store(true, Ordering::Release);
        let mut thread = self
            .state
            .thread
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if let Some(worker) = thread.take() {
            self.state.shared.set_analysis_enabled(false);
            self.state.shared.set_correlation_enabled(false);
            worker.thread().unpark();
            let _ = worker.join();
            self.state.shared.monitoring.claimed.store(false, Ordering::Release);
        }
    }
}

impl AnalysisWorker {
    /// Claims a consumer independently of capture enablement and starts a private cancellable worker.
    ///
    /// # Errors
    /// Reports another owner or native thread creation failure.
    /// Unsupported rates are worker status, not startup failure.
    pub fn new(shared: Arc<SharedState>) -> Result<Self, std::io::Error> {
        let stop = Arc::new(AtomicBool::new(false));
        let pending = Arc::new(Mutex::new(PendingBatch {
            points: Vec::with_capacity(MAXIMUM_POINTS),
            update: AnalysisUpdate {
                generation: shared.analysis_generation(),
                status: AnalysisStatus::Waiting,
            },
        }));
        let shutdown = AnalysisShutdown {
            state: Arc::new(AnalysisShutdownState {
                shared: Arc::clone(&shared),
                stop: Arc::clone(&stop),
                thread: Mutex::new(None),
            }),
        };
        if shared
            .monitoring
            .claimed
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Err(std::io::Error::new(
                std::io::ErrorKind::WouldBlock,
                "an analyzer already owns this instance",
            ));
        }
        shared.set_analysis_enabled(true);
        let worker_shared = Arc::clone(&shared);
        let worker_pending = Arc::clone(&pending);
        let thread = thread::Builder::new()
            .name("imagecontrol-analysis".into())
            .spawn(move || {
                run_worker(&worker_shared, &stop, &worker_pending);
            })
            .inspect_err(|_| {
                shared.set_analysis_enabled(false);
                shared.monitoring.claimed.store(false, Ordering::Release);
            })?;
        *shutdown
            .state
            .thread
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner) = Some(thread);
        drop(shared);
        Ok(Self { shutdown, pending })
    }

    /// Clones non-realtime teardown ownership independently of a retained editor update borrow.
    #[must_use]
    pub fn shutdown_handle(&self) -> AnalysisShutdown {
        self.shutdown.clone()
    }

    /// Swaps reusable completed storage and returns its identity even when no particles exist.
    pub fn drain_batch(&self, destination: &mut Vec<SpatialParticle>) -> AnalysisUpdate {
        let shared = &self.shutdown.state.shared;
        let generation = shared.analysis_generation();
        let mut pending = self.pending.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        destination.clear();
        if pending.update.generation != generation {
            pending.points.clear();
            pending.update = AnalysisUpdate {
                generation,
                status: shared.analysis_status(),
            };
        }
        std::mem::swap(destination, &mut pending.points);
        pending.update
    }

    /// Compatibility drain for consumers that do not retain GPU history.
    pub fn drain(&self, destination: &mut Vec<SpatialParticle>) {
        let _ = self.drain_batch(destination);
    }
}

impl Drop for AnalysisWorker {
    fn drop(&mut self) {
        self.shutdown.shutdown();
    }
}

/// Runs FFTs only for the current source and checks cancellation between fixed-size drain chunks.
fn run_worker(shared: &SharedState, stop: &AtomicBool, pending: &Mutex<PendingBatch>) {
    let mut queued: Vec<AnalysisSample> = Vec::with_capacity(CHUNK_FRAMES);
    let mut frames = Vec::with_capacity(CHUNK_FRAMES);
    let mut generation = u64::MAX;
    let mut rate_bits = 0;
    let mut analyzer: Option<SpatialAnalyzer> = None;
    let mut builder = ParticleBuilder::default();
    while !stop.load(Ordering::Acquire) {
        let Some((source, rate)) = shared.monitoring.configuration() else {
            thread::park_timeout(POLL_INTERVAL);
            continue;
        };
        if source != generation {
            // The preference setter advances source before unlocking. A second source read therefore
            // rejects settings that were published after the stable rate/source snapshot above.
            let preferences = shared.display_preferences();
            if source != shared.analysis_generation() {
                continue;
            }
            generation = source;
            if rate.to_bits() != rate_bits {
                analyzer = SpatialAnalyzer::new(rate).ok();
                rate_bits = rate.to_bits();
            } else if let Some(analyzer) = &mut analyzer {
                analyzer.reset();
            }
            builder.reset();
            if source != shared.analysis_generation() {
                continue;
            }
            // Settings are sampled only at stream boundaries, never half-way through a tier deadline.
            apply_settings(&mut builder, analyzer.as_mut(), preferences);
            let status = if !capture_enabled(source) {
                AnalysisStatus::Disabled
            } else if analyzer.is_none() {
                AnalysisStatus::UnsupportedRate
            } else {
                AnalysisStatus::Waiting
            };
            publish(shared, pending, source, status, &[]);
        }
        for _ in 0..CHUNKS_PER_PASS {
            if stop.load(Ordering::Acquire) || source != shared.analysis_generation() {
                break;
            }
            shared.drain_analysis(&mut queued);
            if queued.is_empty() {
                break;
            }
            frames.clear();
            frames.extend(
                queued
                    .iter()
                    .filter(|sample| sample.generation == source)
                    .map(|sample| sample.frame),
            );
            if !capture_enabled(source) || frames.is_empty() {
                continue;
            }
            let Some(analyzer) = &mut analyzer else {
                continue;
            };
            if analyzer
                .push_frames(&frames, |points, info| builder.add_frame(points, info))
                .is_err()
            {
                analyzer.reset();
                builder.reset();
                publish(shared, pending, source, AnalysisStatus::Failed, &[]);
                break;
            }
            if source != shared.analysis_generation() {
                break;
            }
            publish(shared, pending, source, AnalysisStatus::Running, builder.particles());
            builder.clear();
        }
        thread::park_timeout(POLL_INTERVAL);
    }
}

/// Adapts the transport's detached preference record to the pure projection model.
fn apply_settings(
    builder: &mut ParticleBuilder,
    analyzer: Option<&mut SpatialAnalyzer>,
    preferences: crate::DisplayPreferences,
) {
    if let Some(analyzer) = analyzer
        && let Some(curve) = crate::StereoPositionCurve::new(
            f64::from(preferences.pan_midpoint_db),
            f64::from(preferences.pan_edge_db),
        )
    {
        analyzer.set_position_curve(curve);
    }
    let frequency_scale = if preferences.mel_scale {
        crate::particles::FrequencyScale::Mel
    } else {
        crate::particles::FrequencyScale::Logarithmic
    };
    let color_mode = match preferences.color_mode {
        crate::ColourMode::Frequency => crate::particles::ColorMode::Frequency,
        crate::ColourMode::Phase => crate::particles::ColorMode::Phase,
    };
    if let Some(settings) = crate::particles::DisplaySettings::new(
        color_mode,
        frequency_scale,
        f64::from(preferences.persistence_seconds),
        preferences.contrast,
    ) {
        builder.set_settings(settings);
    }
}

/// Rejects stale completion and retains newest bounded work. This mutex is never touched by audio.
fn publish(
    shared: &SharedState,
    pending: &Mutex<PendingBatch>,
    generation: u64,
    status: AnalysisStatus,
    points: &[SpatialParticle],
) {
    let mut pending = pending.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
    if generation != shared.analysis_generation() {
        return;
    }
    if pending.update.generation != generation {
        pending.points.clear();
    }
    pending.update = AnalysisUpdate { generation, status };
    let incoming = points
        .get(points.len().saturating_sub(MAXIMUM_POINTS)..)
        .unwrap_or_default();
    let remove = (pending.points.len() + incoming.len()).saturating_sub(MAXIMUM_POINTS);
    if remove > 0 {
        pending.points.drain(..remove);
    }
    pending.points.extend_from_slice(incoming);
    drop(pending);
    shared.monitoring.status.store(status as u8, Ordering::Release);
    shared.monitoring.status_generation.store(generation, Ordering::Release);
}

#[cfg(test)]
mod test_worker {
    use super::AnalysisWorker;
    use crate::Imager;
    use std::sync::Arc;
    use std::sync::atomic::Ordering;

    #[test]
    fn disabled_capture_does_not_release_consumer_ownership() {
        let imager = Imager::new();
        let shared = imager.shared();
        let worker = AnalysisWorker::new(Arc::clone(&shared)).expect("consumer");
        shared.set_analysis_enabled(false);
        assert!(AnalysisWorker::new(Arc::clone(&shared)).is_err());
        drop(worker);
        assert!(AnalysisWorker::new(shared).is_ok());
    }

    #[test]
    fn shutdown_handle_joins_live_owner_without_disabling_a_replacement() {
        let imager = Imager::new();
        let shared = imager.shared();
        let worker = AnalysisWorker::new(Arc::clone(&shared)).expect("consumer");
        let shutdown = worker.shutdown_handle();
        shutdown.shutdown();
        assert_eq!(Arc::strong_count(&worker.pending), 1);
        let replacement = AnalysisWorker::new(Arc::clone(&shared)).expect("replacement");
        shutdown.shutdown();
        drop(worker);
        assert!(shared.monitoring.claimed.load(Ordering::Acquire));
        drop(replacement);
        assert!(!shared.monitoring.claimed.load(Ordering::Acquire));
    }

    #[test]
    fn shutdown_clones_are_concurrently_idempotent() {
        let imager = Imager::new();
        let worker = AnalysisWorker::new(imager.shared()).expect("consumer");
        let handles = [worker.shutdown_handle(), worker.shutdown_handle()];
        std::thread::scope(|scope| {
            for handle in &handles {
                scope.spawn(move || handle.shutdown());
            }
        });
        assert_eq!(Arc::strong_count(&worker.pending), 1);
    }
}
