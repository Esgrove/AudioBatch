//! Preview playback engine for the `AudioBatch` GUI.
//!
//! Plays one audio file at a time through an output device.
//! A decode thread feeds a lock-free `rtrb` ring buffer that the `cpal` audio callback reads,
//! with `rubato` resampling when the file rate differs from the device rate.
//! The engine mirrors the C++ `AudioPreviewPlayer` and has no GUI dependency.
//!
//! `Player` must stay on the thread that created it, because the cpal stream is not `Send` on every platform.

mod device;
mod engine;
mod output;
mod pipeline;

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::thread::JoinHandle;

use anyhow::{Context, Result, bail};
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{BufferSize, Device, FromSample, SampleFormat, SizedSample, Stream, StreamConfig};
use crossbeam_channel::{Receiver, Sender, unbounded};
use rtrb::RingBuffer;

pub use device::{DeviceCapabilities, OutputConfig, device_capabilities};
pub use engine::PlayerEvent;
pub use pipeline::{Frame, FramePipeline};

use device::{device_display_name, find_device, stream_config};
use engine::{Command, run_decode_thread};
use output::{NO_GENERATION, OutputRenderer, SharedState};

/// Ring buffer length in seconds of output audio.
/// Long enough to ride out a slow disk, short enough that a seek drops little decoded audio.
const BUFFER_SECONDS: f64 = 0.5;

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
    decode_thread: Option<JoinHandle<()>>,
    _stream: Stream,
    device_name: String,
    output_rate: u32,
    /// Buffer size in frames the stream was opened with, or `None` for the device default.
    buffer_size: Option<u32>,
    next_generation: u32,
    loaded_path: Option<PathBuf>,
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
    /// Opens the named output device, or the default one, and starts its stream.
    /// The stream outputs silence until a file is loaded and played.
    ///
    /// # Errors
    ///
    /// Returns an error when no output device is available or its stream cannot be started.
    pub fn new(device_name: Option<&str>) -> Result<Self> {
        Self::with_config(&OutputConfig {
            device: device_name.map(str::to_string),
            ..OutputConfig::default()
        })
    }

    /// Opens an output device with a chosen sample rate and buffer size, and starts its stream.
    /// Values the config leaves unset use the device defaults.
    ///
    /// # Errors
    ///
    /// Returns an error when the device is not available, does not support the sample rate,
    /// or its stream cannot be started.
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
        let renderer = OutputRenderer::new(Arc::clone(&shared), consumer, channels);
        let (event_sender, events) = unbounded();
        let stream = build_stream(&device, &config, sample_format, renderer, event_sender.clone())?;
        stream.play().context("Could not start the output stream")?;

        let (commands, command_receiver) = unbounded();
        let thread_shared = Arc::clone(&shared);
        let decode_thread = std::thread::Builder::new()
            .name(String::from("preview-decode"))
            .spawn(move || {
                run_decode_thread(producer, &command_receiver, &event_sender, &thread_shared, output_rate);
            })
            .context("Could not start the decode thread")?;

        Ok(Self {
            shared,
            commands,
            events,
            decode_thread: Some(decode_thread),
            _stream: stream,
            device_name: device_display_name(&device),
            output_rate,
            buffer_size,
            next_generation: NO_GENERATION,
            loaded_path: None,
        })
    }

    /// Returns the names of the available output devices.
    #[must_use]
    pub fn output_device_names() -> Vec<String> {
        cpal::default_host()
            .output_devices()
            .map(|devices| devices.map(|device| device_display_name(&device)).collect())
            .unwrap_or_default()
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
        let at_end = self.shared.finished_generation.load(Ordering::Acquire)
            == self.shared.generation.load(Ordering::Acquire)
            && self
                .duration_seconds()
                .is_some_and(|duration| self.position_seconds() >= duration - 0.01);
        if at_end {
            self.seek(0.0);
        }
        self.shared.playing.store(true, Ordering::Relaxed);
    }

    /// Pauses playback at the current position.
    pub fn pause(&self) {
        self.shared.playing.store(false, Ordering::Relaxed);
    }

    /// Toggles between playing and paused.
    pub fn toggle(&mut self) {
        if self.is_playing() {
            self.pause();
        } else {
            self.play();
        }
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

    /// Sets the linear output gain.
    pub fn set_gain(&self, gain: f32) {
        self.shared.gain_bits.store(gain.max(0.0).to_bits(), Ordering::Relaxed);
    }

    /// Returns the linear output gain.
    #[must_use]
    pub fn gain(&self) -> f32 {
        self.shared.gain()
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

    /// Returns the events since the last call, including the end of playback.
    #[must_use]
    pub fn poll_events(&self) -> Vec<PlayerEvent> {
        let mut events: Vec<PlayerEvent> = self.events.try_iter().collect();
        if self.shared.ended.swap(false, Ordering::AcqRel) {
            events.push(PlayerEvent::Ended);
        }
        events
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
    renderer: OutputRenderer,
    events: Sender<PlayerEvent>,
) -> Result<Stream> {
    match sample_format {
        SampleFormat::F32 => build_typed_stream::<f32>(device, config, renderer, events),
        SampleFormat::F64 => build_typed_stream::<f64>(device, config, renderer, events),
        SampleFormat::I16 => build_typed_stream::<i16>(device, config, renderer, events),
        SampleFormat::I32 => build_typed_stream::<i32>(device, config, renderer, events),
        SampleFormat::U16 => build_typed_stream::<u16>(device, config, renderer, events),
        other => bail!("Unsupported output sample format {other}"),
    }
}

/// Builds an output stream that renders into samples of type `T`.
fn build_typed_stream<T>(
    device: &Device,
    config: &StreamConfig,
    mut renderer: OutputRenderer,
    events: Sender<PlayerEvent>,
) -> Result<Stream>
where
    T: SizedSample + FromSample<f32>,
{
    device
        .build_output_stream(
            *config,
            move |output: &mut [T], _| renderer.render(output, T::from_sample),
            move |error| {
                let _ = events.send(PlayerEvent::Error {
                    path: None,
                    message: format!("Audio output error: {error}"),
                });
            },
            None,
        )
        .context("Could not open the output stream")
}
