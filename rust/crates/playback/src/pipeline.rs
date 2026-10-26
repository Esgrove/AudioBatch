//! Turns a decoded file into stereo frames at the output device rate.
//!
//! `FramePipeline` runs on the decode thread.
//! It decodes packets with the shared core decoder, maps the source channels to stereo,
//! resamples with rubato when the file and device rates differ,
//! and tags every frame with the playback generation and its position in the source,
//! so the audio callback can drop stale frames after a seek and report an exact playhead.

use std::path::Path;

use anyhow::{Context, Result};
use audioadapter_buffers::direct::SequentialSliceOfVecs;
use audiobatch_core::decode::{AudioFileDecoder, StreamInfo};
use rubato::{Fft, FixedSync, Resampler};

/// Input chunk size of the resampler in frames.
const RESAMPLER_CHUNK_FRAMES: usize = 1024;

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
    output_rate: u32,
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
    /// Source frames per output frame.
    step: f64,
    /// Output frames still to drop to compensate for the resampler delay.
    skip_output: usize,
}

impl FramePipeline {
    /// Opens a file for playback at the given output rate, starting at `start_seconds`.
    ///
    /// # Errors
    ///
    /// Returns an error when the file cannot be decoded, seeked, or resampled.
    pub fn open(path: &Path, output_rate: u32, generation: u32, start_seconds: f64) -> Result<Self> {
        let decoder = AudioFileDecoder::open(path)?;
        let source_rate = decoder.info().sample_rate;
        let resampler = (source_rate != output_rate)
            .then(|| ResamplerStage::new(source_rate, output_rate))
            .transpose()?;
        let mut pipeline = Self {
            decoder,
            generation,
            output_rate,
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

    /// Returns the output sample rate.
    #[must_use]
    pub const fn output_rate(&self) -> u32 {
        self.output_rate
    }

    /// Moves to the given time and starts tagging frames with a new generation.
    ///
    /// # Errors
    ///
    /// Returns an error when the container cannot seek.
    pub fn seek(&mut self, seconds: f64, generation: u32) -> Result<()> {
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
    pub fn produce(&mut self, output: &mut Vec<Frame>) -> Result<bool> {
        if self.finished {
            return Ok(false);
        }
        let Some(chunk) = self.decoder.next_chunk()? else {
            if let Some(stage) = &mut self.resampler {
                stage.flush(self.position, self.generation, output)?;
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
    fn new(source_rate: u32, output_rate: u32) -> Result<Self> {
        let resampler = Fft::<f32>::new(
            source_rate as usize,
            output_rate as usize,
            RESAMPLER_CHUNK_FRAMES,
            2,
            FixedSync::Input,
        )
        .context("Could not create the resampler")?;
        let output_capacity = resampler.output_frames_max();
        let skip_output = resampler.output_delay();
        Ok(Self {
            resampler,
            input: vec![vec![0.0; RESAMPLER_CHUNK_FRAMES]; 2],
            output: vec![vec![0.0; output_capacity]; 2],
            filled: 0,
            start_position: 0,
            emitted: 0,
            step: f64::from(source_rate) / f64::from(output_rate),
            skip_output,
        })
    }

    /// Clears the resampler state for a new start position.
    fn reset(&mut self, position: u64) {
        self.resampler.reset();
        self.filled = 0;
        self.start_position = position;
        self.emitted = 0;
        self.skip_output = self.resampler.output_delay();
    }

    /// Returns the source position of the next emitted frame.
    fn next_position(&self) -> u64 {
        self.start_position + (self.emitted as f64 * self.step) as u64
    }

    /// Buffers decoded samples and resamples every complete input chunk.
    fn push(&mut self, left: &[f32], right: &[f32], generation: u32, output: &mut Vec<Frame>) -> Result<()> {
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
    /// Frames past the end of the source are dropped.
    fn flush(&mut self, end_position: u64, generation: u32, output: &mut Vec<Frame>) -> Result<()> {
        let mut remaining_chunks = 1 + self.skip_output.div_ceil(RESAMPLER_CHUNK_FRAMES) + 1;
        while remaining_chunks > 0 && self.next_position() < end_position {
            for channel in &mut self.input {
                if let Some(tail) = channel.get_mut(self.filled..) {
                    tail.fill(0.0);
                }
            }
            self.filled = RESAMPLER_CHUNK_FRAMES;
            self.process_chunk(generation, Some(end_position), output)?;
            remaining_chunks -= 1;
        }
        Ok(())
    }

    /// Resamples one full input chunk and appends the output frames.
    fn process_chunk(&mut self, generation: u32, end_position: Option<u64>, output: &mut Vec<Frame>) -> Result<()> {
        let output_capacity = self.output.first().map_or(0, Vec::len);
        let input = SequentialSliceOfVecs::new(&self.input, 2, RESAMPLER_CHUNK_FRAMES)
            .map_err(|error| anyhow::anyhow!("Invalid resampler input buffer: {error:?}"))?;
        let mut resampled = SequentialSliceOfVecs::new_mut(&mut self.output, 2, output_capacity)
            .map_err(|error| anyhow::anyhow!("Invalid resampler output buffer: {error:?}"))?;
        let (_, written) = self
            .resampler
            .process_into_buffer(&input, &mut resampled, None)
            .context("Resampling failed")?;

        let skipped = self.skip_output.min(written);
        self.skip_output -= skipped;
        let (Some(left), Some(right)) = (self.output.first(), self.output.get(1)) else {
            return Ok(());
        };
        // Positions follow the emitted frames after the delay, so the playhead lines up with the audio.
        for index in skipped..written {
            let position = self.start_position + (self.emitted as f64 * self.step) as u64;
            if end_position.is_some_and(|end| position >= end) {
                break;
            }
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

    fn produce_all(pipeline: &mut FramePipeline) -> Vec<Frame> {
        let mut frames = Vec::new();
        while pipeline.produce(&mut frames).expect("decodes") {}
        frames
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
    fn resampling_produces_the_output_rate() {
        let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
        let mut pipeline = FramePipeline::open(&path, 48_000, 1, 0.0).expect("opens");
        let frames = produce_all(&mut pipeline);
        let expected = 48_000_i64;
        let produced = i64::try_from(frames.len()).expect("fits");
        assert!((produced - expected).abs() < 1200, "{produced} frames");
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
}
