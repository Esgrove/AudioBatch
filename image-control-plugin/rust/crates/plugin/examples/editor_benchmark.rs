//! Sequential single-editor benchmark of one explicit build-tree VST3, without audio devices or pixel capture.
//!
//! Native command-encoding endpoints are preferred, with refresh-counter fallback for earlier/native reports.
//! Neither timestamp source establishes GPU completion or physical scanout.

use std::ffi::OsString;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context as _, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use serde_json::json;
use vst3_host::{EditorWindow, Module, PluginInstance, PluginState, ProcessMode, UiThread};

#[path = "support/editor_benchmark_platform.rs"]
mod platform;

/// This benchmark deliberately uses the same stereo sample rate and block size for either implementation.
const SAMPLE_RATE: usize = 48_000;
/// A callback's absolute deadline is derived from its ordinal, never from the previous callback's completion.
const BLOCK_FRAMES: usize = 512;
/// Bounds host memory, plugin telemetry, and unattended visible-window lifetime.
const MAXIMUM_SECONDS: f64 = 120.0;

/// Only supported appearance states are applied through ordinary component state before activation.
#[derive(Clone, Copy, Debug, Serialize)]
#[serde(rename_all = "kebab-case")]
enum Case {
    Default,
    Solid,
    PersistenceContrast,
    Maximize,
}

/// One invocation owns exactly one module, component, processor, and visible native editor.
struct Options {
    bundle: PathBuf,
    seconds: f64,
    warmup: f64,
    report: PathBuf,
    width: u32,
    height: u32,
    case: Case,
}

/// Precomputed planar PCM is reused without signal generation or hashing in timed processing.
struct Signal {
    channels: [Vec<f32>; 2],
    hash: String,
}

/// Callback timestamps include scheduling lateness separately from plugin processing time.
#[derive(Serialize)]
struct CallbackSample {
    #[serde(rename = "elapsed_seconds")]
    elapsed: f64,
    #[serde(rename = "process_seconds")]
    process: f64,
    #[serde(rename = "deadline_lateness_seconds")]
    deadline_lateness: f64,
}

/// Host-owned processing observations retain their wall origin independently of worker startup scheduling.
struct CallbackRun {
    started_unix_seconds: f64,
    samples: Vec<CallbackSample>,
}

/// Plugin reports have a shared narrow schema while retaining implementation-specific metadata in raw JSON.
#[derive(Deserialize)]
struct RenderReport {
    started_unix_seconds: f64,
    dropped_samples: u64,
    samples: Vec<RefreshSample>,
    #[serde(default)]
    native_frame_samples: Option<Vec<NativeFrameSample>>,
    #[serde(default)]
    dropped_native_frame_samples: u64,
}

/// Successful encoding endpoints do not depend on ordinary application-refresh messages.
#[derive(Deserialize)]
struct NativeFrameSample {
    elapsed_seconds: f64,
    successful_frames: u64,
    encoding_seconds: f64,
}

/// Timestamps identify counter observations rather than fabricated command-encoding instants.
#[derive(Deserialize)]
struct RefreshSample {
    elapsed_seconds: f64,
    successful_frames: u64,
    update_seconds: f64,
}

