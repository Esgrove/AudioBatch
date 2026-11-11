//! Renderer-only benchmark using the production spectrogram pipeline, without audio, analysis, or a window.
//!
//! Input validation, hashing, allocation, warmup, and report serialization are outside measured rendering.
//! GPU timestamps span encoded production passes, not uploads, presentation, or end-to-end display latency.

use std::ffi::OsString;
use std::path::PathBuf;
use std::sync::mpsc;
use std::time::{Duration, Instant};

use iced_baseview::futures::executor::block_on;
use iced_baseview::wgpu;
use image_control_dsp::DisplayPreferences;
use serde::Serialize;
use sha2::{Digest, Sha256};

use crate::spectrogram::BenchmarkRenderer;

const MAGIC: &[u8; 8] = b"ICRB0001";
const HEADER_BYTES: usize = 24;
const ATTRIBUTE_COUNT: usize = 8;
const PARTICLE_BYTES: usize = ATTRIBUTE_COUNT * 4;
const MAX_PARTICLES: usize = 65_536;
const MAX_INPUT_BYTES: u64 = 512 * 1024 * 1024;
const MAX_FRAMES: usize = 100_000;
const BATCH_FRAMES: usize = 32;
const COMPLETION_TIMEOUT: Duration = Duration::from_secs(30);
const OUTPUT_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Bgra8UnormSrgb;

/// Help is recognized before parsing other options, reading files, or creating a GPU instance.
pub const USAGE: &str = "Usage: render_benchmark [OPTIONS]
  -h, --help                 Show help before filesystem or GPU initialization
  -i, --input PATH           ICRB0001 little-endian particle workload (required)
  -w, --width W              Physical output width, 1..4096 [1000]
  -H, --height H             Physical output height, 1..4096 [600]
  -s, --scale F              Display scale, 0 < F <= 8 [1]
  -m, --mode MODE            particles|solid [particles]
  -f, --frames N             Measured frames, 1..100000 [120]
  -W, --warmup N             Excluded warmup frames, 0..100000 [32]
  -r, --report PATH          JSON output path (required)
  -T, --timestamps MODE      auto|off [auto]

Uses headless Metal on macOS and DX12 on Windows, with production shaders and passes.
Input frames repeat cyclically across warmup and measurement. Width and height are physical pixels.
Scale controls production particle sizing and corner rounding, not output dimensions.
Timestamp support is detected and explicitly enabled on the device when available.
Completion throughput includes delayed timestamp readback and a bounded wait every 32 frames.
No pixels are read, no presentation occurs, and no per-frame GPU completion wait is used.
";

