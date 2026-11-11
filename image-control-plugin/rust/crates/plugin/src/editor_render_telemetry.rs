//! Opt-in bounded refresh and successful native encoding observations, written only when the editor closes.

use std::io::Write as _;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, Weak};
use std::time::{Instant, SystemTime, UNIX_EPOCH};

use serde::Serialize;

/// At 240 Hz this retains over four minutes without growing storage during refresh or encoding.
const MAXIMUM_SAMPLES: usize = 65_536;

/// A counter observation is not a GPU completion fence or physical presentation timestamp.
#[derive(Serialize)]
struct RefreshSample {
    elapsed_seconds: f64,
    successful_frames: u64,
    update_seconds: f64,
    logical_width: f32,
    logical_height: f32,
}

/// A successful CPU command-encoding endpoint, not GPU completion or compositor presentation.
#[derive(Serialize)]
struct NativeFrameSample {
    elapsed_seconds: f64,
    successful_frames: u64,
    encoding_seconds: f64,
    physical_plot_width: u32,
    physical_plot_height: u32,
}

/// Deterministic four-second triangular UI gesture, shared numerically with the C++ benchmark driver.
fn sweep_preferences(
    elapsed: f64,
    mut preferences: image_control_dsp::DisplayPreferences,
) -> image_control_dsp::DisplayPreferences {
    let phase = (elapsed / 4.0).rem_euclid(1.0);
    let triangle = 1.0 - phase.mul_add(2.0, -1.0).abs();
    preferences.persistence_seconds = triangle.mul_add(2.4, 0.1) as f32;
    preferences.contrast = triangle.mul_add(0.8, 0.1) as f32;
    preferences
}

/// The editor alone owns this opt-in store. Cached shader programs retain only a weak reference.
/// Its mutex is never reachable from an audio callback or retained by an analysis worker.
pub struct FrameSamples {
    started: Instant,
    samples: Vec<NativeFrameSample>,
    dropped_samples: u64,
    closed: bool,
    sweep_enabled: bool,
    sweep_updates: u64,
    sweep_rejected_updates: u64,
    sweep_generation_changes: u64,
    sweep_epoch_changes: u64,
    sweep_first_generation: Option<u64>,
    sweep_last_generation: Option<u64>,
    sweep_min_persistence: Option<f32>,
    sweep_max_persistence: Option<f32>,
    sweep_min_contrast: Option<f32>,
    sweep_max_contrast: Option<f32>,
}

impl FrameSamples {
    /// Allocates complete bounded storage once during editor boot, before the native renderer uses it.
    fn new(started: Instant) -> Self {
        Self {
            started,
            samples: Vec::with_capacity(MAXIMUM_SAMPLES),
            dropped_samples: 0,
            closed: false,
            sweep_enabled: false,
            sweep_updates: 0,
            sweep_rejected_updates: 0,
            sweep_generation_changes: 0,
            sweep_epoch_changes: 0,
            sweep_first_generation: None,
            sweep_last_generation: None,
            sweep_min_persistence: None,
            sweep_max_persistence: None,
            sweep_min_contrast: None,
            sweep_max_contrast: None,
        }
    }

    /// Drives real appearance setters only from the shader widget's UI-thread native redraw event.
    /// No restore, parameter gesture, audio callback, or offscreen render benchmark uses this path.
    pub(crate) fn sweep(&mut self, shared: &image_control_dsp::SharedState) {
        self.sweep_at(shared, self.started.elapsed().as_secs_f64());
    }

