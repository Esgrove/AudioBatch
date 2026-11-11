//! Actual packaged CLAP contracts through Clack, independent expectations, and explicit build-artifact selection.

#![cfg(feature = "clap")]

use std::ffi::CString;
use std::io::{self, Read, Write};
use std::thread;

use anyhow::{Context as _, Result, ensure};
use clack_extensions::configurable_audio_ports::{
    AudioPortRequest, AudioPortRequestDetails, PluginConfigurableAudioPorts,
};
use clack_extensions::gui::PluginGui;
use clack_extensions::params::{ParamInfoBuffer, ParamInfoFlags, PluginParams};
use clack_extensions::render::RenderMode;
use clack_extensions::state::PluginState;
use clack_extensions::tail::{PluginTail, TailLength};
use clack_host::plugin::PluginInstanceError;
use clack_host::utils::ClapId;

#[path = "../examples/support/clap_host.rs"]
mod host;

/// Independently specified native parameter metadata, not copied from the plugin's registry.
struct ExpectedParameter {
    minimum: f64,
    maximum: f64,
    default: f64,
    stepped: bool,
}

/// Independent plain-unit ranges and neutral defaults, deliberately not derived from the plugin's registry.
const fn expected_parameter(id: u32) -> ExpectedParameter {
    match id {
        0 => ExpectedParameter {
            minimum: -24.0,
            maximum: 12.0,
            default: 0.0,
            stepped: false,
        },
        1..=3 => ExpectedParameter {
            minimum: 20.0,
            maximum: 20_000.0,
            default: match id {
                1 => 100.0,
                2 => 500.0,
                _ => 5000.0,
            },
            stepped: false,
        },
        4..=27 => match (id - 4) % 6 {
            0 | 1 => ExpectedParameter {
                minimum: -60.0,
                maximum: 12.0,
                default: 0.0,
                stepped: false,
            },
            2 => ExpectedParameter {
                minimum: 0.0,
                maximum: 2.0,
                default: 1.0,
                stepped: false,
            },
            3 => ExpectedParameter {
                minimum: 0.0,
                maximum: 200.0,
                default: 0.0,
                stepped: false,
            },
            4 => ExpectedParameter {
                minimum: -100.0,
                maximum: 100.0,
                default: 0.0,
                stepped: false,
            },
            _ => ExpectedParameter {
                minimum: 0.0,
                maximum: 1.0,
                default: 0.0,
                stepped: true,
            },
        },
        _ => ExpectedParameter {
            minimum: 0.0,
            maximum: 1.0,
            default: 0.0,
            stepped: true,
        },
    }
}