/// Failures are explicit so unavailable GPU timing never masquerades as a successful zero measurement.
#[derive(Debug, thiserror::Error)]
pub enum BenchmarkError {
    /// Filesystem failures outside measured rendering.
    #[error(transparent)]
    Io(#[from] std::io::Error),
    /// Invalid workload or command-line input.
    #[error("{0}")]
    Invalid(String),
    /// Adapter, device, bounded completion, or readback failure.
    #[error("GPU benchmark failed: {0}")]
    Gpu(String),
}

/// Fully validated CLI settings, constructed before any GPU initialization.
pub struct Options {
    input: PathBuf,
    report: PathBuf,
    width: u32,
    height: u32,
    scale: f32,
    mode: String,
    frames: usize,
    warmup: usize,
    timestamps: bool,
}

/// Decoded and hashed immutable workload, never parsed inside the measured frame loop.
struct Workload {
    frames: Vec<Vec<[f32; ATTRIBUTE_COUNT]>>,
    particles_per_frame: usize,
    step_seconds: f32,
    persistence: f32,
    sha256: String,
    fnv1a64: String,
    byte_count: usize,
}

/// Actual production intermediate dimensions, including floor-halved bloom levels.
#[derive(Serialize)]
pub(crate) struct TargetSizes {
    pub(crate) accumulation: [u32; 2],
    pub(crate) bloom: [[u32; 2]; 5],
    pub(crate) solid_field: [u32; 2],
}

/// Versioned artifact retaining raw timings rather than only unstable aggregate percentiles.
#[derive(Serialize)]
pub struct Report {
    schema_version: u32,
    renderer: &'static str,
    adapter: AdapterReport,
    workload: WorkloadReport,
    settings: SettingsReport,
    targets: TargetsReport,
    timing: TimingReport,
    frames: Vec<FrameTiming>,
}

/// Native identity and timestamp capability requested explicitly during device creation.
#[derive(Serialize)]
struct AdapterReport {
    name: String,
    backend: String,
    device_type: String,
    vendor: u32,
    device: u32,
    driver: String,
    driver_info: String,
    timestamp_supported: bool,
    timestamp_enabled: bool,
    timestamp_status: String,
    timestamp_period_ns: Option<f32>,
}

/// Exact source identity and deterministic animation settings.
#[derive(Serialize)]
struct WorkloadReport {
    path: PathBuf,
    sha256: String,
    fnv1a64: String,
    byte_count: usize,
    frame_count: usize,
    particles_per_frame: usize,
    step_seconds: f32,
    persistence_seconds: f32,
}

/// Requested physical geometry and scale are separate from bounded intermediate dimensions.
#[derive(Serialize)]
struct SettingsReport {
    physical_size: [u32; 2],
    scale: f32,
    mode: String,
    measured_frames: usize,
    warmup_frames: usize,
}

/// Native output and actual float history dimensions and formats.
#[derive(Serialize)]
struct TargetsReport {
    output_size: [u32; 2],
    output_format: String,
    energy_format: &'static str,
    sizes: TargetSizes,
}

/// Throughput ends only after all submitted measured work and timestamp transfers complete.
#[derive(Serialize)]
struct TimingReport {
    elapsed_seconds: f64,
    completed_frames: usize,
    frames_per_second: f64,
    completion_batch_frames: usize,
    completion_batches: usize,
    completion_timeout_seconds: u64,
    gpu_timing_scope: &'static str,
    cpu_timing_scope: &'static str,
}

/// Raw CPU stages exclude completion waits. GPU spans all production passes for this frame.
#[derive(Serialize)]
struct FrameTiming {
    frame_index: usize,
    input_frame_index: usize,
    cpu_prepare_ms: f64,
    cpu_upload_ms: f64,
    cpu_encode_ms: f64,
    cpu_submit_ms: f64,
    cpu_ms: f64,
    gpu_ms: Option<f64>,
}

/// One delayed timestamp slot is reused only after the entire bounded batch completes.
struct TimestampSlot {
    queries: wgpu::QuerySet,
    resolve: wgpu::Buffer,
    readback: wgpu::Buffer,
}

/// A fixed-size ring bounds in-flight work and optional timestamp ownership.
struct Runner {
    device: wgpu::Device,
    queue: wgpu::Queue,
    renderer: BenchmarkRenderer,
    target: wgpu::TextureView,
    output_size: [u32; 2],
    timestamps: Vec<TimestampSlot>,
    invalid_timestamps: bool,
}

/// Physical production-renderer configuration for an in-process benchmark, independent of filesystem reports.
#[derive(Clone, Copy, Debug)]
pub struct RenderSettings {
    /// Output and accumulation width in physical pixels, limited to the production target bounds.
    pub width: u32,
    /// Output and accumulation height in physical pixels, limited to the production target bounds.
    pub height: u32,
    /// Sprite and rounded-corner scale, independent of the physical dimensions.
    pub scale: f32,
    /// Includes production solid-density layers and contours when enabled.
    pub solid: bool,
}

/// Persistent native resources for Criterion. Construction, parsing, and cold warmup are outside measurement.
/// The caller must run instances sequentially so separate cases do not compete for the GPU.
pub struct PreparedRenderer {
    runner: Runner,
    workload: Workload,
    next_input: usize,
}

impl PreparedRenderer {
    /// Validates immutable shared-workload bytes and primes all production passes with timestamps disabled.
    ///
    /// # Errors
    /// Rejects invalid input/geometry, unavailable native GPUs, or failed bounded warmup completion.
    pub fn new(input: &[u8], settings: RenderSettings) -> Result<Self, BenchmarkError> {
        if !(1..=4096).contains(&settings.width)
            || !(1..=4096).contains(&settings.height)
            || !settings.scale.is_finite()
            || !(0.125..=8.0).contains(&settings.scale)
        {
            return Err(BenchmarkError::Invalid("invalid in-process renderer geometry".into()));
        }
        let workload = Workload::parse(input)?;
        let options = Options {
            input: PathBuf::new(),
            report: PathBuf::new(),
            width: settings.width,
            height: settings.height,
            scale: settings.scale,
            mode: if settings.solid { "solid" } else { "particles" }.into(),
            frames: BATCH_FRAMES,
            warmup: BATCH_FRAMES,
            timestamps: false,
        };
        let initialized = initialize(&options, &workload)?;
        let mut prepared = Self {
            runner: initialized.runner,
            workload,
            next_input: 0,
        };
        prepared.measure_completed_frames(BATCH_FRAMES as u64)?;
        prepared.validate()?;
        // Ordinary samples need not allocate or resolve native timestamp queries.
        prepared.runner.device.push_error_scope(wgpu::ErrorFilter::Validation);
        Ok(prepared)
    }

    /// Packs the next immutable particle frame into reused production upload storage without GPU submission.
    /// This isolates CPU attribute preparation, not rendering or GPU completion.
    ///
    /// # Errors
    /// Reports an inconsistent workload cursor rather than indexing unchecked input.
    pub fn prepare_particles(&mut self) -> Result<usize, BenchmarkError> {
        let particles = self
            .workload
            .frames
            .get(self.next_input)
            .ok_or_else(|| BenchmarkError::Invalid("missing prepared workload frame".into()))?;
        self.runner.renderer.prepare(particles);
        let count = particles.len();
        self.advance_input();
        Ok(count)
    }

    /// Measures packing, upload, encoding, submission, and completed GPU work for Criterion's frame iterations.
    /// Waits only at bounded 32-frame batch boundaries, never after every frame.
    ///
    /// # Errors
    /// Reports inconsistent workload state or a native completion timeout/failure.
    pub fn measure_completed_frames(&mut self, frames: u64) -> Result<Duration, BenchmarkError> {
        if frames == 0 {
            return Ok(Duration::ZERO);
        }
        let started = Instant::now();
        self.measure_batches(frames, false)?;
        Ok(started.elapsed())
    }

