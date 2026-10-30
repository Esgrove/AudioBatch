//! Audio analysis of a single file: sample peak, true peak, and EBU R128 loudness.
//!
//! The file is decoded packet by packet into the `BlockMeter` of the `meter` module,
//! which measures the loudness, the peaks, and the waveform overview,
//! and the measurements are reduced to the left, right, and overall values of the record.

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::Path;
use std::sync::Arc;

use crate::decode::{AudioFileDecoder, DecodeError};
use crate::meter::{BlockMeter, MeterError};
use crate::overload::OverloadHistory;
use crate::record::{AnalysisError, AnalysisFailure, AnalysisRecord, AnalysisStatus, ShortTermLoudnessHistory};
use crate::waveform::WaveformOverview;

/// Error message of a cancelled analysis, which callers must not cache.
pub const CANCELLED_MESSAGE: &str = "Analysis was cancelled";

/// A decoding, cancellation, or loudness measurement failure.
#[derive(Debug, thiserror::Error)]
enum MeasurementError {
    /// The audio stream could not be decoded.
    #[error(transparent)]
    Decode(#[from] DecodeError),
    /// The analysis was cancelled.
    #[error(transparent)]
    Analysis(#[from] AnalysisFailure),
    /// The loudness meter failed.
    #[error(transparent)]
    Meter(#[from] MeterError),
}

impl MeasurementError {
    /// Returns the classification that determines whether a failed analysis can be cached.
    const fn kind(&self) -> AnalysisError {
        match self {
            Self::Decode(error) => error.analysis_kind(),
            Self::Analysis(failure) => failure.kind,
            Self::Meter(_) => AnalysisError::MeasurementFailed,
        }
    }
}

/// Left, right, and overall values reduced from per-channel values.
#[derive(Debug, Clone, Copy)]
struct ChannelSummary<T> {
    left: T,
    right: T,
    overall: T,
}

/// Measurements gathered while decoding a file.
struct DecodeMeasurements {
    format_name: String,
    sample_rate: u32,
    channels: u32,
    bits_per_sample: u32,
    length_in_samples: u64,
    signed_peaks: Vec<f32>,
    true_peaks: Vec<f64>,
    integrated_lufs: f64,
    max_short_term_lufs: f64,
    short_term_loudness: Arc<ShortTermLoudnessHistory>,
    waveform: Arc<WaveformOverview>,
    overloads: Arc<OverloadHistory>,
}

/// Analyzes a single audio file and returns the populated result record.
///
/// When `is_cancelled` starts returning true, decoding stops and a failed record is returned,
/// which callers must not cache.
/// The decoded fraction of the file is reported to `on_progress` while decoding,
/// when the container states the length.
pub fn analyze_file(path: &Path, is_cancelled: &dyn Fn() -> bool, on_progress: &mut dyn FnMut(f32)) -> AnalysisRecord {
    let _span = tracing::info_span!("analyze", file = %path.display()).entered();
    let mut record = AnalysisRecord::from_path(path);
    if !path.is_file() {
        return fail(record, AnalysisError::MissingFile, "File does not exist");
    }
    match measure_file(path, is_cancelled, on_progress) {
        Ok(measurements) => {
            let peaks = summarize(&measurements.signed_peaks, dominant_peak_f32);
            let true_peaks = summarize(&measurements.true_peaks, dominant_peak_f64);

            record.format_name = measurements.format_name;
            record.sample_rate = measurements.sample_rate;
            record.channels = measurements.channels;
            record.bits_per_sample = measurements.bits_per_sample;
            record.length_in_samples = measurements.length_in_samples;
            record.duration_seconds = if measurements.sample_rate > 0 {
                measurements.length_in_samples as f64 / f64::from(measurements.sample_rate)
            } else {
                0.0
            };
            record.peak_left = peaks.left;
            record.peak_right = peaks.right;
            record.overall_peak = peaks.overall;
            record.true_peak_left = true_peaks.left;
            record.true_peak_right = true_peaks.right;
            record.overall_true_peak = true_peaks.overall;
            record.max_short_term_lufs = measurements.max_short_term_lufs;
            record.integrated_lufs = measurements.integrated_lufs;
            record.short_term_loudness = Some(measurements.short_term_loudness);
            record.waveform = Some(measurements.waveform);
            record.overloads = Some(measurements.overloads);
            record.status = AnalysisStatus::Analyzed;
            record.from_cache = false;
            record
        }
        Err(error) => fail(record, error.kind(), &error.to_string()),
    }
}

/// Analyzes a single audio file like `analyze_file`,
/// and returns a failed record when a decoder panics on a malformed file.
///
/// Worker pool jobs analyze through this, since a panic in a pool job would otherwise abort the whole process,
/// and a panic while analyzing an output that was already committed must not read as the operation failing.
pub fn analyze_file_catching_panics(
    path: &Path,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> AnalysisRecord {
    catch_unwind(AssertUnwindSafe(|| analyze_file(path, is_cancelled, on_progress))).unwrap_or_else(|_| {
        fail(
            AnalysisRecord::from_path(path),
            AnalysisError::Panicked,
            "Analysis failed unexpectedly",
        )
    })
}

/// Picks the sample extreme with the larger magnitude, preserving its sign.
#[must_use]
pub(crate) fn signed_peak_from_extrema(minimum: f32, maximum: f32) -> f32 {
    if minimum.abs() > maximum.abs() {
        minimum
    } else {
        maximum
    }
}

/// Decodes the whole file block by block, measuring loudness, peaks, and the waveform.
/// Cancellation is checked before each packet and fails the measurement.
fn measure_file(
    path: &Path,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> Result<DecodeMeasurements, MeasurementError> {
    let mut decoder = AudioFileDecoder::open(path)?;
    let info = decoder.info().clone();
    let mut meter = BlockMeter::new(info.channels, info.sample_rate, info.expected_frames)?;

    loop {
        if is_cancelled() {
            return Err(AnalysisFailure::new(AnalysisError::Cancelled, CANCELLED_MESSAGE).into());
        }
        let Some(chunk) = decoder.next_chunk()? else {
            break;
        };
        meter.push(chunk);
        if let Some(expected) = info.expected_frames.filter(|&frames| frames > 0) {
            on_progress((decoder.position() as f64 / expected as f64).min(1.0) as f32);
        }
    }
    on_progress(1.0);

    let length_in_samples = decoder.position();
    let output = meter.finish(length_in_samples);
    let short_term_loudness = Arc::new(ShortTermLoudnessHistory {
        values: output.history,
        sample_rate: f64::from(info.sample_rate),
        length_in_samples,
        samples_per_point: output.samples_per_point,
    });

    Ok(DecodeMeasurements {
        format_name: info.format_name,
        sample_rate: info.sample_rate,
        channels: info.channels,
        bits_per_sample: info.bits_per_sample,
        length_in_samples,
        signed_peaks: output.signed_peaks,
        true_peaks: output.true_peaks,
        integrated_lufs: output.integrated_lufs,
        max_short_term_lufs: output.max_short_term_lufs,
        short_term_loudness,
        waveform: output.waveform,
        overloads: output.overloads,
    })
}

/// Marks the record as failed with the given kind and message.
fn fail(mut record: AnalysisRecord, kind: AnalysisError, message: &str) -> AnalysisRecord {
    record.status = AnalysisStatus::Failed;
    record.error = Some(kind);
    record.error_message = message.to_string();
    record
}

/// Returns whichever sample peak has the larger magnitude, keeping its sign.
const fn dominant_peak_f32(first: f32, second: f32) -> f32 {
    if first.abs() > second.abs() { first } else { second }
}

/// Returns whichever true peak has the larger magnitude, keeping its sign.
const fn dominant_peak_f64(first: f64, second: f64) -> f64 {
    if first.abs() > second.abs() { first } else { second }
}

/// Reduces per-channel values to the left, right, and overall value.
/// A mono source reports its only channel as both left and right.
fn summarize<T: Copy + Default>(values: &[T], dominant: fn(T, T) -> T) -> ChannelSummary<T> {
    let left = values.first().copied().unwrap_or_default();
    let right = values.get(1).copied().unwrap_or(left);
    let overall = values.iter().skip(1).copied().fold(left, dominant);
    ChannelSummary { left, right, overall }
}

#[cfg(test)]
mod test_fixture_levels {
    use crate::test_helpers::analyze;

    use test_fixtures::{FIXTURE_LOUDNESS, FIXTURE_PEAK_DECIBELS, TONE_FIXTURES};

    /// Returns an amplitude in decibels relative to full scale.
    fn decibels(amplitude: f64) -> f64 {
        20.0 * amplitude.abs().log10()
    }

    #[test]
    fn every_fixture_reads_the_tone_level() {
        for tone in TONE_FIXTURES {
            let record = analyze(&tone.path());
            let name = tone.file_name;
            assert!(!record.has_error(), "{name}: {}", record.error_message);
            assert_eq!(record.channels, tone.channels, "{name}");
            assert_eq!(record.sample_rate, tone.sample_rate, "{name}");
            assert_eq!(record.bits_per_sample, tone.bits_per_sample, "{name}");
            let tolerance = tone.tolerance();
            let peak = decibels(f64::from(record.overall_peak));
            assert!((peak - FIXTURE_PEAK_DECIBELS).abs() < tolerance, "{name}: peak {peak}");
            let true_peak = decibels(record.overall_true_peak);
            assert!(
                (true_peak - FIXTURE_PEAK_DECIBELS).abs() < tolerance,
                "{name}: true peak {true_peak}"
            );
            assert!(
                (record.integrated_lufs - FIXTURE_LOUDNESS).abs() < tolerance,
                "{name}: loudness {}",
                record.integrated_lufs
            );
            assert!(
                (record.duration_seconds - 1.0).abs() < 0.1,
                "{name}: {}",
                record.duration_seconds
            );
        }
    }

    #[test]
    fn each_measurement_is_reported_in_its_own_field() {
        let record = analyze(&test_fixtures::fixture(test_fixtures::INTERSAMPLE_FIXTURE));
        assert!(!record.has_error(), "{}", record.error_message);
        let tolerance = test_fixtures::LOSSLESS_TOLERANCE;
        let peak = decibels(f64::from(record.overall_peak));
        assert!(
            (peak - test_fixtures::INTERSAMPLE_PEAK_DECIBELS).abs() < tolerance,
            "peak {peak}"
        );
        let true_peak = decibels(record.overall_true_peak);
        assert!(
            (true_peak - test_fixtures::INTERSAMPLE_TRUE_PEAK_DECIBELS).abs() < tolerance,
            "true peak {true_peak}"
        );
        assert!(
            (record.integrated_lufs - test_fixtures::INTERSAMPLE_LOUDNESS).abs() < tolerance,
            "loudness {}",
            record.integrated_lufs
        );
    }

    #[test]
    fn history_has_one_reading_per_block() {
        for tone in TONE_FIXTURES {
            let record = analyze(&tone.path());
            let history = record.short_term_loudness.expect("history");
            let block = u64::from(history.samples_per_point);
            let expected = record.length_in_samples.div_ceil(block);
            assert_eq!(history.values.len() as u64, expected, "{}", tone.file_name);
            let last_time = history.time_at(history.values.len() - 1);
            assert!((last_time - record.duration_seconds).abs() < 1e-7, "{}", tone.file_name);
        }
    }

    #[test]
    fn short_term_history_of_stereo_wav() {
        let record = analyze(&test_fixtures::fixture("sine_stereo_44100_16.wav"));
        let history = record.short_term_loudness.expect("history");
        assert_eq!(history.values.len(), 10);
        assert!((history.time_at(0) - 0.1).abs() < 1e-6);
        assert!((history.time_at(9) - 1.0).abs() < 1e-6);
        let first = history.values.first().copied().unwrap_or_default();
        let last = history.values.last().copied().unwrap_or_default();
        assert!(first < last);
    }

    #[test]
    fn overload_history_matches_the_decoded_stream() {
        for tone in TONE_FIXTURES {
            let record = analyze(&tone.path());
            let overloads = record.overloads.as_ref().expect("source overload history");
            assert!(overloads.matches_stream(record.sample_rate, record.length_in_samples, record.channels));
            assert!(overloads.flags.iter().all(|&flags| flags == 0), "{}", tone.file_name);
        }
        let failed = analyze(&test_fixtures::fixture("broken.wav"));
        assert!(failed.overloads.is_none());
    }

    #[test]
    fn waveform_matches_the_tone() {
        let record = analyze(&test_fixtures::fixture("sine_stereo_44100_16.wav"));
        let waveform = record.waveform.expect("waveform");
        assert_eq!(waveform.channels(), 2);
        assert_eq!(waveform.length_in_frames(), 44_100);
        let columns = waveform.column_peaks(0, 0.0, 44_100.0, 50);
        assert!(columns.iter().all(|&(_, maximum)| (maximum - 0.5).abs() < 0.02));
    }
}

#[cfg(test)]
mod test_failures {
    use super::*;
    use crate::record::NEGATIVE_INFINITY_LOUDNESS;

    #[test]
    fn silence_has_no_peak_and_no_loudness() {
        let record = analyze_file(
            &test_fixtures::fixture("silence_stereo_44100_16.wav"),
            &|| false,
            &mut |_| {},
        );
        assert!(!record.has_error(), "{}", record.error_message);
        assert!(record.overall_peak.abs() < f32::EPSILON);
        assert!((record.integrated_lufs - NEGATIVE_INFINITY_LOUDNESS).abs() < f64::EPSILON);
        let history = record.short_term_loudness.expect("history");
        assert_eq!(history.values.len(), 10);
        assert!(
            history
                .values
                .iter()
                .all(|&value| (f64::from(value) - NEGATIVE_INFINITY_LOUDNESS).abs() < f64::EPSILON)
        );
    }

    #[test]
    fn broken_file_fails() {
        let record = analyze_file(&test_fixtures::fixture("broken.wav"), &|| false, &mut |_| {});
        assert!(record.has_error());
        assert_ne!(record.error_message, "");
        assert!(record.short_term_loudness.is_none());
    }

    #[test]
    fn missing_file_fails() {
        let record = analyze_file(Path::new("/does/not/exist.wav"), &|| false, &mut |_| {});
        assert!(record.has_error());
        assert_eq!(record.error_message, "File does not exist");
    }

    #[test]
    fn cancellation_fails_without_history() {
        let record = analyze_file(
            &test_fixtures::fixture("sine_stereo_44100_16.wav"),
            &|| true,
            &mut |_| {},
        );
        assert!(record.has_error());
        assert!(record.error_message.contains("cancelled"), "{}", record.error_message);
        assert!(record.short_term_loudness.is_none());
    }

    #[test]
    fn cancellation_during_decoding_fails_without_history() {
        let checks = std::cell::Cell::new(0);
        let record = analyze_file(
            &test_fixtures::fixture("sine_stereo_44100_16.flac"),
            &|| {
                checks.set(checks.get() + 1);
                checks.get() > 3
            },
            &mut |_| {},
        );
        assert!(checks.get() > 3);
        assert_eq!(record.error, Some(AnalysisError::Cancelled));
        assert_eq!(record.error_message, CANCELLED_MESSAGE);
        assert!(record.short_term_loudness.is_none() && record.waveform.is_none());
    }

    #[test]
    fn progress_is_reported_in_order_up_to_one() {
        let mut reported = Vec::new();
        let record = analyze_file(
            &test_fixtures::fixture("sine_stereo_44100_16.wav"),
            &|| false,
            &mut |progress| reported.push(progress),
        );
        assert!(!record.has_error());
        assert_ne!(reported, Vec::<f32>::new());
        assert!(reported.is_sorted());
        assert!(reported.last().is_some_and(|&last| (last - 1.0).abs() < f32::EPSILON));
    }
}

#[cfg(test)]
mod test_damaged_files {
    use super::*;

    use tempfile::TempDir;

    use crate::test_helpers::{analyze, half_fixture_copy, scrambled_fixture_copy, truncated_fixture_copy};

    /// Fixtures of the formats whose decoders read a file up to where it ends.
    const STREAMING_FIXTURES: [&str; 3] = [
        "sine_stereo_44100_16.wav",
        "sine_stereo_44100_16.flac",
        "sine_stereo_44100_320k.mp3",
    ];

    #[test]
    fn a_truncated_file_is_measured_up_to_where_it_ends() {
        let directory = TempDir::new().expect("temp dir");
        for fixture in STREAMING_FIXTURES {
            let record = analyze(&half_fixture_copy(&directory, fixture, &format!("half_{fixture}")));
            assert!(record.is_ready(), "{fixture}: {}", record.error_message);
            assert!(
                record.length_in_samples > 0 && record.length_in_samples < 44_100,
                "{fixture}: {}",
                record.length_in_samples
            );
            let peak = 20.0 * f64::from(record.overall_peak.abs()).log10();
            assert!(
                (peak - test_fixtures::FIXTURE_PEAK_DECIBELS).abs() < test_fixtures::LOSSY_TOLERANCE,
                "{fixture}: peak {peak}"
            );
        }
    }

    #[test]
    fn a_file_cut_inside_its_header_is_unsupported() {
        let directory = TempDir::new().expect("temp dir");
        for tone in test_fixtures::TONE_FIXTURES {
            let name = format!("header_{}", tone.file_name);
            let record = analyze(&truncated_fixture_copy(&directory, tone.file_name, &name, 30));
            assert_eq!(record.error, Some(AnalysisError::Unsupported), "{}", tone.file_name);
            assert_eq!(record.error_message, "Unsupported or unreadable audio file");
            assert!(record.short_term_loudness.is_none() && record.waveform.is_none());
        }
    }

    #[test]
    fn an_mp4_file_cut_before_its_index_is_unsupported() {
        let directory = TempDir::new().expect("temp dir");
        let path = half_fixture_copy(&directory, test_fixtures::AAC_FIXTURE, "half.m4a");
        let record = analyze(&path);
        assert_eq!(record.error, Some(AnalysisError::Unsupported));
    }

    #[test]
    fn damaged_packets_end_the_measurement_early() {
        let directory = TempDir::new().expect("temp dir");
        for fixture in ["sine_stereo_44100_16.flac", "sine_stereo_44100_320k.mp3"] {
            let record = analyze(&scrambled_fixture_copy(
                &directory,
                fixture,
                &format!("scrambled_{fixture}"),
            ));
            assert!(record.is_ready(), "{fixture}: {}", record.error_message);
            assert!(
                record.length_in_samples > 0 && record.length_in_samples < 44_100,
                "{fixture}: {}",
                record.length_in_samples
            );
        }
    }

    #[test]
    fn an_unreadable_codec_packet_fails_the_analysis_without_a_repeatable_error() {
        let directory = TempDir::new().expect("temp dir");
        let path = scrambled_fixture_copy(&directory, test_fixtures::AAC_FIXTURE, "scrambled.m4a");
        let record = analyze(&path);
        assert!(record.has_error());
        assert!(
            record.error_message.starts_with("Audio decoding failed at sample"),
            "{}",
            record.error_message
        );
        assert!(
            !record.error.is_some_and(AnalysisError::is_repeatable),
            "{:?}",
            record.error
        );
        assert!(record.short_term_loudness.is_none());
    }
}

#[cfg(test)]
mod test_peaks {
    use super::*;

    #[test]
    fn signed_peak_keeps_the_larger_magnitude() {
        assert!((signed_peak_from_extrema(-0.8, 0.5) + 0.8).abs() < f32::EPSILON);
        assert!((signed_peak_from_extrema(-0.2, 0.5) - 0.5).abs() < f32::EPSILON);
    }

    #[test]
    fn mono_reports_its_channel_as_left_and_right() {
        let summary = summarize(&[0.25_f32], dominant_peak_f32);
        assert!((summary.left - 0.25).abs() < f32::EPSILON);
        assert!((summary.right - 0.25).abs() < f32::EPSILON);
        assert!((summary.overall - 0.25).abs() < f32::EPSILON);
    }

    #[test]
    fn overall_is_the_dominant_channel() {
        let summary = summarize(&[0.25_f32, -0.75], dominant_peak_f32);
        assert!((summary.overall + 0.75).abs() < f32::EPSILON);
    }
}