    /// Keeps deterministic gesture generation independently testable
    /// without sleeping or process-global environment edits.
    fn sweep_at(&mut self, shared: &image_control_dsp::SharedState, elapsed: f64) {
        if !self.sweep_enabled || self.closed {
            return;
        }
        let before_generation = shared.analysis_generation();
        let before = shared.display_preferences_snapshot();
        self.sweep_first_generation.get_or_insert(before_generation);
        if self
            .sweep_last_generation
            .is_some_and(|generation| generation != before_generation)
        {
            self.sweep_generation_changes += 1;
        }
        let preferences = sweep_preferences(elapsed, **before);
        if !shared.set_display_preferences(preferences) {
            self.sweep_rejected_updates += 1;
            return;
        }
        let after_generation = shared.analysis_generation();
        let after = shared.display_preferences_snapshot();
        if before_generation != after_generation {
            self.sweep_generation_changes += 1;
        }
        if !before.analysis_matches(&after) {
            self.sweep_epoch_changes += 1;
        }
        self.sweep_last_generation = Some(after_generation);
        self.sweep_updates += 1;
        self.sweep_min_persistence = Some(
            self.sweep_min_persistence
                .map_or(preferences.persistence_seconds, |value| {
                    value.min(preferences.persistence_seconds)
                }),
        );
        self.sweep_max_persistence = Some(
            self.sweep_max_persistence
                .map_or(preferences.persistence_seconds, |value| {
                    value.max(preferences.persistence_seconds)
                }),
        );
        self.sweep_min_contrast = Some(
            self.sweep_min_contrast
                .map_or(preferences.contrast, |value| value.min(preferences.contrast)),
        );
        self.sweep_max_contrast = Some(
            self.sweep_max_contrast
                .map_or(preferences.contrast, |value| value.max(preferences.contrast)),
        );
    }

    /// Records only a successfully encoded composite. Locking and timestamps are benchmark-only render work.
    pub(crate) fn observe(&mut self, started: Instant, successful_frames: u64, physical_size: [u32; 2]) {
        if self.closed {
            return;
        }
        if self.samples.len() == MAXIMUM_SAMPLES {
            self.dropped_samples = self.dropped_samples.saturating_add(1);
            return;
        }
        let encoded = Instant::now();
        let [physical_plot_width, physical_plot_height] = physical_size;
        self.samples.push(NativeFrameSample {
            elapsed_seconds: encoded.saturating_duration_since(self.started).as_secs_f64(),
            successful_frames,
            encoding_seconds: encoded.saturating_duration_since(started).as_secs_f64(),
            physical_plot_width,
            physical_plot_height,
        });
    }
}

/// Dedicated benchmark hosts opt in before loading the plugin, without changing ordinary host behavior.
#[derive(Serialize)]
pub(super) struct Recorder {
    #[serde(skip)]
    path: PathBuf,
    #[serde(skip)]
    started: Instant,
    #[serde(skip)]
    native_frames: Arc<Mutex<FrameSamples>>,
    schema_version: u32,
    implementation: &'static str,
    frame_timestamp_kind: &'static str,
    counter_kind: &'static str,
    started_unix_seconds: f64,
    maximum_samples: usize,
    dropped_samples: u64,
    samples: Vec<RefreshSample>,
}

impl Recorder {
    /// Reads the opt-in path on the GUI thread and allocates all bounded storage once.
    pub(super) fn from_environment() -> Option<Self> {
        let path = PathBuf::from(std::env::var_os("AUDIO_IMAGECONTROL_RENDER_REPORT")?);
        if !path.is_absolute() {
            return None;
        }
        let started_unix_seconds = SystemTime::now().duration_since(UNIX_EPOCH).ok()?.as_secs_f64();
        let recorder = Self::new(path, started_unix_seconds);
        recorder
            .native_frames
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .sweep_enabled = std::env::var_os("AUDIO_IMAGECONTROL_RENDER_SWEEP").is_some_and(|value| value == "1");
        Some(recorder)
    }