    /// Measures CPU batch submission with completion waits outside the returned duration.
    /// Queue backpressure within submission is included. This is not exclusive CPU or GPU execution time.
    ///
    /// # Errors
    /// Reports inconsistent workload state or a native completion timeout/failure.
    pub fn measure_submission_frames(&mut self, frames: u64) -> Result<Duration, BenchmarkError> {
        self.measure_batches(frames, true)
    }

    /// Checks accumulated native validation errors after the sample loop, outside measured intervals.
    ///
    /// # Errors
    /// Returns a native validation failure. Call once when finished with this benchmark case.
    pub fn validate(&self) -> Result<(), BenchmarkError> {
        if let Some(error) = block_on(self.runner.device.pop_error_scope()) {
            return Err(BenchmarkError::Gpu(error.to_string()));
        }
        Ok(())
    }

    /// Advances through stored frames without a wall-clock animation or data-dependent input generator.
    const fn advance_input(&mut self) {
        self.next_input = (self.next_input + 1) % self.workload.frames.len();
    }

    /// Keeps at most one bounded batch in flight and drains it before returning to Criterion.
    fn measure_batches(&mut self, mut remaining: u64, submission_only: bool) -> Result<Duration, BenchmarkError> {
        let mut submitted = Duration::ZERO;
        while remaining > 0 {
            let batch = remaining.min(BATCH_FRAMES as u64);
            let started = Instant::now();
            for _ in 0..batch {
                self.prepare_particles()?;
                self.runner
                    .renderer
                    .upload(&self.runner.queue, self.workload.step_seconds);
                let mut encoder = self
                    .runner
                    .device
                    .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                        label: Some("ImageControl Criterion production frame"),
                    });
                self.runner
                    .renderer
                    .encode(&mut encoder, &self.runner.target, self.runner.output_size, None);
                self.runner.queue.submit([encoder.finish()]);
            }
            if submission_only {
                submitted += started.elapsed();
            }
            self.runner.complete(&mut [])?;
            remaining -= batch;
        }
        Ok(submitted)
    }
}

/// Parses UTF-8 numeric values while keeping filesystem paths in their native representation.
fn number<T: std::str::FromStr>(value: &OsString, option: &str) -> Result<T, BenchmarkError> {
    value
        .to_str()
        .and_then(|text| text.parse().ok())
        .ok_or_else(|| BenchmarkError::Invalid(format!("{option} requires a number")))
}

/// Validates all settings before reading input or creating an instance.
///
/// # Errors
/// Rejects unknown options, missing values, invalid dimensions, and unbounded frame counts.
pub fn parse_options(arguments: Vec<OsString>) -> Result<Option<Options>, BenchmarkError> {
    if arguments
        .iter()
        .any(|argument| argument == "--help" || argument == "-h")
    {
        return Ok(None);
    }
    let mut options = Options {
        input: PathBuf::new(),
        report: PathBuf::new(),
        width: 1000,
        height: 600,
        scale: 1.0,
        mode: "particles".into(),
        frames: 120,
        warmup: 32,
        timestamps: true,
    };
    let mut arguments = arguments.into_iter();
    while let Some(option) = arguments.next() {
        let option = option
            .to_str()
            .ok_or_else(|| BenchmarkError::Invalid("option must be UTF-8".into()))?;
        let value = arguments
            .next()
            .ok_or_else(|| BenchmarkError::Invalid(format!("{option} requires a value")))?;
        match option {
            "--input" | "-i" => options.input = value.into(),
            "--report" | "-r" => options.report = value.into(),
            "--width" | "-w" => options.width = number(&value, option)?,
            "--height" | "-H" => options.height = number(&value, option)?,
            "--scale" | "-s" => options.scale = number(&value, option)?,
            "--frames" | "-f" => options.frames = number(&value, option)?,
            "--warmup" | "-W" => options.warmup = number(&value, option)?,
            "--mode" | "-m" => {
                options.mode = value
                    .into_string()
                    .map_err(|_| BenchmarkError::Invalid("mode must be UTF-8".into()))?;
            }
            "--timestamps" | "-T" => {
                options.timestamps = match value.to_str() {
                    Some("auto") => true,
                    Some("off") => false,
                    _ => return Err(BenchmarkError::Invalid("--timestamps must be auto or off".into())),
                }
            }
            _ => return Err(BenchmarkError::Invalid(format!("unknown option {option}"))),
        }
    }
    if options.input.as_os_str().is_empty() || options.report.as_os_str().is_empty() {
        return Err(BenchmarkError::Invalid("--input and --report are required".into()));
    }
    if !(1..=4096).contains(&options.width)
        || !(1..=4096).contains(&options.height)
        || !options.scale.is_finite()
        || options.scale <= 0.0
        || options.scale > 8.0
        || !(1..=MAX_FRAMES).contains(&options.frames)
        || options.warmup > MAX_FRAMES
        || !matches!(options.mode.as_str(), "particles" | "solid")
    {
        return Err(BenchmarkError::Invalid(
            "invalid dimensions, scale, mode, or frame count. See --help".into(),
        ));
    }
    Ok(Some(options))
}

/// Computes the dependency-free shared input identity, independently of the stronger provenance hash.
fn fnv1a64_hex(bytes: &[u8]) -> String {
    let value = bytes.iter().fold(0xcbf2_9ce4_8422_2325_u64, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(0x0100_0000_01b3)
    });
    format!("{value:016x}")
}

