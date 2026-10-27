//! Audio analysis of a single file: sample peak, true peak, and EBU R128 loudness.
//!
//! The decoded packets are regrouped into fixed blocks of one tenth of a second,
//! one block per short-term loudness update as EBU Tech 3341 asks for,
//! and each block feeds the ebur128 meter, the per-channel sample extremes, and the waveform overview.
//! A mono source is measured as dual mono, so it reads the same loudness as the same content in stereo.

use std::path::Path;
use std::sync::Arc;

use anyhow::{Context, Result};
use ebur128::{Channel, EbuR128, Mode};

use crate::decode::AudioFileDecoder;
use crate::record::{
    AnalysisError, AnalysisFailure, AnalysisRecord, AnalysisStatus, NEGATIVE_INFINITY_LOUDNESS,
    ShortTermLoudnessHistory, normalize_loudness,
};
use crate::waveform::{WaveformBuilder, WaveformOverview};

/// Error message of a cancelled analysis, which callers must not cache.
pub const CANCELLED_MESSAGE: &str = "Analysis was cancelled";

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
    min_samples: Vec<f32>,
    max_samples: Vec<f32>,
    true_peaks: Vec<f64>,
    integrated_lufs: f64,
    max_short_term_lufs: f64,
    short_term_loudness: Arc<ShortTermLoudnessHistory>,
    waveform: Arc<WaveformOverview>,
}

/// Accumulates decoded audio into fixed-size analysis blocks and measures each completed block.
struct BlockMeter {
    meter: EbuR128,
    block_size: usize,
    block: Vec<Vec<f32>>,
    min_samples: Vec<f32>,
    max_samples: Vec<f32>,
    max_short_term_lufs: f64,
    history: Vec<f32>,
    waveform: WaveformBuilder,
}

impl BlockMeter {
    /// Creates a meter for the stream, with integrated, short-term, and true peak measurement.
    fn new(channels: u32, sample_rate: u32, expected_frames: Option<u64>) -> Result<Self> {
        let mut meter = EbuR128::new(channels, sample_rate, Mode::I | Mode::S | Mode::TRUE_PEAK)
            .context("Could not initialize loudness analyzer")?;
        if channels == 1 {
            meter
                .set_channel(0, Channel::DualMono)
                .context("Could not initialize loudness analyzer")?;
        }
        let channel_count = channels as usize;
        let block_size = ShortTermLoudnessHistory::samples_per_point_for_rate(sample_rate) as usize;
        Ok(Self {
            meter,
            block_size,
            block: vec![Vec::with_capacity(block_size); channel_count],
            min_samples: vec![0.0; channel_count],
            max_samples: vec![0.0; channel_count],
            max_short_term_lufs: NEGATIVE_INFINITY_LOUDNESS,
            history: Vec::new(),
            waveform: WaveformBuilder::new(channel_count, expected_frames),
        })
    }

    /// Appends decoded samples, measuring every block that fills up.
    fn push(&mut self, chunk: &[Vec<f32>]) -> Result<()> {
        let frame_count = chunk.first().map_or(0, Vec::len);
        let mut offset = 0;
        while offset < frame_count {
            let filled = self.block.first().map_or(0, Vec::len);
            let take = (self.block_size - filled).min(frame_count - offset);
            for (block_channel, samples) in self.block.iter_mut().zip(chunk) {
                if let Some(segment) = samples.get(offset..offset + take) {
                    block_channel.extend_from_slice(segment);
                } else {
                    block_channel.resize(block_channel.len() + take, 0.0);
                }
            }
            offset += take;
            if filled + take == self.block_size {
                self.measure_block()?;
            }
        }
        Ok(())
    }

    /// Measures a final partial block and returns the history.
    fn finish(mut self) -> Result<Self> {
        if self.block.first().is_some_and(|samples| !samples.is_empty()) {
            self.measure_block()?;
        }
        Ok(self)
    }

    /// Feeds the buffered block to the meter, the sample extremes, and the waveform, then empties it.
    fn measure_block(&mut self) -> Result<()> {
        for ((samples, minimum), maximum) in self.block.iter().zip(&mut self.min_samples).zip(&mut self.max_samples) {
            for &sample in samples {
                *minimum = minimum.min(sample);
                *maximum = maximum.max(sample);
            }
        }
        let planes: Vec<&[f32]> = self.block.iter().map(Vec::as_slice).collect();
        self.waveform.push(&planes);
        self.meter
            .add_frames_planar_f32(&planes)
            .context("Loudness analysis failed while processing audio")?;
        let short_term = normalize_loudness(
            self.meter
                .loudness_shortterm()
                .context("Short-term loudness analysis failed")?,
        );
        self.max_short_term_lufs = self.max_short_term_lufs.max(short_term);
        self.history.push(short_term as f32);
        for samples in &mut self.block {
            samples.clear();
        }
        Ok(())
    }
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
            let signed_peaks: Vec<f32> = measurements
                .min_samples
                .iter()
                .zip(&measurements.max_samples)
                .map(|(&minimum, &maximum)| signed_peak_from_extrema(minimum, maximum))
                .collect();
            let peaks = summarize(&signed_peaks, dominant_peak_f32);
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
            record.status = AnalysisStatus::Analyzed;
            record.from_cache = false;
            record
        }
        Err(error) => {
            // Failures without a kind come from the measurement, which a later run may get past.
            let kind = error
                .downcast_ref::<AnalysisFailure>()
                .map_or(AnalysisError::MeasurementFailed, |failure| failure.kind);
            fail(record, kind, &error.to_string())
        }
    }
}

/// Picks the sample extreme with the larger magnitude, preserving its sign.
#[must_use]
pub fn signed_peak_from_extrema(minimum: f32, maximum: f32) -> f32 {
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
) -> Result<DecodeMeasurements> {
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
        meter.push(chunk)?;
        if let Some(expected) = info.expected_frames.filter(|&frames| frames > 0) {
            on_progress((decoder.position() as f64 / expected as f64).min(1.0) as f32);
        }
    }
    on_progress(1.0);

    let length_in_samples = decoder.position();
    let meter = meter.finish()?;
    let true_peaks = (0..info.channels)
        .map(|channel| meter.meter.true_peak(channel))
        .collect::<Result<Vec<f64>, _>>()
        .context("True peak analysis failed")?;
    let integrated_lufs = normalize_loudness(
        meter
            .meter
            .loudness_global()
            .context("Integrated loudness analysis failed")?,
    );

    let short_term_loudness = Arc::new(ShortTermLoudnessHistory {
        values: meter.history,
        sample_rate: f64::from(info.sample_rate),
        length_in_samples,
        samples_per_point: meter.block_size as u32,
    });

    Ok(DecodeMeasurements {
        format_name: info.format_name,
        sample_rate: info.sample_rate,
        channels: info.channels,
        bits_per_sample: info.bits_per_sample,
        length_in_samples,
        min_samples: meter.min_samples,
        max_samples: meter.max_samples,
        true_peaks,
        integrated_lufs,
        max_short_term_lufs: meter.max_short_term_lufs,
        short_term_loudness,
        waveform: meter.waveform.finish(info.sample_rate, length_in_samples),
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
    use super::*;

    use test_fixtures::{FIXTURE_LOUDNESS, FIXTURE_PEAK_DECIBELS, TONE_FIXTURES};

    fn decibels(amplitude: f64) -> f64 {
        20.0 * amplitude.abs().log10()
    }

    fn analyze(path: &Path) -> AnalysisRecord {
        analyze_file(path, &|| false, &mut |_| {})
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
