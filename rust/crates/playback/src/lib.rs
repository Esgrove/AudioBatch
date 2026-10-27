//! Preview playback engine for the `AudioBatch` GUI.
//!
//! Plays one audio file at a time through an output device.
//! A decode thread feeds a lock-free `rtrb` ring buffer that the `cpal` audio callback reads,
//! with `rubato` resampling when the file rate differs from the device rate.
//! The engine has no GUI dependency.
//! A `PreviewInsert`, such as a plugin chain, can process the output inside the audio callback.
//! The engine knows nothing about plugins, only the trait.
//!
//! `Player` must stay on the thread that created it, because the cpal stream is not `Send` on every platform.
//!
//! The output device is open only while playing, so other applications can use it the rest of the time:
//! playing opens the stream, and `close_idle_stream` closes it once playback has stopped for a moment.
//! Changing the file while playing keeps it open.

mod device;
mod engine;
mod output;
mod pipeline;
mod preview_insert;

use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{BufferSize, Device, ErrorKind, FromSample, SampleFormat, SizedSample, Stream, StreamConfig};
use crossbeam_channel::{Receiver, Sender, bounded, unbounded};
use rtrb::RingBuffer;

pub use device::{DeviceCapabilities, OutputConfig, device_capabilities};
pub use engine::PlayerEvent;
pub use preview_insert::PreviewInsert;

use device::{device_display_name, find_device, stream_config};
use engine::{Command, run_decode_thread};
use output::insert::DeviceFormat;
use output::{NO_GENERATION, OutputRenderer, SharedState};
use preview_insert::{InsertControl, insert_channel};

/// What the player reports when the preview insert produced output that is not finite.
pub(crate) const NON_FINITE_INSERT_MESSAGE: &str = "A live plugin produced non-finite audio. Preview was stopped.";

/// Ring buffer length in seconds of output audio.
/// Long enough to ride out a slow disk, short enough that a seek drops little decoded audio.
const BUFFER_SECONDS: f64 = 0.5;

/// Output stream errors kept until the next poll. Later errors are dropped when it is full.
const STREAM_ERROR_CAPACITY: usize = 8;

/// How long playback stays stopped before `close_idle_stream` closes the output stream,
/// long enough for the fade of a pause to finish playing.
const STREAM_IDLE_TIMEOUT: Duration = Duration::from_millis(300);

/// Read-only view of the playback position, cheap to clone and safe to read from any thread.
///
/// The GUI reads it on every animation frame to draw the playhead without borrowing the player.
#[derive(Debug, Clone)]
pub struct PlayheadHandle {
    shared: Arc<SharedState>,
}

/// Plays audio files through an output device.
pub struct Player {
    shared: Arc<SharedState>,
    commands: Sender<Command>,
    events: Receiver<PlayerEvent>,
    /// Reports a stream that could not be opened, the same way the decode thread reports its errors.
    event_sender: Sender<PlayerEvent>,
    /// Errors from the stream error callback, formatted on the GUI thread instead of the audio thread.
    stream_errors: Receiver<cpal::Error>,
    stream_error_sender: Sender<cpal::Error>,
    decode_thread: Option<JoinHandle<()>>,
    /// The renderer every stream of this player calls.
    /// The audio callback only tries the lock, and a stream that misses it outputs silence,
    /// so the player can make insert changes itself while no stream runs.
    renderer: Arc<Mutex<OutputRenderer>>,
    /// The open output stream, only while playing and shortly after.
    stream: Option<Stream>,
    /// When playback was first seen stopped with the stream open.
    idle_since: Option<Instant>,
    device: Device,
    config: StreamConfig,
    sample_format: SampleFormat,
    device_name: String,
    output_rate: u32,
    /// Buffer size in frames the stream was opened with, or `None` for the device default.
    buffer_size: Option<u32>,
    next_generation: u32,
    loaded_path: Option<PathBuf>,
    /// Hands preview inserts to the audio callback and takes back the ones it replaced.
    insert: InsertControl,
}

/// The state the stream error callback needs.
/// The callback can run on the audio thread, so it only stores atomics.
/// It sends the error unformatted through a bounded channel, which does not allocate.
struct StreamErrors {
    shared: Arc<SharedState>,
    sender: Sender<cpal::Error>,
}

