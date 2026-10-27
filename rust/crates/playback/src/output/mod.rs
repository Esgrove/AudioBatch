//! The realtime side of playback: shared state and the audio callback renderer.
//!
//! `SharedState` holds the atomics the GUI, the decode thread, and the audio callback communicate through.
//! `OutputRenderer` runs inside the cpal callback.
//! It never allocates, locks, or blocks:
//! it drops frames from older generations, writes the device channels,
//! and publishes the source position of the last played frame for the playhead.
//! While a preview insert is attached, playback goes through it instead, see `insert`,
//! and only then is the insert gain applied, before the insert.

pub mod insert;

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use rtrb::Consumer;

use crate::pipeline::Frame;
use insert::{DeviceFormat, InsertRenderer};

/// Generation value meaning that no file is loaded.
pub const NO_GENERATION: u32 = 0;

/// Bit pattern of a linear gain of 1.0 as an `f32`.
const UNITY_GAIN_BITS: u32 = 0x3F80_0000;

/// State shared between the GUI thread, the decode thread, and the audio callback.
#[derive(Debug)]
pub struct SharedState {
    /// Current load or seek. Frames of other generations are stale.
    pub generation: AtomicU32,
    /// Generation of the latest load. The decode thread skips older loads and does not publish their properties.
    pub load_generation: AtomicU32,
    /// True while the user wants audio to play.
    pub playing: AtomicBool,
    /// Linear gain applied before the preview insert, stored as `f32` bits.
    pub insert_gain_bits: AtomicU32,
    /// Source position of the last played frame, or of the seek target until playback catches up.
    pub position: AtomicU64,
    /// Sample rate of the loaded file, 0 when nothing is loaded.
    pub source_rate: AtomicU32,
    /// Length of the loaded file in source frames, 0 when unknown.
    pub source_frames: AtomicU64,
    /// True when the loaded file has one channel,
    /// so the output of the preview insert is folded back to mono, the way processing writes the file.
    pub source_mono: AtomicBool,
    /// Generation whose decoding has reached the end of the file.
    pub finished_generation: AtomicU32,
    /// Set by the callback when playback ran to the end, cleared when read.
    pub ended: AtomicBool,
    /// Generation whose playback ran to the end, so playing again starts over.
    pub ended_generation: AtomicU32,
    /// Count of callbacks that ran out of buffered audio while playing.
    pub underruns: AtomicU64,
    /// Frames the last audio callback asked for, the device buffer size in practice, 0 before the first callback.
    pub callback_frames: AtomicU32,
    /// Number of insert changes the audio callback has made.
    pub insert_changes_applied: AtomicU64,
    /// Frames of output that may follow the end of the input in the current insert.
    pub insert_tail_frames: AtomicU64,
    /// Whether a reset insert is pre-rolled for its latency, so the output lines up with the playhead.
    pub latency_compensation: AtomicBool,
    /// Set by the callback when the insert produced output that is not finite, cleared when read.
    pub insert_failed: AtomicBool,
    /// Set by the stream error callback when the stream stopped for good, such as when its device went away,
    /// so the next play opens a new stream instead of reusing the dead one.
    pub stream_lost: AtomicBool,
}

/// Renders buffered frames into the device buffer inside the audio callback.
pub struct OutputRenderer {
    shared: Arc<SharedState>,
    consumer: Consumer<Frame>,
    device: DeviceFormat,
    insert: InsertRenderer,
}

impl SharedState {
    /// Creates the state for an idle player with unity gain.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            generation: AtomicU32::new(NO_GENERATION),
            load_generation: AtomicU32::new(NO_GENERATION),
            playing: AtomicBool::new(false),
            insert_gain_bits: AtomicU32::new(UNITY_GAIN_BITS),
            position: AtomicU64::new(0),
            source_rate: AtomicU32::new(0),
            source_frames: AtomicU64::new(0),
            source_mono: AtomicBool::new(false),
            finished_generation: AtomicU32::new(NO_GENERATION),
            ended: AtomicBool::new(false),
            ended_generation: AtomicU32::new(NO_GENERATION),
            callback_frames: AtomicU32::new(0),
            underruns: AtomicU64::new(0),
            insert_changes_applied: AtomicU64::new(0),
            insert_tail_frames: AtomicU64::new(0),
            latency_compensation: AtomicBool::new(true),
            insert_failed: AtomicBool::new(false),
            stream_lost: AtomicBool::new(false),
        }
    }

    /// Returns the linear gain applied before the preview insert.
    #[must_use]
    pub fn insert_gain(&self) -> f32 {
        f32::from_bits(self.insert_gain_bits.load(Ordering::Relaxed))
    }
}

impl Default for SharedState {
    fn default() -> Self {
        Self::new()
    }
}

impl OutputRenderer {
    /// Creates a renderer for a device format, receiving preview inserts through `insert`.
    #[must_use]
    pub const fn new(
        shared: Arc<SharedState>,
        consumer: Consumer<Frame>,
        device: DeviceFormat,
        insert: InsertRenderer,
    ) -> Self {
        Self {
            shared,
            consumer,
            device,
            insert,
        }
    }

