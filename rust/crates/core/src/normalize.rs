//! Normalization of analyzed files to a sample peak or loudness target, written as AIFF.
//!
//! The gain comes from the analyzed levels and is refused rather than capped when it would exceed 40 dB.
//! Every file is written as a sibling `<name>.aif` at the source sample rate,
//! with the source bit depth when AIFF supports it, so 32-bit float sources are written as 24-bit.
//! The audio is rendered into a hidden temporary file, the tags are copied over, the result is validated,
//! and only then is it committed, so a failure or a cancellation leaves the original untouched.
//! A converted source is disposed of after the output is in place,
//! and an `.aif` source rewritten in place has a copy of it disposed of first,
//! both through the disposer in the options, which moves files to the system trash by default.
//! No dither is applied, so `scripts/compare_outputs.py` can expect the same samples as the C++ application writes.

use std::collections::HashSet;
use std::fmt;
use std::fs::File;
use std::io::BufWriter;
use std::path::{Path, PathBuf};

use crate::aiff::{AiffWriter, SUPPORTED_BIT_DEPTHS};
use crate::analysis::analyze_file;
use crate::decode::{AudioFileDecoder, DEFAULT_LOSSY_BITS_PER_SAMPLE, is_mp3_path};
use crate::format::gain_to_decibels;
use crate::metadata::copy_metadata;
use crate::operation::{OperationOutcome, OperationResult, progress_range};
use crate::output_file::{
    FileDisposer, check_output_path, commit_temporary_output, create_temporary_output, dispose_of_copy, trash_disposer,
    validate_written_audio,
};
use crate::record::{AnalysisRecord, NEGATIVE_INFINITY_LOUDNESS};

/// Lowest sample peak target, in dBFS.
pub const MINIMUM_TARGET_PEAK_DBFS: f64 = -12.0;

/// Highest sample peak target, in dBFS.
pub const MAXIMUM_TARGET_PEAK_DBFS: f64 = 0.0;

/// Default sample peak target, in dBFS.
pub const DEFAULT_TARGET_PEAK_DBFS: f64 = 0.0;

/// Lowest integrated loudness target, in LUFS.
pub const MINIMUM_TARGET_LUFS: f64 = -24.0;

/// Highest integrated loudness target, in LUFS.
pub const MAXIMUM_TARGET_LUFS: f64 = -6.0;

/// Default integrated loudness target, in LUFS.
pub const DEFAULT_TARGET_LUFS: f64 = -14.0;

/// True peak ceiling that limits the gain of loudness normalization, in dBTP.
pub const DEFAULT_TRUE_PEAK_CEILING_DBTP: f64 = -0.3;

/// Largest gain normalization applies, in dB.
pub const MAXIMUM_GAIN_DB: f64 = 40.0;

/// Error message of a cancelled normalization.
pub const CANCELLED_MESSAGE: &str = "Normalization was cancelled";

/// Extension of the normalized output file.
const OUTPUT_EXTENSION: &str = "aif";

/// Floor for decibel conversions, low enough not to hide how quiet a nearly silent file is.
const DECIBEL_FLOOR: f64 = -1000.0;

/// Share of a file's progress taken by analyzing a source that changed since its last analysis.
const SOURCE_ANALYSIS_PROGRESS_SHARE: f32 = 0.3;

/// Progress at which rendering ends and analyzing the output begins.
const RENDER_PROGRESS_END: f32 = 0.8;

/// The level normalization brings a file to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum NormalizationMode {
    /// Bring the sample peak to the target in dBFS.
    #[default]
    Peak,
    /// Bring the integrated loudness to the target in LUFS, limited by the true peak ceiling.
    Loudness,
}

/// Settings for normalizing files.
#[derive(Clone)]
pub struct NormalizationOptions {
    /// Which level to normalize.
    pub mode: NormalizationMode,
    /// Sample peak target in dBFS, for peak mode.
    pub target_peak_dbfs: f64,
    /// Integrated loudness target in LUFS, for loudness mode.
    pub target_lufs: f64,
    /// True peak ceiling in dBTP, for loudness mode.
    pub true_peak_ceiling_dbtp: f64,
    /// Replace an existing output file that is not the source.
    pub overwrite_existing: bool,
    /// Disposes of the files that normalization replaces.
    pub dispose: FileDisposer,
}