impl PlayheadHandle {
    /// Returns true while audio is playing.
    #[must_use]
    pub fn is_playing(&self) -> bool {
        self.shared.playing.load(Ordering::Relaxed)
    }

    /// Returns the playhead position in seconds.
    #[must_use]
    pub fn position_seconds(&self) -> f64 {
        let rate = self.shared.source_rate.load(Ordering::Relaxed);
        if rate == 0 {
            return 0.0;
        }
        self.shared.position.load(Ordering::Relaxed) as f64 / f64::from(rate)
    }

    /// Returns the duration of the loaded file in seconds, when known.
    #[must_use]
    pub fn duration_seconds(&self) -> Option<f64> {
        let rate = self.shared.source_rate.load(Ordering::Relaxed);
        let frames = self.shared.source_frames.load(Ordering::Relaxed);
        (rate > 0 && frames > 0).then(|| frames as f64 / f64::from(rate))
    }
}

impl Player {
    /// Chooses the named output device, or the default one, without opening it yet.
    ///
    /// # Errors
    ///
    /// Returns an error when no output device is available.
    pub fn new(device_name: Option<&str>) -> Result<Self> {
        Self::with_config(&OutputConfig {
            device: device_name.map(str::to_string),
            ..OutputConfig::default()
        })
    }

    /// Chooses an output device with a sample rate and buffer size, without opening it yet.
    /// Values the config leaves unset use the device defaults.
    ///
    /// # Errors
    ///
    /// Returns an error when the device is not available or does not support the sample rate.
    pub fn with_config(output: &OutputConfig) -> Result<Self> {
        let device = find_device(output.device.as_deref())?;
        let (config, sample_format) = stream_config(&device, output)?;
        let output_rate = config.sample_rate;
        let buffer_size = match config.buffer_size {
            BufferSize::Fixed(frames) => Some(frames),
            BufferSize::Default => None,
        };
        let channels = usize::from(config.channels);

        let shared = Arc::new(SharedState::new());
        let capacity = (f64::from(output_rate) * BUFFER_SECONDS) as usize;
        let (producer, consumer) = RingBuffer::new(capacity.max(1024));
        let (insert, insert_renderer) = insert_channel(Arc::clone(&shared));
        let device_format = DeviceFormat {
            channels,
            rate: output_rate,
        };
        let renderer = OutputRenderer::new(Arc::clone(&shared), consumer, device_format, insert_renderer);
        let (event_sender, events) = unbounded();
        let (stream_error_sender, stream_errors) = bounded(STREAM_ERROR_CAPACITY);

        let renderer = Arc::new(Mutex::new(renderer));
        // A lock may set itself up on first use, which must not happen in the first audio callback.
        drop(renderer.lock());

        let (commands, command_receiver) = unbounded();
        let thread_shared = Arc::clone(&shared);
        let thread_events = event_sender.clone();
        let decode_thread = std::thread::Builder::new()
            .name(String::from("preview-decode"))
            .spawn(move || {
                run_decode_thread(producer, &command_receiver, &thread_events, &thread_shared, output_rate);
            })
            .context("Could not start the decode thread")?;

        Ok(Self {
            shared,
            commands,
            events,
            event_sender,
            stream_errors,
            stream_error_sender,
            decode_thread: Some(decode_thread),
            renderer,
            stream: None,
            idle_since: None,
            device_name: device_display_name(&device),
            device,
            config,
            sample_format,
            output_rate,
            buffer_size,
            next_generation: NO_GENERATION,
            loaded_path: None,
            insert,
        })
    }

    /// Returns the name of the device this player outputs to.
    #[must_use]
    pub fn device_name(&self) -> &str {
        &self.device_name
    }

    /// Returns the device sample rate in hertz.
    #[must_use]
    pub const fn output_rate(&self) -> u32 {
        self.output_rate
    }

    /// Returns the buffer size in frames the stream was opened with, or `None` for the device default.
    #[must_use]
    pub const fn buffer_size(&self) -> Option<u32> {
        self.buffer_size
    }

