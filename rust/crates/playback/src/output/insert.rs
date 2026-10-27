//! Playback through a preview insert inside the audio callback.
//!
//! Frames are pulled from the ring buffer into preallocated stereo blocks,
//! the gain is applied, and the insert processes blocks no longer than it accepts.
//! Each start, seek, resume from pause, and insert change resets the insert.
//! With latency compensation on, the reset is followed by a silent pre-roll of the insert's latency,
//! while the playhead stays still, so what is heard lines up with the waveform.
//! The first frames after a reset fade in, and a pause fades out, so neither clicks.
//! At the end of the file, silence is fed through the insert for its latency and tail
//! while the playhead holds at the end, so delayed audio and decaying tails are heard to the end.
//! Output that is not finite stops playback, and the player reports it.

use std::sync::atomic::Ordering;

use rtrb::{Consumer, Producer};

use super::{NO_GENERATION, SharedState, drop_stale_frames, pop_current_frame, write_frame};
use crate::pipeline::Frame;
use crate::preview_insert::PreviewInsert;

/// Length of the fade after a reset and of the fade when pausing, in frames.
pub const TRANSITION_FRAMES: usize = 64;

/// Most frames the insert processes in one call, whatever it accepts. The blocks are allocated up front.
const SCRATCH_FRAMES: usize = 4096;

/// The channel count and the sample rate of the output device.
#[derive(Debug, Clone, Copy)]
pub struct DeviceFormat {
    /// Interleaved channels per device frame.
    pub channels: usize,
    /// Frames per second.
    pub rate: u32,
}

/// The audio callback's side of the insert queues, and the playback through the current insert.
pub struct InsertRenderer {
    /// Receives the next insert, or none to remove the current one.
    commands: Consumer<Option<Box<dyn PreviewInsert>>>,
    /// Hands replaced inserts back to the player.
    retired: Producer<Box<dyn PreviewInsert>>,
    /// The current insert.
    insert: Option<Box<dyn PreviewInsert>>,
    /// Left channel block.
    left: Vec<f32>,
    /// Right channel block.
    right: Vec<f32>,
    /// Where playback through the insert is.
    playback: InsertPlayback,
}

/// The state of playback through the insert between callbacks.
#[derive(Debug, Default)]
struct InsertPlayback {
    /// True from a reset until playback stops, ends, or fails.
    active: bool,
    /// Generation the insert was last reset for.
    generation: u32,
    /// Set when the insert changed, so the next playing callback resets it.
    needs_reset: bool,
    /// Frames of the silent pre-roll still to process.
    pre_roll: usize,
    /// Frames of the fade-in already output.
    faded_in: usize,
    /// Frames of the pause fade still to output, while pausing.
    fade_out: Option<usize>,
    /// Frames of silence still to feed through after the end of the file, while draining.
    drain: Option<usize>,
    /// Source position of the first frame pulled after the reset.
    start_position: Option<u64>,
    /// Source position after the last frame pulled.
    end_position: u64,
    /// Frames output since the reset, after the pre-roll.
    emitted: u64,
}

impl InsertRenderer {
    /// Creates the audio callback's side of the insert queues, with blocks allocated for the largest insert block.
    pub fn new(commands: Consumer<Option<Box<dyn PreviewInsert>>>, retired: Producer<Box<dyn PreviewInsert>>) -> Self {
        Self {
            commands,
            retired,
            insert: None,
            left: vec![0.0; SCRATCH_FRAMES],
            right: vec![0.0; SCRATCH_FRAMES],
            playback: InsertPlayback::default(),
        }
    }

    /// Returns true when an insert is attached.
    pub const fn has_insert(&self) -> bool {
        self.insert.is_some()
    }

    /// Makes the insert changes the player sent since the last callback.
    /// A change is only made while the retire queue has room for the insert it replaces,
    /// so an insert is never dropped on the audio thread.
    pub fn apply_changes(&mut self, shared: &SharedState) {
        while self.retired.slots() > 0 {
            let Ok(next) = self.commands.pop() else {
                break;
            };
            if let Some(replaced) = std::mem::replace(&mut self.insert, next) {
                let _ = self.retired.push(replaced);
            }
            self.playback.needs_reset = true;
            shared.insert_changes_applied.fetch_add(1, Ordering::Release);
        }
    }

