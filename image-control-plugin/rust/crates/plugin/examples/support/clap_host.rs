//! Bounded Clack host for explicitly selected packaged CLAP artifacts, without installation or audio hardware.

#![expect(
    unsafe_code,
    reason = "loads only explicitly authorized build-tree CLAP libraries through Clack"
)]

use std::ffi::CString;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

use anyhow::{Context as _, Result, ensure};
use clack_extensions::audio_ports::{AudioPortFlags, AudioPortInfoBuffer, AudioPortType, PluginAudioPorts};
use clack_extensions::configurable_audio_ports::{
    AudioPortRequest, AudioPortRequestDetails, PluginConfigurableAudioPorts,
};
use clack_extensions::latency::PluginLatency;
#[cfg(test)]
use clack_extensions::params::PluginParams;
use clack_extensions::render::{PluginRender, RenderMode};
use clack_host::entry::LibraryEntry;
use clack_host::events::event_types::ParamValueEvent;
use clack_host::plugin::PluginInstanceError;
use clack_host::prelude::*;
use clack_host::process::audio_buffers::{InputAudioBuffers, OutputAudioBuffers};
use clack_host::utils::ClapId;

#[path = "artifacts.rs"]
mod artifacts;

/// Independent persisted plugin identity, not discovered from an installed-plugin registry.
pub const PLUGIN_ID: &std::ffi::CStr = c"com.esgrove.imagecontrol-rs-own";
/// Independent latency at the tested sample rate.
pub const LATENCY: usize = 4352;
/// Independent tail beyond the latency-compensated endpoint.
#[cfg(test)]
pub const TAIL: usize = 4096;
/// Non-partition-aligned callback size exercises the native block adapter.
pub const BLOCK_FRAMES: usize = 257;
/// Explicit sample rate shared by activation and analytical expectations.
pub const SAMPLE_RATE: f64 = 48_000.0;

/// One process deadline covers loading, callbacks, worker joins, and native destruction.
pub struct Deadline {
    /// Completion notification sent only after all native plugin ownership has been retired.
    finished: mpsc::Sender<()>,
    /// Watchdog joins outside the measured audio region.
    worker: Option<JoinHandle<()>>,
}

impl Deadline {
    /// Makes a blocked native callback fatal instead of allowing an indefinitely hung integration test.
    pub fn start() -> Result<Self> {
        let (finished, receiver) = mpsc::channel();
        let worker = thread::Builder::new()
            .name("clap-contract-deadline".into())
            .spawn(move || {
                if receiver.recv_timeout(Duration::from_secs(20)) == Err(mpsc::RecvTimeoutError::Timeout) {
                    eprintln!("CASE_FAILED packaged CLAP exceeded its internal 20-second deadline");
                    std::process::abort();
                }
            })?;
        Ok(Self {
            finished,
            worker: Some(worker),
        })
    }
}

