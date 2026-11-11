//! Atomic monitoring configuration, detached display preferences, and generation-tagged audio transport.
//!
//! Audio owns only the producer and correlation accumulators. All configuration payloads are atomic,
//! and a callback never retries a configuration read or waits for an editor.

use std::sync::atomic::{AtomicBool, AtomicU8, AtomicU32, AtomicU64, Ordering};

use serde::{Deserialize, Serialize};

/// Source flag selecting unprocessed input instead of final output.
const PRE_FLAG: u64 = 1;
/// Source flag permitting audio capture.
const CAPTURE_FLAG: u64 = 2;
/// Transient source flag preventing capture while a sample rate is published.
const RATE_UNSTABLE_FLAG: u64 = 4;
/// Generation increment preserving the source flags.
const GENERATION_STEP: u64 = 8;

/// Non-automatable source of the spatial analysis.
#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub enum AnalysisTap {
    /// Untouched host input, before FIR latency and imaging.
    Pre,
    /// Final output after gain, Delta, and Mono.
    #[default]
    Post,
}

/// Worker availability, independent of audio preparation success.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum AnalysisStatus {
    /// Capture has not produced a completed frame yet.
    Waiting,
    /// Analysis is accepting the current stream.
    Running,
    /// Optional display work is disabled.
    Disabled,
    /// Audio remains available but the analyzer rate policy rejects this rate.
    UnsupportedRate,
    /// An internal transform failure prevented the latest analysis pass.
    Failed,
}

/// Choice of linear-light particle palette, independent of sound parameters.
#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub enum ColourMode {
    /// Frequency palette from bass red to treble violet.
    #[default]
    Frequency,
    /// Spectral phase coherence from in-phase blue to opposite-phase red.
    Phase,
}

/// Validated, detached preferences safe to serialize off the audio thread.
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct DisplayPreferences {
    /// Exponential display persistence in seconds.
    pub persistence_seconds: f32,
    /// Normalized particle contrast in zero through one.
    pub contrast: f32,
    /// Frequency or phase-coherence palette.
    pub color_mode: ColourMode,
}

impl Default for DisplayPreferences {
    fn default() -> Self {
        Self {
            persistence_seconds: 0.3,
            contrast: 0.6,
            color_mode: ColourMode::Frequency,
        }
    }
}

impl DisplayPreferences {
    /// Rejects malformed preferences before any state publication occurs.
    #[must_use]
    pub fn is_valid(self) -> bool {
        self.persistence_seconds.is_finite()
            && (0.05..=3.0).contains(&self.persistence_seconds)
            && self.contrast.is_finite()
            && (0.0..=1.0).contains(&self.contrast)
    }
}

/// Independent meter observations with lifecycle identity and callback progress.
#[derive(Clone, Copy, Debug)]
pub struct CorrelationSnapshot {
    /// Latest values, with NaN representing unavailable signal power.
    pub readings: [f32; 4],
    /// Lifecycle identity used to discard old presentation history.
    pub generation: u64,
    /// Audio callback progress, used for UI-side stale-reading expiry.
    pub revision: u64,
}

/// One copyable queue item. Source identity is captured before selecting its audio tap.
#[derive(Clone, Copy, Debug)]
pub struct AnalysisSample {
    pub(crate) frame: [f32; 2],
    pub(crate) generation: u64,
}

/// Atomic scalar storage with no callback-owned heap objects or GUI references.
pub struct Monitoring {
    pub(crate) source: AtomicU64,
    pub(crate) rate: AtomicU32,
    pub(crate) claimed: AtomicBool,
    pub(crate) status: AtomicU8,
    pub(crate) status_generation: AtomicU64,
    preferences: AtomicU64,
    pub(crate) correlation_enabled: AtomicBool,
    pub(crate) correlation_generation: AtomicU64,
    pub(crate) correlation_revision: AtomicU64,
    pub(crate) correlation_published_generation: AtomicU64,
    pub(crate) readings: [AtomicU32; 4],
    processing_active: AtomicBool,
    maximum_block_frames: AtomicU32,
}

impl Monitoring {
    /// Allocates no storage and starts with optional measurements disabled.
    pub(crate) fn new() -> Self {
        Self {
            source: AtomicU64::new(0),
            rate: AtomicU32::new(48_000.0_f32.to_bits()),
            claimed: AtomicBool::new(false),
            status: AtomicU8::new(AnalysisStatus::Disabled as u8),
            status_generation: AtomicU64::new(0),
            preferences: AtomicU64::new(encode_preferences(DisplayPreferences::default())),
            correlation_enabled: AtomicBool::new(false),
            correlation_generation: AtomicU64::new(0),
            correlation_revision: AtomicU64::new(0),
            correlation_published_generation: AtomicU64::new(u64::MAX),
            readings: std::array::from_fn(|_| AtomicU32::new(f32::NAN.to_bits())),
            processing_active: AtomicBool::new(true),
            maximum_block_frames: AtomicU32::new(0),
        }
    }

