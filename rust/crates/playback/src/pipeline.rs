//! Turns a decoded file into stereo frames at the output device rate.
//!
//! `FramePipeline` runs on the decode thread.
//! It decodes packets with the shared core decoder, maps the source channels to stereo,
//! resamples with rubato when the file and device rates differ,
//! and tags every frame with the playback generation and its position in the source,
//! so the audio callback can drop stale frames after a seek and report an exact playhead.

use std::path::Path;

use audioadapter_buffers::SizeError;
use audioadapter_buffers::direct::SequentialSliceOfVecs;
use audiobatch_core::decode::{AudioFileDecoder, DecodeError, StreamInfo};
use rubato::{Fft, FixedSync, ResampleError, Resampler, ResamplerConstructionError};
use thiserror::Error;

/// Input chunk size of the resampler in frames.
const RESAMPLER_CHUNK_FRAMES: usize = 1024;

/// Failures while decoding a file into stereo output frames on the decode thread.
#[derive(Debug, Error)]
pub enum PipelineError {
    /// The shared decoder could not open, seek, or decode the source.
    #[error(transparent)]
    Decode(#[from] DecodeError),
    /// Preview only supports mono and stereo input.
    #[error("Audio preview requires a mono or stereo file")]
    UnsupportedChannels {
        /// Number of source channels.
        channels: u32,
    },
    /// The requested rate conversion could not be initialized.
    #[error("Could not create the resampler")]
    CreateResampler(#[source] ResamplerConstructionError),
    /// The resampler input adapter rejected the buffer dimensions.
    #[error("Invalid resampler input buffer: {0:?}")]
    InputBuffer(#[source] SizeError),
    /// The resampler output adapter rejected the buffer dimensions.
    #[error("Invalid resampler output buffer: {0:?}")]
    OutputBuffer(#[source] SizeError),
    /// Processing a resampler chunk failed.
    #[error("Resampling failed")]
    Resample(#[source] ResampleError),
    /// The FFT geometry did not produce the required output within its finite drain budget.
    #[error("Resampler drain produced {actual} frames instead of {expected}")]
    IncompleteDrain {
        /// Exact number of output frames required by the source length and rate ratio.
        expected: u64,
        /// Number of output frames actually emitted.
        actual: u64,
    },
}

/// One stereo output frame with the bookkeeping the audio callback needs.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct Frame {
    /// Load or seek the frame belongs to. Frames of older generations are discarded.
    pub generation: u32,
    /// Position of the frame in the source file, in source frames.
    pub position: u64,
    /// Left channel sample.
    pub left: f32,
    /// Right channel sample, a copy of the left one for mono files.
    pub right: f32,
}

/// Decodes one file into tagged stereo frames at the output rate.
pub struct FramePipeline {
    decoder: AudioFileDecoder,
    generation: u32,
    /// Source position of the next decoded frame.
    position: u64,
    /// Decoded frames before this source position are dropped, for sample-accurate seeks.
    skip_until: u64,
    resampler: Option<ResamplerStage>,
    finished: bool,
}

/// Sample rate conversion from the file rate to the device rate.
struct ResamplerStage {
    resampler: Fft<f32>,
    input: Vec<Vec<f32>>,
    output: Vec<Vec<f32>>,
    filled: usize,
    /// Source position where resampling started after the last open or seek.
    start_position: u64,
    /// Output frames emitted since the last open or seek.
    emitted: u64,
    /// Actual source frames accepted since the last open or seek, excluding EOF padding.
    received_input: u64,
    /// Input frames already passed to the FFT resampler, including EOF padding.
    processed_input: u64,
    /// Source frames per second, used for exact rational frame accounting.
    source_rate: u32,
    /// Output frames per second, used for exact rational frame accounting.
    output_rate: u32,
    /// Output frames still to drop to compensate for the resampler delay.
    skip_output: usize,
}

impl PipelineError {
    /// Formats the event text without repeating diagnostics already included in the display.
    pub(crate) fn message(&self) -> String {
        match self {
            Self::InputBuffer(_)
            | Self::OutputBuffer(_)
            | Self::Decode(DecodeError::Decode { .. } | DecodeError::Seek(_)) => self.to_string(),
            _ => crate::error_message(self),
        }
    }
}

impl FramePipeline {
    /// Opens a file for playback at the given output rate, starting at `start_seconds`.
    ///
    /// # Errors
    ///
    /// Returns an error when the file cannot be decoded, seeked, or resampled,
    /// or has more than two channels, which the preview refuses.
    pub fn open(path: &Path, output_rate: u32, generation: u32, start_seconds: f64) -> Result<Self, PipelineError> {
        let decoder = AudioFileDecoder::open(path)?;
        if !(1..=2).contains(&decoder.info().channels) {
            return Err(PipelineError::UnsupportedChannels {
                channels: decoder.info().channels,
            });
        }
        let source_rate = decoder.info().sample_rate;
        let resampler = (source_rate != output_rate)
            .then(|| ResamplerStage::new(source_rate, output_rate))
            .transpose()?;
        let mut pipeline = Self {
            decoder,
            generation,
            position: 0,
            skip_until: 0,
            resampler,
            finished: false,
        };
        if start_seconds > 0.0 {
            pipeline.seek(start_seconds, generation)?;
        }
        Ok(pipeline)
    }

    /// Returns the stream properties of the file.
    #[must_use]
    pub const fn info(&self) -> &StreamInfo {
        self.decoder.info()
    }

    /// Returns the generation the produced frames are tagged with.
    #[must_use]
    pub const fn generation(&self) -> u32 {
        self.generation
    }

    /// Returns true once the whole file has been produced.
    #[must_use]
    pub const fn is_finished(&self) -> bool {
        self.finished
    }

    /// Moves to the given time and starts tagging frames with a new generation.
    ///
    /// # Errors
    ///
    /// Returns an error when the container cannot seek.
    pub fn seek(&mut self, seconds: f64, generation: u32) -> Result<(), PipelineError> {
        let target = (seconds.max(0.0) * f64::from(self.info().sample_rate)).round() as u64;
        let actual = self.decoder.seek(seconds)?;
        self.position = actual.min(target);
        self.skip_until = target;
        self.generation = generation;
        self.finished = false;
        if let Some(stage) = &mut self.resampler {
            stage.reset(target);
        }
        Ok(())
    }

    /// Decodes the next packet and appends its output frames.
    /// Returns false once the end of the file has been reached and every frame has been produced.
    ///
    /// # Errors
    ///
    /// Returns an error when decoding fails.
    pub fn produce(&mut self, output: &mut Vec<Frame>) -> Result<bool, PipelineError> {
        if self.finished {
            return Ok(false);
        }
        let Some(chunk) = self.decoder.next_chunk()? else {
            if let Some(stage) = &mut self.resampler {
                stage.flush(self.generation, output)?;
            }
            self.finished = true;
            return Ok(false);
        };
        let frame_count = chunk.first().map_or(0, Vec::len);
        let left = chunk.first().map_or(&[][..], Vec::as_slice);
        let right = chunk.get(1).map_or(left, Vec::as_slice);

        // Frames before the seek target are decoded but not played.
        let skip = usize::try_from(self.skip_until.saturating_sub(self.position))
            .unwrap_or(usize::MAX)
            .min(frame_count);
        let start_position = self.position + skip as u64;
        let left = left.get(skip..).unwrap_or_default();
        let right = right.get(skip..).unwrap_or_default();
        self.position += frame_count as u64;

        match &mut self.resampler {
            Some(stage) => stage.push(left, right, self.generation, output)?,
            None => output.extend(
                left.iter()
                    .zip(right)
                    .zip(start_position..)
                    .map(|((&left, &right), position)| Frame {
                        generation: self.generation,
                        position,
                        left,
                        right,
                    }),
            ),
        }
        Ok(true)
    }
}

impl ResamplerStage {
    /// Creates a stereo resampler between the given rates.
    fn new(source_rate: u32, output_rate: u32) -> Result<Self, PipelineError> {
        let resampler = Fft::<f32>::new(
            source_rate as usize,
            output_rate as usize,
            RESAMPLER_CHUNK_FRAMES,
            2,
            FixedSync::Input,
        )
        .map_err(PipelineError::CreateResampler)?;
        let output_capacity = resampler.output_frames_max();
        let skip_output = resampler.output_delay();
        Ok(Self {
            resampler,
            input: vec![vec![0.0; RESAMPLER_CHUNK_FRAMES]; 2],
            output: vec![vec![0.0; output_capacity]; 2],
            filled: 0,
            start_position: 0,
            emitted: 0,
            received_input: 0,
            processed_input: 0,
            source_rate,
            output_rate,
            skip_output,
        })
    }

    /// Clears the resampler state for a new start position.
    fn reset(&mut self, position: u64) {
        self.resampler.reset();
        self.filled = 0;
        self.start_position = position;
        self.emitted = 0;
        self.received_input = 0;
        self.processed_input = 0;
        self.skip_output = self.resampler.output_delay();
    }

    /// Buffers decoded samples and resamples every complete input chunk.
    fn push(
        &mut self,
        left: &[f32],
        right: &[f32],
        generation: u32,
        output: &mut Vec<Frame>,
    ) -> Result<(), PipelineError> {
        self.received_input += left.len() as u64;
        let mut offset = 0;
        while offset < left.len() {
            let take = (RESAMPLER_CHUNK_FRAMES - self.filled).min(left.len() - offset);
            for (channel, source) in self.input.iter_mut().zip([left, right]) {
                if let (Some(destination), Some(samples)) = (
                    channel.get_mut(self.filled..self.filled + take),
                    source.get(offset..offset + take),
                ) {
                    destination.copy_from_slice(samples);
                }
            }
            self.filled += take;
            offset += take;
            if self.filled == RESAMPLER_CHUNK_FRAMES {
                self.process_chunk(generation, None, output)?;
            }
        }
        Ok(())
    }

    /// Resamples the remaining partial chunk and the resampler delay at the end of the file.
    /// The required FFT blocks include the output delay, in output frames.
    /// Converting those blocks to input frames gives a finite budget even when one FFT block spans many input chunks.
    fn flush(&mut self, generation: u32, output: &mut Vec<Frame>) -> Result<(), PipelineError> {
        let expected = self.output_length();
        let raw_output = expected + self.resampler.output_delay() as u64;
        let fft_blocks = raw_output.div_ceil(self.resampler.fft_size_out() as u64);
        let required_input = fft_blocks * self.resampler.fft_size_in() as u64;
        let remaining_chunks = required_input
            .saturating_sub(self.processed_input)
            .div_ceil(RESAMPLER_CHUNK_FRAMES as u64);
        for _ in 0..remaining_chunks {
            if self.emitted == expected {
                break;
            }
            for channel in &mut self.input {
                if let Some(tail) = channel.get_mut(self.filled..) {
                    tail.fill(0.0);
                }
            }
            self.process_chunk(generation, Some(expected), output)?;
        }
        if self.emitted != expected {
            return Err(PipelineError::IncompleteDrain {
                expected,
                actual: self.emitted,
            });
        }
        Ok(())
    }

    /// Returns the exact number of output timestamps strictly before the end of the accepted input.
    fn output_length(&self) -> u64 {
        (self.received_input * u64::from(self.output_rate)).div_ceil(u64::from(self.source_rate))
    }

    /// Resamples one full input chunk and appends the output frames.
    fn process_chunk(
        &mut self,
        generation: u32,
        output_limit: Option<u64>,
        output: &mut Vec<Frame>,
    ) -> Result<(), PipelineError> {
        let output_capacity = self.output.first().map_or(0, Vec::len);
        let input =
            SequentialSliceOfVecs::new(&self.input, 2, RESAMPLER_CHUNK_FRAMES).map_err(PipelineError::InputBuffer)?;
        let mut resampled = SequentialSliceOfVecs::new_mut(&mut self.output, 2, output_capacity)
            .map_err(PipelineError::OutputBuffer)?;
        let (_, written) = self
            .resampler
            .process_into_buffer(&input, &mut resampled, None)
            .map_err(PipelineError::Resample)?;

        self.processed_input += RESAMPLER_CHUNK_FRAMES as u64;
        let skipped = self.skip_output.min(written);
        self.skip_output -= skipped;
        let (Some(left), Some(right)) = (self.output.first(), self.output.get(1)) else {
            return Ok(());
        };
        // Positions follow the emitted frames after the delay, so the playhead lines up with the audio.
        for index in skipped..written {
            if self.emitted >= output_limit.unwrap_or_else(|| self.output_length()) {
                break;
            }
            let position =
                self.start_position + self.emitted * u64::from(self.source_rate) / u64::from(self.output_rate);
            output.push(Frame {
                generation,
                position,
                left: left.get(index).copied().unwrap_or_default(),
                right: right.get(index).copied().unwrap_or_default(),
            });
            self.emitted += 1;
        }
        self.filled = 0;
        Ok(())
    }
}

#[cfg(test)]
mod test_frame_pipeline {
    use super::*;

    /// Runs the pipeline to the end of the file and returns every frame.
    fn produce_all(pipeline: &mut FramePipeline) -> Vec<Frame> {
        let mut frames = Vec::new();
        while pipeline.produce(&mut frames).expect("decodes") {}
        frames
    }

    /// Writes a small mono WAV with frame-identifying samples at an arbitrary integer rate.
    fn ramp_wav(rate: u32, frames: u32) -> tempfile::NamedTempFile {
        let data_bytes = frames * 2;
        let mut wav = Vec::new();
        wav.extend_from_slice(b"RIFF");
        wav.extend_from_slice(&(36 + data_bytes).to_le_bytes());
        wav.extend_from_slice(b"WAVEfmt ");
        wav.extend_from_slice(&16_u32.to_le_bytes());
        wav.extend_from_slice(&1_u16.to_le_bytes());
        wav.extend_from_slice(&1_u16.to_le_bytes());
        wav.extend_from_slice(&rate.to_le_bytes());
        wav.extend_from_slice(&(rate * 2).to_le_bytes());
        wav.extend_from_slice(&2_u16.to_le_bytes());
        wav.extend_from_slice(&16_u16.to_le_bytes());
        wav.extend_from_slice(b"data");
        wav.extend_from_slice(&data_bytes.to_le_bytes());
        for index in 0..frames {
            wav.extend_from_slice(&((index % 4096) as i16).to_le_bytes());
        }
        let file = tempfile::Builder::new()
            .suffix(".wav")
            .tempfile()
            .expect("temporary WAV");
        std::fs::write(file.path(), wav).expect("written");
        file
    }

    /// Checks exact output length and rational source positions across a short-stream matrix.
    fn assert_eof_lengths(source_rate: u32, output_rate: u32, lengths: &[u32]) {
        for &length in lengths {
            let file = ramp_wav(source_rate, length);
            let mut pipeline = FramePipeline::open(file.path(), output_rate, 9, 0.0).expect("opens");
            let frames = produce_all(&mut pipeline);
            let expected = (u64::from(length) * u64::from(output_rate)).div_ceil(u64::from(source_rate));
            assert_eq!(
                frames.len() as u64,
                expected,
                "{source_rate} -> {output_rate}, length {length}"
            );
            for (index, frame) in frames.iter().enumerate() {
                assert_eq!(frame.generation, 9);
                assert_eq!(
                    frame.position,
                    index as u64 * u64::from(source_rate) / u64::from(output_rate)
                );
                assert!(frame.left.is_finite());
            }
            assert!(!pipeline.produce(&mut Vec::new()).expect("already finished"));
        }
    }

    #[test]
    fn eof_drains_exact_lengths_for_common_rates() {
        assert_eof_lengths(44_100, 48_000, &[1, 147, 1023, 44_100]);
        assert_eof_lengths(48_000, 44_100, &[1, 147, 1023, 48_000]);
    }

    #[test]
    fn eof_drains_exact_short_lengths_for_coprime_rates() {
        assert_eof_lengths(44_101, 48_000, &[1, 147, 1023]);
    }

    #[test]
    fn eof_drains_a_complete_second_at_coprime_rates() {
        assert_eof_lengths(44_101, 48_000, &[44_101]);
    }

    #[test]
    fn resampled_seeks_reset_delay_and_exact_length_accounting() {
        for (source_rate, output_rate) in [(44_100, 48_000), (48_000, 44_100), (44_101, 48_000)] {
            let length = 4097;
            let file = ramp_wav(source_rate, length);
            let mut pipeline = FramePipeline::open(file.path(), output_rate, 1, 0.0).expect("opens");
            produce_all(&mut pipeline);
            for (generation, target) in [(2, 137), (3, 4096), (4, 4097), (5, 0)] {
                pipeline
                    .seek(f64::from(target) / f64::from(source_rate), generation)
                    .expect("seeks");
                let frames = produce_all(&mut pipeline);
                let expected = (u64::from(length - target) * u64::from(output_rate)).div_ceil(u64::from(source_rate));
                assert_eq!(
                    frames.len() as u64,
                    expected,
                    "{source_rate} -> {output_rate}, target {target}"
                );
                for (index, frame) in frames.iter().enumerate() {
                    assert_eq!(frame.generation, generation);
                    assert_eq!(
                        frame.position,
                        u64::from(target) + index as u64 * u64::from(source_rate) / u64::from(output_rate)
                    );
                }
            }
        }
    }

    #[test]
    fn same_rate_passes_every_frame() {
        let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
        let mut pipeline = FramePipeline::open(&path, 44_100, 7, 0.0).expect("opens");
        let frames = produce_all(&mut pipeline);
        assert_eq!(frames.len(), 44_100);
        assert!(frames.iter().all(|frame| frame.generation == 7));
        assert!(frames.windows(2).all(|pair| pair[1].position == pair[0].position + 1));
        assert!(pipeline.is_finished());
    }

    #[test]
    fn mono_is_copied_to_both_channels() {
        let path = test_fixtures::fixture("sine_mono_44100_16.wav");
        let mut pipeline = FramePipeline::open(&path, 44_100, 1, 0.0).expect("opens");
        let frames = produce_all(&mut pipeline);
        assert!(
            frames
                .iter()
                .all(|frame| (frame.left - frame.right).abs() < f32::EPSILON)
        );
    }

    #[test]
    fn a_file_with_more_than_two_channels_is_refused() {
        const CHANNELS: u16 = 3;
        const FRAMES: u32 = 64;
        let data_bytes = FRAMES * u32::from(CHANNELS) * 2;
        let mut wav = Vec::new();
        wav.extend_from_slice(b"RIFF");
        wav.extend_from_slice(&(36 + data_bytes).to_le_bytes());
        wav.extend_from_slice(b"WAVEfmt ");
        wav.extend_from_slice(&16_u32.to_le_bytes());
        wav.extend_from_slice(&1_u16.to_le_bytes());
        wav.extend_from_slice(&CHANNELS.to_le_bytes());
        wav.extend_from_slice(&44_100_u32.to_le_bytes());
        wav.extend_from_slice(&(44_100 * u32::from(CHANNELS) * 2).to_le_bytes());
        wav.extend_from_slice(&(CHANNELS * 2).to_le_bytes());
        wav.extend_from_slice(&16_u16.to_le_bytes());
        wav.extend_from_slice(b"data");
        wav.extend_from_slice(&data_bytes.to_le_bytes());
        wav.resize(wav.len() + data_bytes as usize, 0);
        let directory = tempfile::tempdir().expect("temporary directory");
        let path = directory.path().join("three_channels.wav");
        std::fs::write(&path, wav).expect("written");

        let error = FramePipeline::open(&path, 44_100, 1, 0.0)
            .err()
            .expect("three channels are refused");
        assert_eq!(error.to_string(), "Audio preview requires a mono or stereo file");
        assert!(matches!(error, PipelineError::UnsupportedChannels { channels: 3 }));
    }

    #[test]
    fn resampling_produces_the_output_rate() {
        let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
        let mut pipeline = FramePipeline::open(&path, 48_000, 1, 0.0).expect("opens");
        let frames = produce_all(&mut pipeline);
        assert_eq!(frames.len(), 48_000);
        assert!(frames.windows(2).all(|pair| pair[1].position >= pair[0].position));
        let peak = frames.iter().map(|frame| frame.left.abs()).fold(0.0_f32, f32::max);
        assert!((peak - 0.5).abs() < 0.05, "peak {peak}");
    }

    #[test]
    fn seek_skips_to_the_requested_frame() {
        let path = test_fixtures::fixture("sine_stereo_48000_24.flac");
        let mut pipeline = FramePipeline::open(&path, 48_000, 1, 0.5).expect("opens");
        let frames = produce_all(&mut pipeline);
        assert_eq!(frames.first().map(|frame| frame.position), Some(24_000));
        assert_eq!(frames.len(), 24_000);
        pipeline.seek(0.25, 2).expect("seeks");
        let frames = produce_all(&mut pipeline);
        assert_eq!(
            frames.first().map(|frame| (frame.position, frame.generation)),
            Some((12_000, 2))
        );
    }

    #[test]
    fn decoder_failures_keep_their_concrete_source_chain() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let error = FramePipeline::open(&directory.path().join("missing.wav"), 44_100, 1, 0.0)
            .err()
            .expect("missing file is refused");
        let PipelineError::Decode(source) = &error else {
            panic!("expected a decoder error");
        };
        assert_eq!(error.to_string(), source.to_string());
        assert!(std::error::Error::source(&error).is_some());
        assert!(error.message().starts_with(&source.to_string()));
    }

    #[test]
    fn invalid_buffer_dimensions_retain_sources_without_repeating_event_text() {
        let error = PipelineError::InputBuffer(SizeError::ChannelsContainer { actual: 1, required: 2 });
        assert!(std::error::Error::source(&error).is_some());
        assert_eq!(
            error.message(),
            "Invalid resampler input buffer: ChannelsContainer { actual: 1, required: 2 }"
        );
        let error = PipelineError::OutputBuffer(SizeError::ChannelsContainer { actual: 1, required: 2 });
        assert!(std::error::Error::source(&error).is_some());
        assert_eq!(
            error.message(),
            "Invalid resampler output buffer: ChannelsContainer { actual: 1, required: 2 }"
        );
    }

    #[test]
    fn decoder_messages_with_embedded_sources_are_not_repeated_in_events() {
        for error in [
            DecodeError::Seek(std::io::Error::other("seek unavailable").into()),
            DecodeError::Decode {
                position: 42,
                source: std::io::Error::other("invalid packet").into(),
            },
        ] {
            let expected = error.to_string();
            let error = PipelineError::from(error);
            assert!(std::error::Error::source(&error).is_some());
            assert_eq!(error.message(), expected);
        }
    }
}