/// Runs all environment mutation before loading plugin code or starting any host/native worker threads.
fn main() -> Result<()> {
    let Some(options) = parse_options(std::env::args_os().skip(1))? else {
        return Ok(());
    };
    image_control_plugin_rs::benchmark_build::require_optimized_native().map_err(anyhow::Error::msg)?;
    let telemetry_directory = tempfile::tempdir().context("temporary editor telemetry")?;
    let telemetry_path = telemetry_directory.path().join("render.json");
    set_report_environment(&telemetry_path, options.case);
    let signal = precompute_signal();
    let signal_hash = signal.hash.clone();
    let ui = UiThread::register_current()?;
    platform::start_application()?;
    let module = Module::load(&ui, &options.bundle)?;
    let class = module
        .classes()
        .iter()
        .find(|class| class.is_audio_effect())
        .context("audio effect class")?;
    let mut instance = PluginInstance::create(&ui, &module, class.id, None)?;
    let state = appearance_state(instance.state(&ui)?, &options)?;
    ensure!(
        instance.set_state(&ui, &state)?,
        "component refused benchmark appearance state"
    );
    instance.configure_main_buses(&ui, 2)?;
    instance.setup(&ui, ProcessMode::Realtime, SAMPLE_RATE as f64, BLOCK_FRAMES)?;
    instance.activate(&ui)?;
    let mut processor = instance.processor(&ui)?;
    let stopped = Arc::new(AtomicBool::new(false));
    let worker_stopped = Arc::clone(&stopped);
    let callback_capacity =
        ((options.seconds + options.warmup + 10.0) * SAMPLE_RATE as f64 / BLOCK_FRAMES as f64).ceil() as usize;
    let mut callback_samples = Vec::with_capacity(callback_capacity);
    let worker = std::thread::spawn(move || -> Result<_> {
        let started_unix_seconds = unix_seconds()?;
        let origin = Instant::now();
        let mut block = 0_usize;
        while !worker_stopped.load(Ordering::Acquire) && callback_samples.len() < callback_capacity {
            let deadline = origin + Duration::from_secs_f64((block * BLOCK_FRAMES) as f64 / SAMPLE_RATE as f64);
            while let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
                if worker_stopped.load(Ordering::Acquire) {
                    break;
                }
                std::thread::park_timeout(remaining.min(Duration::from_millis(10)));
            }
            if worker_stopped.load(Ordering::Acquire) {
                break;
            }
            for (channel, source) in signal.channels.iter().enumerate() {
                let input = processor.input_channel_mut(channel).context("stereo input plane")?;
                for (offset, sample) in input.iter_mut().enumerate() {
                    *sample = *source
                        .get((block * BLOCK_FRAMES + offset) % SAMPLE_RATE)
                        .context("precomputed PCM")?;
                }
            }
            let started = Instant::now();
            processor.process(BLOCK_FRAMES)?;
            callback_samples.push(CallbackSample {
                elapsed: started.saturating_duration_since(origin).as_secs_f64(),
                process: started.elapsed().as_secs_f64(),
                deadline_lateness: started.saturating_duration_since(deadline).as_secs_f64(),
            });
            block += 1;
        }
        drop(processor);
        Ok(CallbackRun {
            started_unix_seconds,
            samples: callback_samples,
        })
    });
    // An attachment error must still retire the live processor before deactivation or instance destruction.
    let editor_result = observe_editor(&ui, &instance, &options);
    stopped.store(true, Ordering::Release);
    let callback_result = worker
        .join()
        .map_err(|_| anyhow::anyhow!("benchmark callback worker panicked"))?;
    instance.deactivate(&ui)?;
    let observation = editor_result?;
    write_report(&options, &telemetry_path, &signal_hash, callback_result?, &observation)
}

/// Assembles reports only after processing and native editor teardown, outside every timed path.
fn write_report(
    options: &Options,
    telemetry_path: &std::path::Path,
    signal_hash: &str,
    callback_run: CallbackRun,
    observation: &EditorObservation,
) -> Result<()> {
    let raw_telemetry: serde_json::Value = serde_json::from_slice(
        &std::fs::read(telemetry_path).context("editor did not flush render telemetry on close, rebuild the bundle")?,
    )?;
    if matches!(options.case, Case::PersistenceContrast) {
        validate_live_sweep(&raw_telemetry)?;
    }
    ensure!(
        raw_telemetry.get("build_type").and_then(serde_json::Value::as_str) == Some("Release"),
        "benchmarked plugin must be rebuilt with Release profile"
    );
    ensure!(
        raw_telemetry
            .get("native_cpu_enabled")
            .and_then(serde_json::Value::as_bool)
            == Some(true),
        "benchmarked plugin must be rebuilt with native CPU optimizations"
    );
    let telemetry: RenderReport = serde_json::from_value(raw_telemetry.clone())?;
    let mut summary = summarize_render(&telemetry, observation.started_unix_seconds, options.seconds)?;
    let expected_refresh = observation
        .native_samples
        .first()
        .and_then(|sample| sample.get("expected_display_refresh_hz"))
        .and_then(serde_json::Value::as_f64);
    if let Some(summary) = summary.as_object_mut() {
        summary.insert("expected_display_refresh_hz".into(), json!(expected_refresh));
        summary.insert(
            "expected_frames_at_display_maximum".into(),
            json!(expected_refresh.map(|rate| rate * options.seconds)),
        );
        let achieved = summary
            .get("successful_frames_per_second")
            .and_then(serde_json::Value::as_f64);
        summary.insert(
            "achieved_fraction_of_display_maximum".into(),
            json!(
                expected_refresh
                    .zip(achieved)
                    .map(|(expected, actual)| actual / expected)
            ),
        );
    }
    let callbacks: Vec<_> = callback_run
        .samples
        .into_iter()
        .filter(|sample| {
            let time = callback_run.started_unix_seconds + sample.elapsed;
            time >= observation.started_unix_seconds && time < observation.started_unix_seconds + options.seconds
        })
        .collect();
    let report = json!({
        "schema_version": 1,
        "tier": "visible-single-production-editor",
        "host_build": {
            "profile": image_control_plugin_rs::benchmark_build::PROFILE,
            "opt_level": image_control_plugin_rs::benchmark_build::OPT_LEVEL,
            "native_cpu_enabled": image_control_plugin_rs::benchmark_build::NATIVE_CPU == "true"
        },
        "bundle": options.bundle,
        "case": options.case,
        "seconds": options.seconds,
        "warmup_seconds": options.warmup,
        "requested_logical_size": {"width": options.width, "height": options.height},
        "machine": {"os": std::env::consts::OS, "architecture": std::env::consts::ARCH,
                    "available_parallelism": std::thread::available_parallelism().ok().map(std::num::NonZero::get)},
        "signal": {"hash_algorithm": "fnv1a64-f32le-channel-major", "hash": signal_hash,
                   "sample_rate": SAMPLE_RATE, "loop_frames": SAMPLE_RATE, "channels": 2, "block_frames": BLOCK_FRAMES},
        "observation": observation,
        "render_summary": summary,
        "callbacks": callbacks,
        "visibility_qualification": visibility_qualification(&observation.native_samples),
        "raw_render_telemetry": raw_telemetry,
        "limits": ["successful command issue/encoding is not GPU completion or physical scanout",
                   "frame_timestamp_source distinguishes native encoding endpoints from refresh-counter fallback",
                   "coalesced counter increments have unresolved individual timing",
                   "native visibility does not prove unobstructed pixels or display scanout",
                   "display maximum refresh is a capability, not measured variable-refresh cadence"],
        "unsupported_cases": {"hover": "not run: no portable host-scoped pointer driver in this example"}
    });
    std::fs::write(&options.report, serde_json::to_vec_pretty(&report)?)?;
    println!("visible-editor report: {}", options.report.display());
    Ok(())
}

