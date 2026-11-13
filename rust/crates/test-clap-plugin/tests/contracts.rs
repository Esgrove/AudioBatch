//! Hardware-free CLAP lifecycle, extension, state, and independent PCM fixture contracts.

use std::ffi::CStr;
use std::io;
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering};

use audiobatch_test_clap_plugin::{
    GAIN_PARAMETER, LATENCY_PARAMETER, MONO_CONFIG, PLUGIN_ID, PROCESS_FAILURE_PARAMETER, REQUEST_PARAMETER,
    SLEEP_PARAMETER, STEREO_CONFIG, WIDTH_PARAMETER, entry, gain_width_plain, latency_plain,
};
use clack_extensions::audio_ports::{AudioPortInfoBuffer, PluginAudioPorts};
use clack_extensions::audio_ports_config::{PluginAudioPortsConfig, PluginAudioPortsConfigInfo};
use clack_extensions::latency::PluginLatency;
use clack_extensions::params::{ParamInfoBuffer, ParamInfoFlags, PluginParams};
use clack_extensions::render::{PluginRender, RenderMode};
use clack_extensions::state::PluginState;
use clack_extensions::tail::{PluginTail, TailLength};
use clack_host::events::event_types::ParamValueEvent;
use clack_host::prelude::*;
use test_fixtures::{PLUGIN_EXPECTATIONS, PluginExpectation};

/// CLAP host requests observed without callback allocation or locks.
#[derive(Default)]
struct Requests {
    restart: AtomicU32,
    process: AtomicU32,
    callback: AtomicU32,
}

/// Shared host handler retaining request counters for the assertions.
struct HostShared(Arc<Requests>);

/// A headless host with no native device or UI dependencies.
struct Host;

/// One independently generated PCM16 source or expected output.
struct Pcm {
    channels: usize,
    samples: Vec<i16>,
}

impl SharedHandler<'_> for HostShared {
    fn request_restart(&self) {
        self.0.restart.fetch_add(1, Ordering::Relaxed);
    }
    fn request_process(&self) {
        self.0.process.fetch_add(1, Ordering::Relaxed);
    }
    fn request_callback(&self) {
        self.0.callback.fetch_add(1, Ordering::Relaxed);
    }
}

impl HostHandlers for Host {
    type Shared<'a> = HostShared;
    type MainThread<'a> = ();
    type AudioProcessor<'a> = ();
}

/// Constructs a real Clack plugin instance, using its ordinary factory and lifecycle.
fn instance() -> PluginInstance<Host> {
    let host_info = HostInfo::new("AudioBatch Tests", "AudioBatch", "https://example.com", "1.0.0").expect("host info");
    PluginInstance::new(
        |()| HostShared(Arc::default()),
        |_| (),
        &entry().expect("entry"),
        PLUGIN_ID,
        &host_info,
    )
    .expect("instance")
}

/// Delivers plain parameter values through the inactive plugin parameter extension.
fn set(plugin: &mut PluginInstance<Host>, values: &[(ClapId, f64)]) {
    let params = plugin.plugin_handle().get_extension::<PluginParams>().expect("params");
    let events: Vec<_> = values
        .iter()
        .map(|&(id, value)| ParamValueEvent::new(0, id, Pckn::match_all(), value))
        .collect();
    let mut ignored = EventBuffer::new();
    params.flush(
        &mut plugin.inactive_plugin_handle().expect("inactive"),
        &InputEvents::from_buffer(&events),
        &mut OutputEvents::from_buffer(&mut ignored),
    );
}

/// Reads the plain current parameter value through the public extension.
fn value(plugin: &mut PluginInstance<Host>, id: ClapId) -> f64 {
    let handle = plugin.plugin_handle();
    handle
        .get_extension::<PluginParams>()
        .expect("params")
        .get_value(&handle, id)
        .expect("value")
}