/// Checks the native metadata and finite text conversion contract for every persisted numeric id.
#[test]
#[cfg_attr(miri, ignore = "dynamically loads and executes an actual packaged CLAP")]
fn packaged_parameter_metadata_and_plain_conversions() -> Result<()> {
    let Some(bundle) = host::selected_bundle()? else {
        return Ok(());
    };
    let _deadline = host::Deadline::start()?;
    let mut instance = host::load(&bundle)?;
    let handle = instance.plugin_handle();
    let has_gui = handle.get_extension::<PluginGui>().is_some();
    if cfg!(windows) {
        ensure!(
            !has_gui,
            "Win32 CLAP must not advertise the unsafe reentrant destroy lifecycle"
        );
    } else if cfg!(feature = "editor") {
        ensure!(has_gui, "macOS packaged CLAP must expose the shared native editor");
    }
    let params = handle.get_extension::<PluginParams>().context("CLAP params")?;
    ensure!(params.count(&handle) == 31, "exact append-only 31-parameter contract");
    let mut scratch = ParamInfoBuffer::new();
    let mut text = [0; 128];
    for id in 0..31 {
        let info = params
            .get_info(&handle, id, &mut scratch)
            .context("parameter metadata")?;
        let ExpectedParameter {
            minimum,
            maximum,
            default,
            stepped,
        } = expected_parameter(id);
        ensure!(info.id.get() == id, "parameter id/index reordered");
        ensure!(!info.name.is_empty(), "parameter name missing");
        ensure!(
            info.min_value.to_bits() == minimum.to_bits() && info.max_value.to_bits() == maximum.to_bits(),
            "wrong plain range for {id}"
        );
        ensure!(
            info.default_value.is_finite()
                && (info.default_value - default).abs() <= default.abs().max(1.0) * f64::from(f32::EPSILON) * 8.0,
            "wrong plain default for {id}"
        );
        ensure!(
            info.flags.contains(ParamInfoFlags::IS_AUTOMATABLE),
            "parameter {id} not automatable"
        );
        ensure!(
            !info
                .flags
                .intersects(ParamInfoFlags::IS_MODULATABLE | ParamInfoFlags::IS_READONLY),
            "unsupported flags for {id}"
        );
        ensure!(
            info.flags.contains(ParamInfoFlags::IS_STEPPED) == stepped,
            "stepped flag for {id}"
        );
        ensure!(
            info.flags.contains(ParamInfoFlags::IS_BYPASS) == (id == 28),
            "only id 28 is bypass"
        );
        let current = params.get_value(&handle, ClapId::new(id)).context("default readback")?;
        ensure!(
            (current - default).abs() <= default.abs().max(1.0) * f64::from(f32::EPSILON) * 8.0,
            "native default is not in plain units"
        );
        for plain in [minimum, default, maximum] {
            let formatted = params.value_to_text(&handle, ClapId::new(id), plain, &mut text)?;
            let formatted = CString::new(formatted.to_vec())?;
            let parsed = params
                .text_to_value(&handle, ClapId::new(id), &formatted)
                .context("formatted plain roundtrip")?;
            ensure!(
                parsed.is_finite() && (parsed - plain).abs() <= plain.abs().max(1.0) * f64::from(f32::EPSILON) * 8.0,
                "plain/text roundtrip for {id}"
            );
        }
        for invalid in [c"NaN", c"inf", c"1e999", c"invalid"] {
            ensure!(
                params.text_to_value(&handle, ClapId::new(id), invalid).is_none(),
                "invalid text accepted for {id}"
            );
        }
        ensure!(
            params
                .value_to_text(&handle, ClapId::new(id), f64::NAN, &mut text)
                .is_err(),
            "NaN formatted"
        );
    }
    ensure!(
        params.get_info(&handle, 31, &mut scratch).is_none(),
        "out-of-range metadata accepted"
    );
    ensure!(
        params.get_value(&handle, ClapId::new(31)).is_none(),
        "out-of-range id accepted"
    );
    verify_plain_automation(&mut instance)?;
    host::service_callbacks(&mut instance);
    Ok(())
}

/// Exercises stopped plain-unit writes and verifies invalid events preserve each prior finite value.
fn verify_plain_automation(instance: &mut host::Instance) -> Result<()> {
    let params = instance
        .plugin_handle()
        .get_extension::<PluginParams>()
        .context("CLAP params")?;
    host::flush(instance, &[(0, -6.0), (6, 1.25), (7, 200.0), (8, -25.0), (28, 1.0)])?;
    let handle = instance.plugin_handle();
    for (id, expected) in [(0, -6.0), (6, 1.25), (7, 200.0), (8, -25.0), (28, 1.0)] {
        let value = params
            .get_value(&handle, ClapId::new(id))
            .context("plain automation readback")?;
        ensure!(
            (value - expected).abs() < 1e-10,
            "plain automation interpreted as normalized for {id}"
        );
    }
    host::flush(instance, &[(0, f64::NAN), (6, f64::INFINITY), (31, 1.0)])?;
    let handle = instance.plugin_handle();
    ensure!(
        params.get_value(&handle, ClapId::new(0)) == Some(-6.0),
        "non-finite automation changed gain"
    );
    ensure!(
        params.get_value(&handle, ClapId::new(6)) == Some(1.25),
        "non-finite automation changed width"
    );
    Ok(())
}