/// Rejects initial-state-only or reset-based runs when the caller requested continuous appearance adjustment.
fn validate_live_sweep(report: &serde_json::Value) -> Result<()> {
    let sweep = report
        .get("appearance_sweep")
        .context("plugin does not expose live sweep telemetry, rebuild it")?;
    ensure!(
        sweep.get("enabled").and_then(serde_json::Value::as_bool) == Some(true),
        "live appearance sweep was not enabled"
    );
    ensure!(
        sweep
            .get("accepted_updates")
            .and_then(serde_json::Value::as_u64)
            .is_some_and(|count| count > 2),
        "insufficient live preference updates"
    );
    ensure!(
        sweep.get("rejected_updates").and_then(serde_json::Value::as_u64) == Some(0),
        "live preference setter rejected sweep values"
    );
    ensure!(
        sweep.get("generation_changes").and_then(serde_json::Value::as_u64) == Some(0),
        "appearance sweep changed analysis generation"
    );
    ensure!(
        sweep
            .get("projection_epoch_changes")
            .and_then(serde_json::Value::as_u64)
            .is_none_or(|count| count == 0),
        "appearance sweep changed projection epoch"
    );
    let minimum_persistence = sweep
        .get("minimum_persistence_seconds")
        .and_then(serde_json::Value::as_f64)
        .context("minimum sweep persistence")?;
    let maximum_persistence = sweep
        .get("maximum_persistence_seconds")
        .and_then(serde_json::Value::as_f64)
        .context("maximum sweep persistence")?;
    let minimum_contrast = sweep
        .get("minimum_contrast")
        .and_then(serde_json::Value::as_f64)
        .context("minimum sweep contrast")?;
    let maximum_contrast = sweep
        .get("maximum_contrast")
        .and_then(serde_json::Value::as_f64)
        .context("maximum sweep contrast")?;
    ensure!(
        maximum_persistence > minimum_persistence && maximum_contrast > minimum_contrast,
        "appearance sweep did not change both preferences"
    );
    Ok(())
}

/// Separates observed visibility from runs that are occluded or cannot expose a native observation.
fn visibility_qualification(samples: &[serde_json::Value]) -> &'static str {
    if samples.iter().any(|sample| {
        sample.get("window_visible").and_then(serde_json::Value::as_bool) == Some(false)
            || sample
                .get("occlusion_state_visible")
                .and_then(serde_json::Value::as_bool)
                == Some(false)
    }) {
        "not-qualified-hidden-or-occluded-observation"
    } else if samples.is_empty()
        || samples.iter().any(|sample| {
            sample
                .get("window_visible")
                .and_then(serde_json::Value::as_bool)
                .is_none()
                || sample
                    .get("occlusion_state_visible")
                    .and_then(serde_json::Value::as_bool)
                    .is_none()
        })
    {
        "unavailable-native-visibility"
    } else {
        "native-visible-observations-not-proof-of-unobstructed-pixels"
    }
}

/// Environment mutation is confined to the dedicated executable's single-threaded startup.
#[expect(
    unsafe_code,
    reason = "dedicated example startup precedes all native/plugin/worker threads"
)]
fn set_report_environment(path: &std::path::Path, case: Case) {
    // SAFETY: main calls this before UiThread registration, native startup, module loading, or spawning threads.
    unsafe {
        std::env::set_var("AUDIO_IMAGECONTROL_RENDER_REPORT", path);
        if matches!(case, Case::PersistenceContrast) {
            std::env::set_var("AUDIO_IMAGECONTROL_RENDER_SWEEP", "1");
        } else {
            std::env::remove_var("AUDIO_IMAGECONTROL_RENDER_SWEEP");
        }
    }
}

/// The host owns this observation interval independently of plugin boot and close-time serialization.
#[derive(Serialize)]
struct EditorObservation {
    started_unix_seconds: f64,
    ended_unix_seconds: f64,
    native_samples: Vec<serde_json::Value>,
}