/// Processes one block through the actual plugin ABI, not a duplicate DSP model.
fn process(
    processor: &mut StartedPluginAudioProcessor<Host>,
    input: &mut [Vec<f32>],
    output: &mut [Vec<f32>],
    events: &[ParamValueEvent],
) -> Result<ProcessStatus, PluginInstanceError> {
    let mut inputs = AudioPorts::with_capacity(input.len(), 1);
    let mut outputs = AudioPorts::with_capacity(output.len(), 1);
    let input = inputs.with_input_buffers([AudioPortBuffer {
        latency: 0,
        channels: AudioPortBufferType::f32_input_only(input.iter_mut().map(InputChannel::variable)),
    }]);
    let mut output = outputs.with_output_buffers([AudioPortBuffer {
        latency: 0,
        channels: AudioPortBufferType::f32_output_only(output.iter_mut().map(Vec::as_mut_slice)),
    }]);
    let mut ignored = EventBuffer::new();
    processor.process(
        &input,
        &mut output,
        &InputEvents::from_buffer(&events),
        &mut OutputEvents::from_buffer(&mut ignored),
        None,
        None,
    )
}

/// Activates bounded blocks for the ordinary fixture sample rate.
fn activate(plugin: &mut PluginInstance<Host>, maximum: u32) -> StartedPluginAudioProcessor<Host> {
    plugin
        .activate(
            |_, ()| (),
            PluginAudioConfiguration {
                sample_rate: 44_100.0,
                min_frames_count: 1,
                max_frames_count: maximum,
            },
        )
        .expect("activate")
        .start_processing()
        .expect("start")
}

/// Loads committed WAV chunks without assuming the data immediately follows the format header.
fn read_pcm(path: &Path) -> io::Result<Pcm> {
    let bytes = std::fs::read(path)?;
    let invalid = || io::Error::new(io::ErrorKind::InvalidData, "invalid PCM16 fixture");
    if bytes.get(..4) != Some(b"RIFF") || bytes.get(8..12) != Some(b"WAVE") {
        return Err(invalid());
    }
    let mut offset = 12;
    let mut channels = None;
    let mut samples = None;
    while let Some(header) = bytes.get(offset..offset + 8) {
        let length = u32::from_le_bytes(
            header
                .get(4..8)
                .ok_or_else(invalid)?
                .try_into()
                .map_err(|_| invalid())?,
        ) as usize;
        let data = bytes.get(offset + 8..offset + 8 + length).ok_or_else(invalid)?;
        match header.get(..4) {
            Some(b"fmt ") => {
                if data.get(..2) != Some(&1_u16.to_le_bytes()) || data.get(14..16) != Some(&16_u16.to_le_bytes()) {
                    return Err(invalid());
                }
                channels = Some(usize::from(u16::from_le_bytes(
                    data.get(2..4).ok_or_else(invalid)?.try_into().map_err(|_| invalid())?,
                )));
            }
            Some(b"data") => {
                samples = Some(
                    data.as_chunks::<2>()
                        .0
                        .iter()
                        .copied()
                        .map(i16::from_le_bytes)
                        .collect(),
                );
            }
            _ => {}
        }
        offset += 8 + length + length % 2;
    }
    Ok(Pcm {
        channels: channels.ok_or_else(invalid)?,
        samples: samples.ok_or_else(invalid)?,
    })
}