/// Checks both matching layouts and rendering modes, including actual reset/start/stop and exact delayed PCM.
#[test]
#[cfg_attr(miri, ignore = "dynamically loads and processes an actual packaged CLAP")]
fn packaged_neutral_latency_tail_reset_and_layouts() -> Result<()> {
    let Some(bundle) = host::selected_bundle()? else {
        return Ok(());
    };
    let _deadline = host::Deadline::start()?;
    for channels in [1, 2] {
        for mode in [RenderMode::Realtime, RenderMode::Offline] {
            let mut instance = host::load(&bundle)?;
            host::configure(&mut instance, channels)?;
            host::set_render(&mut instance, mode)?;
            let stopped = host::activate(&mut instance)?;
            let buffers = host::Buffers::new(channels as usize);
            let (stopped, result) = thread::spawn(move || {
                let mut processor = stopped
                    .start_processing()
                    .map_err(PluginInstanceError::from)
                    .expect("start CLAP processing");
                let mut buffers = buffers;
                let result = (|| -> Result<()> {
                    let handle = processor.plugin_handle();
                    let tail = handle.get_extension::<PluginTail>().context("CLAP tail")?;
                    ensure!(
                        tail.get(&handle) == TailLength::Finite(host::TAIL as u32),
                        "independent 4096-sample tail"
                    );
                    // Reset clears the complete delayed history on the owning fresh audio thread.
                    for pass in 0..2 {
                        if pass == 1 {
                            host::reset_region(&mut processor);
                        }
                        for block in 0..40 {
                            for (channel, samples) in buffers.input.iter_mut().enumerate() {
                                for (frame, sample) in samples.iter_mut().enumerate() {
                                    *sample = signal(block * host::BLOCK_FRAMES + frame, channel);
                                }
                            }
                            buffers.process(&mut processor, &[])?;
                            for (channel, samples) in buffers.output.iter().enumerate() {
                                for (frame, &sample) in samples.iter().enumerate() {
                                    let absolute = block * host::BLOCK_FRAMES + frame;
                                    let expected = absolute
                                        .checked_sub(host::LATENCY)
                                        .map_or(0.0, |frame| signal(frame, channel));
                                    ensure!(
                                        sample.to_bits() == expected.to_bits(),
                                        "neutral bits differ at {absolute}, channel {channel}, pass {pass}"
                                    );
                                }
                            }
                        }
                    }
                    Ok(())
                })();
                (processor.stop_processing(), result)
            })
            .join()
            .map_err(|_| anyhow::anyhow!("CLAP callback thread panicked"))?;
            instance.deactivate(stopped);
            result?;
        }
    }
    Ok(())
}

/// Finite exactly represented stereo/mono samples, followed by a complete latency-plus-tail zero drain.
fn signal(frame: usize, channel: usize) -> f32 {
    if frame >= 769 {
        return 0.0;
    }
    let value = ((frame * 37 + channel * 19) % 257) as f32 - 128.0;
    value * (1.0 / 512.0)
}

/// Host stream that deliberately accepts only a few bytes per read/write.
struct PartialStream {
    /// Independent payload buffer used for both saving and subsequent restore.
    bytes: Vec<u8>,
    /// Reader position, independent of write progress.
    position: usize,
    /// Largest accepted transfer per callback.
    chunk: usize,
}

impl Read for PartialStream {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        let remaining = self.bytes.get(self.position..).unwrap_or_default();
        let count = remaining.len().min(output.len()).min(self.chunk);
        if let (Some(output), Some(input)) = (output.get_mut(..count), remaining.get(..count)) {
            output.copy_from_slice(input);
        }
        self.position += count;
        Ok(count)
    }
}