/// Keeps the native window shown, pumps real host events, and destroys the editor on its opening thread.
fn observe_editor(ui: &UiThread, instance: &PluginInstance, options: &Options) -> Result<EditorObservation> {
    let editor = EditorWindow::open(ui, instance, "ImageControl visible benchmark", None, None)?;
    let warmup_end = Instant::now() + Duration::from_secs_f64(options.warmup);
    while Instant::now() < warmup_end {
        ensure!(!editor.is_closed(), "editor closed during warmup");
        platform::handle_events(ui);
    }
    editor.show(ui);
    let visibility_settle = Instant::now() + Duration::from_millis(100);
    while Instant::now() < visibility_settle {
        platform::handle_events(ui);
    }
    let started_unix_seconds = unix_seconds()?;
    let deadline = Instant::now() + Duration::from_secs_f64(options.seconds);
    let mut native_samples = Vec::with_capacity(options.seconds.ceil() as usize + 2);
    let mut next_observation = Instant::now();
    while Instant::now() < deadline {
        ensure!(
            !editor.is_closed(),
            "editor closed before the bounded measurement completed"
        );
        if Instant::now() >= next_observation {
            native_samples.push(platform::native_observation(ui, &editor)?);
            next_observation += Duration::from_secs(1);
        }
        platform::handle_events(ui);
    }
    let ended_unix_seconds = unix_seconds()?;
    drop(editor);
    // Native close may schedule owner-thread resource retirement. Bounded pumping completes that work.
    let close_deadline = Instant::now() + Duration::from_millis(100);
    while Instant::now() < close_deadline {
        platform::handle_events(ui);
    }
    Ok(EditorObservation {
        started_unix_seconds,
        ended_unix_seconds,
        native_samples,
    })
}

/// Builds one second of a repeatable stereo comb and hashes exact precomputed sample bits before timing.
fn precompute_signal() -> Signal {
    let channels = std::array::from_fn(|channel| {
        (0..SAMPLE_RATE)
            .map(|frame| {
                let time = frame as f64 / SAMPLE_RATE as f64;
                [40.0, 80.0, 150.0, 300.0, 600.0, 1200.0, 2400.0, 4800.0, 9600.0]
                    .into_iter()
                    .enumerate()
                    .fold(0.0, |value, (tone, frequency)| {
                        let right_gain = [0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 1.0, 0.5]
                            .get(tone)
                            .copied()
                            .expect("tone");
                        let gain = if channel == 0 { 1.0 } else { right_gain };
                        ((std::f64::consts::TAU * frequency * time).sin() * gain).mul_add(0.015, value)
                    }) as f32
            })
            .collect::<Vec<_>>()
    });
    let hash = channels
        .iter()
        .flatten()
        .flat_map(|sample| sample.to_le_bytes())
        .fold(0xcbf2_9ce4_8422_2325_u64, |hash, byte| {
            (hash ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01b3)
        });
    Signal {
        channels,
        hash: format!("{hash:016x}"),
    }
}

/// Changes only editor properties in captured native state, leaving sound and all global host settings untouched.
fn appearance_state(mut state: PluginState, options: &Options) -> Result<PluginState> {
    let prefix: [u8; 4] = state.component.get(..4).context("component state prefix")?.try_into()?;
    let payload_length = u32::from_le_bytes(prefix) as usize;
    if let Some(payload) = state
        .component
        .get(4..)
        .filter(|payload| payload.len() == payload_length)
        && let Ok(mut document) = serde_json::from_slice::<serde_json::Value>(payload)
    {
        ensure!(
            document.get("version") == Some(&json!(1)),
            "unsupported Rust state generation"
        );
        let object = document.as_object_mut().context("Rust state object")?;
        object.insert(
            "geometry".into(),
            json!({"width": options.width, "height": options.height}),
        );
        object
            .entry("layout")
            .or_insert_with(|| json!({}))
            .as_object_mut()
            .context("Rust layout state")?
            .insert(
                "maximize_spectrogram".into(),
                json!(matches!(options.case, Case::Maximize)),
            );
        let display = object
            .get_mut("display")
            .and_then(serde_json::Value::as_object_mut)
            .context("Rust display state")?;
        display.insert("solid_rendering".into(), json!(matches!(options.case, Case::Solid)));
        if matches!(options.case, Case::PersistenceContrast) {
            display.insert("persistence_seconds".into(), json!(1.5));
            display.insert("contrast".into(), json!(0.9));
        }
        let payload = serde_json::to_vec(&document)?;
        state.component = u32::try_from(payload.len())?.to_le_bytes().to_vec();
        state.component.extend(payload);
        return Ok(state);
    }
    // JUCE's binary XML header is a magic u32 and a little-endian UTF-8 byte count.
    ensure!(
        prefix == 0x2132_4356_u32.to_le_bytes(),
        "unsupported component state encoding"
    );
    let length = u32::from_le_bytes(state.component.get(4..8).context("JUCE XML length")?.try_into()?) as usize;
    let xml_end = 8_usize.checked_add(length).context("JUCE XML overflow")?;
    let mut xml = std::str::from_utf8(state.component.get(8..xml_end).context("JUCE XML")?)?.to_owned();
    let start = xml.find("<EditorState").context("JUCE EditorState")?;
    let end = xml
        .get(start..)
        .and_then(|tail| tail.find("/>"))
        .context("JUCE leaf editor state")?
        + start
        + 2;
    let properties = format!(
        "<EditorState editorWidth=\"{}\" editorHeight=\"{}\" solidRendering=\"{}\" persistence=\"{}\" contrast=\"{}\" maximizeSpectrogram=\"{}\"/>",
        options.width,
        options.height,
        u8::from(matches!(options.case, Case::Solid)),
        if matches!(options.case, Case::PersistenceContrast) {
            1.5
        } else {
            0.3
        },
        if matches!(options.case, Case::PersistenceContrast) {
            90
        } else {
            60
        },
        u8::from(matches!(options.case, Case::Maximize)),
    );
    xml.replace_range(start..end, &properties);
    let trailer = state.component.get(xml_end..).context("JUCE wrapper trailer")?.to_vec();
    state.component = prefix.to_vec();
    state.component.extend(u32::try_from(xml.len())?.to_le_bytes());
    state.component.extend(xml.as_bytes());
    state.component.extend(trailer);
    Ok(state)
}