    /// Establishes one monotonic origin for refreshes and real native encoding endpoints.
    fn new(path: PathBuf, started_unix_seconds: f64) -> Self {
        let started = Instant::now();
        Self {
            path,
            started,
            native_frames: Arc::new(Mutex::new(FrameSamples::new(started))),
            schema_version: 1,
            implementation: "rust-iced-wgpu",
            frame_timestamp_kind: "first_refresh_observing_successful_counter_not_encoding_time",
            counter_kind: "successful_spectrum_command_encoding_not_gpu_completion_or_scanout",
            started_unix_seconds,
            maximum_samples: MAXIMUM_SAMPLES,
            dropped_samples: 0,
            samples: Vec::with_capacity(MAXIMUM_SAMPLES),
        }
    }

    /// Gives cached shader programs non-owning access without extending the editor's benchmark lifetime.
    pub(super) fn frame_samples(&self) -> Weak<Mutex<FrameSamples>> {
        Arc::downgrade(&self.native_frames)
    }

    /// Records an ordinary application refresh. Native cadence is independently sampled by successful encoding.
    pub(super) fn observe(&mut self, started: Instant, successful_frames: u64, size: iced_baseview::Size) {
        if self.samples.len() == MAXIMUM_SAMPLES {
            self.dropped_samples = self.dropped_samples.saturating_add(1);
            return;
        }
        self.samples.push(RefreshSample {
            elapsed_seconds: started.saturating_duration_since(self.started).as_secs_f64(),
            successful_frames,
            update_seconds: started.elapsed().as_secs_f64(),
            logical_width: size.width,
            logical_height: size.height,
        });
    }