    /// Returns the buffer size in frames the device actually uses:
    /// the size the stream was opened with, or what the audio callbacks ask for with the device default.
    /// `None` before the first callback of a default-sized stream.
    #[must_use]
    pub fn buffer_frames(&self) -> Option<u32> {
        self.buffer_size.or_else(|| {
            let frames = self.shared.callback_frames.load(Ordering::Relaxed);
            (frames > 0).then_some(frames)
        })
    }

    /// Loads a file, starting at `start_seconds`, and starts playing it when `play` is set.
    /// Opening happens on the decode thread, which reports the result as an event.
    pub fn load(&mut self, path: &Path, start_seconds: f64, play: bool) {
        let generation = self.advance_generation();
        self.shared.load_generation.store(generation, Ordering::Release);
        self.shared.source_rate.store(0, Ordering::Relaxed);
        self.shared.source_frames.store(0, Ordering::Relaxed);
        self.shared.position.store(0, Ordering::Relaxed);
        self.shared.ended.store(false, Ordering::Relaxed);
        self.shared.playing.store(play, Ordering::Relaxed);
        self.loaded_path = Some(path.to_path_buf());
        let _ = self.commands.send(Command::Load {
            path: path.to_path_buf(),
            generation,
            start_seconds,
        });
        if play {
            self.ensure_stream();
        }
    }

    /// Closes the current file and stops playback.
    pub fn unload(&mut self) {
        self.advance_generation();
        self.shared.playing.store(false, Ordering::Relaxed);
        self.shared.source_rate.store(0, Ordering::Relaxed);
        self.shared.position.store(0, Ordering::Relaxed);
        self.loaded_path = None;
        let _ = self.commands.send(Command::Unload);
    }

    /// Starts or resumes playback, from the start when the previous playback reached the end.
    pub fn play(&mut self) {
        if self.loaded_path.is_none() {
            return;
        }
        let generation = self.shared.generation.load(Ordering::Acquire);
        let ended = self.shared.ended_generation.load(Ordering::Acquire) == generation;
        let at_end = ended
            || (self.shared.finished_generation.load(Ordering::Acquire) == generation
                && self
                    .duration_seconds()
                    .is_some_and(|duration| self.position_seconds() >= duration - 0.01));
        if at_end {
            self.seek(0.0);
        }
        self.shared.playing.store(true, Ordering::Relaxed);
        self.ensure_stream();
    }

    /// Closes the output stream once playback has been stopped for a moment,
    /// so the device is free for other applications. Call it regularly from the owner's thread.
    /// A stream that stopped because its device went away is closed this way too, and the next play opens it again.
    pub fn close_idle_stream(&mut self) {
        if self.stream.is_none() || self.is_playing() {
            self.idle_since = None;
            return;
        }
        let idle_since = *self.idle_since.get_or_insert_with(Instant::now);
        if idle_since.elapsed() >= STREAM_IDLE_TIMEOUT {
            self.stream = None;
            self.idle_since = None;
            self.apply_insert_changes_without_stream();
        }
    }

    /// Pauses playback at the current position.
    pub fn pause(&self) {
        self.shared.playing.store(false, Ordering::Relaxed);
    }

    /// Stops playback and moves back to the start.
    pub fn stop(&mut self) {
        self.pause();
        self.seek(0.0);
    }

    /// Moves playback to the given time in seconds. The playhead jumps immediately.
    pub fn seek(&mut self, seconds: f64) {
        if self.loaded_path.is_none() {
            return;
        }
        let seconds = self
            .duration_seconds()
            .map_or(seconds, |duration| seconds.clamp(0.0, duration));
        let generation = self.advance_generation();
        let rate = self.shared.source_rate.load(Ordering::Relaxed);
        self.shared
            .position
            .store((seconds.max(0.0) * f64::from(rate)) as u64, Ordering::Relaxed);
        self.shared.ended.store(false, Ordering::Relaxed);
        let _ = self.commands.send(Command::Seek { seconds, generation });
    }

    /// Sets the linear gain applied before the preview insert, such as a file's custom gain.
    /// Playback without an insert plays the file unchanged.
    pub fn set_insert_gain(&self, gain: f32) {
        self.shared
            .insert_gain_bits
            .store(gain.max(0.0).to_bits(), Ordering::Relaxed);
    }