/// Summarizes the measured interval without interpreting GUI polls as successfully encoded frames.
fn summarize_render(report: &RenderReport, start: f64, seconds: f64) -> Result<serde_json::Value> {
    ensure!(report.dropped_samples == 0, "bounded production telemetry saturated");
    let relative_start = start - report.started_unix_seconds;
    if let Some(native_samples) = &report.native_frame_samples {
        return summarize_native_frames(report, native_samples, relative_start, seconds);
    }
    let samples: Vec<_> = report
        .samples
        .iter()
        .filter(|sample| sample.elapsed_seconds >= relative_start && sample.elapsed_seconds < relative_start + seconds)
        .collect();
    ensure!(
        samples.len() >= 2,
        "insufficient production editor refresh observations"
    );
    let first = samples.first().context("first refresh")?;
    let last = samples.last().context("last refresh")?;
    let frames = last.successful_frames.saturating_sub(first.successful_frames);
    let observed_seconds = last.elapsed_seconds - first.elapsed_seconds;
    let mut previous = first.successful_frames;
    let mut frame_observations = Vec::new();
    let mut coalesced_frames = 0_u64;
    for sample in &samples {
        let increment = sample.successful_frames.saturating_sub(previous);
        ensure!(increment < 4096, "implausible successful frame counter delta");
        if increment > 1 {
            coalesced_frames += increment;
        }
        for _ in 0..increment {
            frame_observations.push(json!({"observed_seconds": sample.elapsed_seconds - relative_start,
                                          "coalesced": increment > 1}));
        }
        previous = sample.successful_frames;
    }
    let refresh_gaps: Vec<_> = samples
        .windows(2)
        .filter_map(|pair| Some(pair.get(1)?.elapsed_seconds - pair.first()?.elapsed_seconds))
        .collect();
    let encoding_observation_gaps: Vec<_> = frame_observations
        .windows(2)
        .filter_map(|pair| {
            Some(pair.get(1)?.get("observed_seconds")?.as_f64()? - pair.first()?.get("observed_seconds")?.as_f64()?)
        })
        .collect();
    Ok(json!({
        "frame_timestamp_source": "first_refresh_observing_successful_counter_not_encoding_time",
        "successful_frames": frames,
        "observed_seconds": observed_seconds,
        "successful_frames_per_second": frames as f64 / observed_seconds,
        "refresh_observations": samples.len(),
        "refresh_gap_seconds": distribution(refresh_gaps),
        "successful_frame_observation_gap_seconds": distribution(encoding_observation_gaps),
        "update_seconds": distribution(samples.iter().map(|sample| sample.update_seconds).collect()),
        "coalesced_frames_without_individual_timestamps": coalesced_frames,
        "per_frame_observations": frame_observations,
        "gpu_completion_seconds": null,
        "physical_scanout_seconds": null
    }))
}