    /// Fills an interleaved device buffer through the insert, converting each sample with `convert`.
    pub fn render<T: Copy>(
        &mut self,
        frames: &mut Consumer<Frame>,
        shared: &SharedState,
        output: &mut [T],
        device: DeviceFormat,
        convert: &impl Fn(f32) -> T,
    ) {
        for sample in output.iter_mut() {
            *sample = convert(0.0);
        }
        let Some(insert) = self.insert.as_deref_mut() else {
            return;
        };
        let playback = &mut self.playback;
        let generation = shared.generation.load(Ordering::Acquire);
        if generation == NO_GENERATION {
            playback.active = false;
            return;
        }
        playback.follow_transport(insert, shared, generation);
        if !playback.active {
            drop_stale_frames(frames, generation);
            return;
        }

        let channels = device.channels.max(1);
        let total = output.len() / channels;
        let block_limit = insert.max_block_frames().clamp(1, SCRATCH_FRAMES);
        let gain = shared.gain();
        let mut offset = 0;
        let mut underrun = false;
        while offset < total && playback.active {
            let mut length = (total - offset).min(block_limit);
            if playback.pre_roll > 0 {
                length = length.min(playback.pre_roll);
            }
            if let Some(remaining) = playback.fade_out {
                length = length.min(remaining);
            }
            let (Some(left), Some(right)) = (self.left.get_mut(..length), self.right.get_mut(..length)) else {
                break;
            };
            left.fill(0.0);
            right.fill(0.0);
            if let Some(remaining) = playback.drain {
                length = length.min(remaining);
                playback.drain = Some(remaining - length);
            } else {
                let pulled = playback.pull(frames, generation, gain, left, right);
                if pulled < length {
                    if shared.finished_generation.load(Ordering::Acquire) == generation {
                        let drain =
                            insert.latency_frames() + shared.insert_tail_frames.load(Ordering::Relaxed) as usize;
                        let silence = (length - pulled).min(drain);
                        length = pulled + silence;
                        playback.drain = Some(drain - silence);
                    } else {
                        length = pulled;
                        underrun = true;
                    }
                }
            }
            let (Some(left), Some(right)) = (left.get_mut(..length), right.get_mut(..length)) else {
                break;
            };
            if length > 0 && !insert.process(left, right) {
                playback.fail(shared);
                for sample in output.iter_mut() {
                    *sample = convert(0.0);
                }
                return;
            }
            if playback.pre_roll > 0 {
                playback.pre_roll -= length;
            } else if let Some(device_frames) = output.get_mut(offset * channels..(offset + length) * channels) {
                playback.write(device_frames, channels, left, right, convert);
            }
            offset += length;
            if playback.drain == Some(0) {
                playback.end(shared, generation);
                return;
            }
            if playback.fade_out == Some(0) {
                playback.active = false;
            }
            if underrun || length == 0 {
                break;
            }
        }
        playback.publish_position(shared, device.rate);
        if underrun {
            shared.underruns.fetch_add(1, Ordering::Relaxed);
        }
    }
}

impl InsertPlayback {
    /// Follows the player's transport.
    /// Playing after a stop, a pause, a seek, or an insert change resets the insert.
    /// A pause fades out, except during the pre-roll, where nothing is heard yet, and after a seek.
    fn follow_transport(&mut self, insert: &mut dyn PreviewInsert, shared: &SharedState, generation: u32) {
        if shared.playing.load(Ordering::Relaxed) {
            if !self.active || self.fade_out.is_some() || self.needs_reset || self.generation != generation {
                self.reset(insert, generation, shared.latency_compensation.load(Ordering::Relaxed));
            }
        } else if self.active {
            if self.pre_roll > 0 || self.generation != generation {
                self.active = false;
            } else if self.fade_out.is_none() {
                self.fade_out = Some(TRANSITION_FRAMES);
            }
        }
    }

    /// Stops playback after the insert produced output that is not finite, for the player to report.
    fn fail(&mut self, shared: &SharedState) {
        self.active = false;
        shared.playing.store(false, Ordering::Relaxed);
        shared.insert_failed.store(true, Ordering::Release);
    }

    /// Ends playback after the latency and the tail were drained, with the playhead at the end.
    fn end(&mut self, shared: &SharedState, generation: u32) {
        self.active = false;
        shared.position.store(self.end_position, Ordering::Relaxed);
        shared.playing.store(false, Ordering::Relaxed);
        shared.ended_generation.store(generation, Ordering::Release);
        shared.ended.store(true, Ordering::Release);
    }

