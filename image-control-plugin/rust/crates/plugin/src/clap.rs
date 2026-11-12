//! Clack-owned CLAP ABI lifecycle and format-independent prepared processing.
//!
//! Clack contains unwinding at every exported callback and enforces the CLAP thread classes.
//! The audio path uses bounded event traversal and the shared runtime's single-attempt DSP admission.

#[cfg(feature = "editor")]
mod gui;
mod parameters;
#[cfg(test)]
mod tests;

use std::cell::Cell;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use clack_extensions::audio_ports::{
    AudioPortFlags, AudioPortInfo, AudioPortInfoWriter, AudioPortType, PluginAudioPorts, PluginAudioPortsImpl,
};
use clack_extensions::configurable_audio_ports::{
    AudioPortRequest, PluginConfigurableAudioPorts, PluginConfigurableAudioPortsImpl,
};
use clack_extensions::latency::{HostLatency, PluginLatency, PluginLatencyImpl};
use clack_extensions::params::PluginParams;
use clack_extensions::render::{PluginRender, PluginRenderImpl, RenderMode};
use clack_extensions::state::PluginState;
use clack_extensions::tail::{PluginTail, PluginTailImpl, TailLength};
use clack_plugin::plugin::features::{AUDIO_EFFECT, MONO, STEREO, UTILITY};
use clack_plugin::prelude::*;
use clack_plugin::process::audio::ChannelPair;
use image_control_dsp::Imager;

use crate::runtime::Instance;

/// One format identity, shared by standalone CLAP and the statically linked AU wrapper.
struct ImageControl;

/// Stable instance storage and cross-thread lifecycle reports, never replaced while active.
struct Shared {
    /// Processing and state ownership common to every native format.
    instance: Instance,
    /// Prepared latency queried without entering mutable DSP.
    latency: AtomicU32,
    /// Offline policy is sampled only by the exclusive audio owner.
    offline: AtomicBool,
    /// Prepared activation prevents configuration changes while processing is available.
    active: AtomicBool,
    /// Bounded UI-to-host automation transport, independent of mutable DSP.
    #[cfg(feature = "editor")]
    edits: std::sync::Arc<gui::EditQueue>,
}

/// CLAP main-thread state contains no mutable audio history.
struct MainThread<'a> {
    /// CLAP guarantees all main-thread operations stay on this host handle's thread.
    host: HostMainThreadHandle<'a>,
    /// Shared state remains live until both main and audio components have been retired.
    shared: &'a Shared,
    /// Matching input/output channels, configurable only while inactive.
    channels: Cell<u32>,
    /// Reusable Iced native attachment and UI-only parameter binding.
    #[cfg(feature = "editor")]
    gui: gui::Gui<'a>,
}

/// Activated processor borrows stable storage and holds its fixed layout and block-size contract.
struct AudioProcessor<'a> {
    /// No Arc reference-count operations are needed inside processing callbacks.
    shared: &'a Shared,
    /// Matching mono or stereo channels fixed for this activation.
    channels: u32,
    /// Maximum block size accepted at activation.
    maximum_frames: u32,
    /// Finite ringing beyond compensated latency.
    tail: u32,
}

impl PluginShared<'_> for Shared {}
impl<'a> PluginMainThread<'a, Shared> for MainThread<'a> {}

impl Plugin for ImageControl {
    type AudioProcessor<'a> = AudioProcessor<'a>;
    type Shared<'a> = Shared;
    type MainThread<'a> = MainThread<'a>;

    /// Declares only the contracts implemented by this adapter, without installing host-global hooks.
    fn declare_extensions(builder: &mut PluginExtensions<Self>, _shared: Option<&Shared>) {
        builder
            .register::<PluginAudioPorts>()
            .register::<PluginConfigurableAudioPorts>()
            .register::<PluginLatency>()
            .register::<PluginTail>()
            .register::<PluginParams>()
            .register::<PluginRender>()
            .register::<PluginState>();
        // The shared editor synchronously retires its native child during CLAP's void destroy callback.
        #[cfg(all(feature = "editor", any(target_os = "macos", windows)))]
        builder.register::<clack_extensions::gui::PluginGui>();
    }
}