/// Renders delayed output and trims precisely the plugin-reported leading latency.
fn render(case: &PluginExpectation, block: usize) -> Vec<i16> {
    let source = read_pcm(&case.source_path()).expect("source fixture");
    let mut plugin = instance();
    let handle = plugin.plugin_handle();
    handle
        .get_extension::<PluginAudioPortsConfig>()
        .expect("ports config")
        .select(
            &handle,
            if source.channels == 1 {
                MONO_CONFIG
            } else {
                STEREO_CONFIG
            },
        )
        .expect("select");
    set(
        &mut plugin,
        &[
            (GAIN_PARAMETER, f64::from(case.gain)),
            (WIDTH_PARAMETER, f64::from(case.width)),
            (LATENCY_PARAMETER, f64::from(case.latency)),
        ],
    );
    let mut processor = activate(&mut plugin, block as u32);
    let latency = plugin
        .plugin_handle()
        .get_extension::<PluginLatency>()
        .expect("latency")
        .get(&plugin.plugin_handle());
    assert_eq!(latency, case.latency);
    let tail = processor.plugin_handle().get_extension::<PluginTail>().expect("tail");
    assert_eq!(tail.get(&processor.plugin_handle()), TailLength::Finite(0));
    let source_frames = source.samples.len() / source.channels;
    let mut rendered = Vec::new();
    let mut start = 0;
    while start < source_frames + latency as usize {
        let frames = block.min(source_frames + latency as usize - start);
        let mut input = vec![vec![0.0; frames]; source.channels];
        for (channel_index, channel) in input.iter_mut().enumerate() {
            for (frame, sample) in channel.iter_mut().enumerate() {
                *sample = f32::from(
                    source
                        .samples
                        .get((start + frame) * source.channels + channel_index)
                        .copied()
                        .unwrap_or(0),
                ) * (1.0 / 32768.0);
            }
        }
        let mut output = vec![vec![0.0; frames]; source.channels];
        process(&mut processor, &mut input, &mut output, &[]).expect("process");
        for frame in 0..frames {
            if start + frame >= latency as usize {
                for channel in &output {
                    let sample = channel.get(frame).copied().expect("output sample");
                    rendered.push((sample * 32768.0).clamp(-32768.0, 32767.0).floor() as i16);
                }
            }
        }
        start += frames;
    }
    plugin.deactivate(processor.stop_processing());
    rendered
}

#[test]
fn parameters_are_plain_and_ports_are_configurable() {
    let mut plugin = instance();
    let handle = plugin.plugin_handle();
    let params = handle.get_extension::<PluginParams>().expect("params");
    let mut buffer = ParamInfoBuffer::new();
    let gain = params.get_info(&handle, 0, &mut buffer).expect("gain info");
    assert_eq!(gain.id, GAIN_PARAMETER);
    assert!((gain.max_value - 2.0).abs() < f64::EPSILON);
    let latency = params.get_info(&handle, 2, &mut buffer).expect("latency info");
    assert!(latency.flags.contains(ParamInfoFlags::IS_STEPPED));
    assert!((latency.max_value - 1024.0).abs() < f64::EPSILON);
    let config = handle.get_extension::<PluginAudioPortsConfig>().expect("config");
    assert_eq!(config.count(&handle), 2);
    config.select(&handle, MONO_CONFIG).expect("mono");
    let config_info = handle
        .get_extension::<PluginAudioPortsConfigInfo>()
        .expect("config info");
    assert_eq!(config_info.current_config(&handle), Some(MONO_CONFIG));
    let ports = handle.get_extension::<PluginAudioPorts>().expect("ports");
    let mut port_buffer = AudioPortInfoBuffer::new();
    assert_eq!(
        ports
            .get(&handle, 0, true, &mut port_buffer)
            .expect("input")
            .channel_count,
        1
    );
    config.select(&handle, STEREO_CONFIG).expect("stereo");
    let render = handle.get_extension::<PluginRender>().expect("render");
    assert!(!render.has_realtime_requirement(&handle));
    render.set(&handle, RenderMode::Offline).expect("offline");
    render.set(&handle, RenderMode::Realtime).expect("realtime");
    assert!((gain_width_plain(0.25) - 0.5).abs() < f64::EPSILON);
    assert!((latency_plain(0.25) - 256.0).abs() < f64::EPSILON);
    set(&mut plugin, &[(GAIN_PARAMETER, 0.5), (WIDTH_PARAMETER, 1.5)]);
    assert!((value(&mut plugin, GAIN_PARAMETER) - 0.5).abs() < f64::EPSILON);
}