    /// Returns one stable source/rate snapshot, or skips this pass during concurrent publication.
    pub(crate) fn configuration(&self) -> Option<(u64, f32)> {
        let before = self.source.load(Ordering::Acquire);
        if before & RATE_UNSTABLE_FLAG != 0 {
            return None;
        }
        let rate = f32::from_bits(self.rate.load(Ordering::Acquire));
        (before == self.source.load(Ordering::Acquire)).then_some((before, rate))
    }

    /// Publishes rate on the serialized preparation owner, never concurrently with audio processing.
    pub(crate) fn set_rate(&self, rate: f32) {
        self.source.fetch_or(RATE_UNSTABLE_FLAG, Ordering::AcqRel);
        self.rate.store(rate.to_bits(), Ordering::Release);
        let _ = self.source.try_update(Ordering::AcqRel, Ordering::Acquire, |source| {
            Some(source.wrapping_add(GENERATION_STEP) & !RATE_UNSTABLE_FLAG)
        });
    }

    /// Invalidates queued observations with one bounded callback-safe atomic operation.
    pub(crate) fn invalidate(&self) {
        self.source.fetch_add(GENERATION_STEP, Ordering::AcqRel);
        self.invalidate_correlation();
    }

    /// Changes a GUI-side source flag and invalidates only when its value actually changes.
    fn set_flag(&self, flag: u64, enabled: bool) {
        let _ = self.source.try_update(Ordering::AcqRel, Ordering::Acquire, |source| {
            if (source & flag != 0) == enabled {
                None
            } else {
                Some((source ^ flag).wrapping_add(GENERATION_STEP))
            }
        });
    }

    /// Publishes unavailable meter values without accessing audio-owned moments.
    pub(crate) fn invalidate_correlation(&self) {
        self.correlation_generation.fetch_add(1, Ordering::AcqRel);
        for reading in &self.readings {
            reading.store(f32::NAN.to_bits(), Ordering::Relaxed);
        }
    }

    /// Reads one coherent packed preference record without acquiring an editor lock.
    pub(crate) fn preferences(&self) -> DisplayPreferences {
        decode_preferences(self.preferences.load(Ordering::Acquire))
    }

    /// Publishes the complete validated preference record atomically.
    pub(crate) fn set_preferences(&self, preferences: DisplayPreferences) -> bool {
        if !preferences.is_valid() {
            return false;
        }
        let encoded = encode_preferences(preferences);
        if self.preferences.swap(encoded, Ordering::AcqRel) != encoded {
            self.source.fetch_add(GENERATION_STEP, Ordering::AcqRel);
        }
        true
    }
}

/// Packs two bounded float values and one mode without lossy quantization.
/// Nonnegative contrast leaves its sign bit available for the palette selection.
fn encode_preferences(preferences: DisplayPreferences) -> u64 {
    u64::from(preferences.persistence_seconds.to_bits())
        | (u64::from(preferences.contrast.to_bits() & 0x7fff_ffff) << 32)
        | (u64::from(preferences.color_mode == ColourMode::Phase) << 63)
}

/// Decodes only records produced from validated nonnegative values.
const fn decode_preferences(encoded: u64) -> DisplayPreferences {
    DisplayPreferences {
        persistence_seconds: f32::from_bits(encoded as u32),
        contrast: f32::from_bits((encoded >> 32) as u32 & 0x7fff_ffff),
        color_mode: if encoded >> 63 == 0 {
            ColourMode::Frequency
        } else {
            ColourMode::Phase
        },
    }
}

impl crate::SharedState {
    /// Returns the exact source identity used for queue and GPU clear validation.
    #[must_use]
    pub fn analysis_generation(&self) -> u64 {
        self.monitoring.source.load(Ordering::Acquire)
    }

    /// Returns the selected non-automatable analyzer tap.
    #[must_use]
    pub fn analysis_tap(&self) -> AnalysisTap {
        if self.analysis_generation() & PRE_FLAG != 0 {
            AnalysisTap::Pre
        } else {
            AnalysisTap::Post
        }
    }