impl DefaultPluginFactory for ImageControl {
    /// Allocates immutable discovery metadata outside processing.
    fn get_descriptor() -> PluginDescriptor {
        PluginDescriptor::new(crate::BUNDLE_ID, crate::PLUGIN_NAME)
            .with_vendor(crate::PLUGIN_VENDOR)
            .with_version(crate::PLUGIN_VERSION)
            .with_description("Multiband mid/side stereo imaging")
            .with_features([AUDIO_EFFECT, UTILITY, MONO, STEREO])
    }

    /// Constructs one independent runtime before any host interface is published.
    fn new_shared(_host: HostSharedHandle<'_>) -> Result<Shared, PluginError> {
        Ok(Shared {
            instance: Instance::new(),
            latency: AtomicU32::new(0),
            offline: AtomicBool::new(false),
            active: AtomicBool::new(false),
            #[cfg(feature = "editor")]
            edits: std::sync::Arc::new(gui::EditQueue::new()),
        })
    }

    /// Retains only lifetime-checked host handles and thread-affine GUI storage.
    fn new_main_thread<'a>(host: HostMainThreadHandle<'a>, shared: &'a Shared) -> Result<MainThread<'a>, PluginError> {
        Ok(MainThread {
            host,
            shared,
            channels: Cell::new(2),
            #[cfg(feature = "editor")]
            gui: gui::Gui::new(host, shared),
        })
    }
}

impl<'a> PluginAudioProcessor<'a, Shared, MainThread<'a>> for AudioProcessor<'a> {
    /// Prepares every DSP allocation on the main thread before admitting realtime callbacks.
    fn activate(
        _host: HostAudioProcessorHandle<'a>,
        main_thread: &MainThread<'a>,
        shared: &'a Shared,
        config: PluginAudioConfiguration,
    ) -> Result<Self, PluginError> {
        if config.min_frames_count == 0 || config.min_frames_count > config.max_frames_count {
            return Err(PluginError::Message("Invalid CLAP block-size range"));
        }
        let channels = main_thread.channels.get();
        let prepared = shared
            .instance
            .prepare(
                config.sample_rate,
                channels as usize,
                shared.offline.load(Ordering::Relaxed),
                config.max_frames_count,
            )
            .ok_or(PluginError::Message("Could not prepare ImageControl audio"))?;
        shared.latency.store(prepared.latency_samples, Ordering::Relaxed);
        shared.active.store(true, Ordering::Release);
        if let Some(extension) = main_thread.host.get_extension::<HostLatency>() {
            extension.changed(&main_thread.host);
        }
        Ok(Self {
            shared,
            channels,
            maximum_frames: config.max_frames_count,
            tail: prepared.tail_samples,
        })
    }

    /// Samples the final finite global event per parameter at block start, then processes paired native buffers.
    fn process(&mut self, _process: Process, mut audio: Audio, events: Events) -> Result<ProcessStatus, PluginError> {
        parameters::apply_events(
            &self.shared.instance.parameters,
            events.input,
            Some(audio.frames_count()),
        );
        #[cfg(feature = "editor")]
        self.shared.edits.flush(events.output, &self.shared.instance.parameters);
        let frames = audio.frames_count();
        if frames > self.maximum_frames {
            return Err(PluginError::Message("CLAP block exceeds the activated maximum"));
        }
        if frames == 0 {
            return Ok(ProcessStatus::Continue);
        }
        if audio.input_port_count() != 1 || audio.output_port_count() != 1 {
            return Err(PluginError::Message(
                "ImageControl requires one matching input/output port",
            ));
        }
        let mut port = audio
            .port_pair(0)
            .ok_or(PluginError::Message("Missing main audio port"))?;
        let mut channels = port
            .channels()?
            .into_f32()
            .ok_or(PluginError::Message("ImageControl requires 32-bit audio"))?;
        if channels.input_channel_count() != self.channels as usize
            || channels.output_channel_count() != self.channels as usize
        {
            return Err(PluginError::Message(
                "CLAP audio layout differs from the activated layout",
            ));
        }
        let mut left = channels
            .channel_pair(0)
            .ok_or(PluginError::Message("Missing left channel"))?;
        let mut right = if self.channels == 2 {
            channels.channel_pair(1)
        } else {
            None
        };
        self.shared
            .instance
            .audio
            .try_with(|imager| {
                self.shared.instance.parameters.publish(&self.shared.instance.shared);
                imager.begin_block();
                process_channels(imager, &mut left, right.as_mut(), frames as usize);
            })
            .ok_or(PluginError::Message("Concurrent audio lifecycle call"))?;
        if let Some(mut output) = port.output() {
            output.set_constant_mask(clack_plugin::process::ConstantMask::from_bits(0));
        }
        Ok(ProcessStatus::Continue)
    }

