//! The realtime side of playback: shared state and the audio callback renderer.
//!
//! `SharedState` holds the atomics the GUI, the decode thread, and the audio callback communicate through.
//! `OutputRenderer` runs inside the cpal callback.
//! It never allocates, locks, or blocks:
//! it drops frames from older generations, applies the gain, writes the device channels,
//! and publishes the source position of the last played frame for the playhead.

use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use rtrb::Consumer;

use crate::pipeline::Frame;

/// Generation value meaning that no file is loaded.
pub const NO_GENERATION: u32 = 0;

/// Bit pattern of a linear gain of 1.0 as an `f32`.
const UNITY_GAIN_BITS: u32 = 0x3F80_0000;

/// State shared between the GUI thread, the decode thread, and the audio callback.
#[derive(Debug)]
pub struct SharedState {
    /// Current load or seek. Frames of other generations are stale.
    pub generation: AtomicU32,
    /// True while the user wants audio to play.
    pub playing: AtomicBool,
    /// Linear output gain, stored as `f32` bits.
    pub gain_bits: AtomicU32,
    /// Source position of the last played frame, or of the seek target until playback catches up.
    pub position: AtomicU64,
    /// Sample rate of the loaded file, 0 when nothing is loaded.
    pub source_rate: AtomicU32,
    /// Length of the loaded file in source frames, 0 when unknown.
    pub source_frames: AtomicU64,
    /// Generation whose decoding has reached the end of the file.
    pub finished_generation: AtomicU32,
    /// Set by the callback when playback ran to the end, cleared when read.
    pub ended: AtomicBool,
    /// Count of callbacks that ran out of buffered audio while playing.
    pub underruns: AtomicU64,
    /// Frames the last audio callback asked for, the device buffer size in practice, 0 before the first callback.
    pub callback_frames: AtomicU32,
}

/// Renders buffered frames into the device buffer inside the audio callback.
pub struct OutputRenderer {
    shared: std::sync::Arc<SharedState>,
    consumer: Consumer<Frame>,
    channels: usize,
}

impl SharedState {
    /// Creates the state for an idle player with unity gain.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            generation: AtomicU32::new(NO_GENERATION),
            playing: AtomicBool::new(false),
            gain_bits: AtomicU32::new(UNITY_GAIN_BITS),
            position: AtomicU64::new(0),
            source_rate: AtomicU32::new(0),
            source_frames: AtomicU64::new(0),
            finished_generation: AtomicU32::new(NO_GENERATION),
            ended: AtomicBool::new(false),
            callback_frames: AtomicU32::new(0),
            underruns: AtomicU64::new(0),
        }
    }

    /// Returns the linear output gain.
    #[must_use]
    pub fn gain(&self) -> f32 {
        f32::from_bits(self.gain_bits.load(Ordering::Relaxed))
    }
}

impl Default for SharedState {
    fn default() -> Self {
        Self::new()
    }
}

impl OutputRenderer {
    /// Creates a renderer for a device with the given channel count.
    #[must_use]
    pub const fn new(shared: std::sync::Arc<SharedState>, consumer: Consumer<Frame>, channels: usize) -> Self {
        Self {
            shared,
            consumer,
            channels,
        }
    }

    /// Fills an interleaved device buffer, converting each sample with `convert`.
    /// Outputs silence while paused, before the first frame of the current generation arrives, and after the end.
    pub fn render<T: Copy>(&mut self, output: &mut [T], convert: impl Fn(f32) -> T) {
        let generation = self.shared.generation.load(Ordering::Acquire);
        let playing = self.shared.playing.load(Ordering::Relaxed);
        let gain = self.shared.gain();
        let channels = self.channels.max(1);
        let mut last_position = None;
        let mut ran_out = false;
        self.shared.callback_frames.store(
            u32::try_from(output.len() / channels).unwrap_or(u32::MAX),
            Ordering::Relaxed,
        );

        for device_frame in output.chunks_mut(channels) {
            let frame = if playing { self.next_frame(generation) } else { None };
            if playing && frame.is_none() {
                ran_out = true;
            }
            let (left, right) = frame.map_or((0.0, 0.0), |frame| {
                last_position = Some(frame.position + 1);
                (frame.left * gain, frame.right * gain)
            });
            write_frame(device_frame, left, right, &convert);
        }

        if let Some(position) = last_position {
            self.shared.position.store(position, Ordering::Relaxed);
        }
        if playing && ran_out {
            if self.shared.finished_generation.load(Ordering::Acquire) == generation && generation != NO_GENERATION {
                self.shared.playing.store(false, Ordering::Relaxed);
                self.shared.ended.store(true, Ordering::Release);
            } else {
                self.shared.underruns.fetch_add(1, Ordering::Relaxed);
            }
        }
    }

