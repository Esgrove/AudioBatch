//! Rendering one audio file through a plugin chain into a 16-bit AIFF file.
//!
//! The output is `<name>.aif` next to the source, the same file normalization writes,
//! and an `.aif` source is rewritten in place after a copy of it is disposed of.
//! Before the first plugin the file gets its custom gain, or else the optional normalization gain.
//! The chain renders the file, then the plugins' latency and tail, into an unquantized temporary file,
//! with the latency removed from the start so the output lines up with the source.
//! Trailing silence from the tail allowance is trimmed, but the output is never shorter than the source.
//! The second pass truncates the samples to 16 bits without dither or gain changes,
//! because dithering and output level are up to the plugins.
//! Samples beyond the 16-bit range saturate, and the result notice reports it.

use std::fmt;
use std::fs::File;
use std::io::{BufReader, BufWriter, Read as _, Seek as _, SeekFrom, Write as _};
use std::path::Path;

use audiobatch_core::aiff::AiffWriter;
use audiobatch_core::analysis::analyze_file;
use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_core::metadata::copy_metadata;
use audiobatch_core::normalize::{NormalizationOptions, normalization_gain, normalization_output_path};
use audiobatch_core::operation::{OperationOutcome, OperationResult, progress_range};
use audiobatch_core::output_file::{
    FileDisposer, check_output_path, commit_temporary_output, create_temporary_output, dispose_of_copy, trash_disposer,
    validate_written_audio,
};
use audiobatch_core::record::AnalysisRecord;

use crate::chain_signal::{ChainSignal, StageFailure};
use crate::processing_chain::{CANCELLED_MESSAGE, PROCESSING_BLOCK_FRAMES, PreparedChain, ProcessingChain};

/// Level below which rendered tail samples count as silence and are trimmed from the end of the output.
/// The value is -96 dB, the noise floor of 16-bit audio.
const SILENCE_THRESHOLD: f32 = 1.5849e-5;

/// Bit depth of every processed output, independent of the source.
const OUTPUT_BITS_PER_SAMPLE: u32 = 16;

/// Scale from floating point full scale to a signed 16-bit value.
const OUTPUT_PCM_SCALE: f32 = 32_768.0;

/// Frames per block when converting the rendered audio to the final output.
const FINALIZE_BLOCK_FRAMES: usize = 32_768;

/// Most channels a source can have. A file with more is refused,
/// because its output would drop the extra channels before the original is replaced.
const MAXIMUM_CHANNELS: u32 = 2;

/// Share of a file's progress taken by analyzing a source that changed since its last analysis.
const SOURCE_ANALYSIS_PROGRESS_SHARE: f32 = 0.1;

/// Progress at which rendering through the chain ends and writing the final output begins.
const RENDER_PROGRESS_END: f32 = 0.7;

/// Progress at which writing the final output ends and analyzing the output begins.
const WRITE_PROGRESS_END: f32 = 0.85;

/// Bytes per rendered sample in the temporary file.
const SAMPLE_BYTES: usize = size_of::<f32>();

/// Settings for processing files through a plugin chain.
#[derive(Clone)]
pub struct ProcessingOptions {
    /// Normalization applied before the first plugin to files without a custom gain, or `None` for no gain.
    /// Only its target settings are used.
    pub normalize_before_plugins: Option<NormalizationOptions>,
    /// Replace an existing output file that is not the source.
    pub overwrite_existing: bool,
    /// Disposes of the files a processed output replaces.
    pub dispose: FileDisposer,
}

/// What rendering through the chain produced.
struct RenderedAudio {
    /// The rendered samples, interleaved native endian `f32`.
    file: BufWriter<File>,
    /// Channels of the rendered audio, the same as the source.
    channels: u32,
    /// Sample rate of the source.
    sample_rate: u32,
    /// Frames the source decoded to.
    source_frames: u64,
    /// Frames written after removing the latency.
    frames_written: u64,
    /// Position of the last sample above the silence threshold, or `None` when the output is silent.
    last_audible_frame: Option<u64>,
    /// Notice about source packets that failed to decode and were rendered as silence.
    decode_notice: Option<String>,
}

/// Reads the source in blocks of the chain's size, applying the input gain.
struct SourceBlocks<'a> {
    /// The source decoder.
    decoder: &'a mut AudioFileDecoder,
    /// The decoded chunk not yet passed on, one buffer per channel.
    pending: Vec<Vec<f32>>,
    /// Frames of `pending` already passed on.
    offset: usize,
    /// Linear gain applied to every sample.
    gain: f32,
    /// Whether the decoder has no more chunks.
    exhausted: bool,
}