/// Formats the complete source identity outside all measured rendering stages.
fn sha256_hex(bytes: &[u8]) -> String {
    use std::fmt::Write;
    let mut output = String::with_capacity(64);
    for byte in Sha256::digest(bytes) {
        let _ = write!(output, "{byte:02x}");
    }
    output
}

/// Consumes exactly one little-endian 32-bit word without alignment assumptions or unchecked slicing.
fn word(bytes: &mut &[u8]) -> Result<u32, BenchmarkError> {
    let (value, remainder) = bytes
        .split_first_chunk::<4>()
        .ok_or_else(|| BenchmarkError::Invalid("truncated workload".into()))?;
    *bytes = remainder;
    Ok(u32::from_le_bytes(*value))
}

impl Workload {
    /// Validates exact size, finite attributes, normalized domains, and the immutable file hash before timing.
    fn parse(bytes: &[u8]) -> Result<Self, BenchmarkError> {
        let invalid = |message: &str| BenchmarkError::Invalid(message.into());
        let (magic, mut data) = bytes
            .split_first_chunk::<8>()
            .ok_or_else(|| invalid("truncated header"))?;
        if magic != MAGIC || bytes.len() as u64 > MAX_INPUT_BYTES {
            return Err(invalid("invalid ICRB0001 magic or oversized input"));
        }
        let frame_count = word(&mut data)? as usize;
        let particles_per_frame = word(&mut data)? as usize;
        let step_seconds = f32::from_bits(word(&mut data)?);
        let persistence = f32::from_bits(word(&mut data)?);
        if !(1..=MAX_FRAMES).contains(&frame_count)
            || particles_per_frame > MAX_PARTICLES
            || !step_seconds.is_finite()
            || step_seconds <= 0.0
            || step_seconds > 0.1
            || !persistence.is_finite()
            || !(0.05..=3.0).contains(&persistence)
        {
            return Err(invalid("invalid frame count, particle count, step, or persistence"));
        }
        let expected = frame_count
            .checked_mul(particles_per_frame)
            .and_then(|count| count.checked_mul(PARTICLE_BYTES))
            .and_then(|count| count.checked_add(HEADER_BYTES))
            .ok_or_else(|| invalid("workload size overflow"))?;
        if expected != bytes.len() {
            return Err(invalid("workload size does not exactly match its header"));
        }
        let mut frames = Vec::with_capacity(frame_count);
        for _ in 0..frame_count {
            let mut frame = Vec::with_capacity(particles_per_frame);
            for _ in 0..particles_per_frame {
                let mut attributes = [0.0; ATTRIBUTE_COUNT];
                for attribute in &mut attributes {
                    *attribute = f32::from_bits(word(&mut data)?);
                }
                let [x, y, spread, red, green, blue, size, emphasis] = attributes;
                if !attributes.iter().all(|value| value.is_finite())
                    || ![x, y, emphasis].iter().all(|value| (0.0..=1.0).contains(value))
                    || ![spread, red, green, blue, size].iter().all(|value| *value >= 0.0)
                {
                    return Err(invalid(
                        "particle attributes are non-finite or outside production domains",
                    ));
                }
                frame.push(attributes);
            }
            frames.push(frame);
        }
        Ok(Self {
            frames,
            particles_per_frame,
            step_seconds,
            persistence,
            sha256: sha256_hex(bytes),
            fnv1a64: fnv1a64_hex(bytes),
            byte_count: bytes.len(),
        })
    }
}

/// Rejects unavailable, sentinel, non-monotonic, and physically unbounded native timestamp intervals.
fn timestamp_milliseconds(start: u64, end: u64, period: f32) -> Option<f64> {
    if [start, end].iter().any(|tick| *tick == 0 || *tick == u64::MAX) {
        return None;
    }
    let milliseconds = end.checked_sub(start)? as f64 * f64::from(period) / 1_000_000.0;
    (milliseconds.is_finite() && milliseconds > 0.0 && milliseconds <= COMPLETION_TIMEOUT.as_secs_f64() * 1000.0)
        .then_some(milliseconds)
}

impl TimestampSlot {
    /// Allocates an independent query pair and aligned resolve/readback storage outside timing.
    fn new(device: &wgpu::Device) -> Self {
        let queries = device.create_query_set(&wgpu::QuerySetDescriptor {
            label: Some("ImageControl benchmark frame timestamps"),
            ty: wgpu::QueryType::Timestamp,
            count: 2,
        });
        let buffer = |label, usage| {
            device.create_buffer(&wgpu::BufferDescriptor {
                label: Some(label),
                size: 16,
                usage,
                mapped_at_creation: false,
            })
        };
        Self {
            queries,
            resolve: buffer(
                "ImageControl benchmark timestamp resolve",
                wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
            ),
            readback: buffer(
                "ImageControl benchmark timestamp readback",
                wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            ),
        }
    }

    /// Reads only timestamp integers after delayed batch completion, never texture pixels.
    fn milliseconds(&self, period: f32) -> Result<Option<f64>, BenchmarkError> {
        let mapped = self.readback.slice(..).get_mapped_range();
        let mut ticks = mapped.as_ref();
        let read_tick = |bytes: &mut &[u8]| -> Result<u64, BenchmarkError> {
            let (value, rest) = bytes
                .split_first_chunk::<8>()
                .ok_or_else(|| BenchmarkError::Gpu("truncated timestamp readback".into()))?;
            *bytes = rest;
            Ok(u64::from_ne_bytes(*value))
        };
        let start = read_tick(&mut ticks)?;
        let end = read_tick(&mut ticks)?;
        let elapsed = timestamp_milliseconds(start, end, period);
        drop(mapped);
        self.readback.unmap();
        Ok(elapsed)
    }
}

