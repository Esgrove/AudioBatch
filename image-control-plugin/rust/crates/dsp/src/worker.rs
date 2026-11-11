//! Bounded generation-aware spatial worker with exclusive consumer ownership and joined editor shutdown.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::monitoring::{AnalysisSample, capture_enabled};
use crate::particles::ParticleBuilder;
use crate::{AnalysisStatus, DisplayPreferences, SharedState, SpatialAnalyzer, SpatialParticle};

/// Hard upper bound on pending instances independently of producer or refresh speed.
const MAXIMUM_POINTS: usize = 65_536;
/// Worker polling never requires an audio-side notification or a GUI callback.
const POLL_INTERVAL: Duration = Duration::from_millis(16);
/// Bounded chunk between cancellation and stream-generation checks.
const CHUNK_FRAMES: usize = 4096;
/// Maximum queue storage examined per pass, even with a continuously refilling producer.
const CHUNKS_PER_PASS: usize = 8;

/// Metadata accompanies empty batches too, allowing GPU history to clear during silence.
#[derive(Clone, Debug)]
pub struct AnalysisUpdate {
    /// Exact stream identity whose observations are in this batch.
    pub generation: u64,
    /// Explicit availability for editor status text.
    pub status: AnalysisStatus,
    /// Immutable projection identity retained through editor queues and GPU history, exclusively off audio.
    pub preferences: Arc<DisplayPreferences>,
}

impl AnalysisUpdate {
    /// Compares both independently published identities, never just their preference values.
    #[must_use]
    pub fn matches(&self, generation: u64, preferences: &Arc<DisplayPreferences>) -> bool {
        self.generation == generation && Arc::ptr_eq(&self.preferences, preferences)
    }
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
                preferences: shared.monitoring.preference_snapshot(),
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
        if pending.update.generation != generation || !shared.monitoring.preferences_match(&pending.update.preferences)
        {
            pending.points.clear();
            pending.update = AnalysisUpdate {
                generation,
                status: shared.analysis_status(),
                preferences: shared.monitoring.preference_snapshot(),
            };
        }
        std::mem::swap(destination, &mut pending.points);
        pending.update.clone()
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
    let mut preferences = shared.monitoring.preference_snapshot();
    while !stop.load(Ordering::Acquire) {
        let Some((source, rate)) = shared.monitoring.configuration() else {
            thread::park_timeout(POLL_INTERVAL);
            continue;
        };
        if source != generation || !shared.monitoring.preferences_match(&preferences) {
            // Source and immutable preferences publish independently. Retain and recheck both identities,
            // including the interval after a preference swap but before its source-generation increment.
            let next_preferences = shared.monitoring.preference_snapshot();
            if !configuration_matches(shared, source, &next_preferences) {
                continue;
            }
            if rate.to_bits() != rate_bits {
                analyzer = SpatialAnalyzer::new(rate).ok();
                rate_bits = rate.to_bits();
            } else if let Some(analyzer) = &mut analyzer {
                analyzer.reset();
            }
            builder.reset();
            if !configuration_matches(shared, source, &next_preferences) {
                continue;
            }
            // Settings are sampled only at configuration boundaries, never half-way through a tier deadline.
            apply_settings(&mut builder, analyzer.as_mut(), *next_preferences);
            preferences = next_preferences;
            generation = source;
            let status = if !capture_enabled(source) {
                AnalysisStatus::Disabled
            } else if analyzer.is_none() {
                AnalysisStatus::UnsupportedRate
            } else {
                AnalysisStatus::Waiting
            };
            publish(shared, pending, source, &preferences, status, &[]);
        }
        for _ in 0..CHUNKS_PER_PASS {
            if stop.load(Ordering::Acquire) || !configuration_matches(shared, source, &preferences) {
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
                publish(shared, pending, source, &preferences, AnalysisStatus::Failed, &[]);
                break;
            }
            if !configuration_matches(shared, source, &preferences) {
                break;
            }
            publish(
                shared,
                pending,
                source,
                &preferences,
                AnalysisStatus::Running,
                builder.particles(),
            );
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

/// Validates both independent configuration identities using only non-audio preference loads.
fn configuration_matches(shared: &SharedState, generation: u64, preferences: &Arc<DisplayPreferences>) -> bool {
    generation == shared.analysis_generation() && shared.monitoring.preferences_match(preferences)
}

/// Rejects stale completion and retains newest bounded work. This mutex is never touched by audio.
fn publish(
    shared: &SharedState,
    pending: &Mutex<PendingBatch>,
    generation: u64,
    preferences: &Arc<DisplayPreferences>,
    status: AnalysisStatus,
    points: &[SpatialParticle],
) {
    let mut pending = pending.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
    if !configuration_matches(shared, generation, preferences) {
        return;
    }
    if !pending.update.matches(generation, preferences) {
        pending.points.clear();
    }
    pending.update = AnalysisUpdate {
        generation,
        status,
        preferences: Arc::clone(preferences),
    };
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
    use super::{AnalysisUpdate, AnalysisWorker, PendingBatch, configuration_matches, publish};
    use crate::{AnalysisStatus, DisplayPreferences, Imager};
    use std::sync::Arc;
    use std::sync::Mutex;
    use std::sync::atomic::Ordering;

    #[test]
    fn replaced_preference_identity_rejects_completion_even_with_the_current_source_generation() {
        let shared = Imager::new().shared();
        let stale = shared.monitoring.preference_snapshot();
        assert!(shared.set_display_preferences(DisplayPreferences {
            mel_scale: true,
            ..DisplayPreferences::default()
        }));
        // Pair stale preferences with the current source, exercising the independent preference check.
        let generation = shared.analysis_generation();
        let current = shared.monitoring.preference_snapshot();
        assert!(!configuration_matches(&shared, generation, &stale));
        assert!(configuration_matches(&shared, generation, &current));
        let pending = Mutex::new(PendingBatch {
            points: Vec::new(),
            update: AnalysisUpdate {
                generation,
                status: AnalysisStatus::Waiting,
                preferences: Arc::clone(&current),
            },
        });
        publish(&shared, &pending, generation, &stale, AnalysisStatus::Running, &[]);
        assert_eq!(
            pending.lock().expect("pending batch").update.status,
            AnalysisStatus::Waiting
        );
        publish(&shared, &pending, generation, &current, AnalysisStatus::Running, &[]);
        assert_eq!(
            pending.lock().expect("pending batch").update.status,
            AnalysisStatus::Running
        );

        assert!(shared.set_display_preferences(*stale));
        assert!(
            !shared.monitoring.preferences_match(&stale),
            "equal values cannot reuse an old identity"
        );
    }

    #[test]
    fn drained_batch_retains_the_exact_projection_identity() {
        let shared = Imager::new().shared();
        let worker = AnalysisWorker::new(Arc::clone(&shared)).expect("worker");
        let mut points = Vec::new();
        let original = shared.display_preferences_snapshot();
        let update = worker.drain_batch(&mut points);
        assert!(update.matches(shared.analysis_generation(), &original));
        assert!(Arc::ptr_eq(&update.preferences, &original));
        assert!(shared.set_display_preferences(DisplayPreferences {
            mel_scale: true,
            ..*original
        }));
        let current = shared.display_preferences_snapshot();
        assert!(!update.matches(shared.analysis_generation(), &current));
        let replacement = worker.drain_batch(&mut points);
        assert!(replacement.matches(shared.analysis_generation(), &current));
        assert!(Arc::ptr_eq(&replacement.preferences, &current));
        assert_eq!(points.len(), 0);
    }

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
