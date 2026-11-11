//! Bounded generation-aware spatial worker with exclusive consumer ownership and joined editor shutdown.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::monitoring::{AnalysisSample, MonitoringConfiguration, capture_enabled};
use crate::particles::ParticleBuilder;
use crate::{AnalysisStatus, DisplayConfiguration, SharedState, SpatialAnalyzer, SpatialParticle};

/// Hard upper bound on pending instances independently of producer or refresh speed.
const MAXIMUM_POINTS: usize = 65_536;
/// Worker polling never requires an audio-side notification or a GUI callback.
/// Bounded fallback draining when native frames pause. Visible editors wake the worker at display cadence.
const POLL_INTERVAL: Duration = Duration::from_millis(50);
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
    /// Coherent settings and retained projection epoch carried through editor queues and GPU history,
    /// exclusively off audio.
    pub preferences: Arc<DisplayConfiguration>,
}

impl AnalysisUpdate {
    /// Compares stream and retained projection epochs, permitting compatible live settings to coexist.
    #[must_use]
    pub fn matches(&self, generation: u64, preferences: &Arc<DisplayConfiguration>) -> bool {
        self.generation == generation && self.preferences.analysis_matches(preferences)
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

    /// Wakes bounded analysis from the UI frame clock, never from audio or a parameter callback.
    pub fn wake(&self) {
        let thread = self
            .shutdown
            .state
            .thread
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if let Some(worker) = thread.as_ref() {
            worker.thread().unpark();
        }
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
        let Some(MonitoringConfiguration {
            source,
            sample_rate: rate,
        }) = shared.monitoring.configuration()
        else {
            thread::park_timeout(POLL_INTERVAL);
            continue;
        };
        if source != generation || !shared.monitoring.preferences_match(&preferences) {
            // Source and projection epoch publish independently. Retain and recheck both identities,
            // including the interval after an incompatible publication but before its source increment.
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
            apply_settings(&mut builder, analyzer.as_mut(), **next_preferences);
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
            if stop.load(Ordering::Acquire) {
                break;
            }
            if !refresh_settings(shared, source, &mut preferences, &mut builder, analyzer.as_mut()) {
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

/// Samples one coherent settings record between chunks, never between tiers sharing one deadline.
/// Compatible edits preserve FFT history, audio time, adaptive reference, and already-weighted particles.
fn refresh_settings(
    shared: &SharedState,
    generation: u64,
    preferences: &mut Arc<DisplayConfiguration>,
    builder: &mut ParticleBuilder,
    analyzer: Option<&mut SpatialAnalyzer>,
) -> bool {
    let latest = shared.monitoring.preference_snapshot();
    if !preferences.analysis_matches(&latest) || !configuration_matches(shared, generation, &latest) {
        return false;
    }
    if !Arc::ptr_eq(preferences, &latest) {
        apply_settings(builder, analyzer, **latest);
        *preferences = latest;
    }
    true
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
fn configuration_matches(shared: &SharedState, generation: u64, preferences: &Arc<DisplayConfiguration>) -> bool {
    generation == shared.analysis_generation() && shared.monitoring.preferences_match(preferences)
}

/// Rejects stale completion and retains newest bounded work. This mutex is never touched by audio.
fn publish(
    shared: &SharedState,
    pending: &Mutex<PendingBatch>,
    generation: u64,
    preferences: &Arc<DisplayConfiguration>,
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

        assert!(shared.set_display_preferences(**stale));
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
            ..**original
        }));
        let current = shared.display_preferences_snapshot();
        assert!(!update.matches(shared.analysis_generation(), &current));
        let replacement = worker.drain_batch(&mut points);
        assert!(replacement.matches(shared.analysis_generation(), &current));
        assert!(Arc::ptr_eq(&replacement.preferences, &current));
        assert_eq!(points.len(), 0);
    }

    #[test]
    fn compatible_edits_keep_completed_and_drainable_particles_at_original_energy() {
        let shared = Imager::new().shared();
        let worker = AnalysisWorker::new(Arc::clone(&shared)).expect("worker");
        // Join the real thread so publication and draining have deterministic ownership in this test.
        worker.shutdown_handle().shutdown();
        let generation = shared.analysis_generation();
        let original = shared.display_preferences_snapshot();
        let mut builder = crate::ParticleBuilder::new();
        let point = crate::SpectralPoint::new(1000.0, 1000.0, 10.0, 0.5, 1.0, 1.0).expect("point");
        builder.add_frame(
            &[point],
            crate::SpectralFrameInfo {
                tier_index: 0,
                frame_seconds: 0.04,
                end_seconds: 0.04,
                last_at_time: true,
            },
        );
        let particles = builder.particles().to_vec();
        publish(
            &shared,
            &worker.pending,
            generation,
            &original,
            AnalysisStatus::Running,
            &particles,
        );
        assert!(shared.set_display_preferences(DisplayPreferences {
            persistence_seconds: 2.5,
            contrast: 0.1,
            solid_rendering: true,
            show_fps: true,
            ..**original
        }));
        let current = shared.display_preferences_snapshot();
        // Work computed before a compatible publication must remain publishable without rescaling.
        publish(
            &shared,
            &worker.pending,
            generation,
            &original,
            AnalysisStatus::Running,
            &particles,
        );
        let mut drained = Vec::new();
        let update = worker.drain_batch(&mut drained);
        assert!(update.matches(generation, &current));
        assert_eq!(drained, particles.repeat(2));
    }

    #[test]
    fn rapid_live_settings_preserve_fft_clock_reference_and_future_particle_weighting() {
        let shared = Imager::new().shared();
        let generation = shared.analysis_generation();
        let mut preferences = shared.display_preferences_snapshot();
        let mut analyzer = crate::SpatialAnalyzer::new(48_000.0).expect("analyzer");
        let mut builder = crate::ParticleBuilder::new();
        let signal: Vec<_> = (0..4096)
            .map(|index| {
                let sample = (std::f32::consts::TAU * 750.0 * index as f32 / 48_000.0).sin();
                [sample, sample * 10.0_f32.powf(-6.0 / 20.0)]
            })
            .collect();
        let mut endpoint = 0.0;
        analyzer
            .push_frames(&signal, |points, info| {
                endpoint = info.end_seconds;
                builder.add_frame(points, info);
            })
            .expect("first chunk");
        let reference = builder.reference_level();
        let original_particles = builder.particles().to_vec();
        assert!(!original_particles.is_empty(), "first chunk must produce particles");
        for index in 0..128 {
            let live = DisplayPreferences {
                persistence_seconds: if index % 2 == 0 { 0.1 } else { 1.2 },
                contrast: index as f32 / 128.0,
                solid_rendering: index % 2 == 0,
                pan_midpoint_db: 6.0,
                pan_edge_db: 18.0,
                smooth_correlation: index % 2 == 0,
                show_fps: index % 2 != 0,
                ..DisplayPreferences::default()
            };
            assert!(shared.set_display_preferences(live));
            assert!(super::refresh_settings(
                &shared,
                generation,
                &mut preferences,
                &mut builder,
                Some(&mut analyzer)
            ));
            assert_eq!(builder.particles(), original_particles);
            assert_eq!(builder.reference_level().to_bits(), reference.to_bits());
        }
        builder.clear();
        let mut low_persistence = crate::ParticleBuilder::new();
        super::apply_settings(
            &mut low_persistence,
            None,
            DisplayPreferences {
                persistence_seconds: 0.3,
                ..**preferences
            },
        );
        analyzer
            .push_frames(&signal[..512], |points, info| {
                assert!(
                    info.end_seconds > endpoint,
                    "live edits cannot restart the FFT audio clock"
                );
                assert_eq!(info.end_seconds.to_bits(), ((4096.0_f64 + 512.0) / 48_000.0).to_bits());
                builder.add_frame(points, info);
                low_persistence.add_frame(points, info);
            })
            .expect("continued chunk");
        assert!(
            !builder.particles().is_empty(),
            "continued clock must produce particles"
        );
        for (actual, low) in builder.particles().iter().zip(low_persistence.particles()) {
            assert!(
                (actual.position()[1] - 0.25).abs() < 0.002,
                "position curve applies to future observations"
            );
            for (actual_energy, low_energy) in actual.color().into_iter().zip(low.color()) {
                assert!(
                    actual_energy.mul_add(2.0, -low_energy).abs() < 1.0e-6,
                    "new persistence changes only future particle weight"
                );
            }
        }
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
