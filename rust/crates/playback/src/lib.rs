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
//!
//! Player preparation returns `PlayerError`, device queries return `DeviceError`,
//! and control-thread insert changes return `InsertError`.
//! Native causes remain available through their source chains.
//! Asynchronous failures become user-facing text only when constructing a `PlayerEvent`.

#![cfg_attr(audiobatch_rtsan, feature(sanitize))]

#[cfg(all(test, audiobatch_rtsan))]
mod realtime_tests;

#[cfg(test)]
mod callback_benchmark;
#[cfg(test)]
mod callback_benchmark_cpu;
#[cfg(test)]
mod callback_benchmark_native;
#[cfg(test)]
mod callback_lifecycle_tests;
#[cfg(test)]
mod callback_transport_tests;

mod callback_access;
mod device;
mod engine;
mod output;
mod pipeline;
mod preview_insert;
mod stream_errors;

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use cpal::traits::{DeviceTrait, StreamTrait};
use cpal::{BufferSize, Device, ErrorKind, FromSample, SampleFormat, SizedSample, Stream, StreamConfig};
use crossbeam_channel::{Receiver, Sender, bounded, unbounded};
use rtrb::RingBuffer;
use thiserror::Error;

pub use device::{DeviceCapabilities, DeviceError, OutputConfig, device_capabilities};
pub use engine::PlayerEvent;
pub use preview_insert::{InsertError, PreviewInsert};

use callback_access::CallbackAccess;
use device::{device_display_name, find_device, output_devices, stream_config};
use engine::{Command, run_decode_thread};
use output::insert::DeviceFormat;
use output::{OutputRenderer, SharedState};
use preview_insert::{InsertControl, insert_channel};
use stream_errors::StreamErrors;

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

/// Control-side interval while native callback closures finish releasing their pinned captures.
const CAPTURE_RETIRE_POLL_INTERVAL: Duration = Duration::from_micros(50);