impl Runner {
    /// Times one frame's independent CPU stages, without waiting for GPU completion.
    fn frame(
        &mut self,
        workload: &Workload,
        index: usize,
        input_offset: usize,
        slot: usize,
    ) -> Result<FrameTiming, BenchmarkError> {
        let input_frame_index = (input_offset + index) % workload.frames.len();
        let particles = workload
            .frames
            .get(input_frame_index)
            .ok_or_else(|| BenchmarkError::Invalid("missing workload frame".into()))?;
        let start = Instant::now();
        self.renderer.prepare(particles);
        let cpu_prepare_ms = start.elapsed().as_secs_f64() * 1000.0;
        let start = Instant::now();
        self.renderer.upload(&self.queue, workload.step_seconds);
        let cpu_upload_ms = start.elapsed().as_secs_f64() * 1000.0;
        let start = Instant::now();
        let mut encoder = self.device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("ImageControl renderer benchmark"),
        });
        let timestamp = self.timestamps.get(slot);
        self.renderer.encode(
            &mut encoder,
            &self.target,
            self.output_size,
            timestamp.map(|slot| &slot.queries),
        );
        if let Some(timestamp) = timestamp {
            encoder.resolve_query_set(&timestamp.queries, 0..2, &timestamp.resolve, 0);
            encoder.copy_buffer_to_buffer(&timestamp.resolve, 0, &timestamp.readback, 0, 16);
        }
        let command_buffer = encoder.finish();
        let cpu_encode_ms = start.elapsed().as_secs_f64() * 1000.0;
        let start = Instant::now();
        self.queue.submit([command_buffer]);
        let cpu_submit_ms = start.elapsed().as_secs_f64() * 1000.0;
        Ok(FrameTiming {
            frame_index: index,
            input_frame_index,
            cpu_prepare_ms,
            cpu_upload_ms,
            cpu_encode_ms,
            cpu_submit_ms,
            cpu_ms: cpu_prepare_ms + cpu_upload_ms + cpu_encode_ms + cpu_submit_ms,
            gpu_ms: None,
        })
    }

    /// Drains one fixed-size ring after submitting the full batch, bounding waits without per-frame polling.
    fn complete(&mut self, frames: &mut [FrameTiming]) -> Result<(), BenchmarkError> {
        let (sender, receiver) = mpsc::channel();
        for timestamp in self.timestamps.iter().take(frames.len()) {
            let sender = sender.clone();
            timestamp
                .readback
                .slice(..)
                .map_async(wgpu::MapMode::Read, move |result| {
                    let _ = sender.send(result);
                });
        }
        self.device
            .poll(wgpu::PollType::Wait {
                submission_index: None,
                timeout: Some(COMPLETION_TIMEOUT),
            })
            .map_err(|error| BenchmarkError::Gpu(error.to_string()))?;
        for _ in self.timestamps.iter().take(frames.len()) {
            receiver
                .recv_timeout(COMPLETION_TIMEOUT)
                .map_err(|error| BenchmarkError::Gpu(error.to_string()))?
                .map_err(|error| BenchmarkError::Gpu(error.to_string()))?;
        }
        for (frame, timestamp) in frames.iter_mut().zip(&self.timestamps) {
            frame.gpu_ms = timestamp.milliseconds(self.queue.get_timestamp_period())?;
            self.invalid_timestamps |= frame.gpu_ms.is_none();
        }
        Ok(())
    }

    /// Measures bounded batches through actual completion into records allocated before timing.
    fn run(
        &mut self,
        workload: &Workload,
        count: usize,
        input_offset: usize,
        records: &mut Vec<FrameTiming>,
    ) -> Result<(), BenchmarkError> {
        for first in (0..count).step_by(BATCH_FRAMES) {
            let length = (count - first).min(BATCH_FRAMES);
            for slot in 0..length {
                records.push(self.frame(workload, first + slot, input_offset, slot)?);
            }
            let batch = records
                .get_mut(first..)
                .ok_or_else(|| BenchmarkError::Gpu("missing completion batch".into()))?;
            self.complete(batch)?;
        }
        Ok(())
    }
}

/// Keeps native initialization results separate from measured records without positional tuple fields.
struct InitializedRenderer {
    runner: Runner,
    adapter: AdapterReport,
}