impl Default for ProcessingOptions {
    fn default() -> Self {
        Self {
            normalize_before_plugins: None,
            overwrite_existing: false,
            dispose: trash_disposer(),
        }
    }
}

impl fmt::Debug for ProcessingOptions {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ProcessingOptions")
            .field("normalize_before_plugins", &self.normalize_before_plugins)
            .field("overwrite_existing", &self.overwrite_existing)
            .finish_non_exhaustive()
    }
}

impl RenderedAudio {
    /// Returns the number of frames to keep.
    /// Trailing silence from the tail allowance is dropped, but the output is never shorter than the source.
    fn final_frames(&self) -> u64 {
        let audible_end = self.last_audible_frame.map_or(0, |frame| frame + 1);
        self.source_frames.max(audible_end).min(self.frames_written)
    }
}

impl<'a> SourceBlocks<'a> {
    /// Starts reading from the decoder.
    const fn new(decoder: &'a mut AudioFileDecoder, gain: f32) -> Self {
        Self {
            decoder,
            pending: Vec::new(),
            offset: 0,
            gain,
            exhausted: false,
        }
    }

    /// Fills the start of the signal's channels with up to `frames` frames, and returns the number filled.
    /// Fewer frames than requested means the source has ended.
    fn read_into(&mut self, signal: &mut ChainSignal, frames: usize) -> Result<usize, String> {
        let mut filled = 0;
        while filled < frames {
            let available = self.pending.first().map_or(0, Vec::len).saturating_sub(self.offset);
            if available == 0 {
                if self.exhausted || !self.decode_next()? {
                    break;
                }
                continue;
            }
            let count = available.min(frames - filled);
            for (channel, samples) in self.pending.iter().enumerate() {
                let Some(target) = signal.channel_mut(channel) else {
                    continue;
                };
                let source = samples.iter().skip(self.offset).take(count);
                for (target, sample) in target.iter_mut().skip(filled).zip(source) {
                    *target = sample * self.gain;
                }
            }
            self.offset += count;
            filled += count;
        }
        Ok(filled)
    }

    /// Decodes the next chunk into `pending`. Returns false when the source has ended.
    fn decode_next(&mut self) -> Result<bool, String> {
        let Some(chunk) = self.decoder.next_chunk().map_err(|error| error.to_string())? else {
            self.exhausted = true;
            return Ok(false);
        };
        self.pending.resize_with(chunk.len(), Vec::new);
        for (target, samples) in self.pending.iter_mut().zip(chunk) {
            target.clear();
            target.extend_from_slice(samples);
        }
        self.offset = 0;
        Ok(true)
    }
}

/// Returns the output path for the source, the same `<name>.aif` that normalization writes.
#[must_use]
pub fn processing_output_path(source: &Path) -> std::path::PathBuf {
    normalization_output_path(source)
}