    /// Returns the linear gain applied before the preview insert.
    #[must_use]
    pub fn insert_gain(&self) -> f32 {
        self.shared.insert_gain()
    }

    /// Returns a handle for reading the playhead from other views or threads.
    #[must_use]
    pub fn playhead(&self) -> PlayheadHandle {
        PlayheadHandle {
            shared: Arc::clone(&self.shared),
        }
    }

    /// Returns true while audio is playing.
    #[must_use]
    pub fn is_playing(&self) -> bool {
        self.playhead().is_playing()
    }

    /// Returns the loaded file, if any.
    #[must_use]
    pub fn loaded_path(&self) -> Option<&Path> {
        self.loaded_path.as_deref()
    }

    /// Returns the playhead position in seconds.
    #[must_use]
    pub fn position_seconds(&self) -> f64 {
        self.playhead().position_seconds()
    }

    /// Returns the duration of the loaded file in seconds, when known.
    #[must_use]
    pub fn duration_seconds(&self) -> Option<f64> {
        self.playhead().duration_seconds()
    }

    /// Returns the number of times the audio callback ran out of decoded audio.
    #[must_use]
    pub fn underrun_count(&self) -> u64 {
        self.shared.underruns.load(Ordering::Relaxed)
    }

    /// Hands an insert to the audio callback, which processes the output through it from its next block,
    /// or removes the current insert with none.
    /// The insert it replaces comes back through `take_retired_inserts`.
    ///
    /// # Errors
    ///
    /// Returns an error when the audio callback is not taking changes, because the stream is not running.
    pub fn set_insert(&mut self, insert: Option<Box<dyn PreviewInsert>>) -> Result<()> {
        self.insert.set(insert)?;
        self.apply_insert_changes_without_stream();
        Ok(())
    }

    /// Returns the inserts the audio callback replaced since the last call,
    /// so they are dropped on this thread instead of the audio thread.
    #[must_use]
    pub fn take_retired_inserts(&mut self) -> Vec<Box<dyn PreviewInsert>> {
        self.insert.take_retired()
    }

    /// Returns true when the audio callback has made every insert change, so a removed insert is no longer used.
    #[must_use]
    pub fn is_insert_settled(&self) -> bool {
        self.insert.is_settled()
    }

    /// Sets how many frames of output may follow the end of the file in the current insert.
    pub fn set_insert_tail(&self, frames: usize) {
        self.insert.set_tail(frames);
    }

    /// Sets whether the insert is pre-rolled for its latency on every start, seek, and change,
    /// so the output lines up with the playhead.
    pub fn set_latency_compensation(&self, enabled: bool) {
        self.shared.latency_compensation.store(enabled, Ordering::Relaxed);
    }

    /// Returns the events since the last call, including output errors, insert failures, and the end of playback.
    #[must_use]
    pub fn poll_events(&self) -> Vec<PlayerEvent> {
        let mut events: Vec<PlayerEvent> = self.events.try_iter().collect();
        events.extend(self.stream_errors.try_iter().map(|error| PlayerEvent::Error {
            path: None,
            message: if is_stream_lost(error.kind()) {
                format!("The audio output stopped: {error}")
            } else {
                format!("Audio output error: {error}")
            },
        }));
        if self.shared.insert_failed.swap(false, Ordering::AcqRel) {
            events.push(PlayerEvent::Error {
                path: None,
                message: String::from(NON_FINITE_INSERT_MESSAGE),
            });
        }
        if self.shared.ended.swap(false, Ordering::AcqRel) {
            events.push(PlayerEvent::Ended);
        }
        events
    }

    /// Returns the names of the available output devices.
    #[must_use]
    pub fn output_device_names() -> Vec<String> {
        cpal::default_host()
            .output_devices()
            .map(|devices| devices.map(|device| device_display_name(&device)).collect())
            .unwrap_or_default()
    }