/// Measures native successful encoding intervals even when the application never emits an ordinary refresh.
fn summarize_native_frames(
    report: &RenderReport,
    native_samples: &[NativeFrameSample],
    relative_start: f64,
    seconds: f64,
) -> Result<serde_json::Value> {
    ensure!(
        report.dropped_native_frame_samples == 0,
        "bounded native frame telemetry saturated"
    );
    let frames: Vec<_> = native_samples
        .iter()
        .filter(|sample| sample.elapsed_seconds >= relative_start && sample.elapsed_seconds < relative_start + seconds)
        .collect();
    ensure!(
        frames.len() >= 2,
        "insufficient successful native command-encoding timestamps"
    );
    let first = frames.first().context("first encoded frame")?;
    let last = frames.last().context("last encoded frame")?;
    let interval_seconds = last.elapsed_seconds - first.elapsed_seconds;
    ensure!(interval_seconds > 0.0, "native frame timestamps did not advance");
    let gaps: Vec<_> = frames
        .windows(2)
        .filter_map(|pair| Some(pair.get(1)?.elapsed_seconds - pair.first()?.elapsed_seconds))
        .collect();
    ensure!(
        gaps.iter().all(|gap| *gap > 0.0),
        "native encoding timestamps are not strictly ordered"
    );
    let refreshes: Vec<_> = report
        .samples
        .iter()
        .filter(|sample| sample.elapsed_seconds >= relative_start && sample.elapsed_seconds < relative_start + seconds)
        .collect();
    let refresh_gaps: Vec<_> = refreshes
        .windows(2)
        .filter_map(|pair| Some(pair.get(1)?.elapsed_seconds - pair.first()?.elapsed_seconds))
        .collect();
    let native_cadence_hz = (frames.len() - 1) as f64 / interval_seconds;
    Ok(json!({
        "frame_timestamp_source": "successful_cpu_command_encoding_endpoint_not_gpu_completion_or_scanout",
        "successful_frames": frames.len(),
        "observed_seconds": seconds,
        "successful_frames_per_second": frames.len() as f64 / seconds,
        "mean_native_cadence_hz": native_cadence_hz,
        "mean_native_frame_interval_seconds": interval_seconds / (frames.len() - 1) as f64,
        "native_interval_seconds": interval_seconds,
        "refresh_observations": refreshes.len(),
        "refresh_gap_seconds": distribution(refresh_gaps),
        "successful_frame_observation_gap_seconds": distribution(gaps),
        "encoding_seconds": distribution(frames.iter().map(|sample| sample.encoding_seconds).collect()),
        "update_seconds": distribution(refreshes.iter().map(|sample| sample.update_seconds).collect()),
        "coalesced_frames_without_individual_timestamps": 0,
        "per_frame_observations": frames.iter().map(|sample| json!({
            "observed_seconds": sample.elapsed_seconds - relative_start,
            "successful_frames": sample.successful_frames,
            "encoding_seconds": sample.encoding_seconds,
            "coalesced": false,
        })).collect::<Vec<_>>(),
        "gpu_completion_seconds": null,
        "physical_scanout_seconds": null,
    }))
}

/// Computes transparent nearest-rank statistics off every timed callback and render path.
fn distribution(mut values: Vec<f64>) -> serde_json::Value {
    values.sort_by(f64::total_cmp);
    let percentile = |fraction: f64| {
        values
            .get(((values.len() as f64 * fraction).ceil() as usize).saturating_sub(1))
            .copied()
    };
    json!({"p50": percentile(0.5), "p95": percentile(0.95), "p99": percentile(0.99), "max": values.last()})
}

/// Captures wall alignment only off the callback, while monotonic clocks remain authoritative for gaps.
fn unix_seconds() -> Result<f64> {
    Ok(SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs_f64())
}

