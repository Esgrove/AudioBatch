//! The decode thread of the player.
//!
//! Receives load and seek commands, runs the `FramePipeline` of the current file,
//! and keeps the ring buffer to the audio callback filled.
//! It polls commands and callback rebuffer requests between packets and while idle,
//! so a seek takes effect within one packet.

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::Duration;

use crossbeam_channel::{Receiver, RecvTimeoutError, Sender};
use rtrb::Producer;

use crate::output::SharedState;
use crate::pipeline::{Frame, FramePipeline};

/// How long the decode thread waits for a command while the ring buffer is full.
const FULL_BUFFER_WAIT: Duration = Duration::from_millis(5);

/// Commands from the player to the decode thread.
#[derive(Debug, Clone)]
pub enum Command {
    /// Opens a file and starts decoding it at the given time.
    Load {
        /// File to play.
        path: PathBuf,
        /// Generation to tag the frames with.
        generation: u32,
        /// Start time in seconds.
        start_seconds: f64,
    },
    /// Moves the current file to a new time.
    Seek {
        /// Target time in seconds.
        seconds: f64,
        /// Generation to tag the frames with.
        generation: u32,
    },
    /// Closes the current file.
    Unload,
    /// Ends the thread.
    Shutdown,
}

/// Notifications from the player to the GUI.
#[derive(Debug, Clone, PartialEq)]
pub enum PlayerEvent {
    /// A file was opened and decoding started.
    Loaded {
        /// The opened file.
        path: PathBuf,
        /// Sample rate of the file in hertz.
        sample_rate: u32,
        /// Duration in seconds, when the container states it.
        duration_seconds: Option<f64>,
    },
    /// Opening, decoding, or the output device failed.
    Error {
        /// The file involved, if any.
        path: Option<PathBuf>,
        /// User-facing description.
        message: String,
    },
    /// Playback reached the end of the file.
    Ended,
}

/// Runs the decode loop until a shutdown command arrives or the player is dropped.
pub fn run_decode_thread(
    mut producer: Producer<Frame>,
    commands: &Receiver<Command>,
    events: &Sender<PlayerEvent>,
    shared: &Arc<SharedState>,
    output_rate: u32,
) {
    let mut pipeline: Option<(PathBuf, FramePipeline)> = None;
    let mut pending: Vec<Frame> = Vec::with_capacity(16_384);
    let mut cursor = 0;
    // The end is published only once the last frames are in the ring buffer,
    // or the callback could stop playback while the tail is still waiting in `pending`.
    let mut finish_after_pending: Option<u32> = None;

    loop {
        let has_pending = cursor < pending.len();
        let decoding = pipeline.as_ref().is_some_and(|(_, current)| !current.is_finished());
        let command = if has_pending || decoding {
            let wait = if producer.slots() == 0 {
                FULL_BUFFER_WAIT
            } else {
                Duration::ZERO
            };
            match commands.recv_timeout(wait) {
                Ok(command) => Some(command),
                Err(RecvTimeoutError::Timeout) => None,
                Err(RecvTimeoutError::Disconnected) => return,
            }
        } else {
            match commands.recv_timeout(FULL_BUFFER_WAIT) {
                Ok(command) => Some(command),
                Err(RecvTimeoutError::Timeout) => None,
                Err(RecvTimeoutError::Disconnected) => return,
            }
        };

        if let Some(command) = command {
            match command {
                Command::Load {
                    path,
                    generation,
                    start_seconds,
                } => {
                    pending.clear();
                    cursor = 0;
                    finish_after_pending = None;
                    pipeline = open_pipeline(path, generation, start_seconds, output_rate, events, shared);
                }
                Command::Seek { seconds, generation } => {
                    pending.clear();
                    cursor = 0;
                    finish_after_pending = None;
                    pipeline = seek_pipeline(pipeline.take(), seconds, generation, events, shared);
                }
                Command::Unload => {
                    pending.clear();
                    cursor = 0;
                    finish_after_pending = None;
                    pipeline = None;
                }
                Command::Shutdown => return,
            }
            continue;
        }

        // Apply queued loads and seeks first, so a callback request cannot seek an unopened or superseded source.
        let rebuffer = shared.rebuffer_generation.swap(0, Ordering::AcqRel);
        if rebuffer != 0 && shared.generation.load(Ordering::Acquire) == rebuffer {
            pending.clear();
            cursor = 0;
            finish_after_pending = None;
            let rate = shared.source_rate.load(Ordering::Relaxed);
            let position = shared.position.load(Ordering::Relaxed);
            let seconds = if rate > 0 {
                position as f64 / f64::from(rate)
            } else {
                0.0
            };
            pipeline = seek_pipeline(pipeline.take(), seconds, rebuffer, events, shared);
        }

        if cursor >= pending.len() {
            pending.clear();
            cursor = 0;
            if let Some((path, current)) = &mut pipeline {
                match current.produce(&mut pending) {
                    Ok(true) => {}
                    Ok(false) => finish_after_pending = Some(current.generation()),
                    Err(error) => {
                        let _ = events.send(PlayerEvent::Error {
                            path: Some(path.clone()),
                            message: error.message(),
                        });
                        finish_after_pending = Some(current.generation());
                        pipeline = None;
                    }
                }
            }
        }

        while let Some(&frame) = pending.get(cursor) {
            if producer.push(frame).is_err() {
                break;
            }
            cursor += 1;
        }
        if cursor >= pending.len()
            && let Some(generation) = finish_after_pending.take()
        {
            shared.finished_generation.store(generation, Ordering::Release);
        }
    }
}

