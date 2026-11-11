//! Hardware-free benchmark of an explicitly selected build-tree `ImageControl` RS VST3.
//!
//! Hyperfine measures the complete process, including module load, prepare, warmup, checking, and teardown.
//! The JSON `callback_seconds` field separately times only warmed production host process calls.
//! No editor, installed-plugin discovery, application storage, or audio device is used.

use std::hint::black_box;
use std::path::{Path, PathBuf};
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, bail, ensure};
use image_control_plugin_rs::{COMPONENT_ID, PLUGIN_NAME};
use serde::Serialize;
use vst3_host::{ClassId, Module, PluginInstance, ProcessMode, ProcessorHandle, UiThread};

#[path = "support/artifacts.rs"]
mod artifacts;
#[path = "../../../benchmarks/workload.rs"]
mod workload;

use workload::{Scenario, input_frame};

/// Help is recognized before parsing, filesystem checks, or native plugin initialization.
const USAGE: &str = "Usage: processing_benchmark [OPTIONS]
  -h, --help                Show help without loading a plugin
  -b, --bundle PATH         Absolute build-tree ImageControl RS.vst3 (required)
  -C, --case NAME           neutral|width|pan|gains|recover|delta|bypass|mono|solo|mono-input [recover]
  -S, --sample-rate HZ      44100|48000|96000|192000 [48000]
  -k, --block FRAMES        1..4096, or irregular (1,17,257,511) [irregular]
  -f, --frames COUNT        Measured audio frames, 4096..4000000 [96000]

JSON includes finite sample count, bit checksum, energy, reported latency, and callback-only elapsed time.
Hyperfine wall time includes setup, signal copies, warmup, output checks, and teardown, not just callbacks.
Neutral and bypass samples are independently checked against the delayed original input bits.
Active-case checksums are observability/determinism diagnostics, not a DSP correctness oracle.
Bundles must be inside the repository or an explicitly supplied CARGO_TARGET_DIR build root.
";

/// Bounded workload, independent of any installed plugin configuration or application data.
struct Options {
    /// Canonical build-tree bundle selected explicitly inside an authorized repository or Cargo target root.
    bundle: PathBuf,
    /// Saved sound controls held constant across the warmed callbacks.
    scenario: Scenario,
    /// Supported sample rate selecting one of four FIR geometries.
    sample_rate: u32,
    /// Repeating host-block schedule, including arbitrary partition boundaries.
    blocks: Vec<usize>,
    /// Exact measured frame count, excluding warmup.
    frames: usize,
}

/// Machine-readable evidence, written only after native processing and teardown succeed.
#[derive(Serialize)]
struct Report {
    /// Schema version for benchmark artifact consumers.
    schema: u32,
    /// Canonical exact bundle loaded from disk.
    bundle: PathBuf,
    /// Factory class name, validated independently before instantiation.
    plugin: String,
    /// Saved sound case name.
    case: &'static str,
    /// Host processing mode, with no synchronous offline crossover design in callbacks.
    mode: &'static str,
    /// Actual setup sample rate.
    sample_rate: u32,
    /// Matching input and output channel count.
    channels: usize,
    /// Repeating host block schedule.
    blocks: Vec<usize>,
    /// Warmup frames excluded from callback timing and measured output statistics.
    warmup_frames: usize,
    /// Successfully processed measured audio frames.
    frames: usize,
    /// Actual measured process invocations.
    callbacks: usize,
    /// Independently counted finite output channel samples.
    finite_samples: usize,
    /// FNV-1a hash of little-endian output f32 bits, diagnostic rather than a reference result.
    checksum: String,
    /// Sum of squared measured output channel samples, proving non-silent work where appropriate.
    energy: f64,
    /// Peak absolute measured output value.
    peak: f32,
    /// Host-reported FIR latency at the selected rate.
    latency_samples: u32,
    /// Sum of warmed host process call durations, excluding filling, checking, and report creation.
    callback_seconds: f64,
}

/// Fatal watchdog bounds even a native ABI call that never returns.
struct Deadline {
    /// Completion signal sent after all native lifecycle ownership has been retired.
    completed: mpsc::Sender<()>,
    /// Watchdog joined outside processing.
    worker: Option<JoinHandle<()>>,
}

impl Deadline {
    /// Starts a generous bound for the largest permitted workload, not a timing acceptance threshold.
    fn start() -> Result<Self> {
        let (completed, receiver) = mpsc::channel();
        let worker = thread::Builder::new()
            .name("processing-benchmark-deadline".into())
            .spawn(move || {
                if receiver.recv_timeout(Duration::from_secs(120)) == Err(mpsc::RecvTimeoutError::Timeout) {
                    eprintln!("processing benchmark exceeded its 120-second native deadline");
                    std::process::abort();
                }
            })?;
        Ok(Self {
            completed,
            worker: Some(worker),
        })
    }
}