    /// Settles current saved targets and clears delay history without allocating or entering GUI state.
    fn reset(&mut self) {
        let _ = self.shared.instance.audio.try_with(|imager| {
            self.shared.instance.parameters.publish(&self.shared.instance.shared);
            imager.reset();
        });
    }

    /// Publishes monitoring activity separately from preparation and GUI visibility.
    fn start_processing(&mut self) -> Result<(), PluginError> {
        self.shared.instance.shared.set_processing_active(true);
        Ok(())
    }

    /// Stops optional monitoring without altering saved parameters or DSP ownership.
    fn stop_processing(&mut self) {
        self.shared.instance.shared.set_processing_active(false);
        self.shared.instance.shared.invalidate_monitoring();
    }

    /// Retires the prepared worker and histories on the main thread, never in the last audio callback.
    fn deactivate(self, _main_thread: &MainThread<'a>) {
        self.shared.active.store(false, Ordering::Release);
        self.shared.instance.shared.set_processing_active(false);
        self.shared.instance.shared.invalidate_monitoring();
        // Release only prepared history, preserving the stable monitoring allocation used by an open editor.
        let _ = self.shared.instance.audio.try_with(Imager::release);
    }
}

/// Reads each input frame before either output write, retaining legal in-place operation.
fn process_channels(
    imager: &mut Imager,
    left: &mut ChannelPair<'_, f32>,
    mut right: Option<&mut ChannelPair<'_, f32>>,
    frames: usize,
) {
    for index in 0..frames {
        let left_sample = read_sample(left, index);
        let right_sample = right
            .as_ref()
            .map_or(left_sample, |channel| read_sample(channel, index));
        let output = imager.process_frame([left_sample, right_sample]);
        write_sample(left, index, output[0]);
        if let Some(channel) = right.as_mut() {
            write_sample(channel, index, output[1]);
        }
    }
}

/// Borrows only for this sample read, rather than extending a channel-pair borrow over the block.
#[expect(
    clippy::indexing_slicing,
    reason = "Clack guarantees frame-count-long channel slices"
)]
const fn read_sample(channel: &ChannelPair<'_, f32>, index: usize) -> f32 {
    match channel {
        ChannelPair::InputOnly(input) | ChannelPair::InputOutput(input, _) => input[index],
        ChannelPair::InPlace(buffer) => buffer[index],
        ChannelPair::OutputOnly(_) => 0.0,
    }
}

/// Writes one output after both input samples have been captured.
#[expect(
    clippy::indexing_slicing,
    reason = "Clack guarantees frame-count-long channel slices"
)]
const fn write_sample(channel: &mut ChannelPair<'_, f32>, index: usize, value: f32) {
    match channel {
        ChannelPair::OutputOnly(output) | ChannelPair::InputOutput(_, output) | ChannelPair::InPlace(output) => {
            output[index] = value;
        }
        ChannelPair::InputOnly(_) => {}
    }
}

impl PluginLatencyImpl for MainThread<'_> {
    /// Queries a prepared atomic report without borrowing audio-owned history.
    fn get(&self) -> u32 {
        self.shared.latency.load(Ordering::Relaxed)
    }
}