impl Write for PartialStream {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        let count = bytes.len().min(self.chunk);
        self.bytes.extend_from_slice(bytes.get(..count).unwrap_or_default());
        Ok(count)
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Saves actual native state without deriving expected state bytes from the implementation.
fn save(instance: &mut host::Instance) -> Result<Vec<u8>> {
    let handle = instance.plugin_handle();
    let state = handle.get_extension::<PluginState>().context("CLAP state")?;
    let mut output = Vec::new();
    state.save(&handle, &mut output)?;
    Ok(output)
}

/// Exercises partial streams, an independent current document, and transactional malformed input.
/// Separate instances verify that restore defaults do not inherit another plugin's choices.
#[test]
#[cfg_attr(miri, ignore = "dynamically loads actual packaged CLAP state interfaces")]
fn packaged_state_partial_streams_and_transactional_rejection() -> Result<()> {
    let Some(bundle) = host::selected_bundle()? else {
        return Ok(());
    };
    let _deadline = host::Deadline::start()?;
    let mut source = host::load(&bundle)?;
    let mut destination = host::load(&bundle)?;
    let defaults = save(&mut destination)?;
    host::flush(&mut source, &[(0, -12.0), (4, -60.0), (7, 200.0), (8, 35.0), (28, 1.0)])?;
    let handle = source.plugin_handle();
    let source_state = handle.get_extension::<PluginState>().context("CLAP state")?;
    let mut partial = PartialStream {
        bytes: Vec::new(),
        position: 0,
        chunk: 3,
    };
    source_state.save(&handle, &mut partial)?;
    ensure!(
        !partial.bytes.is_empty() && partial.bytes.len() <= 4096,
        "bounded saved payload"
    );
    ensure!(save(&mut destination)? == defaults, "state leaked between instances");
    let state = destination
        .plugin_handle()
        .get_extension::<PluginState>()
        .context("destination CLAP state")?;
    state.load(&destination.plugin_handle(), &mut partial)?;
    let before = save(&mut destination)?;
    ensure!(before == partial.bytes, "partial save/load did not roundtrip");
    for bytes in [
        Vec::new(),
        vec![b' '; 16_385],
        b"{".to_vec(),
        br#"{"version":99,"parameters":{"gain":0.75}}"#.to_vec(),
        br#"{"version":2,"parameters":{"gain":0.75}}"#.to_vec(),
        br#"{"version":1,"parameters":{"gain":1.1}}"#.to_vec(),
        br#"{"version":1,"parameters":{"gain":1e999}}"#.to_vec(),
        br#"{"version":1,"parameters":{"gain":0.75},"geometry":{"width":1200,"height":599}}"#.to_vec(),
        br#"{"version":1,"parameters":{"gain":0.75,"unknown":0.5}}"#.to_vec(),
    ] {
        let mut invalid = PartialStream {
            bytes,
            position: 0,
            chunk: 7,
        };
        ensure!(
            state.load(&destination.plugin_handle(), &mut invalid).is_err(),
            "malformed state accepted"
        );
        ensure!(
            save(&mut destination)? == before,
            "malformed state partially mutated controls"
        );
    }
    let mut no_progress = PartialStream {
        bytes: before,
        position: 0,
        chunk: 0,
    };
    ensure!(
        state.load(&destination.plugin_handle(), &mut no_progress).is_err(),
        "zero-read stream accepted"
    );
    ensure!(
        state.save(&destination.plugin_handle(), &mut no_progress).is_err(),
        "zero-write stream accepted"
    );
    let current = br#"{"version":1,"parameters":{"gain":0.25}}"#;
    state.load(&destination.plugin_handle(), &mut current.as_slice())?;
    let handle = destination.plugin_handle();
    let params = handle.get_extension::<PluginParams>().context("CLAP params")?;
    ensure!(
        params.get_value(&handle, ClapId::new(0)) == Some(-15.0),
        "current normalized gain mapping"
    );
    ensure!(
        params.get_value(&handle, ClapId::new(28)) == Some(0.0),
        "missing bypass used live value instead of fixed default"
    );
    state.load(
        &destination.plugin_handle(),
        &mut br#"{"version":1,"parameters":{}}"#.as_slice(),
    )?;
    ensure!(
        save(&mut destination)? == defaults,
        "missing fields did not restore fixed defaults"
    );
    Ok(())
}

/// Mismatched, unsupported, and invalid-index layout requests are rejected atomically before activation.
#[test]
#[cfg_attr(miri, ignore = "dynamically loads actual packaged CLAP ports")]
fn packaged_port_configuration_rejects_mismatched_layouts() -> Result<()> {
    let Some(bundle) = host::selected_bundle()? else {
        return Ok(());
    };
    let _deadline = host::Deadline::start()?;
    let mut instance = host::load(&bundle)?;
    host::configure(&mut instance, 2)?;
    let mut handle = instance.inactive_plugin_handle().context("inactive ports")?;
    let configurable = handle
        .get_extension::<PluginConfigurableAudioPorts>()
        .context("configurable ports")?;
    for requests in [
        vec![
            AudioPortRequest::new(true, 0, AudioPortRequestDetails::mono()),
            AudioPortRequest::new(false, 0, AudioPortRequestDetails::stereo()),
        ],
        vec![
            AudioPortRequest::new(true, 0, AudioPortRequestDetails::untyped(3)),
            AudioPortRequest::new(false, 0, AudioPortRequestDetails::untyped(3)),
        ],
        vec![
            AudioPortRequest::new(true, 1, AudioPortRequestDetails::mono()),
            AudioPortRequest::new(false, 0, AudioPortRequestDetails::mono()),
        ],
    ] {
        ensure!(
            !configurable.can_apply_configuration(&mut handle, &requests),
            "invalid configuration reported supported"
        );
        ensure!(
            !configurable.apply_configuration(&mut handle, &requests),
            "invalid configuration applied"
        );
    }
    host::configure(&mut instance, 1)?;
    Ok(())
}