/// Renders a file through the chain into a 16-bit AIFF file, commits it, and analyzes the result.
///
/// Runs on a worker thread. Plugin setup and release run on the UI thread through the chain's dispatcher,
/// and the UI thread must keep running jobs while a worker is inside this call, unless `is_cancelled` returns true.
/// The chain must only be used by one call at a time.
/// When the file changed on disk since it was analyzed, it is analyzed again first.
/// A call cancelled before its output is committed fails and leaves the original untouched.
/// Progress of the whole file, through any source analysis, the render, the final write, and the output analysis,
/// is reported to `on_progress`.
pub fn process_file(
    source_record: &AnalysisRecord,
    options: &ProcessingOptions,
    chain: &ProcessingChain,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> OperationResult {
    let source = source_record.path.as_path();
    let _span = tracing::info_span!("process", file = %source.display()).entered();
    match try_process_file(source_record, options, chain, is_cancelled, on_progress) {
        Ok(result) => result,
        Err(error) => OperationResult::failure(source, error),
    }
}

/// Processes a file, returning the user-facing error of a failure that left the original untouched.
fn try_process_file(
    source_record: &AnalysisRecord,
    options: &ProcessingOptions,
    chain: &ProcessingChain,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> Result<OperationResult, String> {
    let source = source_record.path.as_path();
    if !source.is_file() {
        return Err("File does not exist".to_string());
    }
    if !source_record.is_ready() {
        return Err("File analysis must finish before processing".to_string());
    }
    if chain.is_empty() {
        return Err("The plugin chain has no enabled plugins".to_string());
    }
    let output = processing_output_path(source);
    check_output_path(source, &output, options.overwrite_existing)?;

    let render_start = if source_record.matches_file_on_disk() {
        0.0
    } else {
        SOURCE_ANALYSIS_PROGRESS_SHARE
    };
    let record = refresh_if_changed(
        source_record,
        is_cancelled,
        &mut progress_range(on_progress, 0.0, render_start),
    );
    if record.has_error() {
        return Err(record.error_message);
    }
    let (gain, gain_notice) = input_gain(&record, options);
    let mut decoder = AudioFileDecoder::open(source).map_err(|_| "Unsupported or unreadable audio file".to_string())?;
    let channels = decoder.info().channels;
    if !(1..=MAXIMUM_CHANNELS).contains(&channels) {
        return Err(format!(
            "Only mono and stereo files can be processed, this file has {channels} channels"
        ));
    }

    let mut rendered = render_chain_to_file(
        &mut decoder,
        chain,
        gain,
        record.length_in_samples,
        is_cancelled,
        &mut progress_range(on_progress, render_start, RENDER_PROGRESS_END),
    )?;
    // Closes the source before it may be replaced in place, since Windows cannot replace an open file.
    drop(decoder);
    let final_frames = rendered.final_frames();
    let temporary = create_temporary_output(&output).map_err(|error| error.to_string())?;
    let clipped = write_final_output(
        &mut rendered,
        temporary.as_file(),
        final_frames,
        is_cancelled,
        &mut progress_range(on_progress, RENDER_PROGRESS_END, WRITE_PROGRESS_END),
    )?;
    if let Err(error) = copy_metadata(source, temporary.path()) {
        tracing::error!("{error:#}");
        return Err("Could not copy metadata onto the processed output".to_string());
    }
    validate_written_audio(temporary.path(), rendered.channels, rendered.sample_rate, final_frames)
        .map_err(|error| format!("Processed output failed validation: {error}"))?;

    // Checked again before touching the original, since cancellation may arrive during the last block.
    if is_cancelled() {
        return Err(CANCELLED_MESSAGE.to_string());
    }
    if output == source && !dispose_of_copy(source, &options.dispose) {
        return Err("Could not move a copy of the original file to the system trash".to_string());
    }
    commit_temporary_output(temporary, source, &output, options.overwrite_existing, &options.dispose)?;

    let notices: Vec<String> = [
        gain_notice,
        clipped.then(|| "Output clipping: the final render exceeded the 16-bit range".to_string()),
        rendered.decode_notice,
    ]
    .into_iter()
    .flatten()
    .collect();
    let analysis = analyze_file(
        &output,
        &|| false,
        &mut progress_range(on_progress, WRITE_PROGRESS_END, 1.0),
    );
    if analysis.has_error() {
        let error = format!(
            "The file was processed, but re-analysis failed: {}",
            analysis.error_message
        );
        return Ok(OperationResult {
            source: source.to_path_buf(),
            outcome: OperationOutcome::CommittedButUnanalyzed {
                output: analysis,
                error,
            },
        });
    }
    let notice = (!notices.is_empty()).then(|| notices.join(". "));
    Ok(OperationResult::success(source, analysis, notice))
}

/// Returns the record unchanged when the file still matches it on disk, and otherwise analyzes the file again,
/// so the input gain is never computed from outdated levels.
/// The custom gain is dropped for a changed file, since it was set for the previous version of the file.
fn refresh_if_changed(
    record: &AnalysisRecord,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> AnalysisRecord {
    if record.matches_file_on_disk() {
        record.clone()
    } else {
        analyze_file(&record.path, is_cancelled, on_progress)
    }
}

/// Returns the linear gain applied before the first plugin, and a notice when normalization was skipped.
/// A custom gain wins, otherwise the file is normalized when the options ask for it, and otherwise left unchanged.
/// A file the normalization refuses, such as silence, is processed at unity gain, and the notice says why.
fn input_gain(record: &AnalysisRecord, options: &ProcessingOptions) -> (f32, Option<String>) {
    if record.custom_gain_db.is_some() {
        return (record.custom_gain_linear(), None);
    }
    let Some(normalization) = &options.normalize_before_plugins else {
        return (1.0, None);
    };
    match normalization_gain(record, normalization) {
        Ok(gain) => (gain, None),
        Err(reason) => (1.0, Some(format!("Not normalized before the plugins: {reason}"))),
    }
}

/// Prepares the chain, renders the whole file through it into an anonymous temporary file, and releases the chain.
fn render_chain_to_file(
    decoder: &mut AudioFileDecoder,
    chain: &ProcessingChain,
    gain: f32,
    expected_frames: u64,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> Result<RenderedAudio, String> {
    let channels = decoder.info().channels;
    let sample_rate = decoder.info().sample_rate;
    let file = tempfile::tempfile().map_err(|_| "Could not create the temporary render file".to_string())?;
    let mut prepared = chain.prepare(channels as usize, f64::from(sample_rate), is_cancelled)?;
    let rendered = render_blocks(
        decoder,
        &mut prepared,
        file,
        gain,
        expected_frames,
        is_cancelled,
        on_progress,
    );
    drop(prepared);
    chain.release(is_cancelled);
    let mut rendered = rendered?;
    rendered.decode_notice = decoder.failures().notice();
    Ok(rendered)
}

/// Streams the source through the chain block by block and writes the processed samples to the file.
/// The chain latency and tail are rendered after the source, and the first `latency_samples` frames are dropped,
/// so the output lines up with the source and its end is not cut off.
fn render_blocks(
    decoder: &mut AudioFileDecoder,
    prepared: &mut PreparedChain,
    file: File,
    gain: f32,
    expected_frames: u64,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> Result<RenderedAudio, String> {
    let channels = decoder.info().channels;
    let sample_rate = decoder.info().sample_rate;
    let latency = prepared.latency_samples;
    let after_source = latency + prepared.tail_samples;
    let mut source = SourceBlocks::new(decoder, gain);
    let mut signal = ChainSignal::new(PROCESSING_BLOCK_FRAMES);
    let mut position: u64 = 0;
    let mut total_frames: Option<u64> = None;
    let mut rendered = RenderedAudio {
        file: BufWriter::new(file),
        channels,
        sample_rate,
        source_frames: 0,
        frames_written: 0,
        last_audible_frame: None,
        decode_notice: None,
    };
    loop {
        if is_cancelled() {
            return Err(CANCELLED_MESSAGE.to_string());
        }
        signal.reset(channels as usize);
        if total_frames.is_none() {
            let read = source.read_into(&mut signal, PROCESSING_BLOCK_FRAMES)?;
            rendered.source_frames += read as u64;
            if read < PROCESSING_BLOCK_FRAMES {
                total_frames = Some(position + read as u64 + after_source);
            }
        }
        let block_frames = total_frames.map_or(PROCESSING_BLOCK_FRAMES as u64, |total| {
            (PROCESSING_BLOCK_FRAMES as u64).min(total - position)
        });
        if block_frames == 0 {
            break;
        }
        signal
            .process(&mut prepared.processors, PROCESSING_BLOCK_FRAMES)
            .map_err(|failure| describe_failed_stage(&prepared.names, failure))?;
        signal.fit_to(channels as usize);
        let skipped = latency.saturating_sub(position).min(block_frames);
        write_block(&signal, skipped as usize, block_frames as usize, &mut rendered)?;
        position += block_frames;
        let estimated_total = total_frames.unwrap_or_else(|| expected_frames.max(position) + after_source);
        on_progress((position as f64 / estimated_total.max(1) as f64).min(1.0) as f32);
    }
    Ok(rendered)
}

/// Describes the failed process call of a plugin for the user, with the plugin's name and its result code.
fn describe_failed_stage(names: &[String], failure: StageFailure) -> String {
    let name = names.get(failure.index).map_or("A plugin", String::as_str);
    format!("{name}: {}", failure.error)
}

/// Appends frames `first..end` of the signal to the render file, interleaved,
/// after checking that they are finite, and tracks the last audible frame.
fn write_block(signal: &ChainSignal, first: usize, end: usize, rendered: &mut RenderedAudio) -> Result<(), String> {
    let channels: Vec<&[f32]> = (0..rendered.channels as usize)
        .filter_map(|channel| signal.channel(channel))
        .collect();
    for frame in first..end {
        let mut audible = false;
        for samples in &channels {
            let sample = samples.get(frame).copied().unwrap_or_default();
            if !sample.is_finite() {
                return Err("The plugin chain produced invalid samples (infinity or NaN)".to_string());
            }
            audible |= sample.abs() > SILENCE_THRESHOLD;
            rendered
                .file
                .write_all(&sample.to_ne_bytes())
                .map_err(|_| "Failed while writing processed audio data".to_string())?;
        }
        if audible {
            rendered.last_audible_frame = Some(rendered.frames_written);
        }
        rendered.frames_written += 1;
    }
    Ok(())
}

/// Converts the first `final_frames` rendered frames to 16-bit AIFF in the output file.
/// Low bits are discarded without dither, and samples beyond the 16-bit range saturate without a gain change.
/// Returns whether any sample saturated.
fn write_final_output(
    rendered: &mut RenderedAudio,
    output: &File,
    final_frames: u64,
    is_cancelled: &dyn Fn() -> bool,
    on_progress: &mut dyn FnMut(f32),
) -> Result<bool, String> {
    let write_error = |_| "Failed while writing processed audio data".to_string();
    let read_error = |_| "Failed while reading the rendered audio".to_string();
    let channels = rendered.channels as usize;
    rendered.file.seek(SeekFrom::Start(0)).map_err(read_error)?;
    let mut reader = BufReader::new(rendered.file.get_ref());
    let mut writer = AiffWriter::new(
        BufWriter::new(output),
        rendered.channels,
        rendered.sample_rate,
        OUTPUT_BITS_PER_SAMPLE,
    )
    .map_err(write_error)?;
    let mut bytes = vec![0_u8; FINALIZE_BLOCK_FRAMES * channels * SAMPLE_BYTES];
    let mut planes: Vec<Vec<i32>> = vec![Vec::with_capacity(FINALIZE_BLOCK_FRAMES); channels];
    let mut clipped = false;
    let mut position: u64 = 0;
    while position < final_frames {
        if is_cancelled() {
            return Err(CANCELLED_MESSAGE.to_string());
        }
        let block_frames = (FINALIZE_BLOCK_FRAMES as u64).min(final_frames - position) as usize;
        let block = bytes
            .get_mut(..block_frames * channels * SAMPLE_BYTES)
            .unwrap_or_default();
        reader.read_exact(block).map_err(read_error)?;
        for plane in &mut planes {
            plane.clear();
        }
        for (index, sample_bytes) in block.as_chunks::<SAMPLE_BYTES>().0.iter().enumerate() {
            let sample = f32::from_ne_bytes(*sample_bytes);
            let (value, saturated) = quantize(sample);
            clipped |= saturated;
            if let Some(plane) = planes.get_mut(index % channels) {
                plane.push(value);
            }
        }
        let plane_slices: Vec<&[i32]> = planes.iter().map(Vec::as_slice).collect();
        writer.write_planar_pcm(&plane_slices).map_err(write_error)?;
        position += block_frames as u64;
        on_progress((position as f64 / final_frames as f64) as f32);
    }
    writer.finish().map_err(write_error)?;
    Ok(clipped)
}

/// Converts a sample to a 16-bit value by flooring, and returns whether it was outside the 16-bit range.
fn quantize(sample: f32) -> (i32, bool) {
    let minimum = -OUTPUT_PCM_SCALE;
    let maximum = OUTPUT_PCM_SCALE - 1.0;
    let scaled = sample * OUTPUT_PCM_SCALE;
    // Values between the largest step and the next one round down to the largest step,
    // so only values from the next step on are cut off.
    let saturated = !(minimum..OUTPUT_PCM_SCALE).contains(&scaled);
    (scaled.clamp(minimum, maximum).floor() as i32, saturated)
}

#[cfg(test)]
mod test_quantize {
    use super::*;

    #[test]
    fn floors_without_dither_and_saturates_out_of_range_samples() {
        assert_eq!(quantize(0.0), (0, false));
        assert_eq!(quantize(0.5), (16_384, false));
        assert_eq!(quantize(-0.5), (-16_384, false));
        assert_eq!(quantize(1.0 / 32_768.0 * 1.75), (1, false));
        assert_eq!(quantize(-1.0 / 32_768.0 * 0.25), (-1, false));
        assert_eq!(quantize(-1.0), (-32_768, false));
        assert_eq!(quantize(32_767.5 / 32_768.0), (32_767, false));
        assert_eq!(quantize(1.0), (32_767, true));
        assert_eq!(quantize(-1.5), (-32_768, true));
    }
}

#[cfg(test)]
mod test_process_file_output {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use audiobatch_test_plugin::{ECHO_DELAY, Kind};
    use tempfile::TempDir;

    use super::*;
    use crate::test_helpers::{
        DecodedAudio, decode, deleting_options, file_names, gain_state, ready_fixture_copy, test_chain,
    };

    /// Processes a fixture copy through a chain and returns the result, the decoded source, and the decoded output.
    fn process_fixture(
        fixture: &str,
        chain: &ProcessingChain,
        options: &ProcessingOptions,
    ) -> (TempDir, OperationResult, DecodedAudio, Option<DecodedAudio>) {
        let directory = TempDir::new().expect("temp dir");
        let extension = Path::new(fixture)
            .extension()
            .and_then(|extension| extension.to_str())
            .unwrap_or("wav");
        let record = ready_fixture_copy(&directory, fixture, &format!("song.{extension}"));
        let source = decode(&record.path);
        let result = process_file(&record, options, chain, &|| false, &mut |_| {});
        let output = result.output().map(|output| decode(&output.path));
        (directory, result, source, output)
    }

    #[test]
    fn a_unity_chain_writes_the_source_samples_as_16_bit_aiff() {
        let chain = test_chain(&[(Kind::Gain, gain_state(1.0))]);
        let (directory, result, source, output) =
            process_fixture("sine_stereo_44100_16.wav", &chain, &deleting_options());
        assert_eq!(result.error(), None);
        let record = result.output().expect("committed");
        assert_eq!(record.path, directory.path().join("song.aif"));
        assert_eq!(record.bits_per_sample, 16);
        let output = output.expect("decoded");
        assert_eq!(output.channels, 2);
        assert_eq!(output.sample_rate, source.sample_rate);
        assert_eq!(output.samples, source.samples);
        assert_eq!(file_names(&directory), ["song.aif"]);
    }

    #[test]
    fn the_saved_state_is_restored_for_every_file() {
        let chain = test_chain(&[(Kind::Gain, gain_state(0.5))]);
        for _ in 0..2 {
            let (_directory, result, source, output) =
                process_fixture("sine_stereo_44100_16.wav", &chain, &deleting_options());
            assert_eq!(result.error(), None);
            let output = output.expect("decoded");
            assert!(source.peak().mul_add(-0.5, output.peak()).abs() < 2.0 / OUTPUT_PCM_SCALE);
        }
    }

    #[test]
    fn the_chain_latency_is_removed_from_the_start() {
        let chain = test_chain(&[(Kind::Delay, gain_state(1.0)), (Kind::Delay, gain_state(1.0))]);
        let (_directory, result, source, output) =
            process_fixture("sine_stereo_44100_16.wav", &chain, &deleting_options());
        assert_eq!(result.error(), None);
        assert_eq!(output.expect("decoded").samples, source.samples);
    }

    #[test]
    fn the_tail_is_rendered_and_trailing_silence_is_trimmed() {
        let echo = test_chain(&[(Kind::Echo, gain_state(1.0))]);
        let (_directory, result, source, output) =
            process_fixture("sine_mono_44100_16.wav", &echo, &deleting_options());
        assert_eq!(result.error(), None);
        let output = output.expect("decoded");
        assert!(output.frames() > source.frames());
        assert!(output.frames() <= source.frames() + ECHO_DELAY as usize);

        let silent = test_chain(&[(Kind::Echo, gain_state(0.0))]);
        let (_directory, result, source, output) =
            process_fixture("sine_mono_44100_16.wav", &silent, &deleting_options());
        assert_eq!(result.error(), None);
        assert_eq!(output.expect("decoded").frames(), source.frames());
    }

    #[test]
    fn a_mono_file_runs_through_a_stereo_plugin_and_stays_mono() {
        let chain = test_chain(&[(Kind::StereoOnly, gain_state(1.0))]);
        let (_directory, result, source, output) =
            process_fixture("sine_mono_44100_16.wav", &chain, &deleting_options());
        assert_eq!(result.error(), None);
        let output = output.expect("decoded");
        assert_eq!(output.channels, 1);
        assert_eq!(output.samples, source.samples);
    }

    #[test]
    fn samples_beyond_the_16_bit_range_saturate_and_are_reported() {
        let chain = test_chain(&[(Kind::Gain, gain_state(4.0))]);
        let (_directory, result, _source, output) =
            process_fixture("sine_stereo_44100_16.wav", &chain, &deleting_options());
        let OperationOutcome::Succeeded { notice, .. } = &result.outcome else {
            panic!("processing failed: {:?}", result.error());
        };
        assert_eq!(
            notice.as_deref(),
            Some("Output clipping: the final render exceeded the 16-bit range")
        );
        assert!((output.expect("decoded").peak() - 1.0).abs() < 1e-4);
    }

    #[test]
    fn an_aif_source_is_rewritten_in_place_after_disposing_of_a_copy() {
        let disposed = Arc::new(AtomicUsize::new(0));
        let counter = Arc::clone(&disposed);
        let options = ProcessingOptions {
            dispose: Arc::new(move |_: &Path| {
                counter.fetch_add(1, Ordering::SeqCst);
                true
            }),
            ..ProcessingOptions::default()
        };
        let chain = test_chain(&[(Kind::Gain, gain_state(0.5))]);
        let (directory, result, _source, _output) = process_fixture("sine_stereo_44100_16.aif", &chain, &options);
        assert_eq!(result.error(), None);
        assert_eq!(
            result.output().map(|output| output.path.clone()),
            Some(directory.path().join("song.aif"))
        );
        assert_eq!(disposed.load(Ordering::SeqCst), 1);
        assert_eq!(file_names(&directory), ["song.aif"]);
    }
}

#[cfg(test)]
mod test_process_file_gain {
    use audiobatch_core::normalize::NormalizationOptions;
    use audiobatch_test_plugin::Kind;
    use tempfile::TempDir;

    use super::*;
    use crate::test_helpers::{decode, deleting_options, gain_state, ready_fixture_copy, test_chain};

    /// Returns options that normalize the sample peak to `target_peak_dbfs` before the plugins.
    fn normalizing_options(target_peak_dbfs: f64) -> ProcessingOptions {
        ProcessingOptions {
            normalize_before_plugins: Some(NormalizationOptions {
                target_peak_dbfs,
                ..NormalizationOptions::default()
            }),
            ..deleting_options()
        }
    }

    /// Returns the peak of a processed output in dBFS.
    fn output_peak_dbfs(result: &OperationResult) -> f64 {
        let output = decode(&result.output().expect("committed").path);
        20.0 * f64::from(output.peak()).log10()
    }

    #[test]
    fn the_file_is_normalized_before_the_first_plugin() {
        let directory = TempDir::new().expect("temp dir");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let chain = test_chain(&[(Kind::Gain, gain_state(1.0))]);
        let result = process_file(&record, &normalizing_options(-12.0), &chain, &|| false, &mut |_| {});
        assert_eq!(result.error(), None);
        assert!((output_peak_dbfs(&result) + 12.0).abs() < 0.01);
    }

    #[test]
    fn a_custom_gain_wins_over_normalization() {
        let directory = TempDir::new().expect("temp dir");
        let mut record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        record.custom_gain_db = Some(3.0);
        let chain = test_chain(&[(Kind::Gain, gain_state(1.0))]);
        let result = process_file(&record, &normalizing_options(-12.0), &chain, &|| false, &mut |_| {});
        assert_eq!(result.error(), None);
        let expected = test_fixtures::FIXTURE_PEAK_DECIBELS + 3.0;
        assert!((output_peak_dbfs(&result) - expected).abs() < 0.01);
        assert_eq!(result.output().and_then(|output| output.custom_gain_db), None);
    }

    #[test]
    fn a_file_normalization_refuses_is_processed_at_unity_gain_with_a_notice() {
        let directory = TempDir::new().expect("temp dir");
        let record = ready_fixture_copy(&directory, "silence_stereo_44100_16.wav", "song.wav");
        let chain = test_chain(&[(Kind::Gain, gain_state(1.0))]);
        let result = process_file(&record, &normalizing_options(-1.0), &chain, &|| false, &mut |_| {});
        let OperationOutcome::Succeeded { notice, output } = &result.outcome else {
            panic!("processing failed: {:?}", result.error());
        };
        assert_eq!(
            notice.as_deref(),
            Some("Not normalized before the plugins: File contains no signal that can be normalized")
        );
        assert_eq!(output.length_in_samples, record.length_in_samples);
    }
}

#[cfg(test)]
mod test_process_file_failures {
    use std::sync::atomic::{AtomicUsize, Ordering};

    use audiobatch_test_plugin::Kind;
    use tempfile::TempDir;

    use super::*;
    use crate::test_helpers::{deleting_options, file_names, gain_state, ready_fixture_copy, test_chain};

    #[test]
    fn non_finite_output_fails_and_leaves_the_source() {
        let directory = TempDir::new().expect("temp dir");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let chain = test_chain(&[(Kind::NotFinite, gain_state(1.0))]);
        let result = process_file(&record, &deleting_options(), &chain, &|| false, &mut |_| {});
        assert_eq!(
            result.error(),
            Some("The plugin chain produced invalid samples (infinity or NaN)")
        );
        assert_eq!(file_names(&directory), ["song.wav"]);
    }

    #[test]
    fn a_plugin_that_fails_to_process_fails_the_file_and_leaves_no_output() {
        let directory = TempDir::new().expect("temp dir");
        let converted = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let in_place = ready_fixture_copy(&directory, "sine_mono_48000_24.aif", "other.aif");
        let original = std::fs::read(&in_place.path).expect("read");
        let chain = test_chain(&[(Kind::Gain, gain_state(1.0)), (Kind::Failing, gain_state(1.0))]);
        for record in [&converted, &in_place] {
            let result = process_file(record, &deleting_options(), &chain, &|| false, &mut |_| {});
            assert_eq!(
                result.error(),
                Some("Test Failing: The plugin failed to process audio (result 1)"),
                "{}",
                record.file_name
            );
            assert!(result.output().is_none());
        }
        assert_eq!(file_names(&directory), ["other.aif", "song.wav"]);
        assert_eq!(std::fs::read(&in_place.path).expect("read"), original);
    }

    #[test]
    fn a_plugin_without_the_file_layout_fails_and_the_chain_stays_usable() {
        let directory = TempDir::new().expect("temp dir");
        let stereo = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "stereo.wav");
        let chain = test_chain(&[(Kind::MonoOnly, gain_state(1.0))]);
        let result = process_file(&stereo, &deleting_options(), &chain, &|| false, &mut |_| {});
        assert_eq!(
            result.error(),
            Some("Plugin \"Test Mono Only\" does not support the file's channel layout (2 channels)")
        );
        let mono = ready_fixture_copy(&directory, "sine_mono_44100_16.wav", "mono.wav");
        let result = process_file(&mono, &deleting_options(), &chain, &|| false, &mut |_| {});
        assert_eq!(result.error(), None);
        assert_eq!(file_names(&directory), ["mono.aif", "stereo.wav"]);
    }

    #[test]
    fn a_cancelled_render_leaves_no_files() {
        let directory = TempDir::new().expect("temp dir");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let chain = test_chain(&[(Kind::Gain, gain_state(1.0))]);
        let checks = AtomicUsize::new(0);
        let is_cancelled = || checks.fetch_add(1, Ordering::SeqCst) >= 3;
        let result = process_file(&record, &deleting_options(), &chain, &is_cancelled, &mut |_| {});
        assert_eq!(result.error(), Some(CANCELLED_MESSAGE));
        assert_eq!(file_names(&directory), ["song.wav"]);
    }

    #[test]
    fn unprocessable_inputs_fail_with_a_reason() {
        let directory = TempDir::new().expect("temp dir");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let empty = test_chain(&[]);
        let result = process_file(&record, &deleting_options(), &empty, &|| false, &mut |_| {});
        assert_eq!(result.error(), Some("The plugin chain has no enabled plugins"));

        let chain = test_chain(&[(Kind::Gain, gain_state(1.0))]);
        let mut pending = record.clone();
        pending.status = audiobatch_core::AnalysisStatus::Pending;
        let result = process_file(&pending, &deleting_options(), &chain, &|| false, &mut |_| {});
        assert_eq!(result.error(), Some("File analysis must finish before processing"));

        std::fs::write(directory.path().join("song.aif"), b"taken").expect("written");
        let result = process_file(&record, &deleting_options(), &chain, &|| false, &mut |_| {});
        assert!(
            result.error().is_some_and(|error| error.contains("song.aif")),
            "{:?}",
            result.error()
        );

        std::fs::remove_file(&record.path).expect("removed");
        let result = process_file(&record, &deleting_options(), &chain, &|| false, &mut |_| {});
        assert_eq!(result.error(), Some("File does not exist"));
    }
}