/// Opens the pipeline for a load command and publishes the stream properties.
/// Reports a failure as an event and returns `None`.
/// A load that a newer one has replaced is skipped, so it never publishes the properties of the wrong file.
fn open_pipeline(
    path: PathBuf,
    generation: u32,
    start_seconds: f64,
    output_rate: u32,
    events: &Sender<PlayerEvent>,
    shared: &SharedState,
) -> Option<(PathBuf, FramePipeline)> {
    let is_current = || shared.load_generation.load(Ordering::Acquire) == generation;
    if !is_current() {
        return None;
    }
    let opened = FramePipeline::open(&path, output_rate, generation, start_seconds);
    if !is_current() {
        return None;
    }
    match opened {
        Ok(pipeline) => {
            let info = pipeline.info();
            let frames = info.expected_frames.unwrap_or(0);
            shared.source_rate.store(info.sample_rate, Ordering::Relaxed);
            shared.source_frames.store(frames, Ordering::Relaxed);
            shared.source_mono.store(info.channels == 1, Ordering::Relaxed);
            let start_frame = (start_seconds.max(0.0) * f64::from(info.sample_rate)).round() as u64;
            shared.position.store(start_frame, Ordering::Relaxed);
            let _ = events.send(PlayerEvent::Loaded {
                path: path.clone(),
                sample_rate: info.sample_rate,
                duration_seconds: (frames > 0).then(|| frames as f64 / f64::from(info.sample_rate)),
            });
            Some((path, pipeline))
        }
        Err(error) => {
            shared.playing.store(false, Ordering::Relaxed);
            shared.finished_generation.store(generation, Ordering::Release);
            let _ = events.send(PlayerEvent::Error {
                path: Some(path),
                message: error.message(),
            });
            None
        }
    }
}

/// Moves the pipeline to a new time and publishes the target as the playhead position.
/// A failed seek, or a seek without an open file, finishes the generation and stops playback,
/// so the player does not wait forever for frames that never come.
fn seek_pipeline(
    pipeline: Option<(PathBuf, FramePipeline)>,
    seconds: f64,
    generation: u32,
    events: &Sender<PlayerEvent>,
    shared: &SharedState,
) -> Option<(PathBuf, FramePipeline)> {
    let Some((path, mut current)) = pipeline else {
        shared.finished_generation.store(generation, Ordering::Release);
        return None;
    };
    match current.seek(seconds, generation) {
        Ok(()) => {
            if shared.generation.load(Ordering::Acquire) == generation {
                let target = (seconds.max(0.0) * f64::from(current.info().sample_rate)).round() as u64;
                shared.position.store(target, Ordering::Relaxed);
            }
            Some((path, current))
        }
        Err(error) => {
            shared.playing.store(false, Ordering::Relaxed);
            shared.finished_generation.store(generation, Ordering::Release);
            let _ = events.send(PlayerEvent::Error {
                path: Some(path),
                message: error.message(),
            });
            None
        }
    }
}

#[cfg(test)]
mod test_decode_thread {
    use std::path::Path;
    use std::thread::JoinHandle;
    use std::time::Instant;

    use crossbeam_channel::unbounded;
    use rtrb::{Consumer, RingBuffer};

    use super::*;

    /// Longest time a test waits for the decode thread.
    const TIMEOUT: Duration = Duration::from_secs(10);

    /// Output rate of the decode thread, the rate of the fixtures, so no resampling takes place.
    const OUTPUT_RATE: u32 = 44_100;