    /// Pops the next frame of the current generation, discarding stale frames on the way.
    fn next_frame(&mut self, generation: u32) -> Option<Frame> {
        while let Ok(frame) = self.consumer.pop() {
            if frame.generation == generation {
                return Some(frame);
            }
        }
        None
    }
}

/// Writes one stereo frame to a device frame of any channel count.
/// Mono devices get the average, and channels past the first two stay silent.
fn write_frame<T: Copy>(device_frame: &mut [T], left: f32, right: f32, convert: &impl Fn(f32) -> T) {
    match device_frame {
        [mono] => *mono = convert(f32::midpoint(left, right)),
        [first, second, rest @ ..] => {
            *first = convert(left);
            *second = convert(right);
            for sample in rest {
                *sample = convert(0.0);
            }
        }
        [] => {}
    }
}

#[cfg(test)]
mod test_output_renderer {
    use super::*;

    use std::sync::Arc;

    use rtrb::RingBuffer;

    fn frame(generation: u32, position: u64, value: f32) -> Frame {
        Frame {
            generation,
            position,
            left: value,
            right: -value,
        }
    }

    fn renderer(frames: &[Frame], channels: usize) -> (OutputRenderer, Arc<SharedState>) {
        let (mut producer, consumer) = RingBuffer::new(64);
        for &value in frames {
            producer.push(value).expect("capacity");
        }
        let shared = Arc::new(SharedState::new());
        (OutputRenderer::new(Arc::clone(&shared), consumer, channels), shared)
    }

    #[test]
    fn paused_output_is_silent() {
        let (mut renderer, shared) = renderer(&[frame(1, 0, 0.5)], 2);
        shared.generation.store(1, Ordering::Relaxed);
        let mut output = [1.0_f32; 4];
        renderer.render(&mut output, |sample| sample);
        assert_eq!(output, [0.0; 4]);
    }

    #[test]
    fn stale_frames_are_skipped_and_gain_applied() {
        let (mut renderer, shared) = renderer(&[frame(1, 0, 0.9), frame(2, 100, 0.5), frame(2, 101, 0.25)], 2);
        shared.generation.store(2, Ordering::Relaxed);
        shared.playing.store(true, Ordering::Relaxed);
        shared.gain_bits.store(0.5_f32.to_bits(), Ordering::Relaxed);
        let mut output = [0.0_f32; 4];
        renderer.render(&mut output, |sample| sample);
        assert_eq!(output, [0.25, -0.25, 0.125, -0.125]);
        assert_eq!(shared.position.load(Ordering::Relaxed), 102);
    }

    #[test]
    fn end_of_file_stops_playback() {
        let (mut renderer, shared) = renderer(&[frame(3, 0, 0.5)], 2);
        shared.generation.store(3, Ordering::Relaxed);
        shared.finished_generation.store(3, Ordering::Relaxed);
        shared.playing.store(true, Ordering::Relaxed);
        let mut output = [0.0_f32; 4];
        renderer.render(&mut output, |sample| sample);
        assert!(!shared.playing.load(Ordering::Relaxed));
        assert!(shared.ended.load(Ordering::Relaxed));
    }

    #[test]
    fn empty_buffer_before_the_end_counts_an_underrun() {
        let (mut renderer, shared) = renderer(&[], 2);
        shared.generation.store(1, Ordering::Relaxed);
        shared.playing.store(true, Ordering::Relaxed);
        let mut output = [0.0_f32; 4];
        renderer.render(&mut output, |sample| sample);
        assert!(shared.playing.load(Ordering::Relaxed));
        assert_eq!(shared.underruns.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn new_state_has_unity_gain() {
        assert_eq!(UNITY_GAIN_BITS, 1.0_f32.to_bits());
        assert!((SharedState::new().gain() - 1.0).abs() < f32::EPSILON);
    }

    #[test]
    fn device_channel_counts_are_mapped() {
        let mut mono = [0.0_f32; 1];
        write_frame(&mut mono, 0.5, 0.25, &|sample| sample);
        assert!((mono[0] - 0.375).abs() < f32::EPSILON);
        let mut surround = [1.0_f32; 4];
        write_frame(&mut surround, 0.5, 0.25, &|sample| sample);
        assert_eq!(surround, [0.5, 0.25, 0.0, 0.0]);
    }
}