    /// Makes the insert changes the player sent, without rendering.
    pub fn apply_insert_changes(&mut self) {
        self.insert.apply_changes(&self.shared);
    }

    /// Fills an interleaved device buffer, converting each sample with `convert`.
    /// Outputs silence while paused, before the first frame of the current generation arrives, and after the end.
    /// Stale frames are dropped while paused too, so a paused seek leaves room for the new position.
    pub fn render<T: Copy>(&mut self, output: &mut [T], convert: impl Fn(f32) -> T) {
        let channels = self.device.channels.max(1);
        self.shared.callback_frames.store(
            u32::try_from(output.len() / channels).unwrap_or(u32::MAX),
            Ordering::Relaxed,
        );
        self.insert.apply_changes(&self.shared);
        if self.insert.has_insert() {
            self.insert
                .render(&mut self.consumer, &self.shared, output, self.device, &convert);
            return;
        }

        let generation = self.shared.generation.load(Ordering::Acquire);
        let playing = self.shared.playing.load(Ordering::Relaxed);
        let mut last_position = None;
        let mut ran_out = false;
        if !playing {
            drop_stale_frames(&mut self.consumer, generation);
        }
        for device_frame in output.chunks_mut(channels) {
            let frame = if playing {
                pop_current_frame(&mut self.consumer, generation)
            } else {
                None
            };
            if playing && frame.is_none() {
                ran_out = true;
            }
            let (left, right) = frame.map_or((0.0, 0.0), |frame| {
                last_position = Some(frame.position + 1);
                (frame.left, frame.right)
            });
            write_frame(device_frame, left, right, &convert);
        }

        if let Some(position) = last_position {
            self.shared.position.store(position, Ordering::Relaxed);
        }
        if playing && ran_out {
            if self.shared.finished_generation.load(Ordering::Acquire) == generation && generation != NO_GENERATION {
                self.shared.playing.store(false, Ordering::Relaxed);
                self.shared.ended_generation.store(generation, Ordering::Release);
                self.shared.ended.store(true, Ordering::Release);
            } else {
                self.shared.underruns.fetch_add(1, Ordering::Relaxed);
            }
        }
    }
}

/// Pops the next frame of the current generation, discarding stale frames on the way.
fn pop_current_frame(consumer: &mut Consumer<Frame>, generation: u32) -> Option<Frame> {
    while let Ok(frame) = consumer.pop() {
        if frame.generation == generation {
            return Some(frame);
        }
    }
    None
}

/// Drops the frames of older generations at the front of the buffer without consuming current ones.
fn drop_stale_frames(consumer: &mut Consumer<Frame>, generation: u32) {
    while consumer.peek().is_ok_and(|frame| frame.generation != generation) {
        let _ = consumer.pop();
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

    use rtrb::RingBuffer;

    use crate::preview_insert::insert_channel;

    /// Returns a stereo frame with the value on both channels.
    fn frame(generation: u32, position: u64, value: f32) -> Frame {
        Frame {
            generation,
            position,
            left: value,
            right: -value,
        }
    }

    /// Creates a renderer for a device with the channel count, with the frames queued, and its shared state.
    fn renderer(frames: &[Frame], channels: usize) -> (OutputRenderer, Arc<SharedState>) {
        let (mut producer, consumer) = RingBuffer::new(64);
        for &value in frames {
            producer.push(value).expect("capacity");
        }
        let shared = Arc::new(SharedState::new());
        let (_control, insert) = insert_channel(Arc::clone(&shared));
        let device = DeviceFormat { channels, rate: 48_000 };
        (
            OutputRenderer::new(Arc::clone(&shared), consumer, device, insert),
            shared,
        )
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
    fn stale_frames_are_skipped_and_the_insert_gain_is_left_to_the_insert() {
        let (mut renderer, shared) = renderer(&[frame(1, 0, 0.9), frame(2, 100, 0.5), frame(2, 101, 0.25)], 2);
        shared.generation.store(2, Ordering::Relaxed);
        shared.playing.store(true, Ordering::Relaxed);
        shared.insert_gain_bits.store(0.5_f32.to_bits(), Ordering::Relaxed);
        let mut output = [0.0_f32; 4];
        renderer.render(&mut output, |sample| sample);
        assert_eq!(output, [0.5, -0.5, 0.25, -0.25]);
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
        assert_eq!(shared.ended_generation.load(Ordering::Relaxed), 3);
    }

    #[test]
    fn paused_output_drops_stale_frames_and_keeps_current_ones() {
        let (mut renderer, shared) = renderer(&[frame(1, 0, 0.9), frame(1, 1, 0.9), frame(2, 50, 0.5)], 2);
        shared.generation.store(2, Ordering::Relaxed);
        let mut output = [0.0_f32; 2];
        renderer.render(&mut output, |sample| sample);
        assert_eq!(output, [0.0; 2]);
        assert_eq!(renderer.consumer.slots(), 1);
        shared.playing.store(true, Ordering::Relaxed);
        renderer.render(&mut output, |sample| sample);
        assert_eq!(output, [0.5, -0.5]);
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
        assert!((SharedState::new().insert_gain() - 1.0).abs() < f32::EPSILON);
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