/// Creates the headless native device and all production resources outside measured work.
fn initialize(options: &Options, workload: &Workload) -> Result<InitializedRenderer, BenchmarkError> {
    let backends = if cfg!(target_os = "macos") {
        wgpu::Backends::METAL
    } else {
        wgpu::Backends::DX12
    };
    let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
        backends,
        ..Default::default()
    });
    let adapter = block_on(instance.request_adapter(&wgpu::RequestAdapterOptions::default()))
        .map_err(|error| BenchmarkError::Gpu(error.to_string()))?;
    let info = adapter.get_info();
    let timestamp_features = wgpu::Features::TIMESTAMP_QUERY;
    let timestamp_supported = adapter.features().contains(timestamp_features);
    let timestamp_enabled = options.timestamps && timestamp_supported;
    let (device, queue) = block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        label: Some("ImageControl renderer benchmark"),
        required_features: if timestamp_enabled {
            timestamp_features
        } else {
            wgpu::Features::empty()
        },
        ..Default::default()
    }))
    .map_err(|error| BenchmarkError::Gpu(error.to_string()))?;
    let output_size = [options.width, options.height];
    if output_size
        .iter()
        .any(|dimension| *dimension > device.limits().max_texture_dimension_2d)
    {
        return Err(BenchmarkError::Invalid(
            "physical output exceeds the native device limit".into(),
        ));
    }
    device.push_error_scope(wgpu::ErrorFilter::Validation);
    let target = device
        .create_texture(&wgpu::TextureDescriptor {
            label: Some("ImageControl headless benchmark output"),
            size: wgpu::Extent3d {
                width: output_size[0],
                height: output_size[1],
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: OUTPUT_FORMAT,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            view_formats: &[],
        })
        .create_view(&wgpu::TextureViewDescriptor::default());
    let renderer = BenchmarkRenderer::new(
        &device,
        &queue,
        OUTPUT_FORMAT,
        output_size,
        options.scale,
        DisplayPreferences {
            persistence_seconds: workload.persistence,
            solid_rendering: options.mode == "solid",
            ..Default::default()
        },
    );
    let timestamps = if timestamp_enabled {
        (0..BATCH_FRAMES).map(|_| TimestampSlot::new(&device)).collect()
    } else {
        Vec::new()
    };
    let adapter_report = AdapterReport {
        name: info.name,
        backend: format!("{:?}", info.backend),
        device_type: format!("{:?}", info.device_type),
        vendor: info.vendor,
        device: info.device,
        driver: info.driver,
        driver_info: info.driver_info,
        timestamp_supported,
        timestamp_enabled,
        timestamp_status: if timestamp_enabled {
            "requested; awaiting validation"
        } else if timestamp_supported {
            "disabled by --timestamps off"
        } else {
            "unsupported by native adapter"
        }
        .into(),
        timestamp_period_ns: timestamp_enabled.then(|| queue.get_timestamp_period()),
    };
    Ok(InitializedRenderer {
        runner: Runner {
            device,
            queue,
            renderer,
            target,
            output_size,
            timestamps,
            invalid_timestamps: false,
        },
        adapter: adapter_report,
    })
}

/// Runs the actual headless production renderer after validating and hashing the complete workload.
///
/// # Errors
/// Returns file validation, native adapter/device, or bounded completion/readback failures.
pub fn run(options: &Options) -> Result<Report, BenchmarkError> {
    crate::benchmark_build::require_optimized_native().map_err(|message| BenchmarkError::Invalid(message.into()))?;
    if std::fs::metadata(&options.input)?.len() > MAX_INPUT_BYTES {
        return Err(BenchmarkError::Invalid("input exceeds the 512 MiB bound".into()));
    }
    let workload = Workload::parse(&std::fs::read(&options.input)?)?;
    let InitializedRenderer {
        mut runner,
        mut adapter,
    } = initialize(options, &workload)?;
    let sizes = runner.renderer.target_sizes();
    let output_size = runner.output_size;
    let mut warmup = Vec::with_capacity(options.warmup);
    let mut frames = Vec::with_capacity(options.frames);
    runner.run(&workload, options.warmup, 0, &mut warmup)?;
    // Complete cold pipeline work even when the caller explicitly requests no warmup.
    runner
        .device
        .poll(wgpu::PollType::Wait {
            submission_index: None,
            timeout: Some(COMPLETION_TIMEOUT),
        })
        .map_err(|error| BenchmarkError::Gpu(error.to_string()))?;
    let start = Instant::now();
    runner.run(&workload, options.frames, options.warmup, &mut frames)?;
    let elapsed_seconds = start.elapsed().as_secs_f64();
    if adapter.timestamp_enabled {
        adapter.timestamp_status = if runner.invalid_timestamps {
            "native adapter advertised timestamps but returned zero or non-monotonic intervals; invalid samples are null; use completion throughput"
        } else { "validated positive monotonic native render-pass intervals" }.into();
    }
    if let Some(error) = block_on(runner.device.pop_error_scope()) {
        return Err(BenchmarkError::Gpu(error.to_string()));
    }
    Ok(Report {
        schema_version: 1,
        renderer: "rust-wgpu",
        adapter,
        workload: WorkloadReport {
            path: options.input.clone(),
            sha256: workload.sha256,
            fnv1a64: workload.fnv1a64,
            byte_count: workload.byte_count,
            frame_count: workload.frames.len(),
            particles_per_frame: workload.particles_per_frame,
            step_seconds: workload.step_seconds,
            persistence_seconds: workload.persistence,
        },
        settings: SettingsReport {
            physical_size: [options.width, options.height],
            scale: options.scale,
            mode: options.mode.clone(),
            measured_frames: options.frames,
            warmup_frames: options.warmup,
        },
        targets: TargetsReport {
            output_size,
            output_format: format!("{OUTPUT_FORMAT:?}"),
            energy_format: "Rgba16Float",
            sizes,
        },
        timing: TimingReport {
            elapsed_seconds,
            completed_frames: frames.len(),
            frames_per_second: frames.len() as f64 / elapsed_seconds,
            completion_batch_frames: BATCH_FRAMES,
            completion_batches: options.frames.div_ceil(BATCH_FRAMES),
            completion_timeout_seconds: COMPLETION_TIMEOUT.as_secs(),
            gpu_timing_scope: "native render-pass timestamps from fade start through composite end; excludes upload, resolve, readback, presentation",
            cpu_timing_scope: "prepare=attribute packing; upload=queue writes and uniforms; encode=encoder creation through finish including timestamp resolve; submit=queue.submit; elapsed includes batch completion/readback",
        },
        frames,
    })
}