/// What rendering wrote, for validating the output.
struct RenderedAudio {
    channels: u32,
    sample_rate: u32,
    frames: u64,
    notice: Option<String>,
}

impl Default for NormalizationOptions {
    fn default() -> Self {
        Self {
            mode: NormalizationMode::Peak,
            target_peak_dbfs: DEFAULT_TARGET_PEAK_DBFS,
            target_lufs: DEFAULT_TARGET_LUFS,
            true_peak_ceiling_dbtp: DEFAULT_TRUE_PEAK_CEILING_DBTP,
            overwrite_existing: false,
            dispose: trash_disposer(),
        }
    }
}

impl fmt::Debug for NormalizationOptions {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("NormalizationOptions")
            .field("mode", &self.mode)
            .field("target_peak_dbfs", &self.target_peak_dbfs)
            .field("target_lufs", &self.target_lufs)
            .field("true_peak_ceiling_dbtp", &self.true_peak_ceiling_dbtp)
            .field("overwrite_existing", &self.overwrite_existing)
            .finish_non_exhaustive()
    }
}

impl RenderedAudio {
    /// Checks that the written file reads back as what was rendered.
    fn validate(&self, path: &Path) -> Result<(), String> {
        validate_written_audio(path, self.channels, self.sample_rate, self.frames)
    }
}

/// Returns the output path for the source, a sibling `<name>.aif`.
///
/// A source that already is that file, compared without case where the file system ignores it,
/// is returned unchanged, so it is rewritten in place.
#[must_use]
pub fn normalization_output_path(source: &Path) -> PathBuf {
    let output = source.with_extension(OUTPUT_EXTENSION);
    if same_path(&output, source) {
        source.to_path_buf()
    } else {
        output
    }
}

/// Returns the sources that cannot be normalized in this batch,
/// because another source in the batch writes the same output file.
///
/// A source that already is its output, such as `<name>.aif` normalized in place, always wins.
/// Otherwise the source that sorts first by path wins, so the result does not depend on input order.
/// A source listed more than once does not conflict with itself.
#[must_use]
pub fn find_batch_output_conflicts(records: &[AnalysisRecord]) -> HashSet<PathBuf> {
    let mut ordered: Vec<&Path> = records.iter().map(|record| record.path.as_path()).collect();
    ordered.sort_by(|first, second| {
        let first_in_place = normalization_output_path(first) == *first;
        let second_in_place = normalization_output_path(second) == *second;
        second_in_place.cmp(&first_in_place).then_with(|| first.cmp(second))
    });
    ordered.dedup();
    let mut claimed: HashSet<String> = HashSet::new();
    let mut conflicts = HashSet::new();
    for source in ordered {
        if !claimed.insert(path_key(&normalization_output_path(source))) {
            conflicts.insert(source.to_path_buf());
        }
    }
    conflicts
}

/// Returns the user-facing error for a source that lost a batch output conflict.
#[must_use]
pub fn batch_output_conflict_message(source: &Path) -> String {
    let output = normalization_output_path(source);
    let name = output
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    format!("Another selected file is also written to {name}")
}

/// Returns the linear gain that brings the analyzed file to the target in the options,
/// or the user-facing reason when the file has no measurable level for that mode.
///
/// A gain above `MAXIMUM_GAIN_DB`, or one that is not finite, is refused instead of capped,
/// since a capped gain would leave the file short of the target without telling the caller.
///
/// # Errors
///
/// Returns the user-facing reason when the file cannot be normalized.
pub fn normalization_gain(record: &AnalysisRecord, options: &NormalizationOptions) -> Result<f32, String> {
    let peak = f64::from(record.overall_peak.abs());
    if !peak.is_finite() || peak <= 0.0 {
        return Err("File contains no signal that can be normalized".to_string());
    }
    let gain_decibels = match options.mode {
        NormalizationMode::Peak => options.target_peak_dbfs - gain_to_decibels(peak, DECIBEL_FLOOR),
        NormalizationMode::Loudness => {
            if record.integrated_lufs <= NEGATIVE_INFINITY_LOUDNESS {
                return Err("File has no measurable loudness to normalize".to_string());
            }
            let gain = options.target_lufs - record.integrated_lufs;
            // The true peak scales with the gain, so the ceiling caps how far the file can be raised.
            let true_peak = record.overall_true_peak.max(peak);
            if true_peak > 0.0 {
                gain.min(options.true_peak_ceiling_dbtp - gain_to_decibels(true_peak, DECIBEL_FLOOR))
            } else {
                gain
            }
        }
    };
    if !gain_decibels.is_finite() || gain_decibels > MAXIMUM_GAIN_DB {
        return Err(format!(
            "Reaching the target would need more than {MAXIMUM_GAIN_DB:.0} dB of gain"
        ));
    }
    let gain = if gain_decibels > DECIBEL_FLOOR {
        10_f64.powf(gain_decibels * 0.05)
    } else {
        0.0
    };
    Ok(gain as f32)
}

