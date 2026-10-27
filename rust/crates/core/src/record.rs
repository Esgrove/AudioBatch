//! Analysis result types shared by the CLI and the GUI.
//!
//! Defines `AnalysisRecord`, which carries the per-file peak, true peak, loudness,
//! short-term loudness history, and waveform overview,
//! along with the `AnalysisStatus` lifecycle and the `AnalysisOptions` input parameters.

use std::fmt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::UNIX_EPOCH;

use crate::waveform::WaveformOverview;

/// Loudness reported for silence and other unmeasurable signals, in LUFS.
/// The cache and the history store this finite sentinel instead of negative infinity,
/// which SQLite and JSON cannot represent.
pub const NEGATIVE_INFINITY_LOUDNESS: f64 = -1000.0;

/// Smallest difference between two custom gains in decibels that counts as a change,
/// well below the 0.1 dB step of the gain slider.
const CUSTOM_GAIN_EPSILON_DB: f32 = 1e-4;

/// Lifecycle states of a file analysis record.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AnalysisStatus {
    /// Waiting to be analyzed.
    #[default]
    Pending,
    /// Loaded from the result cache without decoding.
    Cached,
    /// Decoded and measured in this run.
    Analyzed,
    /// Decoding or measuring failed, see the error message.
    Failed,
}

/// Why an analysis failed, which decides whether the cache keeps the failed record.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum AnalysisError {
    /// The file does not exist.
    MissingFile,
    /// The file exists but could not be opened for reading,
    /// for example because of its permissions or a network drive that went away, so a later run may succeed.
    Unreadable,
    /// No decoder reads the file, or its stream parameters are unusable.
    /// Reading the same bytes again fails the same way.
    Unsupported,
    /// Too many audio packets failed to decode, which a transient read error, such as on a network drive, can cause.
    DecodeFailed,
    /// The loudness measurement failed.
    MeasurementFailed,
    /// The analysis was cancelled before it finished.
    Cancelled,
    /// The analysis panicked.
    Panicked,
}

/// Input parameters shared by the GUI and CLI analysis flows.
#[derive(Debug, Default, Clone)]
pub struct AnalysisOptions {
    /// Files and directories to analyze.
    pub input_paths: Vec<PathBuf>,
    /// Search directories recursively.
    pub recursive: bool,
    /// Ignore cached results and analyze every file again.
    pub refresh: bool,
}

/// Combined-channel short-term LUFS readings, one per analysis block.
///
/// Each reading describes the trailing three seconds, with the zero-filled startup history of the meter.
/// Times are block ends, with the final partial block clamped to the source length.
#[derive(Debug, Default, Clone, PartialEq)]
pub struct ShortTermLoudnessHistory {
    /// Short-term loudness readings in LUFS, with the sentinel for silence.
    pub values: Vec<f32>,
    /// Sample rate of the analyzed stream in hertz.
    pub sample_rate: f64,
    /// Length of the analyzed stream in frames.
    pub length_in_samples: u64,
    /// Number of frames between two readings.
    pub samples_per_point: u32,
}

/// Analysis metadata and derived peak information for a single audio file.
#[derive(Debug, Default, Clone)]
pub struct AnalysisRecord {
    /// Absolute path of the analyzed file.
    pub path: PathBuf,
    /// File name without the directory.
    pub file_name: String,
    /// Container format name, for example "WAV file".
    pub format_name: String,
    /// Reason for a failed analysis, empty otherwise.
    pub error_message: String,
    /// Kind of a failed analysis, `None` otherwise.
    pub error: Option<AnalysisError>,
    /// Short-term loudness readings across the file, absent for failed records.
    pub short_term_loudness: Option<Arc<ShortTermLoudnessHistory>>,
    /// Min and max overview of the decoded samples, absent for failed records.
    pub waveform: Option<Arc<WaveformOverview>>,
    /// File size in bytes when the record was made.
    pub file_size: u64,
    /// Modification time in milliseconds since the Unix epoch when the record was made.
    pub modified_time_ms: i64,
    /// Decoded length in frames.
    pub length_in_samples: u64,
    /// Decoded duration in seconds.
    pub duration_seconds: f64,
    /// Signed sample peak of the left channel.
    pub peak_left: f32,
    /// Signed sample peak of the right channel, the left channel for mono files.
    pub peak_right: f32,
    /// Signed sample peak with the largest magnitude across all channels.
    pub overall_peak: f32,
    /// True peak of the left channel as a linear amplitude.
    pub true_peak_left: f64,
    /// True peak of the right channel, the left channel for mono files.
    pub true_peak_right: f64,
    /// Largest true peak across all channels.
    pub overall_true_peak: f64,
    /// Highest short-term loudness seen across the file, in LUFS.
    pub max_short_term_lufs: f64,
    /// Integrated loudness of the whole file, in LUFS.
    pub integrated_lufs: f64,
    /// Sample rate in hertz.
    pub sample_rate: u32,
    /// Number of channels.
    pub channels: u32,
    /// Source bit depth, or 0 when the format has none.
    pub bits_per_sample: u32,
    /// Per-file gain in decibels that the user set for this version of the file, applied to the preview.
    pub custom_gain_db: Option<f32>,
    /// Lifecycle state of the record.
    pub status: AnalysisStatus,
    /// True when the record was loaded from the cache.
    pub from_cache: bool,
}