    /// Takes the bounded native snapshot at editor close and releases its lock before serialization or file I/O.
    /// Only editor teardown calls this in production, never processing, refresh, or GPU command encoding.
    pub(super) fn flush(&self) -> std::io::Result<()> {
        let mut native = self
            .native_frames
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        native.closed = true;
        let native_frame_samples = std::mem::take(&mut native.samples);
        let dropped_native_frame_samples = native.dropped_samples;
        let appearance_sweep = serde_json::json!({
            "enabled": native.sweep_enabled,
            "accepted_updates": native.sweep_updates,
            "rejected_updates": native.sweep_rejected_updates,
            "generation_changes": native.sweep_generation_changes,
            "projection_epoch_changes": native.sweep_epoch_changes,
            "first_generation": native.sweep_first_generation,
            "last_generation": native.sweep_last_generation,
            "minimum_persistence_seconds": native.sweep_min_persistence,
            "maximum_persistence_seconds": native.sweep_max_persistence,
            "minimum_contrast": native.sweep_min_contrast,
            "maximum_contrast": native.sweep_max_contrast,
            "driver": "native_redraw_ui_thread_production_display_preferences_setter",
            "period_seconds": 4.0,
        });
        drop(native);
        let mut report = serde_json::to_value(self).map_err(std::io::Error::other)?;
        if let Some(object) = report.as_object_mut() {
            object.insert(
                "build_type".into(),
                if crate::benchmark_build::PROFILE == "release" {
                    "Release"
                } else {
                    "non-Release"
                }
                .into(),
            );
            object.insert("build_profile".into(), crate::benchmark_build::PROFILE.into());
            object.insert("opt_level".into(), crate::benchmark_build::OPT_LEVEL.into());
            object.insert(
                "native_cpu_enabled".into(),
                (crate::benchmark_build::NATIVE_CPU == "true").into(),
            );
            object.insert("appearance_sweep".into(), appearance_sweep);
            object.insert(
                "native_frame_timestamp_kind".into(),
                "successful_cpu_command_encoding_endpoint_not_gpu_completion_or_scanout".into(),
            );
            object.insert(
                "native_frame_samples".into(),
                serde_json::to_value(native_frame_samples).map_err(std::io::Error::other)?,
            );
            object.insert(
                "dropped_native_frame_samples".into(),
                dropped_native_frame_samples.into(),
            );
        }
        let file = std::fs::File::create(&self.path)?;
        let mut writer = std::io::BufWriter::new(file);
        serde_json::to_writer(&mut writer, &report).map_err(std::io::Error::other)?;
        writer.flush()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Refresh storage stays fixed and counts saturation explicitly rather than overwriting early observations.
    #[test]
    fn bounded_storage_and_schema_preserve_counter_semantics() {
        let directory = tempfile::tempdir().expect("temporary report");
        let mut recorder = Recorder::new(directory.path().join("render.json"), 0.0);
        for index in 0..=MAXIMUM_SAMPLES {
            recorder.observe(Instant::now(), index as u64, iced_baseview::Size::new(1000.0, 764.0));
        }
        assert_eq!(recorder.samples.len(), MAXIMUM_SAMPLES);
        assert_eq!(recorder.dropped_samples, 1);
        recorder.flush().expect("close-time report");
        let report: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&recorder.path).expect("report")).expect("JSON");
        assert_eq!(report["dropped_samples"], 1);
        assert_eq!(report["samples"][1]["successful_frames"], 1);
    }

    /// Actual production preference publication changes both controls without creating analysis or projection epochs.
    #[test]
    fn live_sweep_uses_validated_setters_without_resetting_analysis() {
        let shared = image_control_dsp::Imager::new().shared();
        let generation = shared.analysis_generation();
        let epoch = shared.display_preferences_snapshot();
        let mut frames = FrameSamples::new(Instant::now());
        frames.sweep_at(&shared, 0.0);
        assert_eq!(
            frames.sweep_updates, 0,
            "disabled instrumentation never drives preferences"
        );
        frames.sweep_enabled = true;
        for step in 0..=480 {
            frames.sweep_at(&shared, f64::from(step) / 120.0);
        }
        assert_eq!(frames.sweep_updates, 481);
        assert_eq!(frames.sweep_rejected_updates, 0);
        assert_eq!(frames.sweep_generation_changes, 0);
        assert_eq!(frames.sweep_epoch_changes, 0);
        assert_eq!(shared.analysis_generation(), generation);
        assert!(epoch.analysis_matches(&shared.display_preferences_snapshot()));
        assert!(
            frames
                .sweep_min_persistence
                .is_some_and(|value| (value - 0.1).abs() < 1e-6)
        );
        assert!(
            frames
                .sweep_max_persistence
                .is_some_and(|value| (value - 2.5).abs() < 1e-6)
        );
        assert!(
            frames
                .sweep_min_contrast
                .is_some_and(|value| (value - 0.1).abs() < 1e-6)
        );
        assert!(
            frames
                .sweep_max_contrast
                .is_some_and(|value| (value - 0.9).abs() < 1e-6)
        );
    }

    /// Native encodings need no application refresh, remain bounded, and are snapshotted only on close.
    #[test]
    fn native_samples_are_independent_bounded_and_weakly_owned() {
        let directory = tempfile::tempdir().expect("temporary report");
        let recorder = Recorder::new(directory.path().join("native.json"), 0.0);
        let weak = recorder.frame_samples();
        let frames = weak.upgrade().expect("live editor");
        for frame in 0..=MAXIMUM_SAMPLES {
            frames
                .lock()
                .expect("native samples")
                .observe(Instant::now(), frame as u64, [2000, 1528]);
        }
        assert!(recorder.samples.is_empty());
        assert_eq!(frames.lock().expect("samples").samples.capacity(), MAXIMUM_SAMPLES);
        recorder.flush().expect("native close snapshot");
        let report: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&recorder.path).expect("report")).expect("JSON");
        assert_eq!(
            report["native_frame_samples"].as_array().expect("frames").len(),
            MAXIMUM_SAMPLES
        );
        assert_eq!(report["dropped_native_frame_samples"], 1);
        assert!(frames.lock().expect("snapshot taken").samples.is_empty());
        drop(frames);
        drop(recorder);
        assert!(weak.upgrade().is_none(), "cached programs cannot keep telemetry alive");
    }
}