/// Returns the AIFF bit depth for output written from the analyzed source.
///
/// Keeps the source depth when AIFF supports it, uses 16 bits for MP3 sources,
/// and otherwise falls back to the highest supported depth so no precision is lost.
#[must_use]
pub fn output_bit_depth(record: &AnalysisRecord) -> u32 {
    let preferred = if record.bits_per_sample > 0 {
        record.bits_per_sample
    } else if is_mp3_path(&record.path) {
        DEFAULT_LOSSY_BITS_PER_SAMPLE
    } else {
        0
    };
    if SUPPORTED_BIT_DEPTHS.contains(&preferred) {
        preferred
    } else {
        SUPPORTED_BIT_DEPTHS.iter().copied().max().unwrap_or(24)
    }
}

/// Rewrites a file to the target level in the options, then analyzes the result.
///
/// Fails without writing anything when the output path already exists and is not the source,
/// unless `overwrite_existing` is set, in which case the existing output is disposed of.
/// When the file changed on disk since it was analyzed, it is analyzed again before computing the gain.
/// A call cancelled before its output is committed fails and leaves the original untouched.
/// Source packets that fail to decode in isolation are written as silence and reported in the notice.
/// Progress of the whole file, through any source analysis, the render, and the output analysis,
/// is reported to `on_progress`.
pub fn normalize_file(
    source_record: &AnalysisRecord,
    options: &NormalizationOptions,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> OperationResult {
    let source = source_record.path.as_path();
    let output = normalization_output_path(source);
    if !source.is_file() {
        return fail(source, "File does not exist");
    }
    if let Err(error) = check_output_path(source, &output, options.overwrite_existing) {
        return fail(source, error);
    }
    if !source_record.is_ready() {
        return fail(source, "File analysis must finish before normalization");
    }

    let source_changed = !source_record.matches_file_on_disk();
    let render_start = if source_changed {
        SOURCE_ANALYSIS_PROGRESS_SHARE
    } else {
        0.0
    };
    let record = if source_changed {
        analyze_file(source, is_cancelled, &mut |fraction| {
            on_progress(fraction * render_start);
        })
    } else {
        source_record.clone()
    };
    if record.has_error() {
        return fail(source, record.error_message);
    }

    let gain = match normalization_gain(&record, options) {
        Ok(gain) => gain,
        Err(error) => return fail(source, error),
    };

    if approximately_equal(gain, 1.0) && output == source {
        let analysis = analyze_file(
            source,
            is_cancelled,
            &mut progress_range(on_progress, render_start, 1.0),
        );
        if analysis.has_error() {
            return fail(source, analysis.error_message);
        }
        return succeed(source, analysis, None);
    }

    let temporary = match create_temporary_output(&output) {
        Ok(temporary) => temporary,
        Err(error) => return fail(source, error.to_string()),
    };
    let rendered = render_normalized_audio(
        &record,
        temporary.as_file(),
        gain,
        is_cancelled,
        &mut progress_range(on_progress, render_start, RENDER_PROGRESS_END),
    );
    let rendered = match rendered {
        Ok(rendered) => rendered,
        Err(error) => return fail(source, error),
    };

    if let Err(error) = copy_metadata(source, temporary.path()) {
        eprintln!("{error:#}");
        return fail(source, "Could not preserve metadata while writing normalized audio");
    }
    if let Err(error) = rendered.validate(temporary.path()) {
        return fail(
            source,
            format!("Normalized output failed validation: {error}. The original file was left unchanged."),
        );
    }

    // Checked again before touching the original, since cancellation may arrive during the last block.
    if is_cancelled() {
        return fail(source, CANCELLED_MESSAGE);
    }
    if output == source && !dispose_of_copy(source, &options.dispose) {
        return fail(source, "Could not move a copy of the original file to the system trash");
    }
    if let Err(error) =
        commit_temporary_output(temporary, source, &output, options.overwrite_existing, &options.dispose)
    {
        return fail(source, error);
    }

    let analysis = analyze_file(
        &output,
        &|| false,
        &mut progress_range(on_progress, RENDER_PROGRESS_END, 1.0),
    );
    if analysis.has_error() {
        let error = format!(
            "The file was normalized, but re-analysis failed: {}",
            analysis.error_message
        );
        return OperationResult {
            source: source.to_path_buf(),
            outcome: OperationOutcome::CommittedButUnanalyzed {
                output: analysis,
                error,
            },
        };
    }
    succeed(source, analysis, rendered.notice)
}

/// Streams the source through the AIFF writer packet by packet, applying the gain.
///
/// Returns the user-facing error on failure, including when cancellation is requested between packets.
fn render_normalized_audio(
    record: &AnalysisRecord,
    file: &File,
    gain: f32,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> Result<RenderedAudio, String> {
    let mut decoder =
        AudioFileDecoder::open(&record.path).map_err(|_| "Unsupported or unreadable audio file".to_string())?;
    let channels = decoder.info().channels;
    let sample_rate = decoder.info().sample_rate;
    let mut writer = AiffWriter::new(BufWriter::new(file), channels, sample_rate, output_bit_depth(record))
        .map_err(|error| error.to_string())?;
    let total_frames = record.length_in_samples.max(1);
    let mut scaled: Vec<Vec<f32>> = vec![Vec::new(); channels as usize];
    loop {
        if is_cancelled() {
            return Err(CANCELLED_MESSAGE.to_string());
        }
        let Some(chunk) = decoder.next_chunk().map_err(|error| error.to_string())? else {
            break;
        };
        for (target, samples) in scaled.iter_mut().zip(chunk) {
            target.clear();
            target.extend(samples.iter().map(|sample| sample * gain));
        }
        let planes: Vec<&[f32]> = scaled.iter().map(Vec::as_slice).collect();
        writer
            .write_planar(&planes)
            .map_err(|_| "Failed while writing normalized audio data".to_string())?;
        on_progress((writer.frames_written() as f64 / total_frames as f64).min(1.0) as f32);
    }
    let frames = writer.frames_written();
    writer
        .finish()
        .map_err(|_| "Failed while writing normalized audio data".to_string())?;
    Ok(RenderedAudio {
        channels,
        sample_rate,
        frames,
        notice: decoder.failures().notice(),
    })
}

/// Builds a failed result, which the caller reports.
fn fail(source: &Path, error: impl Into<String>) -> OperationResult {
    OperationResult::failure(source, error)
}

/// Builds a successful result.
fn succeed(source: &Path, output: AnalysisRecord, notice: Option<String>) -> OperationResult {
    OperationResult::success(source, output, notice)
}

/// Returns true when two gains differ by no more than float rounding.
fn approximately_equal(first: f32, second: f32) -> bool {
    let difference = (first - second).abs();
    difference <= f32::MIN_POSITIVE || difference <= f32::EPSILON * first.abs().max(second.abs())
}

/// Returns true when two paths name the same file, ignoring case where the file system usually does.
fn same_path(first: &Path, second: &Path) -> bool {
    path_key(first) == path_key(second)
}

/// Returns the path as a comparison key, lowercased on platforms whose file systems usually ignore case.
fn path_key(path: &Path) -> String {
    let text = path.to_string_lossy();
    if cfg!(any(target_os = "macos", target_os = "windows")) {
        text.to_lowercase()
    } else {
        text.into_owned()
    }
}

#[cfg(test)]
mod test_normalization_gain {
    use super::*;
    use crate::record::AnalysisStatus;

    /// Builds an analyzed record with the given peak, true peak, and loudness in decibels.
    fn record(peak_decibels: f64, true_peak_decibels: f64, loudness: f64) -> AnalysisRecord {
        AnalysisRecord {
            overall_peak: 10_f64.powf(peak_decibels / 20.0) as f32,
            overall_true_peak: 10_f64.powf(true_peak_decibels / 20.0),
            integrated_lufs: loudness,
            status: AnalysisStatus::Analyzed,
            ..AnalysisRecord::default()
        }
    }

    /// Converts a linear gain to decibels.
    fn decibels(gain: f32) -> f64 {
        20.0 * f64::from(gain).log10()
    }

    #[test]
    fn peak_mode_brings_the_peak_to_the_target() {
        let gain = normalization_gain(&record(-6.0, -6.0, -10.0), &NormalizationOptions::default()).expect("gain");
        assert!((decibels(gain) - 6.0).abs() < 1e-4, "{}", decibels(gain));

        let options = NormalizationOptions {
            target_peak_dbfs: -1.0,
            ..NormalizationOptions::default()
        };
        let gain = normalization_gain(&record(-6.0, -6.0, -10.0), &options).expect("gain");
        assert!((decibels(gain) - 5.0).abs() < 1e-4, "{}", decibels(gain));
    }

    #[test]
    fn loudness_mode_brings_the_loudness_to_the_target() {
        let options = NormalizationOptions {
            mode: NormalizationMode::Loudness,
            ..NormalizationOptions::default()
        };
        let gain = normalization_gain(&record(-6.0, -6.0, -10.0), &options).expect("gain");
        assert!((decibels(gain) + 4.0).abs() < 1e-4, "{}", decibels(gain));
    }

    #[test]
    fn true_peak_ceiling_limits_the_loudness_gain() {
        let options = NormalizationOptions {
            mode: NormalizationMode::Loudness,
            ..NormalizationOptions::default()
        };
        let gain = normalization_gain(&record(-2.0, -1.0, -20.0), &options).expect("gain");
        assert!((decibels(gain) - 0.7).abs() < 1e-4, "{}", decibels(gain));
    }

    #[test]
    fn silent_file_is_refused() {
        let silent = AnalysisRecord {
            status: AnalysisStatus::Analyzed,
            integrated_lufs: NEGATIVE_INFINITY_LOUDNESS,
            ..AnalysisRecord::default()
        };
        assert_eq!(
            normalization_gain(&silent, &NormalizationOptions::default()),
            Err("File contains no signal that can be normalized".to_string())
        );
        let options = NormalizationOptions {
            mode: NormalizationMode::Loudness,
            ..NormalizationOptions::default()
        };
        assert_eq!(
            normalization_gain(&record(-60.0, -60.0, NEGATIVE_INFINITY_LOUDNESS), &options),
            Err("File has no measurable loudness to normalize".to_string())
        );
    }

    #[test]
    fn gain_above_the_maximum_is_refused() {
        assert_eq!(
            normalization_gain(&record(-41.0, -41.0, -50.0), &NormalizationOptions::default()),
            Err("Reaching the target would need more than 40 dB of gain".to_string())
        );
    }
}

#[cfg(test)]
mod test_output_files {
    use super::*;

    /// Builds a record for the path.
    fn record(path: &str) -> AnalysisRecord {
        record_with_depth(path, 0)
    }

    /// Builds a record with the path and bit depth.
    fn record_with_depth(path: &str, bits_per_sample: u32) -> AnalysisRecord {
        AnalysisRecord {
            path: PathBuf::from(path),
            bits_per_sample,
            ..AnalysisRecord::default()
        }
    }

    #[test]
    fn output_is_a_sibling_aif() {
        assert_eq!(
            normalization_output_path(Path::new("/music/song.wav")),
            PathBuf::from("/music/song.aif")
        );
        assert_eq!(
            normalization_output_path(Path::new("/music/song.aif")),
            PathBuf::from("/music/song.aif")
        );
        assert_eq!(
            normalization_output_path(Path::new("/music/song.aiff")),
            PathBuf::from("/music/song.aif")
        );
    }

    #[test]
    fn in_place_source_wins_a_conflict() {
        let records = [
            record("/music/song.wav"),
            record("/music/song.aif"),
            record("/music/song.flac"),
        ];
        let conflicts = find_batch_output_conflicts(&records);
        assert_eq!(
            conflicts,
            HashSet::from([PathBuf::from("/music/song.wav"), PathBuf::from("/music/song.flac")])
        );
    }

    #[test]
    fn first_path_wins_without_an_in_place_source() {
        let records = [record("/music/song.wav"), record("/music/song.flac")];
        assert_eq!(
            find_batch_output_conflicts(&records),
            HashSet::from([PathBuf::from("/music/song.wav")])
        );
    }

    #[test]
    fn repeated_source_does_not_conflict_with_itself() {
        let records = [record("/music/song.wav"), record("/music/song.wav")];
        assert!(find_batch_output_conflicts(&records).is_empty());
    }

    #[test]
    fn conflict_message_names_the_output() {
        assert_eq!(
            batch_output_conflict_message(Path::new("/music/song.wav")),
            "Another selected file is also written to song.aif"
        );
    }

    #[test]
    fn keeps_supported_depths_and_falls_back_to_24() {
        assert_eq!(output_bit_depth(&record_with_depth("a.wav", 16)), 16);
        assert_eq!(output_bit_depth(&record_with_depth("a.wav", 24)), 24);
        assert_eq!(output_bit_depth(&record_with_depth("a.wav", 32)), 24);
        assert_eq!(output_bit_depth(&record_with_depth("a.mp3", 0)), 16);
        assert_eq!(output_bit_depth(&record_with_depth("a.m4a", 0)), 24);
    }
}

#[cfg(test)]
mod test_normalize_file {
    use std::fs;
    use std::sync::{Arc, Mutex};

    use super::*;
    use crate::record::AnalysisStatus;
    use crate::tags::read_tags;
    use crate::test_helpers::ready_fixture_copy;

    /// Files disposed of during a test, by file name.
    type Disposed = Arc<Mutex<Vec<String>>>;

    /// Options with a disposer that deletes files and records their names.
    fn options(mode: NormalizationMode) -> (NormalizationOptions, Disposed) {
        let disposed: Disposed = Arc::new(Mutex::new(Vec::new()));
        let recorded = Arc::clone(&disposed);
        let options = NormalizationOptions {
            mode,
            dispose: Arc::new(move |path: &Path| {
                recorded
                    .lock()
                    .expect("lock")
                    .push(path.file_name().expect("name").to_string_lossy().into_owned());
                fs::remove_file(path).is_ok()
            }),
            ..NormalizationOptions::default()
        };
        (options, disposed)
    }

    /// Normalizes without cancellation or progress.
    fn normalize(record: &AnalysisRecord, options: &NormalizationOptions) -> OperationResult {
        normalize_file(record, options, &|| false, &mut |_| {})
    }

    /// Returns the names of the files in the directory, sorted.
    fn file_names(directory: &tempfile::TempDir) -> Vec<String> {
        let mut names: Vec<String> = fs::read_dir(directory.path())
            .expect("lists")
            .filter_map(|entry| entry.ok().map(|entry| entry.file_name().to_string_lossy().into_owned()))
            .collect();
        names.sort();
        names
    }

    #[test]
    fn peak_normalizes_a_wav_file_to_aiff() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let (options, disposed) = options(NormalizationMode::Peak);

        let result = normalize(&record, &options);

        assert!(result.error().is_none(), "{:?}", result.error());
        let output = result.output().expect("has output");
        assert_eq!(output.path, directory.path().join("song.aif"));
        assert_eq!(output.format_name, "AIFF file");
        assert_eq!(output.channels, 2);
        assert_eq!(output.sample_rate, 44_100);
        assert_eq!(output.bits_per_sample, 16);
        assert_eq!(output.length_in_samples, record.length_in_samples);
        let peak = gain_to_decibels(f64::from(output.overall_peak.abs()), DECIBEL_FLOOR);
        assert!(peak.abs() < 0.01, "{peak}");
        assert_eq!(*disposed.lock().expect("lock"), vec!["song.wav".to_string()]);
        assert_eq!(file_names(&directory), vec!["song.aif".to_string()]);
    }

    #[test]
    fn loudness_normalizes_to_the_target() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_48000_24.flac", "song.flac");
        let (options, _) = options(NormalizationMode::Loudness);

        let result = normalize(&record, &options);

        let output = result.output().expect("has output");
        assert_eq!(output.bits_per_sample, 24);
        assert!(
            (output.integrated_lufs - DEFAULT_TARGET_LUFS).abs() < 0.05,
            "{}",
            output.integrated_lufs
        );
    }

    #[test]
    fn float_source_is_written_as_24_bit() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_32f.wav", "song.wav");
        let (options, _) = options(NormalizationMode::Peak);

        let result = normalize(&record, &options);

        assert_eq!(result.output().expect("has output").bits_per_sample, 24);
    }

    #[test]
    fn mp3_source_keeps_its_length_and_is_written_as_16_bit() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_320k.mp3", "song.mp3");
        let (options, _) = options(NormalizationMode::Peak);

        let result = normalize(&record, &options);

        let output = result.output().expect("has output");
        assert_eq!(output.bits_per_sample, 16);
        assert_eq!(output.length_in_samples, record.length_in_samples);
    }

    #[test]
    fn tags_are_copied_to_the_output() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "tagged_stereo_44100_16.flac", "song.flac");
        let (options, _) = options(NormalizationMode::Peak);

        let result = normalize(&record, &options);

        let tags = read_tags(&result.output().expect("has output").path).expect("reads tags");
        assert_eq!(tags.title.as_deref(), Some("Test Tone"));
        assert_eq!(tags.artist.as_deref(), Some("AudioBatch"));
    }

    #[test]
    fn aiff_source_is_rewritten_in_place_with_a_copy_disposed_of() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.aif", "song.aif");
        let (options, disposed) = options(NormalizationMode::Peak);

        let result = normalize(&record, &options);

        let output = result.output().expect("has output");
        assert_eq!(output.path, record.path);
        assert!(output.overall_peak.abs() > 0.99);
        assert_eq!(*disposed.lock().expect("lock"), vec!["song.aif".to_string()]);
        assert_eq!(file_names(&directory), vec!["song.aif".to_string()]);
    }

    #[test]
    fn file_already_at_target_is_left_unchanged() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.aif", "song.aif");
        let (options, disposed) = options(NormalizationMode::Peak);
        let normalized = normalize(&record, &options).output().expect("has output").clone();
        let contents = fs::read(&normalized.path).expect("reads");
        disposed.lock().expect("lock").clear();

        let result = normalize(&normalized, &options);

        assert!(result.error().is_none(), "{:?}", result.error());
        assert_eq!(fs::read(&normalized.path).expect("reads"), contents);
        assert!(disposed.lock().expect("lock").is_empty());
    }

    #[test]
    fn existing_output_needs_overwrite() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        fs::write(directory.path().join("song.aif"), b"existing").expect("writes");
        let (mut options, disposed) = options(NormalizationMode::Peak);

        let result = normalize(&record, &options);
        assert_eq!(
            result.error(),
            Some("Output file song.aif already exists. Use force overwrite to replace it.")
        );
        assert!(record.path.is_file());

        options.overwrite_existing = true;
        let result = normalize(&record, &options);
        assert!(result.error().is_none(), "{:?}", result.error());
        assert_eq!(
            *disposed.lock().expect("lock"),
            vec!["song.wav".to_string(), ".song.aif.backup".to_string()]
        );
    }

    #[test]
    fn unfinished_analysis_is_refused() {
        let directory = tempfile::tempdir().expect("creates directory");
        let mut record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        record.status = AnalysisStatus::Pending;
        let (options, _) = options(NormalizationMode::Peak);

        assert_eq!(
            normalize(&record, &options).error(),
            Some("File analysis must finish before normalization")
        );
    }

    #[test]
    fn cancelled_normalization_leaves_the_original() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let (options, disposed) = options(NormalizationMode::Peak);

        let result = normalize_file(&record, &options, &|| true, &mut |_| {});

        assert_eq!(result.error(), Some(CANCELLED_MESSAGE));
        assert!(disposed.lock().expect("lock").is_empty());
        assert_eq!(file_names(&directory), vec!["song.wav".to_string()]);
    }

    #[test]
    fn progress_reaches_the_end() {
        let directory = tempfile::tempdir().expect("creates directory");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let (options, _) = options(NormalizationMode::Peak);
        let mut reported = Vec::new();

        let result = normalize_file(&record, &options, &|| false, &mut |fraction| reported.push(fraction));

        assert!(result.error().is_none(), "{:?}", result.error());
        assert!(
            reported.windows(2).all(|pair| pair.first() <= pair.get(1)),
            "{reported:?}"
        );
        assert!(
            reported.last().is_some_and(|&last| (last - 1.0).abs() < 1e-6),
            "{reported:?}"
        );
    }
}
