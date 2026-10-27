//! The decode thread of the player.
//!
//! Receives load and seek commands, runs the `FramePipeline` of the current file,
//! and keeps the ring buffer to the audio callback filled.
//! It blocks on the command channel while idle and polls it between packets while decoding,
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
            match commands.recv() {
                Ok(command) => Some(command),
                Err(_) => return,
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
                            message: format!("{error:#}"),
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
            let start_frame = (start_seconds.max(0.0) * f64::from(info.sample_rate)) as u64;
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
                message: format!("{error:#}"),
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
                let target = (seconds.max(0.0) * f64::from(current.info().sample_rate)) as u64;
                shared.position.store(target, Ordering::Relaxed);
            }
            Some((path, current))
        }
        Err(error) => {
            shared.playing.store(false, Ordering::Relaxed);
            shared.finished_generation.store(generation, Ordering::Release);
            let _ = events.send(PlayerEvent::Error {
                path: Some(path),
                message: format!("{error:#}"),
            });
            None
        }
    }
}