impl Drop for Deadline {
    fn drop(&mut self) {
        let _ = self.completed.send(());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

/// Parses canonical integer options without accepting non-numeric or unbounded workloads.
fn integer(value: &std::ffi::OsStr, option: &str) -> Result<usize> {
    value
        .to_str()
        .with_context(|| format!("{option} requires a UTF-8 integer"))?
        .parse()
        .with_context(|| format!("{option} requires an integer"))
}

/// Requires explicit build-tree input, using the shared canonical repository/target authorization policy.
fn checked_bundle(path: &Path) -> Result<PathBuf> {
    let canonical = artifacts::checked_artifact(path).context("--bundle is not an authorized build artifact")?;
    ensure!(canonical.is_dir(), "--bundle must be a packaged build-tree directory");
    ensure!(
        canonical.extension().is_some_and(|extension| extension == "vst3"),
        "--bundle must end in .vst3"
    );
    Ok(canonical)
}

/// Parses all options before native initialization. Help works even beside malformed options.
fn options() -> Result<Option<Options>> {
    let arguments: Vec<_> = std::env::args_os().skip(1).collect();
    if arguments
        .iter()
        .any(|argument| argument == "-h" || argument == "--help")
    {
        print!("{USAGE}");
        return Ok(None);
    }
    let mut bundle = None;
    let mut scenario = Scenario::Recover;
    let mut sample_rate = 48_000;
    let mut blocks = vec![1, 17, 257, 511];
    let mut frames = 96_000;
    let mut arguments = arguments.into_iter();
    while let Some(option) = arguments.next() {
        let name = option.to_str().context("option must be UTF-8")?;
        ensure!(
            matches!(
                name,
                "-b" | "--bundle" | "-C" | "--case" | "-S" | "--sample-rate" | "-k" | "--block" | "-f" | "--frames"
            ),
            "unknown option {name}"
        );
        let value = arguments.next().with_context(|| format!("{name} requires a value"))?;
        match name {
            "-b" | "--bundle" => bundle = Some(PathBuf::from(value)),
            "-C" | "--case" => {
                scenario = Scenario::ALL
                    .into_iter()
                    .find(|scenario| value == scenario.name())
                    .context("unknown sound case")?;
            }
            "-S" | "--sample-rate" => sample_rate = u32::try_from(integer(&value, name)?)?,
            "-k" | "--block" => {
                blocks = if value == "irregular" {
                    vec![1, 17, 257, 511]
                } else {
                    vec![integer(&value, name)?]
                };
            }
            "-f" | "--frames" => frames = integer(&value, name)?,
            _ => bail!("unknown option {name}"),
        }
    }
    ensure!(
        matches!(sample_rate, 44_100 | 48_000 | 96_000 | 192_000),
        "unsupported benchmark rate"
    );
    ensure!(
        blocks.iter().all(|block| (1..=4096).contains(block)),
        "block must be 1..4096"
    );
    ensure!((4096..=4_000_000).contains(&frames), "frames must be 4096..4000000");
    Ok(Some(Options {
        bundle: checked_bundle(&bundle.context("--bundle is required")?)?,
        scenario,
        sample_rate,
        blocks,
        frames,
    }))
}

/// Publishes the same normalized sound cases as the direct DSP tier, without changing crossover kernels.
fn configure(ui: &UiThread, instance: &PluginInstance, scenario: Scenario) -> Result<()> {
    if scenario == Scenario::Gains {
        let normalized = 20.0_f64.mul_add(0.8_f64.log10(), 24.0) / 36.0;
        instance.set_parameter(ui, 0, normalized)?;
    }
    for band in 0..4 {
        for (field, value) in scenario.band(band).into_iter().enumerate() {
            instance.set_parameter(ui, u32::try_from(4 + band * 6 + field)?, value)?;
        }
    }
    instance.set_parameter(ui, 28, f64::from(scenario == Scenario::Bypass))?;
    instance.set_parameter(ui, 29, f64::from(scenario == Scenario::Delta))?;
    instance.set_parameter(ui, 30, f64::from(scenario == Scenario::Mono))?;
    instance.flush_parameter_edits(ui)?;
    Ok(())
}

/// Drives warmup and measured callbacks from immutable input, excluding copies from the callback-only timer.
fn render(
    processor: &mut ProcessorHandle,
    input: &[[f32; 2]],
    output: &mut [[f32; 2]],
    options: &Options,
) -> Result<(Duration, usize)> {
    let warmup_frames = input.len() - output.len();
    let channels = options.scenario.channels();
    let mut callback_time = Duration::ZERO;
    let mut callbacks = 0;
    let mut position = 0;
    for size in options.blocks.iter().cycle() {
        if position == input.len() {
            break;
        }
        let boundary = if position < warmup_frames {
            warmup_frames
        } else {
            input.len()
        };
        let frames = (*size).min(boundary - position);
        for channel in 0..channels {
            let destination = processor.input_channel_mut(channel).context("missing input channel")?;
            for (destination, source) in destination
                .iter_mut()
                .take(frames)
                .zip(input.get(position..position + frames).context("bounded input")?)
            {
                *destination = *source.get(channel).context("channel in input frame")?;
            }
        }
        if position >= warmup_frames {
            let started = Instant::now();
            processor.process(black_box(frames))?;
            callback_time += started.elapsed();
            callbacks += 1;
            for channel in 0..channels {
                let source = processor.output_channel(channel).context("missing output channel")?;
                for (source, destination) in source.iter().take(frames).zip(
                    output
                        .get_mut(position - warmup_frames..position - warmup_frames + frames)
                        .context("bounded output")?,
                ) {
                    *destination.get_mut(channel).context("channel in output frame")? = *source;
                }
            }
        } else {
            processor.process(frames)?;
        }
        position += frames;
    }
    Ok((callback_time, callbacks))
}

/// Loads the exact production bundle and executes a finite deterministic stream with preallocated host buffers.
fn run(options: Options) -> Result<Report> {
    let _deadline = Deadline::start()?;
    let ui = UiThread::register_current()?;
    let module = Module::load(&ui, &options.bundle)?;
    let class_id = ClassId::from_tuid(&COMPONENT_ID);
    let class = module
        .class(class_id)
        .context("bundle lacks the primary ImageControl RS identity")?;
    ensure!(
        class.is_audio_effect() && class.name == PLUGIN_NAME,
        "not the primary ImageControl RS VST3"
    );
    let channels = options.scenario.channels();
    let maximum_block = *options.blocks.iter().max().context("nonempty block schedule")?;
    let mut instance = PluginInstance::create(&ui, &module, class_id, None)?;
    let layout = instance.configure_main_buses(&ui, channels)?;
    ensure!(
        layout.inputs == [channels] && layout.outputs == [channels],
        "unexpected main bus layout"
    );
    instance.setup(
        &ui,
        ProcessMode::Realtime,
        f64::from(options.sample_rate),
        maximum_block,
    )?;
    instance.activate(&ui)?;
    configure(&ui, &instance, options.scenario)?;
    let latency_samples = instance.latency_samples(&ui);
    let warmup_frames =
        latency_samples as usize * 2 + instance.tail_samples(&ui) as usize + options.sample_rate as usize / 10;
    let input: Vec<_> = (0..warmup_frames + options.frames)
        .map(|index| input_frame(index, f64::from(options.sample_rate), channels))
        .collect();
    let mut output = vec![[0.0_f32; 2]; options.frames];
    let mut processor = instance.processor(&ui)?;
    processor.reset(0);
    let (callback_time, callbacks) = render(&mut processor, &input, &mut output, &options)?;
    drop(processor);
    instance.deactivate(&ui)?;
    drop(instance);
    let mut finite_samples = 0;
    let mut checksum = 0xcbf2_9ce4_8422_2325_u64;
    let mut energy = 0.0;
    let mut peak = 0.0_f32;
    for (frame, samples) in output.iter().enumerate() {
        for (channel, &sample) in samples.iter().take(channels).enumerate() {
            ensure!(
                sample.is_finite(),
                "non-finite output at frame {frame} channel {channel}"
            );
            finite_samples += 1;
            energy += f64::from(sample).powi(2);
            peak = peak.max(sample.abs());
            for byte in sample.to_bits().to_le_bytes() {
                checksum = (checksum ^ u64::from(byte)).wrapping_mul(0x0100_0000_01b3);
            }
            if matches!(options.scenario, Scenario::Neutral | Scenario::Bypass) {
                let expected = input
                    .get(warmup_frames + frame - latency_samples as usize)
                    .and_then(|frame| frame.get(channel))
                    .context("delayed source sample")?;
                ensure!(
                    sample.to_bits() == expected.to_bits(),
                    "neutral/bypass delayed bits differ"
                );
            }
        }
    }
    ensure!(
        finite_samples == options.frames * channels,
        "output sample count differs"
    );
    ensure!(
        energy.is_finite() && energy > 0.0,
        "benchmark produced no finite signal energy"
    );
    Ok(Report {
        schema: 1,
        bundle: options.bundle,
        plugin: class.name.clone(),
        case: options.scenario.name(),
        mode: "realtime-warm-callbacks",
        sample_rate: options.sample_rate,
        channels,
        blocks: options.blocks,
        warmup_frames,
        frames: options.frames,
        callbacks,
        finite_samples,
        checksum: format!("{checksum:016x}"),
        energy,
        peak,
        latency_samples,
        callback_seconds: callback_time.as_secs_f64(),
    })
}

/// Reports success only after checked native work, without initializing application storage or an audio device.
fn main() -> Result<()> {
    if let Some(options) = options()? {
        println!("{}", serde_json::to_string_pretty(&run(options)?)?);
    }
    Ok(())
}