/// A failed analysis step with the kind of failure, so callers can tell which failures repeat.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct AnalysisFailure {
    /// Kind of the failure.
    pub kind: AnalysisError,
    /// User-facing description of the failure.
    pub message: String,
}

impl AnalysisError {
    /// Returns true for an error that reading the same bytes again would repeat, so a cached failure stays valid.
    #[must_use]
    pub(crate) const fn is_repeatable(self) -> bool {
        matches!(self, Self::Unsupported)
    }
}

impl ShortTermLoudnessHistory {
    /// EBU-mode meter update rate in hertz, independent of the three-second measurement window.
    pub(crate) const UPDATE_RATE_HZ: f64 = 10.0;

    /// Returns the number of frames per reading for a sample rate, one reading per meter update.
    #[must_use]
    pub(crate) fn samples_per_point_for_rate(sample_rate: u32) -> u32 {
        (f64::from(sample_rate) / Self::UPDATE_RATE_HZ).round().max(1.0) as u32
    }

    /// Returns the exact time of a reading in seconds, including a final block shorter than the normal interval.
    #[must_use]
    pub fn time_at(&self, index: usize) -> f64 {
        if self.sample_rate <= 0.0 || self.samples_per_point == 0 || self.length_in_samples == 0 {
            return 0.0;
        }
        if index >= self.values.len() {
            return 0.0;
        }
        let position = ((index as u64 + 1) * u64::from(self.samples_per_point)).min(self.length_in_samples);
        position as f64 / self.sample_rate
    }
}

impl AnalysisRecord {
    /// Builds a baseline record from file system metadata before analysis begins.
    #[must_use]
    pub(crate) fn from_path(path: &Path) -> Self {
        let (file_size, modified_time_ms) = file_identity(path).unwrap_or_default();
        Self {
            path: path.to_path_buf(),
            file_name: path
                .file_name()
                .map(|name| name.to_string_lossy().into_owned())
                .unwrap_or_default(),
            file_size,
            modified_time_ms,
            max_short_term_lufs: NEGATIVE_INFINITY_LOUDNESS,
            integrated_lufs: NEGATIVE_INFINITY_LOUDNESS,
            ..Default::default()
        }
    }

    /// Returns true when the record represents a failed analysis attempt.
    #[must_use]
    pub fn has_error(&self) -> bool {
        self.status == AnalysisStatus::Failed
    }

    /// Returns true when the record has usable cached or freshly analyzed data.
    #[must_use]
    pub const fn is_ready(&self) -> bool {
        matches!(self.status, AnalysisStatus::Cached | AnalysisStatus::Analyzed)
    }

    /// Returns true when the file still has the size and modification time recorded at analysis,
    /// so the measured levels still describe it.
    #[must_use]
    pub fn matches_file_on_disk(&self) -> bool {
        file_identity(&self.path) == Some((self.file_size, self.modified_time_ms))
    }

    /// Returns the path as a display string.
    #[must_use]
    pub fn path_string(&self) -> String {
        self.path.to_string_lossy().into_owned()
    }

    /// Returns the custom gain as a linear factor, 1 for a file without one.
    #[must_use]
    pub fn custom_gain_linear(&self) -> f32 {
        self.custom_gain_db
            .map_or(1.0, |decibels| decibels_to_gain(f64::from(decibels)) as f32)
    }
}

impl AnalysisFailure {
    /// Creates a failure of the given kind with its user-facing message.
    #[must_use]
    pub fn new(kind: AnalysisError, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
        }
    }
}

