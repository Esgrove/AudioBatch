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
                    pipeline = open_pipeline(path, generation, start_seconds, output_rate, events, shared);
                }
                Command::Seek { seconds, generation } => {
                    pending.clear();
                    cursor = 0;
                    if let Some((path, current)) = &mut pipeline
                        && let Err(error) = current.seek(seconds, generation)
                    {
                        let _ = events.send(PlayerEvent::Error {
                            path: Some(path.clone()),
                            message: format!("{error:#}"),
                        });
                    }
                }
                Command::Unload => {
                    pending.clear();
                    cursor = 0;
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
                    Ok(false) => shared
                        .finished_generation
                        .store(current.generation(), Ordering::Release),
                    Err(error) => {
                        let _ = events.send(PlayerEvent::Error {
                            path: Some(path.clone()),
                            message: format!("{error:#}"),
                        });
                        shared
                            .finished_generation
                            .store(current.generation(), Ordering::Release);
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
    }
}

/// Opens the pipeline for a load command and publishes the stream properties.
/// Reports a failure as an event and returns `None`.
fn open_pipeline(
    path: PathBuf,
    generation: u32,
    start_seconds: f64,
    output_rate: u32,
    events: &Sender<PlayerEvent>,
    shared: &SharedState,
) -> Option<(PathBuf, FramePipeline)> {
    match FramePipeline::open(&path, output_rate, generation, start_seconds) {
        Ok(pipeline) => {
            let info = pipeline.info();
            let frames = info.expected_frames.unwrap_or(0);
            shared.source_rate.store(info.sample_rate, Ordering::Relaxed);
            shared.source_frames.store(frames, Ordering::Relaxed);
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
            let _ = events.send(PlayerEvent::Error {
                path: Some(path),
                message: format!("{error:#}"),
            });
            None
        }
    }
}