impl Drop for Deadline {
    fn drop(&mut self) {
        let _ = self.finished.send(());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

/// Requires an absolute authorized package and validates the actual platform-specific payload.
/// Neither this resolver nor Clack searches default installed-plugin directories.
pub fn checked_bundle(path: &Path) -> Result<PathBuf> {
    let bundle = artifacts::checked_artifact(path).context("CLAP must be an explicit authorized build artifact")?;
    ensure!(
        bundle.extension().is_some_and(|extension| extension == "clap"),
        "expected a .clap package"
    );
    #[cfg(target_os = "macos")]
    {
        ensure!(bundle.is_dir(), "macOS CLAP package must be a directory");
        let binary = artifacts::checked_artifact(&bundle.join("Contents/MacOS/ImageControl RS"))?;
        ensure!(
            binary.is_file() && binary.starts_with(&bundle),
            "CLAP binary escaped its package"
        );
    }
    #[cfg(windows)]
    ensure!(bundle.is_file(), "Windows CLAP package must be a single .clap library");
    Ok(bundle)
}

/// Resolves only the caller's environment selection. Ordinary local test runs skip visibly when absent.
#[cfg(test)]
pub fn selected_bundle() -> Result<Option<PathBuf>> {
    let Some(path) = std::env::var_os("IMAGE_CONTROL_CLAP_BUNDLE") else {
        ensure!(
            std::env::var_os("CI").is_none(),
            "CI must provide IMAGE_CONTROL_CLAP_BUNDLE"
        );
        eprintln!("SKIP packaged CLAP contract: set IMAGE_CONTROL_CLAP_BUNDLE to an absolute build-tree .clap package");
        return Ok(None);
    };
    checked_bundle(Path::new(&path)).map(Some)
}

/// Minimal hardware-free host with atomically retained plugin requests.
pub struct ContractHost;

/// Requests remain observable without callback-side allocation, locking, logging, or wakeups.
#[derive(Default)]
pub struct Shared {
    /// Deferred main-thread callback request.
    callback: AtomicBool,
    /// Deferred restart request.
    restart: AtomicBool,
    /// Deferred processing request.
    process: AtomicBool,
}

impl HostHandlers for ContractHost {
    type Shared<'a> = Shared;
    type MainThread<'a> = ();
    type AudioProcessor<'a> = ();
}

impl SharedHandler<'_> for Shared {
    fn request_restart(&self) {
        self.restart.store(true, Ordering::Release);
    }
    fn request_process(&self) {
        self.process.store(true, Ordering::Release);
    }
    fn request_callback(&self) {
        self.callback.store(true, Ordering::Release);
    }
}

/// Main-thread-owned instance. Clack retains its dynamic entry until all processor handles are retired.
pub type Instance = PluginInstance<ContractHost>;
/// Only this prepared stopped handle is transferred to a fresh callback thread.
pub type Stopped = StoppedPluginAudioProcessor<ContractHost>;
/// Exclusive started handle used within callback markers.
pub type Started = StartedPluginAudioProcessor<ContractHost>;

/// Loads the real packaged binary, validates its descriptor, and instantiates by its stable CLAP identity.
pub fn load(bundle: &Path) -> Result<Instance> {
    let bundle = checked_bundle(bundle)?;
    #[cfg(target_os = "macos")]
    let binary = bundle.join("Contents/MacOS/ImageControl RS");
    #[cfg(not(target_os = "macos"))]
    let binary = bundle.clone();
    let path = CString::new(bundle.to_str().context("CLAP package path must be UTF-8")?)?;
    // SAFETY: The caller explicitly selects an authorized project build artifact.
    // Clack owns its library and contains all subsequent CLAP ABI and instance lifetime handling.
    let entry = unsafe { PluginEntry::load_from(LibraryEntry::load_from_path(binary)?, &path)? };
    let factory = entry.get_plugin_factory().context("packaged CLAP plugin factory")?;
    let descriptors: Vec<_> = factory.plugin_descriptors().collect();
    ensure!(descriptors.len() == 1, "package must advertise exactly one plugin");
    let descriptor = descriptors.first().context("CLAP descriptor")?;
    ensure!(descriptor.id() == Some(PLUGIN_ID), "wrong packaged CLAP identity");
    ensure!(
        descriptor.name() == Some(c"ImageControl RS"),
        "wrong packaged CLAP name"
    );
    ensure!(descriptor.vendor() == Some(c"Esgrove"), "wrong packaged CLAP vendor");
    let info = HostInfo::new("ImageControl contract host", "Esgrove", "https://esgrove.com", "1")?;
    Ok(Instance::new(|()| Shared::default(), |_| (), &entry, PLUGIN_ID, &info)?)
}

/// Applies matching mono/stereo ports while inactive and verifies the resulting advertised main buses.
pub fn configure(instance: &mut Instance, channels: u32) -> Result<()> {
    ensure!(channels == 1 || channels == 2, "only mono/stereo are supported");
    let mut handle = instance
        .inactive_plugin_handle()
        .context("configure only while inactive")?;
    let configurable = handle
        .get_extension::<PluginConfigurableAudioPorts>()
        .context("CLAP configurable-audio-ports")?;
    let details = if channels == 1 {
        AudioPortRequestDetails::mono()
    } else {
        AudioPortRequestDetails::stereo()
    };
    let requests = [
        AudioPortRequest::new(true, 0, details),
        AudioPortRequest::new(false, 0, details),
    ];
    ensure!(
        configurable.can_apply_configuration(&mut handle, &requests),
        "matching port request rejected"
    );
    ensure!(
        configurable.apply_configuration(&mut handle, &requests),
        "matching port configuration failed"
    );
    let handle = instance.plugin_handle();
    let ports = handle.get_extension::<PluginAudioPorts>().context("CLAP audio-ports")?;
    for input in [true, false] {
        ensure!(ports.count(&handle, input) == 1, "one main port in each direction");
        let mut scratch = AudioPortInfoBuffer::new();
        let port = ports
            .get(&handle, 0, input, &mut scratch)
            .context("main port metadata")?;
        ensure!(port.channel_count == channels, "configured channel count not published");
        ensure!(port.flags.contains(AudioPortFlags::IS_MAIN), "main audio port flag");
        let expected = if channels == 1 {
            AudioPortType::MONO
        } else {
            AudioPortType::STEREO
        };
        ensure!(port.port_type == Some(expected), "configured mono/stereo port type");
        ensure!(
            ports.get(&handle, 1, input, &mut scratch).is_none(),
            "extra port index accepted"
        );
    }
    Ok(())
}

/// Selects the CLAP rendering policy on the lifecycle thread, never from inside a marked callback.
pub fn set_render(instance: &mut Instance, mode: RenderMode) -> Result<()> {
    let handle = instance.plugin_handle();
    let render = handle.get_extension::<PluginRender>().context("CLAP render")?;
    ensure!(
        !render.has_realtime_requirement(&handle),
        "offline rendering must be supported"
    );
    render.set(&handle, mode)?;
    Ok(())
}

/// Prepares every plugin buffer before transferring a stopped processor to its callback thread.
pub fn activate(instance: &mut Instance) -> Result<Stopped> {
    let processor = instance.activate(
        |_, ()| (),
        PluginAudioConfiguration {
            sample_rate: SAMPLE_RATE,
            min_frames_count: 1,
            max_frames_count: u32::try_from(BLOCK_FRAMES)?,
        },
    )?;
    let handle = instance.plugin_handle();
    let latency = handle.get_extension::<PluginLatency>().context("CLAP latency")?;
    ensure!(
        latency.get(&handle) as usize == LATENCY,
        "independent 4352-sample latency contract"
    );
    Ok(processor)
}

/// Sends plain-unit automation while inactive, outside audio execution and state save/restore.
#[cfg(test)]
pub fn flush(instance: &mut Instance, values: &[(u32, f64)]) -> Result<()> {
    let events: Vec<_> = values.iter().map(|&(id, value)| event(id, value)).collect();
    let mut handle = instance.inactive_plugin_handle().context("inactive flush")?;
    let params = handle.get_extension::<PluginParams>().context("CLAP params")?;
    params.flush(
        &mut handle,
        &InputEvents::from_buffer(&events),
        &mut OutputEvents::void(),
    );
    Ok(())
}

/// Creates a global plain-unit parameter event with no per-note or per-channel addressing.
pub const fn event(id: u32, value: f64) -> ParamValueEvent {
    ParamValueEvent::new(0, ClapId::new(id), Pckn::match_all(), value)
}

/// Owns all host sample storage and preallocated port-pointer arrays before the first callback.
pub struct Buffers {
    /// Fixed input sample buffers owned by the host.
    pub input: Vec<Vec<f32>>,
    /// Fixed output sample buffers owned by the host.
    pub output: Vec<Vec<f32>>,
    /// Prepared Clack input pointer scratch.
    input_ports: AudioPorts,
    /// Prepared Clack output pointer scratch.
    output_ports: AudioPorts,
}

impl Buffers {
    /// Allocates sample and pointer storage before the fresh processing thread starts.
    pub fn new(channels: usize) -> Self {
        Self {
            input: vec![vec![0.0; BLOCK_FRAMES]; channels],
            output: vec![vec![0.0; BLOCK_FRAMES]; channels],
            input_ports: AudioPorts::with_capacity(channels, 1),
            output_ports: AudioPorts::with_capacity(channels, 1),
        }
    }