#[test]
fn independent_instances_support_repeated_activation_and_drop() {
    let mut mono = instance();
    let mut stereo = instance();
    let mono_handle = mono.plugin_handle();
    let configurations = mono_handle
        .get_extension::<PluginAudioPortsConfig>()
        .expect("configurations");
    configurations.select(&mono_handle, MONO_CONFIG).expect("mono");
    let mono_info = mono_handle
        .get_extension::<PluginAudioPortsConfigInfo>()
        .expect("mono configuration info");
    let stereo_handle = stereo.plugin_handle();
    let stereo_info = stereo_handle
        .get_extension::<PluginAudioPortsConfigInfo>()
        .expect("stereo configuration info");
    assert_eq!(mono_info.current_config(&mono_handle), Some(MONO_CONFIG));
    assert_eq!(stereo_info.current_config(&stereo_handle), Some(STEREO_CONFIG));
    for maximum in [4, 8, 4] {
        for plugin in [&mut mono, &mut stereo] {
            let mut processor = activate(plugin, maximum);
            processor.reset();
            plugin.deactivate(processor.stop_processing());
            assert!((value(plugin, GAIN_PARAMETER) - 1.0).abs() < f64::EPSILON);
            assert!((value(plugin, WIDTH_PARAMETER) - 1.0).abs() < f64::EPSILON);
            assert!(value(plugin, LATENCY_PARAMETER).abs() < f64::EPSILON);
        }
    }
    drop(mono);
    let processor = activate(&mut stereo, 4);
    stereo.deactivate(processor.stop_processing());
}

#[test]
fn state_round_trips_and_rejects_invalid_data_atomically() {
    let mut plugin = instance();
    set(
        &mut plugin,
        &[
            (GAIN_PARAMETER, 0.5),
            (WIDTH_PARAMETER, 1.5),
            (LATENCY_PARAMETER, 256.0),
        ],
    );
    let handle = plugin.plugin_handle();
    let state = handle.get_extension::<PluginState>().expect("state");
    let mut saved = Vec::new();
    state.save(&handle, &mut saved).expect("save");
    assert_eq!(saved.len(), 20);
    set(
        &mut plugin,
        &[(GAIN_PARAMETER, 1.0), (WIDTH_PARAMETER, 1.0), (LATENCY_PARAMETER, 0.0)],
    );
    state
        .load(&plugin.plugin_handle(), &mut saved.as_slice())
        .expect("load");
    assert!((value(&mut plugin, GAIN_PARAMETER) - 0.5).abs() < f64::EPSILON);
    assert!((value(&mut plugin, WIDTH_PARAMETER) - 1.5).abs() < f64::EPSILON);
    assert!((value(&mut plugin, LATENCY_PARAMETER) - 256.0).abs() < f64::EPSILON);
    for invalid in [
        saved.get(..19).expect("truncated").to_vec(),
        [saved.clone(), vec![1]].concat(),
        vec![0; 20],
    ] {
        assert!(state.load(&plugin.plugin_handle(), &mut invalid.as_slice()).is_err());
        assert!((value(&mut plugin, GAIN_PARAMETER) - 0.5).abs() < f64::EPSILON);
    }
}

#[test]
#[cfg_attr(miri, ignore = "reads committed audio fixtures")]
fn every_independent_fixture_matches_exact_pcm16_across_block_sizes() {
    for case in PLUGIN_EXPECTATIONS {
        let expected = read_pcm(&case.expected_path()).expect("expected fixture");
        for block in [127, 512] {
            assert_eq!(
                render(&case, block),
                expected.samples,
                "{} at block {block}",
                case.expected
            );
        }
    }
}

