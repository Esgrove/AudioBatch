//! UI-owned CLAP lifecycle, state, bus negotiation, and periodic callback service.

#![expect(
    unsafe_code,
    reason = "shares UI-owned lifecycle storage with one exclusively leased audio processor"
)]

use std::cell::{RefCell, UnsafeCell};
use std::ffi::{CString, NulError};
use std::mem::ManuallyDrop;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;

use clack_extensions::audio_ports::{AudioPortFlags, AudioPortInfoBuffer, PluginAudioPorts};
use clack_extensions::audio_ports_config::{AudioPortsConfigBuffer, PluginAudioPortsConfig};
use clack_extensions::configurable_audio_ports::{
    AudioPortRequest, AudioPortRequestDetails, PluginConfigurableAudioPorts,
};
use clack_extensions::latency::PluginLatency;
use clack_extensions::params::{ParamInfoBuffer, ParamInfoFlags, PluginParams};
use clack_extensions::render::{PluginRender, PluginRenderError, RenderMode};
use clack_extensions::state::{PluginState, StateError};
use clack_extensions::tail::{PluginTail, TailLength};
use clack_extensions::timer::PluginTimer;
use clack_host::events::event_types::ParamValueEvent;
use clack_host::plugin::{PluginInstance as ClackInstance, PluginInstanceError};
use clack_host::prelude::*;
use clack_host::process::PluginAudioProcessor;
use clack_host::utils::ClapId;
use thiserror::Error;

use crate::host::{
    AudioHandler, AudioScope, Host, MainThread, OutputSink, QueuedEdit, RestartFlags, Shared, SharedHandlerData,
};
use crate::processor::ProcessorStorage;
use crate::{Module, ProcessorHandle, UiDispatcher, UiThread};

thread_local! {
    /// Retains ownership until a UI tick can safely perform native teardown and release buffers.
    static INSTANCES: RefCell<ManuallyDrop<Vec<Arc<InstanceCore>>>> = const { RefCell::new(ManuallyDrop::new(Vec::new())) };
}