/// Writes the versioned artifact only after successful GPU completion, outside all recorded timings.
///
/// # Errors
/// Returns JSON serialization or filesystem failures without creating a misleading success report.
pub fn write_report(options: &Options, report: &Report) -> Result<(), Box<dyn std::error::Error>> {
    let shared = shared_report(report);
    let bytes = serde_json::to_vec_pretty(&shared)?;
    std::fs::write(&options.report, bytes)?;
    Ok(())
}

/// Exposes the shared flat comparison contract while retaining every native stage sample in `details`.
/// Native compiler invocation and provenance collection occur after timing, never inside the plugin renderer.
fn shared_report(report: &Report) -> serde_json::Value {
    let target = |name: String, size: [u32; 2], format: &str| {
        serde_json::json!({
            "name": name, "width": size[0], "height": size[1], "format": format,
        })
    };
    let mut targets = vec![target(
        "output".into(),
        report.targets.output_size,
        &report.targets.output_format,
    )];
    for index in 0..2 {
        targets.push(target(
            format!("accumulation-{index}"),
            report.targets.sizes.accumulation,
            report.targets.energy_format,
        ));
    }
    for (index, size) in report.targets.sizes.bloom.iter().enumerate() {
        targets.push(target(format!("bloom-{index}"), *size, report.targets.energy_format));
    }
    targets.push(target(
        "solid-field".into(),
        report.targets.sizes.solid_field,
        report.targets.energy_format,
    ));
    let compiler = std::process::Command::new("rustc")
        .arg("--version")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .map_or_else(
            || "rustc version unavailable at runtime; compiler provenance belongs to executable build manifest".into(),
            |output| format!("{} (runtime toolchain diagnostic; executable build compiler must be captured by the build manifest)", output.trim()),
        );
    let driver = if report.adapter.driver.is_empty() {
        "native backend driver version not exposed by wgpu".to_owned()
    } else {
        format!("{} {}", report.adapter.driver, report.adapter.driver_info)
    };
    let complete_gpu_samples =
        report.adapter.timestamp_enabled && report.frames.iter().all(|frame| frame.gpu_ms.is_some());
    let gpu_frame_ns = complete_gpu_samples.then(|| {
        report
            .frames
            .iter()
            .map(|frame| frame.gpu_ms.map(|value| value * 1_000_000.0))
            .collect::<Vec<_>>()
    });
    serde_json::json!({
        "schema_version": 1, "implementation": "rust", "render_scope": "offscreen-production-renderer",
        "input_fnv1a64": report.workload.fnv1a64,
        "width": report.targets.output_size[0], "height": report.targets.output_size[1], "scale": report.settings.scale,
        "mode": report.settings.mode, "frames": report.settings.measured_frames, "warmup": report.settings.warmup_frames,
        "batch_frames": report.timing.completion_batch_frames,
        "particles_per_frame": report.workload.particles_per_frame, "step_seconds": report.workload.step_seconds,
        "persistence_seconds": report.workload.persistence_seconds,
        "backend": report.adapter.backend, "gpu": report.adapter.name, "vendor": format!("0x{:04x}", report.adapter.vendor),
        "driver": driver, "compiler": compiler, "build_type": if cfg!(debug_assertions) { "Debug" } else { "Release" },
        "build_profile": crate::benchmark_build::PROFILE,
        "opt_level": crate::benchmark_build::OPT_LEVEL,
        "native_cpu_enabled": crate::benchmark_build::NATIVE_CPU == "true",
        "accumulation_format": report.targets.energy_format, "output_format": report.targets.output_format,
        "targets": targets, "capabilities": { "gpu_timestamps": report.adapter.timestamp_supported },
        "cpu_frame_ns": report.frames.iter().map(|frame| frame.cpu_ms * 1_000_000.0).collect::<Vec<_>>(),
        "gpu_frame_ns": gpu_frame_ns, "gpu_batch_ns": null, "batch_wall_ns": report.timing.elapsed_seconds * 1_000_000_000.0,
        "timing_method": { "cpu": "submit-and-encode", "gpu": if complete_gpu_samples { "per-frame-timestamps" } else { "unsupported" } },
        "details": report,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Builds an independently specified little-endian workload without GPU or DSP ownership.
    fn fixture() -> Vec<u8> {
        let mut bytes = MAGIC.to_vec();
        bytes.extend_from_slice(&1_u32.to_le_bytes());
        bytes.extend_from_slice(&1_u32.to_le_bytes());
        bytes.extend_from_slice(&0.02_f32.to_le_bytes());
        bytes.extend_from_slice(&0.3_f32.to_le_bytes());
        for value in [0.25_f32, 0.75, 0.01, 2.0, 3.0, 4.0, 1.5, 0.5] {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        bytes
    }

    #[test]
    fn prepared_renderer_rejects_invalid_configuration_before_gpu_initialization() {
        let settings = RenderSettings {
            width: 64,
            height: 32,
            scale: 1.0,
            solid: false,
        };
        for invalid in [
            RenderSettings { width: 0, ..settings },
            RenderSettings {
                height: 4097,
                ..settings
            },
            RenderSettings {
                scale: f32::NAN,
                ..settings
            },
            RenderSettings { scale: 0.0, ..settings },
        ] {
            assert!(matches!(
                PreparedRenderer::new(&fixture(), invalid),
                Err(BenchmarkError::Invalid(_))
            ));
        }
        assert!(matches!(
            PreparedRenderer::new(b"truncated", settings),
            Err(BenchmarkError::Invalid(_))
        ));
    }

    #[test]
    fn prepared_renderer_executes_both_timing_boundaries_with_bounded_completion() {
        for solid in [false, true] {
            let mut prepared = PreparedRenderer::new(
                &fixture(),
                RenderSettings {
                    width: 64,
                    height: 32,
                    scale: 1.0,
                    solid,
                },
            )
            .expect("native production benchmark");
            assert!(prepared.runner.timestamps.is_empty());
            assert_eq!(prepared.prepare_particles().expect("packing"), 1);
            assert_eq!(prepared.measure_completed_frames(0).expect("empty"), Duration::ZERO);
            prepared.measure_submission_frames(33).expect("submission batches");
            prepared.measure_completed_frames(33).expect("completed batches");
            prepared.validate().expect("native validation");
        }
    }

    #[test]
    fn parser_retains_normalized_attributes_and_hdr_energy() {
        let workload = Workload::parse(&fixture()).expect("valid workload");
        assert_eq!(workload.frames.len(), 1);
        assert_eq!(workload.particles_per_frame, 1);
        assert_eq!(
            workload
                .frames
                .first()
                .and_then(|frame| frame.first())
                .expect("particle")
                .map(f32::to_bits),
            [0.25_f32, 0.75, 0.01, 2.0, 3.0, 4.0, 1.5, 0.5].map(f32::to_bits)
        );
        assert_eq!(
            sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(workload.sha256, sha256_hex(&fixture()));
        assert_eq!(fnv1a64_hex(b"hello"), "a430d84680aabd0b");
    }

    #[test]
    fn parser_rejects_truncation_trailing_data_and_invalid_domains() {
        let bytes = fixture();
        for end in 0..bytes.len() {
            assert!(Workload::parse(bytes.get(..end).expect("prefix")).is_err());
        }
        let mut trailing = bytes.clone();
        trailing.push(0);
        assert!(Workload::parse(&trailing).is_err());
        for offset in [0, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52] {
            let mut invalid = bytes.clone();
            invalid
                .get_mut(offset..offset + 4)
                .expect("field")
                .copy_from_slice(&f32::NAN.to_le_bytes());
            assert!(Workload::parse(&invalid).is_err(), "field {offset}");
        }
        let mut invalid = bytes;
        invalid
            .get_mut(24..28)
            .expect("x")
            .copy_from_slice(&1.1_f32.to_le_bytes());
        assert!(Workload::parse(&invalid).is_err());
    }

    #[test]
    fn parser_accepts_empty_frames_and_renderer_capped_elapsed_steps() {
        let mut bytes = MAGIC.to_vec();
        for word in [3_u32, 0, 0.05_f32.to_bits(), 1.0_f32.to_bits()] {
            bytes.extend_from_slice(&word.to_le_bytes());
        }
        let workload = Workload::parse(&bytes).expect("valid silent workload");
        assert_eq!(workload.frames.len(), 3);
        assert!(workload.frames.iter().all(Vec::is_empty));
        assert_eq!(workload.step_seconds.to_bits(), 0.05_f32.to_bits());
        assert_eq!(workload.fnv1a64, fnv1a64_hex(&bytes));
        bytes
            .get_mut(16..20)
            .expect("step")
            .copy_from_slice(&0.5_f32.to_le_bytes());
        assert!(
            Workload::parse(&bytes).is_err(),
            "steps beyond the shared production fade cap must be rejected"
        );
    }

    #[test]
    fn unavailable_or_invalid_timestamp_intervals_remain_null() {
        for (start, end) in [
            (0, 0),
            (0, 10),
            (10, 0),
            (10, 10),
            (20, 10),
            (10, u64::MAX),
            (1, 40_000_000_001),
        ] {
            assert!(timestamp_milliseconds(start, end, 1.0).is_none());
        }
        assert_eq!(
            timestamp_milliseconds(10, 1_000_010, 1.0)
                .expect("one millisecond")
                .to_bits(),
            1.0_f64.to_bits()
        );
    }

    #[test]
    fn help_precedes_invalid_options_and_required_paths() {
        assert!(
            parse_options(vec!["--bad".into(), "--help".into()])
                .expect("help")
                .is_none()
        );
        assert!(parse_options(Vec::new()).is_err());
        let base = ["--input", "missing.icrb", "--report", "report.json"];
        assert!(
            parse_options(base.map(OsString::from).into())
                .expect("options")
                .is_some()
        );
        for extra in [
            ["--scale", "NaN"],
            ["--width", "0"],
            ["--frames", "0"],
            ["--mode", "unknown"],
            ["--warmup", "100001"],
        ] {
            assert!(parse_options(base.into_iter().chain(extra).map(OsString::from).collect()).is_err());
        }
    }
}