    /// Changes the tap without changing sound controls or touching queue indices.
    pub fn set_analysis_tap(&self, tap: AnalysisTap) {
        self.monitoring.set_flag(PRE_FLAG, tap == AnalysisTap::Pre);
    }

    /// Enables capture independently of exclusive worker ownership. Call from editor/control threads only.
    pub fn set_analysis_enabled(&self, enabled: bool) {
        self.monitoring.set_flag(CAPTURE_FLAG, enabled);
    }

    /// Returns whether optional audio capture is requested.
    #[must_use]
    pub fn analysis_enabled(&self) -> bool {
        capture_enabled(self.analysis_generation())
    }

    /// Reports analysis availability without constraining supported audio rates.
    #[must_use]
    pub fn analysis_status(&self) -> AnalysisStatus {
        let source = self.analysis_generation();
        if !capture_enabled(source) {
            return AnalysisStatus::Disabled;
        }
        let published = self.monitoring.status_generation.load(Ordering::Acquire);
        let status = self.monitoring.status.load(Ordering::Acquire);
        if source != published || source != self.analysis_generation() {
            return AnalysisStatus::Waiting;
        }
        match status {
            0 => AnalysisStatus::Waiting,
            1 => AnalysisStatus::Running,
            2 => AnalysisStatus::Disabled,
            3 => AnalysisStatus::UnsupportedRate,
            _ => AnalysisStatus::Failed,
        }
    }

    /// Returns a coherent detached preference record for GUI or non-audio state callbacks.
    #[must_use]
    pub fn display_preferences(&self) -> DisplayPreferences {
        self.monitoring.preferences()
    }

    /// Publishes validated appearance preferences without host sound-parameter gestures.
    pub fn set_display_preferences(&self, preferences: DisplayPreferences) -> bool {
        self.monitoring.set_preferences(preferences)
    }

    /// Requests optional meters. Only the processing owner changes their accumulated moments.
    pub fn set_correlation_enabled(&self, enabled: bool) {
        if self.monitoring.correlation_enabled.swap(enabled, Ordering::AcqRel) != enabled {
            self.monitoring.invalidate_correlation();
        }
    }

    /// Returns independent readings and progress, rejecting observations crossing a lifecycle change.
    #[must_use]
    pub fn correlation_snapshot(&self) -> CorrelationSnapshot {
        let generation = self.monitoring.correlation_generation.load(Ordering::Acquire);
        let published_generation = self.monitoring.correlation_published_generation.load(Ordering::Acquire);
        let mut readings = self
            .monitoring
            .readings
            .each_ref()
            .map(|value| f32::from_bits(value.load(Ordering::Relaxed)));
        if generation != self.monitoring.correlation_generation.load(Ordering::Acquire)
            || generation != published_generation
            || !self.monitoring.correlation_enabled.load(Ordering::Acquire)
            || !self.monitoring.processing_active.load(Ordering::Acquire)
        {
            readings.fill(f32::NAN);
        }
        CorrelationSnapshot {
            readings,
            generation,
            revision: self.monitoring.correlation_revision.load(Ordering::Acquire),
        }
    }

    /// Invalidates display generations on reset/stop without locking or freeing callback-owned storage.
    pub fn invalidate_monitoring(&self) {
        self.monitoring.invalidate();
    }

    /// Publishes host processing availability without touching DSP history or consulting a clock.
    pub fn set_processing_active(&self, active: bool) {
        if self.monitoring.processing_active.swap(active, Ordering::AcqRel) != active {
            self.invalidate_monitoring();
        }
    }

    /// Records the prepared block bound for UI-side stale meter expiry.
    pub fn set_maximum_block_frames(&self, frames: u32) {
        self.monitoring.maximum_block_frames.store(frames, Ordering::Relaxed);
    }

    /// Returns the expiry allowance in seconds, at least 250 ms or two maximum host blocks.
    #[must_use]
    pub fn correlation_expiry_seconds(&self) -> f32 {
        (2.0 * self.monitoring.maximum_block_frames.load(Ordering::Relaxed) as f32 / self.sample_rate()).max(0.25)
    }
}

/// Checks the captured source rather than resampling a possibly changed tap after processing.
pub const fn capture_enabled(source: u64) -> bool {
    source & (CAPTURE_FLAG | RATE_UNSTABLE_FLAG) == CAPTURE_FLAG
}

/// Returns the selected samples using the same token that will accompany them in the queue.
pub const fn select_frame(source: u64, input: [f32; 2], output: [f32; 2]) -> [f32; 2] {
    if source & PRE_FLAG != 0 { input } else { output }
}