    /// Opens and starts the output stream unless it is open.
    /// A failure stops playback and is reported as an error event.
    fn ensure_stream(&mut self) {
        self.idle_since = None;
        if self.shared.stream_lost.swap(false, Ordering::AcqRel) {
            self.stream = None;
            self.apply_insert_changes_without_stream();
        }
        if self.stream.is_some() {
            return;
        }
        let errors = StreamErrors {
            shared: Arc::clone(&self.shared),
            sender: self.stream_error_sender.clone(),
        };
        let opened = build_stream(
            &self.device,
            &self.config,
            self.sample_format,
            Arc::clone(&self.renderer),
            errors,
        )
        .and_then(|stream| {
            stream.play().context("Could not start the output stream")?;
            Ok(stream)
        });
        match opened {
            Ok(stream) => self.stream = Some(stream),
            Err(error) => {
                self.shared.playing.store(false, Ordering::Relaxed);
                let _ = self.event_sender.send(PlayerEvent::Error {
                    path: None,
                    message: format!("{error:#}"),
                });
            }
        }
    }

    /// Makes the queued insert changes on this thread while no stream calls the renderer,
    /// so a detached insert comes back at once instead of waiting for a stream that is not running.
    fn apply_insert_changes_without_stream(&self) {
        if self.stream.is_none()
            && let Ok(mut renderer) = self.renderer.lock()
        {
            renderer.apply_insert_changes();
        }
    }

    /// Makes a new generation current, so the callback drops every frame of the previous one.
    fn advance_generation(&mut self) -> u32 {
        self.next_generation = self.next_generation.wrapping_add(1);
        if self.next_generation == NO_GENERATION {
            self.next_generation = 1;
        }
        self.shared.generation.store(self.next_generation, Ordering::Release);
        self.next_generation
    }
}

impl Drop for Player {
    fn drop(&mut self) {
        let _ = self.commands.send(Command::Shutdown);
        if let Some(thread) = self.decode_thread.take() {
            let _ = thread.join();
        }
    }
}

/// Builds the output stream for the device sample format.
fn build_stream(
    device: &Device,
    config: &StreamConfig,
    sample_format: SampleFormat,
    renderer: Arc<Mutex<OutputRenderer>>,
    errors: StreamErrors,
) -> Result<Stream> {
    match sample_format {
        SampleFormat::F32 => build_typed_stream::<f32>(device, config, renderer, errors),
        SampleFormat::F64 => build_typed_stream::<f64>(device, config, renderer, errors),
        SampleFormat::I16 => build_typed_stream::<i16>(device, config, renderer, errors),
        SampleFormat::I32 => build_typed_stream::<i32>(device, config, renderer, errors),
        SampleFormat::U16 => build_typed_stream::<u16>(device, config, renderer, errors),
        other => bail!("Unsupported output sample format {other}"),
    }
}

/// Builds an output stream that renders into samples of type `T`.
fn build_typed_stream<T>(
    device: &Device,
    config: &StreamConfig,
    renderer: Arc<Mutex<OutputRenderer>>,
    errors: StreamErrors,
) -> Result<Stream>
where
    T: SizedSample + FromSample<f32>,
{
    device
        .build_output_stream(
            *config,
            move |output: &mut [T], _| {
                // Only a closing stream or the player while no stream runs holds the lock, so it is never waited on.
                if let Ok(mut renderer) = renderer.try_lock() {
                    renderer.render(output, T::from_sample);
                } else {
                    output.fill(T::from_sample(0.0));
                }
            },
            move |error: cpal::Error| {
                if matches!(
                    error.kind(),
                    ErrorKind::Xrun | ErrorKind::DeviceChanged | ErrorKind::RealtimeDenied
                ) {
                    return;
                }
                if is_stream_lost(error.kind()) {
                    errors.shared.stream_lost.store(true, Ordering::Release);
                    errors.shared.playing.store(false, Ordering::Relaxed);
                }
                let _ = errors.sender.try_send(error);
            },
            None,
        )
        .context("Could not open the output stream")
}

/// Returns true for the errors after which the stream no longer plays, such as a disconnected device.
const fn is_stream_lost(kind: ErrorKind) -> bool {
    matches!(
        kind,
        ErrorKind::DeviceNotAvailable | ErrorKind::StreamInvalidated | ErrorKind::HostUnavailable
    )
}