/// Typed failures of lifecycle, state and automation operations.
#[derive(Debug, Error)]
pub enum InstanceError {
    /// Native CLAP operation failed.
    #[error(transparent)]
    Plugin(#[from] PluginInstanceError),
    /// Silent idle processing failed while servicing plugin requests.
    #[error(transparent)]
    Process(#[from] crate::ProcessError),
    /// Invalid host metadata supplied at build time.
    #[error("Invalid CLAP host information: {0}")]
    HostInfo(#[source] NulError),
    /// Invalid CLAP class identifier.
    #[error(transparent)]
    InvalidId(#[from] NulError),
    /// A processor still leases the prepared state.
    #[error("The plugin is in use by an audio processor")]
    InUse,
    /// Configuration cannot change while active.
    #[error("The plugin must be inactive to change its setup")]
    Active,
    /// Preparation has not happened.
    #[error("The plugin must be set up before activation or processing")]
    SetupBeforeProcessing,
    /// Processing requires activation.
    #[error("The plugin must be active before processing")]
    ActivationBeforeProcessing,
    /// Rate or block length is invalid.
    #[error("Invalid CLAP processing setup: {sample_rate} Hz, {max_block_frames} frames")]
    InvalidSetup {
        /// Requested sample rate.
        sample_rate: f64,
        /// Requested maximum block length.
        max_block_frames: usize,
    },
    /// A render-policy change was refused.
    #[error(transparent)]
    Render(#[from] PluginRenderError),
    /// Hardware-backed effects cannot render offline.
    #[error("The plugin requires realtime processing")]
    RealtimeRequired,
    /// No usable main audio output exists.
    #[error("The CLAP plugin has no main audio output")]
    MissingOutput,
    /// Missing or unreasonable port metadata.
    #[error("Invalid CLAP audio port layout")]
    InvalidPorts,
    /// No supported layout can carry this signal.
    #[error("The plugin does not support {channels} channel audio")]
    UnsupportedChannels {
        /// Requested channel count.
        channels: usize,
    },
    /// State capture failed.
    #[error(transparent)]
    State(#[from] StateError),
    /// Reconstruction refused its state.
    #[error("The reloaded plugin refused its saved settings")]
    RestoreState,
    /// Pending edits have not reached audio yet.
    #[error("The plugin's audio processor has not received every parameter change")]
    EditsPending,
    /// The plugin has no writable parameter with this identity.
    #[error("Unknown or read-only CLAP parameter {id}")]
    InvalidParameter {
        /// Requested parameter identity.
        id: u32,
    },
    /// Nonfinite parameter values are never sent to native code.
    #[error("Parameter {id} value must be finite")]
    NonFiniteParameter {
        /// Requested parameter identity.
        id: u32,
    },
    /// The bounded automation queue is full, without dropping an earlier accepted edit.
    #[error("The plugin parameter queue is full")]
    EditQueueFull,
}

/// Processing policy selected before activation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessMode {
    /// Live processing subject to audio callback deadlines.
    Realtime,
    /// File rendering without realtime deadlines.
    Offline,
}

/// Channel counts in native port order, together with the actual main-port indexes.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BusLayout {
    /// Channel count of every input port, including sidechains.
    pub inputs: Vec<usize>,
    /// Channel count of every output port.
    pub outputs: Vec<usize>,
    /// Main input port, when the plugin consumes audio.
    pub main_input: Option<usize>,
    /// Main output port.
    pub main_output: usize,
}

/// Immutable prepared audio geometry.
#[derive(Debug, Clone, Copy)]
pub struct ProcessingSetup {
    pub(crate) sample_rate: f64,
    pub(crate) max_block_frames: usize,
}

/// UI-thread-only mutable instance state.
pub struct Parts {
    pub(crate) plugin: ClackInstance<Host>,
    pub(crate) module: Arc<Module>,
    class_id: String,
    dispatcher: Option<UiDispatcher>,
    layout: Option<BusLayout>,
    setup: Option<ProcessingSetup>,
}

/// Shared lifetime anchor. Native lifecycle and buffer destruction only occur from UI service.
pub struct InstanceCore {
    pub(crate) parts: RefCell<Parts>,
    pub(crate) shared: Arc<Shared>,
    pub(crate) processor: UnsafeCell<Option<PluginAudioProcessor<Host>>>,
    pub(crate) storage: UnsafeCell<Option<ProcessorStorage>>,
    pub(crate) leased: AtomicBool,
}

/// One UI-controlled CLAP plugin, independently owned from its audio processor and editor.
pub struct PluginInstance {
    pub(crate) core: Arc<InstanceCore>,
}

// SAFETY: Parts is only reached with UiThread, and processor access is exclusive through the lease.
// The UI registry retains the final Arc so native and sample-storage destruction occurs on that UI thread.
#[expect(
    clippy::non_send_fields_in_send_ty,
    reason = "UI registry owns lifecycle fields, audio lease only accesses dedicated UnsafeCell storage"
)]
// SAFETY: Only token-gated UI calls reach Parts. The lease exclusively owns separate audio storage.
unsafe impl Send for InstanceCore {}
// SAFETY: Shared access uses atomics. RefCell parts is only used by the registered UI thread.
// Mutable processor access is restricted to the single lease or the UI thread while no lease exists.
unsafe impl Sync for InstanceCore {}

impl InstanceCore {
    /// Runs lifecycle callbacks and registered timers, without holding the registry borrow across plugin calls.
    fn service(&self, ui: &UiThread) {
        let _ = ui;
        let Ok(mut parts) = self.parts.try_borrow_mut() else {
            return;
        };
        if self.shared.callback.swap(false, Ordering::AcqRel) {
            parts.plugin.call_on_main_thread_callback();
        }
        let now = Instant::now();
        let due = parts.plugin.access_handler(|handler| {
            let mut timers = handler.timers.borrow_mut();
            timers
                .iter_mut()
                .filter_map(|timer| {
                    if timer.deadline > now {
                        return None;
                    }
                    timer.deadline = now + timer.period;
                    Some(timer.id)
                })
                .collect::<Vec<_>>()
        });
        for id in due {
            // A timer callback can unregister another due timer. Recheck before delivery.
            let registered = parts
                .plugin
                .access_handler(|handler| handler.timers.borrow().iter().any(|timer| timer.id == id));
            if registered {
                let handle = parts.plugin.plugin_handle();
                if let Some(timer) = handle.get_extension::<PluginTimer>() {
                    timer.on_timer(&handle, id);
                }
            }
        }
    }

    /// Flushes bounded edits while the caller exclusively controls the audio processor.
    fn flush(&self, parts: &mut Parts) {
        self.shared.flush.swap(false, Ordering::AcqRel);
        self.shared.requires_process.swap(false, Ordering::AcqRel);
        let Some(params) = parts.plugin.plugin_handle().get_extension::<PluginParams>() else {
            return;
        };
        let mut completed = self.shared.completed_edits.load(Ordering::Acquire);
        let mut edits = Vec::new();
        // SAFETY: The caller excludes audio leases before entering this UI-only flush.
        if let Some(storage) = unsafe { &mut *self.storage.get() }
            && let Some(sequence) = storage.append_pending_edits(&mut edits)
        {
            completed = sequence;
        }
        edits.extend(
            (0..self.shared.edits.len())
                .filter_map(|_| self.shared.edits.pop())
                .map(|edit| {
                    completed = edit.sequence;
                    edit.event
                }),
        );
        let input = InputEvents::from_buffer(&edits);
        let mut sink = OutputSink(&self.shared);
        let mut output = OutputEvents::from_buffer(&mut sink);
        // SAFETY: The caller holds the UI lifecycle borrow, and has checked that no processor lease exists.
        if let Some(processor) = unsafe { &mut *self.processor.get() } {
            let _audio = AudioScope::enter();
            params.flush_active(&mut processor.plugin_handle(), &input, &mut output);
        } else if let Some(mut handle) = parts.plugin.inactive_plugin_handle() {
            params.flush(&mut handle, &input, &mut output);
        }
        self.shared.completed_edits.store(completed, Ordering::Release);
    }
}
impl Drop for InstanceCore {
    fn drop(&mut self) {
        // SAFETY: The UI registry owns the final Arc, so no audio lease exists during teardown.
        if let Some(processor) = self.processor.get_mut().take() {
            let _audio = AudioScope::enter();
            let stopped = processor.into_stopped();
            self.parts.get_mut().plugin.deactivate(stopped);
        }
    }
}

impl PluginInstance {
    /// Instantiates a stable CLAP class id on the registered UI thread.
    ///
    /// # Errors
    /// Returns native instantiation failure or an invalid class id.
    pub fn create(
        _ui: &UiThread,
        module: &Arc<Module>,
        class_id: &str,
        dispatcher: Option<UiDispatcher>,
    ) -> Result<Self, InstanceError> {
        let id = CString::new(class_id)?;
        let shared = Arc::new(Shared::new());
        let host_shared = Arc::clone(&shared);
        let info = HostInfo::new(
            "AudioBatch",
            "Esgrove",
            "https://esgrove.com",
            env!("CARGO_PKG_VERSION"),
        )
        .map_err(InstanceError::HostInfo)?;
        let plugin = ClackInstance::<Host>::new(
            move |()| SharedHandlerData(host_shared),
            |shared| MainThread::new(Arc::clone(&shared.0)),
            &module.entry,
            &id,
            &info,
        )?;
        let core = Arc::new(InstanceCore {
            parts: RefCell::new(Parts {
                plugin,
                module: Arc::clone(module),
                class_id: class_id.to_owned(),
                dispatcher,
                layout: None,
                setup: None,
            }),
            shared,
            processor: UnsafeCell::new(None),
            storage: UnsafeCell::new(None),
            leased: AtomicBool::new(false),
        });
        INSTANCES.with_borrow_mut(|instances| instances.push(Arc::clone(&core)));
        Ok(Self { core })
    }

    /// Replaces an unleased instance and restores its saved settings before it is configured.
    ///
    /// # Errors
    /// Returns `InUse`, a creation failure, or `RestoreState`.
    pub fn recreate(self, ui: &UiThread, state: &[u8]) -> Result<Self, InstanceError> {
        if self.has_processor_handles() || Arc::strong_count(&self.core) > 2 {
            return Err(InstanceError::InUse);
        }
        let parts = self.core.parts.borrow();
        let module = Arc::clone(&parts.module);
        let id = parts.class_id.clone();
        let dispatcher = parts.dispatcher.clone();
        drop(parts);
        drop(self);
        service_main_thread(ui);
        let mut replacement = Self::create(ui, &module, &id, dispatcher)?;
        if !replacement.set_state(ui, state)? {
            return Err(InstanceError::RestoreState);
        }
        Ok(replacement)
    }

    /// Returns whether a prepared processor is still leased to a renderer.
    #[must_use]
    pub fn has_processor_handles(&self) -> bool {
        self.core.leased.load(Ordering::Acquire)
    }

    /// Reads the current ports without selecting another layout during scanning.
    ///
    /// # Errors
    /// Returns invalid or missing port metadata from the plugin.
    pub fn current_bus_layout(&self, _ui: &UiThread) -> Result<BusLayout, InstanceError> {
        read_layout(&mut self.core.parts.borrow_mut().plugin)
    }

    /// Configures a main mono/stereo bus, then falls back to a compatible advertised fixed layout.
    /// Sidechains remain allocated and receive silence.
    ///
    /// # Errors
    /// Returns lifecycle exclusion, invalid metadata, or unsupported channel count.
    pub fn configure_main_buses(&mut self, _ui: &UiThread, channels: usize) -> Result<BusLayout, InstanceError> {
        self.ensure_inactive()?;
        if !(1..=64).contains(&channels) {
            return Err(InstanceError::UnsupportedChannels { channels });
        }
        let mut parts = self.core.parts.borrow_mut();
        let mut layout = read_layout(&mut parts.plugin)?;
        for candidate in [channels, if channels == 1 { 2 } else { channels }] {
            if candidate <= 2 {
                let details = if candidate == 1 {
                    AudioPortRequestDetails::mono()
                } else {
                    AudioPortRequestDetails::stereo()
                };
                if let Some(mut handle) = parts.plugin.inactive_plugin_handle()
                    && let Some(configurable) = handle.get_extension::<PluginConfigurableAudioPorts>()
                {
                    let mut requests = Vec::new();
                    if let Some(index) = layout.main_input {
                        requests.push(AudioPortRequest::new(true, index as u32, details));
                    }
                    requests.push(AudioPortRequest::new(false, layout.main_output as u32, details));
                    if configurable.can_apply_configuration(&mut handle, &requests) {
                        let _ = configurable.apply_configuration(&mut handle, &requests);
                    }
                }
            }
            layout = read_layout(&mut parts.plugin)?;
            if main_matches(&layout, candidate) {
                break;
            }
            let handle = parts.plugin.plugin_handle();
            if let Some(configs) = handle.get_extension::<PluginAudioPortsConfig>() {
                let mut scratch = AudioPortsConfigBuffer::new();
                for index in 0..configs.count(&handle).min(1024) {
                    if let Some(config) = configs.get(&handle, index, &mut scratch)
                        && config
                            .main_output
                            .is_some_and(|port| port.channel_count as usize == candidate)
                        && config
                            .main_input
                            .is_none_or(|port| port.channel_count as usize == candidate)
                        && configs.select(&handle, config.id).is_ok()
                    {
                        break;
                    }
                }
            }
            layout = read_layout(&mut parts.plugin)?;
            if main_matches(&layout, candidate) {
                break;
            }
        }
        let output = layout.outputs.get(layout.main_output).copied().unwrap_or(0);
        let input = layout
            .main_input
            .and_then(|index| layout.inputs.get(index).copied())
            .unwrap_or(channels);
        if input < channels || output < channels {
            return Err(InstanceError::UnsupportedChannels { channels });
        }
        parts.layout = Some(layout.clone());
        Ok(layout)
    }

    /// Selects the render policy and validates the setup before activation.
    ///
    /// # Errors
    /// Returns lifecycle exclusion, invalid geometry, or a rejected render mode.
    pub fn setup(
        &mut self,
        _ui: &UiThread,
        mode: ProcessMode,
        sample_rate: f64,
        max_block_frames: usize,
    ) -> Result<(), InstanceError> {
        self.ensure_inactive()?;
        if !sample_rate.is_finite()
            || sample_rate <= 0.0
            || max_block_frames == 0
            || max_block_frames > u32::MAX as usize
        {
            return Err(InstanceError::InvalidSetup {
                sample_rate,
                max_block_frames,
            });
        }
        let mut parts = self.core.parts.borrow_mut();
        let handle = parts.plugin.plugin_handle();
        if let Some(render) = handle.get_extension::<PluginRender>() {
            if mode == ProcessMode::Offline && render.has_realtime_requirement(&handle) {
                return Err(InstanceError::RealtimeRequired);
            }
            render.set(
                &handle,
                match mode {
                    ProcessMode::Realtime => RenderMode::Realtime,
                    ProcessMode::Offline => RenderMode::Offline,
                },
            )?;
        }
        parts.setup = Some(ProcessingSetup {
            sample_rate,
            max_block_frames,
        });
        Ok(())
    }

    /// Activates and starts the CLAP processor outside any renderer callback.
    ///
    /// # Errors
    /// Returns a missing setup or native activation/start failure.
    pub fn activate(&mut self, _ui: &UiThread) -> Result<(), InstanceError> {
        if self.has_processor_handles() {
            return Err(InstanceError::InUse);
        }
        let mut parts = self.core.parts.borrow_mut();
        // SAFETY: The UI thread has exclusive lifecycle access and no lease exists.
        let slot = unsafe { &mut *self.core.processor.get() };
        if slot.is_some() {
            return Ok(());
        }
        let setup = parts.setup.ok_or(InstanceError::SetupBeforeProcessing)?;
        let layout = parts
            .layout
            .clone()
            .map_or_else(|| read_layout(&mut parts.plugin), Ok)?;
        let storage = ProcessorHandle::prepare_storage(&layout, setup);
        let stopped = parts.plugin.activate(
            |shared, _| AudioHandler(Arc::clone(&shared.0)),
            PluginAudioConfiguration {
                sample_rate: setup.sample_rate,
                min_frames_count: 1,
                max_frames_count: setup.max_block_frames as u32,
            },
        )?;
        let _audio = AudioScope::enter();
        match stopped.start_processing() {
            Ok(mut started) => {
                update_tail(&self.core.shared, &started.plugin_handle());
                *slot = Some(started.into());
                // SAFETY: Activation runs on the UI thread without any audio lease.
                unsafe {
                    *self.core.storage.get() = Some(storage);
                }
                Ok(())
            }
            Err(error) => {
                parts.plugin.deactivate(error.into_stopped_processor());
                Err(PluginInstanceError::StartProcessingFailed.into())
            }
        }
    }

    /// Stops and deactivates only after the renderer releases its lease.
    ///
    /// # Errors
    /// Returns `InUse` while a renderer still owns its processor handle.
    pub fn deactivate(&mut self, _ui: &UiThread) -> Result<(), InstanceError> {
        if self.has_processor_handles() {
            return Err(InstanceError::InUse);
        }
        // SAFETY: No audio lease exists, and the UI thread owns lifecycle operations.
        if let Some(processor) = unsafe { &mut *self.core.processor.get() }.take() {
            let _audio = AudioScope::enter();
            self.core.parts.borrow_mut().plugin.deactivate(processor.into_stopped());
        }
        if self.core.shared.accepted_edits.load(Ordering::Acquire)
            != self.core.shared.completed_edits.load(Ordering::Acquire)
        {
            self.core.flush(&mut self.core.parts.borrow_mut());
        }
        Ok(())
    }

    /// Returns the current latency through the main-thread extension.
    #[must_use]
    pub fn latency_samples(&self, _ui: &UiThread) -> u32 {
        let mut parts = self.core.parts.borrow_mut();
        let handle = parts.plugin.plugin_handle();
        handle
            .get_extension::<PluginLatency>()
            .map_or(0, |latency| latency.get(&handle))
    }

    /// Returns the last audio-thread tail report. `u32::MAX` represents an infinite tail.
    #[must_use]
    pub fn tail_samples(&self, _ui: &UiThread) -> u32 {
        self.core.shared.tail.load(Ordering::Acquire)
    }

    /// Captures state after all accepted host automation has been delivered.
    ///
    /// # Errors
    /// Returns `InUse` while a renderer is leased or a native state error.
    pub fn state(&self, ui: &UiThread) -> Result<Vec<u8>, InstanceError> {
        if self.has_processor_handles() {
            return Err(InstanceError::InUse);
        }
        self.flush_parameter_edits(ui)?;
        self.capture_state()
    }

    /// Captures main-thread CLAP state while processing continues, once queued edits have reached audio.
    ///
    /// # Errors
    /// Returns `EditsPending` for undelivered edits, or a native state error.
    pub fn live_state(&self, ui: &UiThread) -> Result<Vec<u8>, InstanceError> {
        if !self.has_processor_handles() {
            return self.state(ui);
        }
        if self.core.shared.accepted_edits.load(Ordering::Acquire)
            != self.core.shared.completed_edits.load(Ordering::Acquire)
        {
            return Err(InstanceError::EditsPending);
        }
        self.capture_state()
    }

    /// Restores state while inactive. Empty state keeps plugin defaults and is accepted.
    ///
    /// # Errors
    /// Returns lifecycle exclusion. A plugin rejection is reported as `Ok(false)`.
    pub fn set_state(&mut self, _ui: &UiThread, state: &[u8]) -> Result<bool, InstanceError> {
        self.ensure_inactive()?;
        if state.is_empty() {
            return Ok(true);
        }
        let mut parts = self.core.parts.borrow_mut();
        // Pending host edits precede the restored snapshot, so they must not overwrite it at the next block.
        self.core.flush(&mut parts);
        let handle = parts.plugin.plugin_handle();
        let Some(extension) = handle.get_extension::<PluginState>() else {
            return Ok(false);
        };
        Ok(extension.load(&handle, &mut &*state).is_ok())
    }

    /// Queues a plain-unit CLAP parameter value, clamped to its advertised range.
    ///
    /// # Errors
    /// Returns unknown/read-only parameter, nonfinite input, or a full bounded queue.
    pub fn set_parameter(&self, _ui: &UiThread, id: u32, value: f64) -> Result<(), InstanceError> {
        if !value.is_finite() {
            return Err(InstanceError::NonFiniteParameter { id });
        }
        let mut parts = self.core.parts.borrow_mut();
        let handle = parts.plugin.plugin_handle();
        let params = handle
            .get_extension::<PluginParams>()
            .ok_or(InstanceError::InvalidParameter { id })?;
        let mut scratch = ParamInfoBuffer::new();
        let info = (0..params.count(&handle)).find_map(|index| {
            let info = params.get_info(&handle, index, &mut scratch)?;
            (info.id.get() == id).then_some((info.min_value, info.max_value, info.flags))
        });
        let Some((minimum, maximum, flags)) = info else {
            return Err(InstanceError::InvalidParameter { id });
        };
        if !minimum.is_finite()
            || !maximum.is_finite()
            || minimum > maximum
            || flags.contains(ParamInfoFlags::IS_READONLY)
        {
            return Err(InstanceError::InvalidParameter { id });
        }
        let mut value = value.clamp(minimum, maximum);
        if flags.contains(ParamInfoFlags::IS_STEPPED) {
            value = value.trunc().clamp(minimum, maximum);
        }
        let sequence = self.core.shared.accepted_edits.load(Ordering::Relaxed).wrapping_add(1);
        self.core
            .shared
            .edits
            .push(QueuedEdit {
                event: ParamValueEvent::new(0, ClapId::new(id), Pckn::match_all(), value),
                sequence,
            })
            .map_err(|_| InstanceError::EditQueueFull)?;
        if flags.contains(ParamInfoFlags::REQUIRES_PROCESS) {
            self.core.shared.requires_process.store(true, Ordering::Release);
        }
        self.core.shared.accepted_edits.store(sequence, Ordering::Release);
        self.core.shared.dirty.store(true, Ordering::Release);
        Ok(())
    }

    /// Flushes edits through the correct inactive or active CLAP params API, outside processing.
    ///
    /// # Errors
    /// Returns `InUse` while a renderer owns the processor.
    pub fn flush_parameter_edits(&self, ui: &UiThread) -> Result<(), InstanceError> {
        self.service_idle(ui)
    }

    /// Reports pending audio work without touching native state or the renderer's leased buffers.
    #[must_use]
    pub fn needs_idle_service(&self) -> bool {
        let shared = &self.core.shared;
        shared.flush.load(Ordering::Acquire)
            || shared.process.load(Ordering::Acquire)
            || shared.requires_process.load(Ordering::Acquire)
            || shared.accepted_edits.load(Ordering::Acquire) != shared.completed_edits.load(Ordering::Acquire)
    }

    /// Services deferred callbacks and pending edits after the idle renderer's lease has retired.
    /// A required process call uses one preallocated silent block with stopped transport.
    /// Newly published requests remain pending for the next service pass.
    ///
    /// # Errors
    /// Returns `InUse` without entering native code while a renderer still owns its lease,
    /// or a native processing failure. Inactive instances defer process requests until activation.
    pub fn service_idle(&self, ui: &UiThread) -> Result<(), InstanceError> {
        if self.has_processor_handles() {
            return Err(InstanceError::InUse);
        }
        self.core.service(ui);
        let shared = &self.core.shared;
        let active = self.core.parts.borrow().plugin.is_active();
        if active && (shared.process.load(Ordering::Acquire) || shared.requires_process.load(Ordering::Acquire)) {
            let mut processor = self.processor(ui)?;
            processor.process_silence()?;
        } else if shared.flush.load(Ordering::Acquire)
            || shared.accepted_edits.load(Ordering::Acquire) != shared.completed_edits.load(Ordering::Acquire)
        {
            self.core.flush(&mut self.core.parts.borrow_mut());
        }
        if shared.accepted_edits.load(Ordering::Acquire) != shared.completed_edits.load(Ordering::Acquire) {
            return Err(InstanceError::EditsPending);
        }
        Ok(())
    }

    /// Consumes deferred restart, I/O, latency, parameter and tail requests.
    #[must_use]
    pub fn take_restart_flags(&self) -> RestartFlags {
        RestartFlags(self.core.shared.restart.swap(0, Ordering::AcqRel))
    }

    /// Consumes a plugin request to resume processing, without waking from its callback.
    #[must_use]
    pub fn take_process_requested(&self) -> bool {
        self.core.shared.process.swap(false, Ordering::AcqRel)
    }

    /// Consumes whether state or parameters changed.
    #[must_use]
    pub fn take_dirty(&self) -> bool {
        self.core.shared.dirty.swap(false, Ordering::AcqRel)
    }

    /// Leases the one prepared CLAP processor. Multiple concurrent processors are never created.
    ///
    /// # Errors
    /// Returns a missing setup/activation or `InUse`.
    pub fn processor(&self, _ui: &UiThread) -> Result<ProcessorHandle, InstanceError> {
        let parts = self.core.parts.borrow();
        if !parts.plugin.is_active() {
            return Err(InstanceError::ActivationBeforeProcessing);
        }
        self.core
            .leased
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| InstanceError::InUse)?;
        Ok(ProcessorHandle::new(Arc::clone(&self.core)))
    }

    /// Captures plugin state without conflating unsupported state with failed saving.
    fn capture_state(&self) -> Result<Vec<u8>, InstanceError> {
        let mut parts = self.core.parts.borrow_mut();
        let handle = parts.plugin.plugin_handle();
        let mut state = Vec::new();
        if let Some(extension) = handle.get_extension::<PluginState>() {
            extension.save(&handle, &mut state)?;
        }
        Ok(state)
    }

    /// Rejects reconfiguration while a processor is leased or the plugin remains active.
    fn ensure_inactive(&self) -> Result<(), InstanceError> {
        if self.has_processor_handles() {
            return Err(InstanceError::InUse);
        }
        if self.core.parts.borrow().plugin.is_active() {
            return Err(InstanceError::Active);
        }
        Ok(())
    }
}

/// Delivers requested main-thread callbacks and timers, then destroys retired instances on the UI thread.
/// Call periodically even while a plugin has no editor, including in CLI worker event loops.
pub fn service_main_thread(ui: &UiThread) {
    #[cfg(feature = "editor")]
    crate::editor::service_editors(ui);
    let instances = INSTANCES.with_borrow(|instances| instances.to_vec());
    for instance in &instances {
        instance.service(ui);
    }
    drop(instances);
    let retired = INSTANCES.with_borrow_mut(|instances| {
        let mut retired = Vec::new();
        let mut index = 0;
        while index < instances.len() {
            if instances
                .get(index)
                .is_some_and(|instance| Arc::strong_count(instance) == 1)
            {
                retired.push(instances.swap_remove(index));
            } else {
                index += 1;
            }
        }
        retired
    });
    drop(retired);
}

/// Reads bounded native audio metadata and preserves actual main-port indexes.
fn read_layout(plugin: &mut ClackInstance<Host>) -> Result<BusLayout, InstanceError> {
    let handle = plugin.plugin_handle();
    let ports = handle
        .get_extension::<PluginAudioPorts>()
        .ok_or(InstanceError::MissingOutput)?;
    let mut layout = BusLayout::default();
    let mut scratch = AudioPortInfoBuffer::new();
    let mut main_output = None;
    for input in [true, false] {
        let count = ports.count(&handle, input);
        if count > 64 {
            return Err(InstanceError::InvalidPorts);
        }
        for index in 0..count {
            let info = ports
                .get(&handle, index, input, &mut scratch)
                .ok_or(InstanceError::InvalidPorts)?;
            if info.channel_count == 0 || info.channel_count > 64 {
                return Err(InstanceError::InvalidPorts);
            }
            let main = info.flags.contains(AudioPortFlags::IS_MAIN);
            if input {
                layout.inputs.push(info.channel_count as usize);
                if main && layout.main_input.replace(index as usize).is_some() {
                    return Err(InstanceError::InvalidPorts);
                }
            } else {
                layout.outputs.push(info.channel_count as usize);
                if main && main_output.replace(index as usize).is_some() {
                    return Err(InstanceError::InvalidPorts);
                }
            }
        }
    }
    layout.main_output = main_output.ok_or(InstanceError::MissingOutput)?;
    Ok(layout)
}

/// Tests that both main ports carry a requested channel layout.
fn main_matches(layout: &BusLayout, channels: usize) -> bool {
    layout.outputs.get(layout.main_output) == Some(&channels)
        && layout
            .main_input
            .is_none_or(|index| layout.inputs.get(index) == Some(&channels))
}

/// Queries tail only through an exclusively owned audio-thread handle.
pub fn update_tail(shared: &Shared, handle: &clack_host::plugin::PluginAudioProcessorHandle<'_>) {
    let tail = handle
        .get_extension::<PluginTail>()
        .map_or(0, |extension| match extension.get(handle) {
            TailLength::Finite(length) => length,
            TailLength::Infinite => u32::MAX,
        });
    shared.tail.store(tail, Ordering::Release);
}