impl fmt::Display for AnalysisFailure {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for AnalysisFailure {}

/// Converts decibels to a linear amplitude factor.
#[must_use]
pub fn decibels_to_gain(decibels: f64) -> f64 {
    10_f64.powf(decibels / 20.0)
}

/// Returns the custom gain to store for a slider value in decibels.
/// A value of about 0 dB, or one that is not finite, means no custom gain.
#[must_use]
pub fn custom_gain_from_decibels(decibels: f32) -> Option<f32> {
    (decibels.is_finite() && decibels.abs() >= CUSTOM_GAIN_EPSILON_DB).then_some(decibels)
}

/// Returns true when neither gain is set, or both are set to about the same value.
#[must_use]
pub fn same_custom_gain(first: Option<f32>, second: Option<f32>) -> bool {
    match (first, second) {
        (None, None) => true,
        (Some(first), Some(second)) => (first - second).abs() < CUSTOM_GAIN_EPSILON_DB,
        _ => false,
    }
}

/// Returns the size in bytes and the modification time in milliseconds of an existing file.
/// These two values identify the version of the file that a cached record describes.
#[must_use]
pub fn file_identity(path: &Path) -> Option<(u64, i64)> {
    let metadata = std::fs::metadata(path).ok()?;
    if !metadata.is_file() {
        return None;
    }
    let modified_time_ms = metadata
        .modified()
        .ok()
        .and_then(|modified| modified.duration_since(UNIX_EPOCH).ok())
        .and_then(|elapsed| i64::try_from(elapsed.as_millis()).ok())
        .unwrap_or_default();
    Some((metadata.len(), modified_time_ms))
}

/// Clamps non-finite or silent loudness readings to the shared negative infinity sentinel.
#[must_use]
pub(crate) fn normalize_loudness(loudness: f64) -> f64 {
    if !loudness.is_finite() || loudness <= NEGATIVE_INFINITY_LOUDNESS {
        NEGATIVE_INFINITY_LOUDNESS
    } else {
        loudness
    }
}

/// Returns true when the loudness value is the silence sentinel.
#[must_use]
pub fn is_silent_loudness(loudness: f64) -> bool {
    loudness <= NEGATIVE_INFINITY_LOUDNESS
}

#[cfg(test)]
mod test_record {
    use super::*;

    #[test]
    fn from_path_reads_file_identity() {
        let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
        let record = AnalysisRecord::from_path(&path);
        assert_eq!(record.file_name, "sine_stereo_44100_16.wav");
        assert!(record.file_size > 0);
        assert!(record.modified_time_ms > 0);
        assert!(record.matches_file_on_disk());
        assert_eq!(record.status, AnalysisStatus::Pending);
    }

    #[test]
    fn missing_file_has_no_identity() {
        let record = AnalysisRecord::from_path(Path::new("/does/not/exist.wav"));
        assert_eq!(record.file_size, 0);
        assert!(!record.matches_file_on_disk());
    }

    #[test]
    fn normalize_loudness_clamps_silence() {
        assert!((normalize_loudness(f64::NEG_INFINITY) - NEGATIVE_INFINITY_LOUDNESS).abs() < f64::EPSILON);
        assert!((normalize_loudness(f64::NAN) - NEGATIVE_INFINITY_LOUDNESS).abs() < f64::EPSILON);
        assert!((normalize_loudness(-14.0) + 14.0).abs() < f64::EPSILON);
        assert!(is_silent_loudness(NEGATIVE_INFINITY_LOUDNESS));
    }

    #[test]
    fn history_time_clamps_final_block() {
        let history = ShortTermLoudnessHistory {
            values: vec![-20.0; 3],
            sample_rate: 10.0,
            length_in_samples: 25,
            samples_per_point: 10,
        };
        assert!((history.time_at(0) - 1.0).abs() < 1e-9);
        assert!((history.time_at(2) - 2.5).abs() < 1e-9);
        assert!(history.time_at(3).abs() < 1e-9);
    }

    #[test]
    fn zero_or_invalid_custom_gain_is_no_gain() {
        assert_eq!(custom_gain_from_decibels(0.0), None);
        assert_eq!(custom_gain_from_decibels(-0.000_01), None);
        assert_eq!(custom_gain_from_decibels(f32::NAN), None);
        assert_eq!(custom_gain_from_decibels(-1.5), Some(-1.5));
    }

    #[test]
    fn custom_gains_compare_with_a_tolerance() {
        assert!(same_custom_gain(None, None));
        assert!(same_custom_gain(Some(1.5), Some(1.500_01)));
        assert!(!same_custom_gain(Some(1.5), Some(1.6)));
        assert!(!same_custom_gain(Some(1.5), None));
    }

    #[test]
    fn custom_gain_converts_to_a_linear_factor() {
        let mut record = AnalysisRecord::default();
        assert!((record.custom_gain_linear() - 1.0).abs() < f32::EPSILON);
        record.custom_gain_db = Some(-6.020_6);
        assert!((record.custom_gain_linear() - 0.5).abs() < 1e-4);
        assert!((decibels_to_gain(20.0) - 10.0).abs() < 1e-9);
    }

    #[test]
    fn samples_per_point_is_one_tenth_of_a_second() {
        assert_eq!(ShortTermLoudnessHistory::samples_per_point_for_rate(44_100), 4410);
        assert_eq!(ShortTermLoudnessHistory::samples_per_point_for_rate(48_000), 4800);
        assert_eq!(ShortTermLoudnessHistory::samples_per_point_for_rate(5), 1);
    }
}