    /// Resets the insert and starts playback through it for a generation.
    fn reset(&mut self, insert: &mut dyn PreviewInsert, generation: u32, compensate: bool) {
        insert.reset();
        *self = Self {
            active: true,
            generation,
            pre_roll: if compensate { insert.latency_frames() } else { 0 },
            ..Self::default()
        };
    }

    /// Pulls frames of the generation into the blocks with the gain applied, and returns how many it pulled.
    fn pull(
        &mut self,
        frames: &mut Consumer<Frame>,
        generation: u32,
        gain: f32,
        left: &mut [f32],
        right: &mut [f32],
    ) -> usize {
        let mut pulled = 0;
        for (left, right) in left.iter_mut().zip(right.iter_mut()) {
            let Some(frame) = pop_current_frame(frames, generation) else {
                break;
            };
            *left = frame.left * gain;
            *right = frame.right * gain;
            self.start_position.get_or_insert(frame.position);
            self.end_position = frame.position + 1;
            pulled += 1;
        }
        pulled
    }

    /// Writes processed frames to device frames, fading in after a reset and fading out while pausing.
    fn write<T: Copy>(
        &mut self,
        device_frames: &mut [T],
        channels: usize,
        left: &[f32],
        right: &[f32],
        convert: &impl Fn(f32) -> T,
    ) {
        for ((device_frame, &left), &right) in device_frames.chunks_mut(channels).zip(left).zip(right) {
            let mut gain = 1.0;
            if self.faded_in < TRANSITION_FRAMES {
                self.faded_in += 1;
                gain *= self.faded_in as f32 / TRANSITION_FRAMES as f32;
            }
            if let Some(remaining) = self.fade_out.as_mut() {
                gain *= *remaining as f32 / TRANSITION_FRAMES as f32;
                *remaining = remaining.saturating_sub(1);
            }
            write_frame(device_frame, left * gain, right * gain, convert);
        }
        self.emitted += left.len() as u64;
    }

    /// Publishes the source position of the last frame output.
    /// Output frame `n` after the pre-roll is the input frame `n` after the reset,
    /// converted from device frames to source frames, and never past the last frame pulled.
    fn publish_position(&self, shared: &SharedState, device_rate: u32) {
        let source_rate = shared.source_rate.load(Ordering::Relaxed);
        if let (Some(start), true, true) = (
            self.start_position,
            self.emitted > 0,
            source_rate > 0 && device_rate > 0,
        ) {
            let advanced = (self.emitted as f64 * f64::from(source_rate) / f64::from(device_rate)).round() as u64;
            shared
                .position
                .store((start + advanced).min(self.end_position), Ordering::Relaxed);
        }
    }
}