    /// Frames the ring buffer holds, far fewer than a fixture has, so the decode thread fills it and waits.
    const RING_CAPACITY: usize = 4096;

    /// A decode thread with its command channel, its events, the consumer of its ring buffer, and the shared state.
    struct DecodeThread {
        commands: Sender<Command>,
        events: Receiver<PlayerEvent>,
        consumer: Consumer<Frame>,
        shared: Arc<SharedState>,
        thread: JoinHandle<()>,
    }

    impl DecodeThread {
        /// Starts a decode thread.
        fn start() -> Self {
            let (producer, consumer) = RingBuffer::new(RING_CAPACITY);
            let (commands, command_receiver) = unbounded();
            let (event_sender, events) = unbounded();
            let shared = Arc::new(SharedState::new());
            let thread_shared = Arc::clone(&shared);
            let thread = std::thread::spawn(move || {
                run_decode_thread(producer, &command_receiver, &event_sender, &thread_shared, OUTPUT_RATE);
            });
            Self {
                commands,
                events,
                consumer,
                shared,
                thread,
            }
        }

        /// Loads a file as the given generation, setting the shared state the way the player does.
        fn load(&self, path: &Path, generation: u32, start_seconds: f64) {
            self.shared.load_generation.store(generation, Ordering::Release);
            self.shared.generation.store(generation, Ordering::Release);
            self.send(Command::Load {
                path: path.to_path_buf(),
                generation,
                start_seconds,
            });
        }

        /// Sends a command to the decode thread.
        fn send(&self, command: Command) {
            self.commands.send(command).expect("the decode thread runs");
        }

        /// Reads frames until the decode thread has finished the generation and the ring buffer is empty.
        fn drain_until_finished(&mut self, generation: u32) -> Vec<Frame> {
            let deadline = Instant::now() + TIMEOUT;
            let mut frames = Vec::new();
            loop {
                // The end is published after the last frame is in the ring buffer, so it is read before draining.
                let finished = self.shared.finished_generation.load(Ordering::Acquire) == generation;
                while let Ok(frame) = self.consumer.pop() {
                    frames.push(frame);
                }
                if finished {
                    return frames;
                }
                assert!(Instant::now() < deadline, "generation {generation} did not finish");
                std::thread::sleep(Duration::from_millis(1));
            }
        }

        /// Returns the next event, failing the test when none arrives in time.
        fn next_event(&self) -> PlayerEvent {
            self.events.recv_timeout(TIMEOUT).expect("an event arrives")
        }
    }