    /// Wraps existing audio storage outside the marked native callback and processes one complete block.
    pub fn process(
        &mut self,
        processor: &mut Started,
        events: &[ParamValueEvent],
    ) -> Result<ProcessStatus, PluginInstanceError> {
        let input = self.input_ports.with_input_buffers([AudioPortBuffer {
            latency: 0,
            channels: AudioPortBufferType::f32_input_only(self.input.iter_mut().map(InputChannel::variable)),
        }]);
        let mut output = self.output_ports.with_output_buffers([AudioPortBuffer {
            latency: 0,
            channels: AudioPortBufferType::f32_output_only(self.output.iter_mut().map(Vec::as_mut_slice)),
        }]);
        process_region(processor, &input, &mut output, &InputEvents::from_buffer(&events))
    }
}

/// Marks the complete Clack/native process call, without signal generation, host allocation, or assertions.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn process_region(
    processor: &mut Started,
    input: &InputAudioBuffers<'_>,
    output: &mut OutputAudioBuffers<'_>,
    events: &InputEvents<'_>,
) -> Result<ProcessStatus, PluginInstanceError> {
    processor.process(input, output, events, &mut OutputEvents::void(), None, None)
}

/// Marks only the real plugin's reset callback on its owning audio thread.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
pub fn reset_region(processor: &mut Started) {
    processor.reset();
}

/// Delivers deferred plugin main-thread requests without logging or dispatching from the audio callback.
pub fn service_callbacks(instance: &mut Instance) {
    if instance.access_shared_handler(|shared| shared.callback.swap(false, Ordering::AcqRel)) {
        instance.call_on_main_thread_callback();
    }
}