impl PluginTailImpl for AudioProcessor<'_> {
    /// Reports the FIR ringing beyond latency compensation, independent of bypass.
    fn get(&self) -> TailLength {
        TailLength::Finite(self.tail)
    }
}

impl PluginRenderImpl for MainThread<'_> {
    /// The DSP can run both asynchronously in realtime and synchronously for deterministic offline renders.
    fn has_hard_realtime_requirement(&self) -> bool {
        false
    }

    /// Applies potentially allocating render-mode transitions on the main thread under bounded DSP admission.
    /// A concurrent callback rejects the change rather than deferring offline resource destruction to realtime audio.
    fn set(&self, mode: RenderMode) -> Result<(), PluginError> {
        let offline = mode == RenderMode::Offline;
        if self.shared.offline.load(Ordering::Acquire) == offline {
            return Ok(());
        }
        if self.shared.active.load(Ordering::Acquire) {
            return Err(PluginError::Message("Deactivate before changing the CLAP render mode"));
        }
        self.shared
            .instance
            .audio
            .try_with(|imager| {
                imager.set_offline(offline);
                self.shared.offline.store(offline, Ordering::Release);
            })
            .ok_or(PluginError::Message(
                "Concurrent audio callback prevents render-mode change",
            ))?;
        Ok(())
    }
}

impl PluginAudioPortsImpl for MainThread<'_> {
    /// Exposes exactly one main port in either direction.
    fn count(&self, _is_input: bool) -> u32 {
        1
    }

    /// Uses stable per-direction ids and explicitly allows matching in-place pairs.
    fn get(&self, index: u32, is_input: bool, writer: &mut AudioPortInfoWriter) {
        if index != 0 {
            return;
        }
        writer.set(&AudioPortInfo {
            id: ClapId::new(0),
            name: if is_input { b"Input" } else { b"Output" },
            channel_count: self.channels.get(),
            flags: AudioPortFlags::IS_MAIN,
            port_type: Some(if self.channels.get() == 1 {
                AudioPortType::MONO
            } else {
                AudioPortType::STEREO
            }),
            in_place_pair: Some(ClapId::new(0)),
        });
    }
}

/// Validates a whole bounded configuration transaction before publishing either direction.
fn requested_channels(current: u32, requests: &[AudioPortRequest<'_>]) -> Option<u32> {
    if requests.len() > 2 {
        return None;
    }
    let mut channels = [current; 2];
    let mut seen = [false; 2];
    for request in requests {
        let direction = usize::from(!request.is_input());
        let details = request.details();
        let count = details.channel_count();
        let expected = match count {
            1 => AudioPortType::MONO,
            2 => AudioPortType::STEREO,
            _ => return None,
        };
        if request.port_index() != 0
            || seen.get(direction).copied().unwrap_or(true)
            || details.port_type().is_some_and(|kind| kind != expected)
            || !details.raw_details().is_null()
        {
            return None;
        }
        *seen.get_mut(direction)? = true;
        *channels.get_mut(direction)? = count;
    }
    (channels[0] == channels[1]).then_some(channels[0])
}

impl PluginConfigurableAudioPortsImpl for MainThread<'_> {
    /// Accepts only an atomic matching mono/mono or stereo/stereo arrangement while inactive.
    fn can_apply_configuration(&self, list: &[AudioPortRequest<'_>]) -> bool {
        !self.shared.active.load(Ordering::Acquire) && requested_channels(self.channels.get(), list).is_some()
    }

    /// Publishes the validated pair together, with actual DSP configuration deferred to activation.
    fn apply_configuration(&self, list: &[AudioPortRequest<'_>]) -> bool {
        if self.shared.active.load(Ordering::Acquire) {
            return false;
        }
        let Some(channels) = requested_channels(self.channels.get(), list) else {
            return false;
        };
        self.channels.set(channels);
        true
    }
}

// The exported descriptor is present in both cdylib and staticlib builds.
// AU's native wrapper references clap_entry directly, retaining its object during static linking.
clack_plugin::clack_export_entry!(SinglePluginEntry<ImageControl>);