    /// Waits until the thread has ended, and returns false when it is still running after the timeout.
    fn wait_for_exit(thread: JoinHandle<()>) -> bool {
        let deadline = Instant::now() + TIMEOUT;
        while !thread.is_finished() {
            if Instant::now() >= deadline {
                return false;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        thread.join().is_ok()
    }

    /// Returns true when the frames have consecutive source positions from `first` on.
    fn consecutive_from(frames: &[Frame], first: u64) -> bool {
        frames.first().is_some_and(|frame| frame.position == first)
            && frames.windows(2).all(|pair| pair[1].position == pair[0].position + 1)
    }

    #[test]
    fn decodes_a_file_into_the_ring_buffer_and_shuts_down() {
        let mut decode = DecodeThread::start();
        let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
        decode.load(&path, 1, 0.0);
        let frames = decode.drain_until_finished(1);

        let PlayerEvent::Loaded {
            path: loaded,
            sample_rate,
            duration_seconds,
        } = decode.next_event()
        else {
            panic!("the file did not load");
        };
        assert_eq!((loaded, sample_rate), (path, 44_100));
        assert!(duration_seconds.is_some_and(|duration| (duration - 1.0).abs() < 1e-9));
        assert_eq!(decode.shared.source_rate.load(Ordering::Relaxed), 44_100);
        assert!(!decode.shared.source_mono.load(Ordering::Relaxed));

        assert_eq!(frames.len(), 44_100);
        assert!(frames.iter().all(|frame| frame.generation == 1));
        assert!(consecutive_from(&frames, 0));
        let peak = frames.iter().map(|frame| frame.left.abs()).fold(0.0_f32, f32::max);
        assert!((peak - 0.5).abs() < 0.01, "peak {peak}");
        assert!(decode.events.try_recv().is_err());

        decode.send(Command::Shutdown);
        assert!(wait_for_exit(decode.thread));
    }

    #[test]
    fn a_load_starts_at_the_requested_time() {
        let mut decode = DecodeThread::start();
        decode.load(&test_fixtures::fixture("sine_mono_44100_16.wav"), 3, 0.5);
        let frames = decode.drain_until_finished(3);
        assert!(matches!(decode.next_event(), PlayerEvent::Loaded { .. }));
        assert!(decode.shared.source_mono.load(Ordering::Relaxed));
        assert_eq!(frames.len(), 22_050);
        assert!(consecutive_from(&frames, 22_050));
        assert!(
            frames
                .iter()
                .all(|frame| (frame.left - frame.right).abs() < f32::EPSILON)
        );
    }

    #[test]
    fn a_new_load_replaces_the_current_file() {
        let mut decode = DecodeThread::start();
        decode.load(&test_fixtures::fixture("sine_mono_44100_16.wav"), 1, 0.0);
        let stereo = test_fixtures::fixture("sine_stereo_44100_16.wav");
        decode.load(&stereo, 2, 0.0);
        let frames = decode.drain_until_finished(2);
        let current: Vec<Frame> = frames.into_iter().filter(|frame| frame.generation == 2).collect();
        assert_eq!(current.len(), 44_100);
        assert!(consecutive_from(&current, 0));
        let loaded: Vec<PathBuf> = decode
            .events
            .try_iter()
            .filter_map(|event| match event {
                PlayerEvent::Loaded { path, .. } => Some(path),
                _ => None,
            })
            .collect();
        assert_eq!(loaded.last(), Some(&stereo));
        assert!(!decode.shared.source_mono.load(Ordering::Relaxed));
    }

    #[test]
    fn a_file_that_cannot_be_opened_reports_an_error_and_the_thread_goes_on() {
        let mut decode = DecodeThread::start();
        let broken = test_fixtures::fixture("broken.wav");
        decode.shared.playing.store(true, Ordering::Relaxed);
        decode.load(&broken, 1, 0.0);
        let PlayerEvent::Error { path, message } = decode.next_event() else {
            panic!("the broken file did not fail");
        };
        assert_eq!(path, Some(broken));
        assert_ne!(message, "");
        assert_eq!(decode.shared.finished_generation.load(Ordering::Acquire), 1);
        assert!(!decode.shared.playing.load(Ordering::Relaxed));

        decode.send(Command::Seek {
            seconds: 0.5,
            generation: 2,
        });
        assert_eq!(decode.drain_until_finished(2).len(), 0);

        decode.load(&test_fixtures::fixture("sine_stereo_44100_16.wav"), 3, 0.0);
        assert_eq!(decode.drain_until_finished(3).len(), 44_100);
    }

    #[test]
    fn callback_rebuffer_after_decoding_eof_restarts_at_the_audible_cursor() {
        let mut decode = DecodeThread::start();
        decode.load(&test_fixtures::fixture("sine_stereo_44100_16.wav"), 1, 0.0);
        let original = decode.drain_until_finished(1);
        assert!(matches!(decode.next_event(), PlayerEvent::Loaded { .. }));
        decode.shared.position.store(137, Ordering::Relaxed);
        let generation = decode.shared.advance_generation();
        decode.shared.rebuffer_generation.store(generation, Ordering::Release);
        let resumed = decode.drain_until_finished(generation);
        assert_eq!(resumed.len(), original.len() - 137);
        assert!(consecutive_from(&resumed, 137));
        for (actual, expected) in resumed.iter().zip(&original[137..]) {
            assert_eq!(actual.generation, generation);
            assert!((actual.left - expected.left).abs() < f32::EPSILON);
            assert!((actual.right - expected.right).abs() < f32::EPSILON);
        }
        decode.send(Command::Shutdown);
        assert!(wait_for_exit(decode.thread));
    }

    #[test]
    fn dropping_the_commands_ends_the_thread_while_idle_and_while_decoding() {
        let DecodeThread { commands, thread, .. } = DecodeThread::start();
        drop(commands);
        assert!(wait_for_exit(thread));

        let decoding = DecodeThread::start();
        decoding.load(&test_fixtures::fixture("sine_stereo_44100_16.wav"), 1, 0.0);
        let deadline = Instant::now() + TIMEOUT;
        while decoding.consumer.slots() < RING_CAPACITY {
            assert!(Instant::now() < deadline, "the ring buffer did not fill");
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_ne!(decoding.shared.finished_generation.load(Ordering::Acquire), 1);
        let DecodeThread {
            commands,
            thread,
            consumer: _consumer,
            ..
        } = decoding;
        drop(commands);
        assert!(wait_for_exit(thread));
    }
}