#[cfg(test)]
mod test_insert_playback {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, AtomicUsize};

    use rtrb::RingBuffer;

    use super::*;
    use crate::output::OutputRenderer;
    use crate::preview_insert::{InsertControl, insert_channel};

    /// Device rate and source rate of the tests, equal so device frames are source frames.
    const RATE: u32 = 48_000;

    /// Most frames the fake insert accepts, small so blocks are split.
    const FAKE_BLOCK_FRAMES: usize = 32;

    /// What a fake insert recorded, readable after it moved into the renderer.
    #[derive(Clone, Default)]
    struct Probe {
        processed: Arc<AtomicUsize>,
        resets: Arc<AtomicUsize>,
        dropped: Arc<AtomicBool>,
    }

    /// An insert that delays its input by its latency, and can output NaN instead.
    struct FakeInsert {
        latency: usize,
        tail: usize,
        non_finite: bool,
        delay_left: Vec<f32>,
        delay_right: Vec<f32>,
        index: usize,
        probe: Probe,
    }

    /// A renderer with a ring buffer to fill and the player's side of the insert queues.
    struct Harness {
        renderer: OutputRenderer,
        shared: Arc<SharedState>,
        control: InsertControl,
        frames: Producer<Frame>,
    }

    impl FakeInsert {
        fn new(latency: usize, tail: usize) -> (Box<Self>, Probe) {
            let probe = Probe::default();
            let insert = Self {
                latency,
                tail,
                non_finite: false,
                delay_left: vec![0.0; latency],
                delay_right: vec![0.0; latency],
                index: 0,
                probe: probe.clone(),
            };
            (Box::new(insert), probe)
        }
    }

    impl PreviewInsert for FakeInsert {
        fn max_block_frames(&self) -> usize {
            FAKE_BLOCK_FRAMES
        }

        fn latency_frames(&self) -> usize {
            self.latency
        }

        fn tail_frames(&self) -> usize {
            self.tail
        }

        fn reset(&mut self) {
            self.delay_left.fill(0.0);
            self.delay_right.fill(0.0);
            self.index = 0;
            self.probe.resets.fetch_add(1, Ordering::Relaxed);
        }

        fn process(&mut self, left: &mut [f32], right: &mut [f32]) -> bool {
            self.probe.processed.fetch_add(left.len(), Ordering::Relaxed);
            if self.non_finite {
                left.fill(f32::NAN);
                return false;
            }
            for (left, right) in left.iter_mut().zip(right.iter_mut()) {
                if let (Some(delayed_left), Some(delayed_right)) = (
                    self.delay_left.get_mut(self.index),
                    self.delay_right.get_mut(self.index),
                ) {
                    *left = std::mem::replace(delayed_left, *left);
                    *right = std::mem::replace(delayed_right, *right);
                    self.index = (self.index + 1) % self.latency;
                }
            }
            true
        }
    }

    impl Drop for FakeInsert {
        fn drop(&mut self) {
            self.probe.dropped.store(true, Ordering::Relaxed);
        }
    }

    impl Harness {
        /// Creates a playing harness for generation 1 with an insert attached.
        fn with_insert(insert: Box<FakeInsert>) -> Self {
            let shared = Arc::new(SharedState::new());
            shared.generation.store(1, Ordering::Relaxed);
            shared.playing.store(true, Ordering::Relaxed);
            shared.source_rate.store(RATE, Ordering::Relaxed);
            let (producer, consumer) = RingBuffer::new(4096);
            let (mut control, insert_renderer) = insert_channel(Arc::clone(&shared));
            control.set(Some(insert)).expect("room for the insert");
            let device = DeviceFormat {
                channels: 2,
                rate: RATE,
            };
            Self {
                renderer: OutputRenderer::new(Arc::clone(&shared), consumer, device, insert_renderer),
                shared,
                control,
                frames: producer,
            }
        }

        /// Queues frames of generation 1 at consecutive positions from `start`, with values from `value`.
        fn push(&mut self, start: u64, count: u64, value: impl Fn(u64) -> f32) {
            for position in start..start + count {
                let sample = value(position - start);
                let frame = Frame {
                    generation: 1,
                    position,
                    left: sample,
                    right: sample,
                };
                self.frames.push(frame).expect("ring buffer capacity");
            }
        }

        /// Renders a number of device frames and returns the left channel.
        fn render(&mut self, frames: usize) -> Vec<f32> {
            let mut output = vec![1.0_f32; frames * 2];
            self.renderer.render(&mut output, |sample| sample);
            output.chunks(2).filter_map(|frame| frame.first().copied()).collect()
        }

        fn position(&self) -> u64 {
            self.shared.position.load(Ordering::Relaxed)
        }

        fn is_playing(&self) -> bool {
            self.shared.playing.load(Ordering::Relaxed)
        }
    }

    /// Returns the gain of the fade-in at an output frame after a reset.
    fn fade_in(frame: usize) -> f32 {
        ((frame + 1) as f32 / TRANSITION_FRAMES as f32).min(1.0)
    }

    /// Returns a value that identifies a frame of input.
    fn ramp(index: u64) -> f32 {
        (index + 1) as f32 / 1000.0
    }

    fn assert_close(actual: f32, expected: f32, context: &str) {
        assert!(
            (actual - expected).abs() < 1e-6,
            "{context}: {actual} is not {expected}"
        );
    }

    #[test]
    fn compensation_discards_exactly_the_latency() {
        for latency in [0, 5, 131, 511] {
            let (insert, _probe) = FakeInsert::new(latency, 0);
            let mut harness = Harness::with_insert(insert);
            harness.push(0, 1000, ramp);
            let output = harness.render(latency + 200);
            let (pre_roll, heard) = output.split_at(latency);
            assert!(
                pre_roll.iter().all(|&sample| sample == 0.0),
                "latency {latency}: pre-roll is silent"
            );
            for (index, &sample) in heard.iter().enumerate() {
                let expected = ramp(index as u64) * fade_in(index);
                assert_close(sample, expected, &format!("latency {latency}, frame {index}"));
            }
            assert_eq!(harness.position(), 200, "latency {latency}");
        }
    }

    #[test]
    fn the_playhead_freezes_only_during_the_pre_roll() {
        let (insert, _probe) = FakeInsert::new(100, 0);
        let mut harness = Harness::with_insert(insert);
        harness.shared.position.store(1000, Ordering::Relaxed);
        harness.push(1000, 1000, |_| 0.5);
        harness.render(60);
        assert_eq!(harness.position(), 1000);
        harness.render(60);
        assert_eq!(harness.position(), 1020);
        harness.render(100);
        assert_eq!(harness.position(), 1120);
    }

    #[test]
    fn the_end_drains_the_latency_and_the_tail() {
        let (insert, probe) = FakeInsert::new(10, 30);
        let mut harness = Harness::with_insert(insert);
        harness.push(0, 50, |_| 0.5);
        harness.shared.finished_generation.store(1, Ordering::Relaxed);
        harness.render(80);
        assert!(harness.is_playing());
        assert!(!harness.shared.ended.load(Ordering::Relaxed));
        assert_eq!(harness.position(), 50);
        let output = harness.render(50);
        assert_eq!(probe.processed.load(Ordering::Relaxed), 90);
        assert!(!harness.is_playing());
        assert!(harness.shared.ended.load(Ordering::Relaxed));
        assert_eq!(harness.position(), 50);
        assert!(output.iter().skip(10).all(|&sample| sample == 0.0));
    }

    #[test]
    fn non_finite_output_stops_playback() {
        let (mut insert, _probe) = FakeInsert::new(0, 0);
        insert.non_finite = true;
        let mut harness = Harness::with_insert(insert);
        harness.push(0, 100, |_| 0.5);
        let output = harness.render(64);
        assert!(output.iter().all(|&sample| sample == 0.0));
        assert!(!harness.is_playing());
        assert!(harness.shared.insert_failed.load(Ordering::Relaxed));
    }

    #[test]
    fn a_pause_fades_out_and_a_resume_resets() {
        let (insert, probe) = FakeInsert::new(0, 0);
        let mut harness = Harness::with_insert(insert);
        harness.push(0, 1000, |_| 0.5);
        harness.render(100);
        harness.shared.playing.store(false, Ordering::Relaxed);
        let output = harness.render(100);
        let (fade, silence) = output.split_at(TRANSITION_FRAMES);
        for (index, &sample) in fade.iter().enumerate() {
            let expected = 0.5 * (TRANSITION_FRAMES - index) as f32 / TRANSITION_FRAMES as f32;
            assert_close(sample, expected, &format!("frame {index}"));
        }
        assert!(silence.iter().all(|&sample| sample == 0.0));
        assert_eq!(harness.position(), 164);
        assert_eq!(probe.resets.load(Ordering::Relaxed), 1);
        harness.shared.playing.store(true, Ordering::Relaxed);
        let output = harness.render(10);
        assert_eq!(probe.resets.load(Ordering::Relaxed), 2);
        assert_close(
            output.first().copied().unwrap_or_default(),
            0.5 * fade_in(0),
            "first frame",
        );
    }

    #[test]
    fn a_swap_retires_the_old_insert_without_dropping_it() {
        let (first, first_probe) = FakeInsert::new(0, 0);
        let mut harness = Harness::with_insert(first);
        harness.push(0, 100, |_| 0.5);
        harness.render(10);
        let (second, second_probe) = FakeInsert::new(0, 0);
        harness.control.set(Some(second)).expect("room for the insert");
        assert!(!harness.control.is_settled());
        harness.render(10);
        assert!(harness.control.is_settled());
        assert_eq!(second_probe.resets.load(Ordering::Relaxed), 1);
        assert!(!first_probe.dropped.load(Ordering::Relaxed));
        let retired = harness.control.take_retired();
        assert_eq!(retired.len(), 1);
        assert!(!first_probe.dropped.load(Ordering::Relaxed));
        drop(retired);
        assert!(first_probe.dropped.load(Ordering::Relaxed));
    }

    #[test]
    fn removing_the_insert_returns_to_direct_playback() {
        let (insert, probe) = FakeInsert::new(0, 0);
        let mut harness = Harness::with_insert(insert);
        harness.push(0, 100, |_| 0.5);
        harness.render(10);
        harness.control.set(None).expect("room for the change");
        let output = harness.render(10);
        assert!(output.iter().all(|&sample| (sample - 0.5).abs() < 1e-6));
        assert_eq!(probe.processed.load(Ordering::Relaxed), 10);
        assert_eq!(harness.control.take_retired().len(), 1);
    }
}