/// Parses help before filesystem checks, environment mutation, native initialization, or plugin loading.
fn parse_options(arguments: impl Iterator<Item = OsString>) -> Result<Option<Options>> {
    let arguments: Vec<_> = arguments.collect();
    if arguments
        .iter()
        .any(|argument| argument == "--help" || argument == "-h")
    {
        println!(
            "Visible single-editor benchmark (one explicit build-tree VST3 per process)\n\
                  -b, --bundle PATH   Required build-tree .vst3 bundle, never installed plugins\n\
                  -s, --seconds N     Measurement duration 1..120 (default 10)\n\
                  -w, --warmup N      Warmup duration 0..120 (default 3)\n\
                  -r, --report PATH   Required new JSON report path\n\
                  -W, --width N       Logical editor width 760..4096 (default 1000)\n\
                  -H, --height N      Logical editor height 600..4096 (default 764)\n\
                  -c, --case NAME     default, solid, persistence-contrast (live sweep), maximize\n\
                  -h, --help          Print help without starting native resources"
        );
        return Ok(None);
    }
    let mut options = Options {
        bundle: PathBuf::new(),
        seconds: 10.0,
        warmup: 3.0,
        report: PathBuf::new(),
        width: 1000,
        height: 764,
        case: Case::Default,
    };
    let mut arguments = arguments.into_iter();
    while let Some(argument) = arguments.next() {
        let value = arguments.next().context("option requires a value")?;
        match argument.to_str().context("UTF-8 option name")? {
            "--bundle" | "-b" => options.bundle = PathBuf::from(value),
            "--seconds" | "-s" => options.seconds = value.to_string_lossy().parse()?,
            "--warmup" | "-w" => options.warmup = value.to_string_lossy().parse()?,
            "--report" | "-r" => options.report = PathBuf::from(value),
            "--width" | "-W" => options.width = value.to_string_lossy().parse()?,
            "--height" | "-H" => options.height = value.to_string_lossy().parse()?,
            "--case" | "-c" => {
                options.case = match value.to_str() {
                    Some("default") => Case::Default,
                    Some("solid") => Case::Solid,
                    Some("persistence-contrast") => Case::PersistenceContrast,
                    Some("maximize") => Case::Maximize,
                    Some("hover") => bail!("hover is unsupported: no portable host-scoped pointer driver"),
                    _ => bail!("unknown appearance case"),
                }
            }
            _ => bail!("unknown option {}", argument.to_string_lossy()),
        }
    }
    ensure!(
        options.seconds.is_finite() && (1.0..=MAXIMUM_SECONDS).contains(&options.seconds),
        "seconds must be 1..120"
    );
    ensure!(
        options.warmup.is_finite() && (0.0..=MAXIMUM_SECONDS).contains(&options.warmup),
        "warmup must be 0..120"
    );
    ensure!(
        (760..=4096).contains(&options.width) && (600..=4096).contains(&options.height),
        "invalid logical dimensions"
    );
    ensure!(
        !options.bundle.as_os_str().is_empty() && options.bundle.extension().is_some_and(|value| value == "vst3"),
        "explicit VST3 bundle required"
    );
    options.bundle = options.bundle.canonicalize().context("explicit build-tree bundle")?;
    ensure!(
        !options.bundle.to_string_lossy().contains("/Library/Audio/Plug-Ins/")
            && !options
                .bundle
                .to_string_lossy()
                .to_ascii_lowercase()
                .contains("common files\\vst3"),
        "installed plugins are not benchmark inputs"
    );
    ensure!(!options.report.as_os_str().is_empty(), "--report required");
    options.report = std::path::absolute(&options.report)?;
    ensure!(!options.report.exists(), "report already exists, choose a new path");
    Ok(Some(options))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Help never requires a plugin or native application, and unsupported pointer workloads fail explicitly.
    #[test]
    fn help_and_unsupported_case_are_side_effect_free() {
        assert!(
            parse_options([OsString::from("--help")].into_iter())
                .expect("help")
                .is_none()
        );
        assert!(parse_options([OsString::from("--case"), OsString::from("hover")].into_iter()).is_err());
    }

    /// Component overrides preserve sound and wrapper bytes and exercise each independent persistence envelope.
    #[test]
    fn appearance_overrides_preserve_sound_and_native_envelopes() {
        let options = Options {
            bundle: PathBuf::new(),
            seconds: 1.0,
            warmup: 0.0,
            report: PathBuf::new(),
            width: 1280,
            height: 900,
            case: Case::PersistenceContrast,
        };
        let document = json!({"version": 1, "parameters": {"gain": 0.123}, "geometry": {},
            "display": {"solid_rendering": false, "contrast": 0.6, "persistence_seconds": 0.3}});
        let payload = serde_json::to_vec(&document).expect("JSON");
        let mut component = u32::try_from(payload.len()).expect("length").to_le_bytes().to_vec();
        component.extend(payload);
        let result = appearance_state(
            PluginState {
                component,
                controller: vec![42],
            },
            &options,
        )
        .expect("Rust state");
        let restored: serde_json::Value =
            serde_json::from_slice(result.component.get(4..).expect("JSON payload")).expect("JSON");
        assert_eq!(restored.get("parameters"), document.get("parameters"));
        assert_eq!(restored.pointer("/geometry/width"), Some(&json!(1280)));
        assert_eq!(restored.pointer("/display/contrast"), Some(&json!(0.9)));
        assert_eq!(result.controller, vec![42]);
        let xml =
            "<ImageControl><PARAM id=\"gain\" value=\"0.123\"/><EditorState editorWidth=\"1000\"/></ImageControl>";
        let mut component = 0x2132_4356_u32.to_le_bytes().to_vec();
        component.extend(u32::try_from(xml.len()).expect("length").to_le_bytes());
        component.extend(xml.as_bytes());
        component.extend([0, 77, 88]);
        let result = appearance_state(
            PluginState {
                component,
                controller: vec![],
            },
            &options,
        )
        .expect("JUCE state");
        let length = u32::from_le_bytes(result.component.get(4..8).expect("length").try_into().expect("u32")) as usize;
        let restored = std::str::from_utf8(result.component.get(8..8 + length).expect("XML")).expect("UTF-8");
        assert!(restored.contains("<PARAM id=\"gain\" value=\"0.123\"/>"));
        assert!(restored.contains("editorWidth=\"1280\""));
        assert_eq!(result.component.get(8 + length..), Some([0, 77, 88].as_slice()));
        let maximized = Options {
            case: Case::Maximize,
            ..options
        };
        let result = appearance_state(result, &maximized).expect("maximized JUCE state");
        assert!(String::from_utf8_lossy(&result.component).contains("maximizeSpectrogram=\"1\""));
        let payload = serde_json::to_vec(&document).expect("JSON");
        let mut component = u32::try_from(payload.len()).expect("length").to_le_bytes().to_vec();
        component.extend(payload);
        let result = appearance_state(
            PluginState {
                component,
                controller: vec![],
            },
            &maximized,
        )
        .expect("maximized Rust state");
        let restored: serde_json::Value =
            serde_json::from_slice(result.component.get(4..).expect("JSON")).expect("state");
        assert_eq!(restored.pointer("/layout/maximize_spectrogram"), Some(&json!(true)));
    }

    /// A coalesced counter delta retains unresolved timing rather than pretending every GUI poll rendered.
    #[test]
    fn render_summary_counts_success_not_refresh_callbacks() {
        let report = RenderReport {
            started_unix_seconds: 1000.0,
            dropped_samples: 0,
            native_frame_samples: None,
            dropped_native_frame_samples: 0,
            samples: vec![
                RefreshSample {
                    elapsed_seconds: 1.0,
                    successful_frames: 10,
                    update_seconds: 0.001,
                },
                RefreshSample {
                    elapsed_seconds: 1.1,
                    successful_frames: 10,
                    update_seconds: 0.002,
                },
                RefreshSample {
                    elapsed_seconds: 1.2,
                    successful_frames: 12,
                    update_seconds: 0.003,
                },
            ],
        };
        let summary = summarize_render(&report, 1001.0, 1.0).expect("summary");
        assert_eq!(summary.get("successful_frames"), Some(&json!(2)));
        assert_eq!(summary.get("refresh_observations"), Some(&json!(3)));
        assert_eq!(
            summary.get("coalesced_frames_without_individual_timestamps"),
            Some(&json!(2))
        );
        assert_eq!(
            summary
                .get("per_frame_observations")
                .and_then(serde_json::Value::as_array)
                .map(Vec::len),
            Some(2)
        );
        assert_eq!(
            visibility_qualification(&[json!({"window_visible": true, "occlusion_state_visible": false})]),
            "not-qualified-hidden-or-occluded-observation"
        );
        assert_eq!(
            visibility_qualification(&[json!({"window_visible": null})]),
            "unavailable-native-visibility"
        );
        assert_eq!(
            visibility_qualification(&[json!({"window_visible": true, "occlusion_state_visible": true})]),
            "native-visible-observations-not-proof-of-unobstructed-pixels"
        );
    }

    /// Native encoding reports remain meaningful with no ordinary application refresh messages.
    #[test]
    fn native_summary_uses_real_encoding_timestamps_without_refreshes() {
        let report = RenderReport {
            started_unix_seconds: 1000.0,
            dropped_samples: 0,
            samples: vec![],
            dropped_native_frame_samples: 0,
            native_frame_samples: Some(vec![
                NativeFrameSample {
                    elapsed_seconds: 1.0,
                    successful_frames: 10,
                    encoding_seconds: 0.001,
                },
                NativeFrameSample {
                    elapsed_seconds: 1.01,
                    successful_frames: 11,
                    encoding_seconds: 0.002,
                },
                NativeFrameSample {
                    elapsed_seconds: 1.02,
                    successful_frames: 12,
                    encoding_seconds: 0.003,
                },
            ]),
        };
        let summary = summarize_render(&report, 1001.0, 1.0).expect("native summary");
        assert_eq!(summary.get("successful_frames"), Some(&json!(3)));
        assert_eq!(summary.get("refresh_observations"), Some(&json!(0)));
        let cadence = summary
            .get("mean_native_cadence_hz")
            .and_then(serde_json::Value::as_f64)
            .expect("cadence");
        assert!((cadence - 100.0).abs() < 1e-9);
        let saturated = RenderReport {
            dropped_native_frame_samples: 1,
            ..report
        };
        assert!(summarize_render(&saturated, 1001.0, 1.0).is_err());
    }

    /// A claimed continuous case must have real successful setters, movement of both controls, and no reset.
    #[test]
    fn continuous_case_rejects_missing_or_resetting_driver() {
        assert!(validate_live_sweep(&json!({})).is_err());
        let sweep = json!({"enabled": true, "accepted_updates": 120, "rejected_updates": 0,
            "generation_changes": 0, "projection_epoch_changes": 0,
            "minimum_persistence_seconds": 0.1, "maximum_persistence_seconds": 2.5,
            "minimum_contrast": 0.1, "maximum_contrast": 0.9});
        assert!(validate_live_sweep(&json!({"appearance_sweep": sweep})).is_ok());
        let mut reset = sweep;
        reset
            .as_object_mut()
            .expect("sweep")
            .insert("generation_changes".into(), json!(1));
        assert!(validate_live_sweep(&json!({"appearance_sweep": reset})).is_err());
    }

    /// A stable precomputed loop has exact-bit identity, with no callback-time trigonometry or hashing.
    #[test]
    fn pcm_identity_is_repeatable() {
        let first = precompute_signal();
        let second = precompute_signal();
        assert_eq!(first.hash, second.hash);
        assert_eq!(first.channels.first().expect("left").len(), SAMPLE_RATE);
        assert!(first.channels.iter().flatten().all(|sample| sample.is_finite()));
    }
}