/// Failures while preparing a player on its owning thread.
#[derive(Debug, Error)]
pub enum PlayerError {
    /// The output device or requested configuration is unavailable.
    #[error(transparent)]
    Device(#[from] DeviceError),
    /// The operating system could not start the decode worker.
    #[error("Could not start the decode thread")]
    DecodeThread(#[source] std::io::Error),
}

/// Failures while opening and starting the output stream on the player's thread.
#[derive(Debug, Error)]
enum OutputStreamError {
    /// The device's sample format has no renderer.
    #[error("Unsupported output sample format {0}")]
    UnsupportedSampleFormat(SampleFormat),
    /// The device could not open the configured output stream.
    #[error("Could not open the output stream")]
    Open(#[source] cpal::Error),
    /// The opened stream could not start playing.
    #[error("Could not start the output stream")]
    Start(#[source] cpal::Error),
}

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
    /// Owner pin for renderer resources, explicitly retired on this thread before backend stream disposal.
    renderer: Arc<RendererAccess>,
    /// Owner pins for callback capture storage, reclaimed only after both backend closures acknowledge disposal.
    captures: Vec<Arc<StreamCapture>>,
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
    loaded_path: Option<PathBuf>,
    /// Hands preview inserts to the audio callback and takes back the ones it replaced.
    insert: InsertControl,
}

/// Callback admission and the stream identity published only while control owns drained access.
struct RendererAccess {
    renderer: CallbackAccess<OutputRenderer>,
    epoch: AtomicU64,
}

/// Pinned callback capture storage, retained by the owner until both native closures are destroyed.
/// Closing the error gate drains any error publication before a replacement stream is admitted.
struct StreamCapture {
    renderer: Arc<RendererAccess>,
    epoch: u64,
    errors: CallbackAccess<StreamErrors>,
    render_released: AtomicBool,
    error_released: AtomicBool,
}

/// Backend closure ownership with an explicit release acknowledgement.
struct CallbackCapture {
    capture: Arc<StreamCapture>,
    is_error: bool,
}

impl RendererAccess {
    /// Keeps the renderer unavailable to any stream until an owner publishes its first epoch.
    const fn new(renderer: OutputRenderer) -> Self {
        Self {
            renderer: CallbackAccess::new(renderer),
            epoch: AtomicU64::new(0),
        }
    }

    /// Settles the captured insert commands with retirement space available, without dropping inserts under exclusion.
    /// The retire queue holds more entries than the entire command queue, so one application pass suffices.
    fn flush_insert_changes(&self, insert: &mut InsertControl) -> Vec<Box<dyn PreviewInsert>> {
        let mut retired = insert.take_retired();
        self.renderer.with_control(OutputRenderer::apply_insert_changes);
        retired.extend(insert.take_retired());
        retired
    }

    /// Advances the stream identity only after every earlier admitted render callback has returned.
    /// A delayed callback checks its epoch after admission and cannot consume replacement-stream audio.
    fn advance_epoch(&self) -> u64 {
        self.renderer
            .with_control(|_| {
                let epoch = self
                    .epoch
                    .load(Ordering::Relaxed)
                    .checked_add(1)
                    .expect("stream epoch exhausted");
                self.epoch.store(epoch, Ordering::Release);
                epoch
            })
            .expect("live player renderer")
    }
}

impl StreamCapture {
    /// Pins all callback captures on the control thread before native stream construction can retain them.
    const fn new(renderer: Arc<RendererAccess>, epoch: u64, errors: StreamErrors) -> Self {
        Self {
            renderer,
            epoch,
            errors: CallbackAccess::new(errors),
            render_released: AtomicBool::new(false),
            error_released: AtomicBool::new(false),
        }
    }

    /// Closes error publication and destroys its owned transport state on the player's thread.
    fn close(&self) {
        drop(self.errors.retire());
    }

    /// Forwards native errors only while this stream remains current.
    /// Owned-error destruction on rejection is part of the separately diagnosed error callback boundary.
    #[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
    fn report(&self, error: cpal::Error) {
        self.errors.try_with_if(
            || self.renderer.epoch.load(Ordering::Acquire) == self.epoch,
            |errors| errors.report(error),
        );
    }

    /// Confirms backend ownership is completely gone before freeing the owner pin.
    fn released(capture: &Arc<Self>) -> bool {
        capture.render_released.load(Ordering::Acquire)
            && capture.error_released.load(Ordering::Acquire)
            && Arc::strong_count(capture) == 1
    }
}

impl CallbackCapture {
    /// Keeps the whole acknowledged capture in the backend closure rather than capturing only its Arc field.
    fn render<T: SizedSample + FromSample<f32>>(&self, output: &mut [T]) {
        render_typed_callback(&self.capture.renderer, self.capture.epoch, output);
    }

    /// Forwards through this stream's drained error-publication gate.
    fn report(&self, error: cpal::Error) {
        self.capture.report(error);
    }
}

impl Drop for CallbackCapture {
    fn drop(&mut self) {
        let released = if self.is_error {
            &self.capture.error_released
        } else {
            &self.capture.render_released
        };
        released.store(true, Ordering::Release);
        // The owner also observes the strong count, so acknowledgement cannot race the field's Arc destruction.
    }
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
    /// Returns `PlayerError` when the output device cannot be configured or the decode thread cannot start.
    pub fn new(device_name: Option<&str>) -> Result<Self, PlayerError> {
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
    /// Returns `PlayerError` when the device is unavailable, the sample rate is unsupported,
    /// or the decode thread cannot start.
    pub fn with_config(output: &OutputConfig) -> Result<Self, PlayerError> {
        let device = find_device(output.device.as_deref())?;
        let device::SelectedStreamConfig { config, sample_format } = stream_config(&device, output)?;
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

        let renderer = Arc::new(RendererAccess::new(renderer));

        let (commands, command_receiver) = unbounded();
        let thread_shared = Arc::clone(&shared);
        let thread_events = event_sender.clone();
        let decode_thread = std::thread::Builder::new()
            .name(String::from("preview-decode"))
            .spawn(move || {
                run_decode_thread(producer, &command_receiver, &thread_events, &thread_shared, output_rate);
            })
            .map_err(PlayerError::DecodeThread)?;

        Ok(Self {
            shared,
            commands,
            events,
            event_sender,
            stream_errors,
            stream_error_sender,
            decode_thread: Some(decode_thread),
            renderer,
            captures: Vec::new(),
            stream: None,
            idle_since: None,
            device_name: device_display_name(&device),
            device,
            config,
            sample_format,
            output_rate,
            buffer_size,
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
        let generation = self.advance_generation();
        self.shared.load_generation.store(generation, Ordering::Release);
        self.shared.playing.store(false, Ordering::Relaxed);
        self.shared.source_rate.store(0, Ordering::Relaxed);
        self.shared.source_frames.store(0, Ordering::Relaxed);
        self.shared.position.store(0, Ordering::Relaxed);
        self.shared.ended.store(false, Ordering::Relaxed);
        self.loaded_path = None;
        let _ = self.commands.send(Command::Unload);
    }

    /// Starts or resumes playback, from the start when the previous playback reached the end.
    pub fn play(&mut self) {
        if self.loaded_path.is_none() {
            return;
        }
        let generation = self.shared.generation.load(Ordering::Acquire);
        // A decoded EOF or a playhead near the end can still leave unheard insert latency or tail.
        // Only the callback's completed drain means a new play should start over.
        if self.shared.ended_generation.load(Ordering::Acquire) == generation {
            self.seek(0.0);
        }
        self.shared.playing.store(true, Ordering::Relaxed);
        self.ensure_stream();
    }

    /// Closes the output stream once playback has been stopped for a moment,
    /// so the device is free for other applications. Call it regularly from the owner's thread.
    /// A stream that stopped because its device went away is closed this way too, and the next play opens it again.
    pub fn close_idle_stream(&mut self) {
        self.collect_captures();
        if self.stream.is_none() || self.is_playing() {
            self.idle_since = None;
            return;
        }
        let idle_since = *self.idle_since.get_or_insert_with(Instant::now);
        if idle_since.elapsed() >= STREAM_IDLE_TIMEOUT {
            self.close_stream();
            self.idle_since = None;
            self.apply_insert_changes_when_idle();
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
        if self.loaded_path.is_none() || !seconds.is_finite() {
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
        let sanitized = if gain.is_finite() { gain.max(0.0) } else { 0.0 };
        self.shared
            .insert_gain_bits
            .store(sanitized.to_bits(), Ordering::Relaxed);
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
    /// Returns `InsertError::QueueFull` when the audio callback is not taking changes,
    /// because the stream is not running.
    pub fn set_insert(&mut self, insert: Option<Box<dyn PreviewInsert>>) -> Result<(), InsertError> {
        self.insert.set(insert)?;
        self.apply_insert_changes_when_idle();
        Ok(())
    }

    /// Forces any queued insert changes to settle on the control thread and returns retired inserts.
    /// Safe to call when a paused or stalled stream has not serviced the queue on its own.
    pub fn flush_insert_changes(&mut self) -> Vec<Box<dyn PreviewInsert>> {
        self.renderer.flush_insert_changes(&mut self.insert)
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

    /// Sets the tail of the latest successfully queued insert, whether or not the callback has attached it yet.
    /// An older insert still awaiting replacement keeps its own tail.
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

    /// Returns the names of the available output devices, using ASIO on Windows.
    #[must_use]
    pub fn output_device_names() -> Vec<String> {
        output_devices().map_or_else(
            |_| Vec::new(),
            |devices| devices.iter().map(device_display_name).collect(),
        )
    }

    /// Opens and starts the output stream unless it is open.
    /// A failure stops playback and is reported as an error event.
    fn ensure_stream(&mut self) {
        self.idle_since = None;
        self.collect_captures();
        if self.shared.stream_lost.swap(false, Ordering::AcqRel) {
            let playing = self.shared.playing.load(Ordering::Relaxed);
            self.close_stream();
            self.shared.playing.store(playing, Ordering::Relaxed);
            self.apply_insert_changes_when_idle();
        }
        if self.stream.is_some() {
            return;
        }
        let epoch = self.renderer.advance_epoch();
        let capture = Arc::new(StreamCapture::new(
            Arc::clone(&self.renderer),
            epoch,
            StreamErrors {
                shared: Arc::clone(&self.shared),
                sender: self.stream_error_sender.clone(),
            },
        ));
        self.captures.push(Arc::clone(&capture));
        let opened = build_stream(&self.device, &self.config, self.sample_format, capture);
        match opened {
            Ok(stream) => match stream.play() {
                Ok(()) => self.stream = Some(stream),
                Err(error) => {
                    // Invalidate and drain before the failing stream's backend captures can be disposed.
                    self.invalidate_stream();
                    drop(stream);
                    self.report_open_failure(&OutputStreamError::Start(error));
                }
            },
            Err(error) => {
                self.invalidate_stream();
                self.report_open_failure(&error);
            }
        }
        self.collect_captures();
    }

    /// Makes the queued insert changes on this thread while no stream runs or playback is stopped/paused,
    /// so a detached insert comes back at once without waiting for an idle audio backend.
    fn apply_insert_changes_when_idle(&self) {
        if self.stream.is_none() || !self.is_playing() {
            self.renderer
                .renderer
                .with_control(OutputRenderer::apply_insert_changes);
        }
    }

    /// Drains native error publication before changing identity under exclusive renderer access.
    fn invalidate_stream(&self) {
        for capture in &self.captures {
            capture.close();
        }
        self.renderer.advance_epoch();
        self.shared.stream_lost.store(false, Ordering::Release);
    }

    /// Invalidates callbacks before disposing the native stream and collecting acknowledged capture pins.
    fn close_stream(&mut self) {
        self.invalidate_stream();
        self.stream = None;
        self.collect_captures();
    }

    /// Reclaims capture storage only on the control thread once native closure ownership is gone.
    fn collect_captures(&mut self) {
        collect_stream_captures(&mut self.captures);
    }

    /// Stops playback and reports a failed open or start with its unchanged native source chain.
    fn report_open_failure(&self, error: &OutputStreamError) {
        self.shared.playing.store(false, Ordering::Relaxed);
        let _ = self.event_sender.send(PlayerEvent::Error {
            path: None,
            message: error_message(error),
        });
    }

    /// Makes a new generation current, so the callback drops every frame of the previous one.
    fn advance_generation(&self) -> u32 {
        self.shared.advance_generation()
    }
}

impl Drop for Player {
    fn drop(&mut self) {
        self.shared.playing.store(false, Ordering::Relaxed);
        self.invalidate_stream();
        // Permanently close access and destroy buffers and live inserts before backend stream teardown.
        drop(self.renderer.renderer.retire());
        self.stream = None;
        self.collect_captures();
        // Keep owner pins until backend closure disposal is acknowledged, including delayed Core Audio captures.
        // Waiting is control-side only. Never release the final owning allocation on a backend callback thread.
        wait_for_stream_captures(&mut self.captures);
        let _ = self.commands.send(Command::Shutdown);
        if let Some(thread) = self.decode_thread.take() {
            let _ = thread.join();
        }
    }
}

/// Releases owner pins only after both backend closures have dropped their shared capture references.
fn collect_stream_captures(captures: &mut Vec<Arc<StreamCapture>>) {
    captures.retain(|capture| !StreamCapture::released(capture));
}

/// Waits only on the owner thread before its final pins can be released.
/// A backend that never disposes its callbacks cannot be safely reclaimed by timing out.
fn wait_for_stream_captures(captures: &mut Vec<Arc<StreamCapture>>) {
    collect_stream_captures(captures);
    while !captures.is_empty() {
        std::thread::sleep(CAPTURE_RETIRE_POLL_INTERVAL);
        collect_stream_captures(captures);
    }
}

/// Builds the output stream for the device sample format.
fn build_stream(
    device: &Device,
    config: &StreamConfig,
    sample_format: SampleFormat,
    capture: Arc<StreamCapture>,
) -> Result<Stream, OutputStreamError> {
    match sample_format {
        SampleFormat::F32 => build_typed_stream::<f32>(device, config, capture),
        SampleFormat::F64 => build_typed_stream::<f64>(device, config, capture),
        SampleFormat::I16 => build_typed_stream::<i16>(device, config, capture),
        SampleFormat::I24 => build_typed_stream::<cpal::I24>(device, config, capture),
        SampleFormat::I32 => build_typed_stream::<i32>(device, config, capture),
        SampleFormat::U16 => build_typed_stream::<u16>(device, config, capture),
        other => {
            // No backend closure was created, so both owner pins can be acknowledged here.
            capture.render_released.store(true, Ordering::Release);
            capture.error_released.store(true, Ordering::Release);
            Err(OutputStreamError::UnsupportedSampleFormat(other))
        }
    }
}

/// Runs the complete typed CPAL callback with one non-retrying atomic admission attempt.
/// The stream epoch is checked after admission, before accessing any renderer state.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
fn render_typed_callback<T: SizedSample + FromSample<f32>>(access: &RendererAccess, epoch: u64, output: &mut [T]) {
    let rendered = access.renderer.try_with_if(
        || access.epoch.load(Ordering::Acquire) == epoch,
        |renderer| renderer.render(output, T::from_sample),
    );
    if rendered.is_none() {
        output.fill(T::from_sample(0.0));
    }
}

/// Builds an output stream that renders into samples of type `T`.
fn build_typed_stream<T>(
    device: &Device,
    config: &StreamConfig,
    capture: Arc<StreamCapture>,
) -> Result<Stream, OutputStreamError>
where
    T: SizedSample + FromSample<f32>,
{
    let render = CallbackCapture {
        capture: Arc::clone(&capture),
        is_error: false,
    };
    let errors = CallbackCapture {
        capture,
        is_error: true,
    };
    device
        .build_output_stream(
            *config,
            move |output: &mut [T], _| render.render(output),
            move |error: cpal::Error| errors.report(error),
            None,
        )
        .map_err(OutputStreamError::Open)
}

/// Returns true for the errors after which the stream no longer plays, such as a disconnected device.
const fn is_stream_lost(kind: ErrorKind) -> bool {
    matches!(
        kind,
        ErrorKind::DeviceNotAvailable | ErrorKind::StreamInvalidated | ErrorKind::HostUnavailable
    )
}

/// Formats the source chain only at the user-facing event boundary, never inside the audio callback.
fn error_message(error: &(dyn std::error::Error + 'static)) -> String {
    let mut message = error.to_string();
    let mut source = error.source();
    while let Some(error) = source {
        message.push_str(": ");
        message.push_str(&error.to_string());
        source = error.source();
    }
    message
}

#[cfg(test)]
mod test_errors {
    use super::*;

    /// Checks the bounds required by application error reporting and decode-thread transport.
    const fn assert_error_bounds<T: std::error::Error + Send + Sync + 'static>() {}

    #[test]
    fn errors_support_cross_thread_reporting() {
        assert_error_bounds::<PlayerError>();
        assert_error_bounds::<DeviceError>();
        assert_error_bounds::<InsertError>();
        assert_error_bounds::<OutputStreamError>();
        assert_error_bounds::<pipeline::PipelineError>();
    }

    #[test]
    fn thread_failure_retains_the_io_error_and_event_context() {
        let error = PlayerError::DecodeThread(std::io::Error::other("worker unavailable"));
        assert_eq!(error.to_string(), "Could not start the decode thread");
        assert!(
            std::error::Error::source(&error)
                .expect("thread source")
                .is::<std::io::Error>()
        );
        assert_eq!(
            error_message(&error),
            "Could not start the decode thread: worker unavailable"
        );
    }

    #[test]
    fn stream_errors_keep_their_native_sources_and_context() {
        for (error, message) in [
            (
                OutputStreamError::Open(cpal::Error::new(ErrorKind::DeviceNotAvailable)),
                "Could not open the output stream",
            ),
            (
                OutputStreamError::Start(cpal::Error::new(ErrorKind::DeviceNotAvailable)),
                "Could not start the output stream",
            ),
        ] {
            let source = std::error::Error::source(&error).expect("native stream source");
            assert!(source.is::<cpal::Error>());
            assert_eq!(error.to_string(), message);
            assert_eq!(error_message(&error), format!("{message}: {source}"));
        }
    }

    #[test]
    fn transparent_device_errors_do_not_repeat_context() {
        let native = cpal::Error::new(ErrorKind::DeviceNotAvailable);
        let expected = format!("Could not read the output device configuration: {native}");
        let error = PlayerError::from(DeviceError::DefaultConfiguration(native));
        assert_eq!(error_message(&error), expected);
        let error = OutputStreamError::UnsupportedSampleFormat(SampleFormat::I8);
        assert_eq!(error.to_string(), "Unsupported output sample format i8");
        assert!(std::error::Error::source(&error).is_none());
    }
}