#[test]
fn timed_events_requests_failure_and_sleep_use_the_real_process_path() {
    let mut plugin = instance();
    let requests = plugin.access_shared_handler(|host| Arc::clone(&host.0));
    let mut processor = activate(&mut plugin, 8);
    let mut input = vec![vec![1.0; 8]; 2];
    let mut output = vec![vec![0.0; 8]; 2];
    let events = [
        ParamValueEvent::new(4, GAIN_PARAMETER, Pckn::match_all(), 0.5),
        ParamValueEvent::new(6, REQUEST_PARAMETER, Pckn::match_all(), 7.0),
    ];
    assert_eq!(
        process(&mut processor, &mut input, &mut output, &events).expect("process"),
        ProcessStatus::Continue
    );
    assert_eq!(output.first().expect("left"), &[1.0, 1.0, 1.0, 1.0, 0.5, 0.5, 0.5, 0.5]);
    assert_eq!(requests.restart.load(Ordering::Relaxed), 1);
    assert_eq!(requests.process.load(Ordering::Relaxed), 1);
    assert_eq!(requests.callback.load(Ordering::Relaxed), 1);
    let events = [ParamValueEvent::new(0, SLEEP_PARAMETER, Pckn::match_all(), 1.0)];
    assert_eq!(
        process(&mut processor, &mut input, &mut output, &events).expect("sleep"),
        ProcessStatus::Sleep
    );
    let events = [ParamValueEvent::new(
        0,
        PROCESS_FAILURE_PARAMETER,
        Pckn::match_all(),
        1.0,
    )];
    assert!(process(&mut processor, &mut input, &mut output, &events).is_err());
    plugin.deactivate(processor.stop_processing());
}

#[test]
fn active_latency_change_requests_restart_and_latches_only_at_activation() {
    let mut plugin = instance();
    let requests = plugin.access_shared_handler(|host| Arc::clone(&host.0));
    let mut processor = activate(&mut plugin, 4);
    let params = processor
        .plugin_handle()
        .get_extension::<PluginParams>()
        .expect("params");
    let events = [ParamValueEvent::new(0, LATENCY_PARAMETER, Pckn::match_all(), 3.0)];
    let mut ignored = EventBuffer::new();
    params.flush_active(
        &mut processor.plugin_handle(),
        &InputEvents::from_buffer(&events),
        &mut OutputEvents::from_buffer(&mut ignored),
    );
    assert_eq!(requests.restart.load(Ordering::Relaxed), 1);
    let latency = plugin
        .plugin_handle()
        .get_extension::<PluginLatency>()
        .expect("latency");
    assert_eq!(latency.get(&plugin.plugin_handle()), 0);
    plugin.deactivate(processor.stop_processing());
    let mut processor = activate(&mut plugin, 4);
    assert_eq!(latency.get(&plugin.plugin_handle()), 3);
    let config = plugin
        .plugin_handle()
        .get_extension::<PluginAudioPortsConfig>()
        .expect("config");
    assert!(config.select(&plugin.plugin_handle(), MONO_CONFIG).is_err());
    let mut input = vec![vec![1.0; 4]; 2];
    let mut output = vec![vec![0.0; 4]; 2];
    process(&mut processor, &mut input, &mut output, &[]).expect("delay");
    assert_eq!(output.first().expect("left"), &[0.0, 0.0, 0.0, 1.0]);
    processor.reset();
    process(&mut processor, &mut input, &mut output, &[]).expect("reset delay");
    assert_eq!(output.first().expect("left"), &[0.0, 0.0, 0.0, 1.0]);
    plugin.deactivate(processor.stop_processing());
}

#[test]
fn plugin_factory_has_stable_identity() {
    let entry = entry().expect("entry");
    let factory = entry.get_plugin_factory().expect("factory");
    assert_eq!(factory.plugin_count(), 1);
    let descriptor = factory.plugin_descriptor(0).expect("descriptor");
    assert_eq!(descriptor.id(), Some(PLUGIN_ID));
    assert_eq!(descriptor.name(), Some(c"AudioBatch Test MS CLAP" as &CStr));
}
