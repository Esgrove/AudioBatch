//! Format dispatch for plugin lifecycle and preallocated audio processing.
//!
//! Native ABI ownership stays in the format host crates. The application layer uses these enums,
//! including in mixed-format chains, without allocating or formatting errors in the audio callback.

use vst3_host::{ProcessMode, RestartAction, UiDispatcher, UiThread};

use crate::identity::{PluginFormat, PluginId};
use crate::library::PluginDescription;

/// A lifecycle failure, retaining the native host's concrete cause.
#[derive(Debug, thiserror::Error)]
pub enum HostError {
    /// The VST3 backend failed.
    #[error(transparent)]
    Vst3(#[from] vst3_host::InstanceError),
    /// The CLAP backend failed.
    #[error(transparent)]
    Clap(#[from] clap_host::InstanceError),
    /// A VST3 module failed to load.
    #[error(transparent)]
    Vst3Module(#[from] vst3_host::ModuleError),
    /// A CLAP module failed to load.
    #[error(transparent)]
    ClapModule(#[from] clap_host::ModuleError),
    /// The persisted identifier does not belong to its declared format.
    #[error("The plugin identifier does not match its format")]
    InvalidIdentity,
}

/// Opaque saved settings with their owning format.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HostState {
    /// Separate VST3 component and controller streams.
    Vst3(vst3_host::PluginState),
    /// The CLAP state stream.
    Clap(Vec<u8>),
}

/// One main-thread-owned plugin instance.
pub enum HostInstance {
    /// A VST3 effect.
    Vst3(vst3_host::PluginInstance),
    /// A CLAP effect.
    Clap(clap_host::PluginInstance),
}

/// Preallocated audio-thread processing state for one chain stage.
#[expect(
    clippy::large_enum_variant,
    reason = "retains native handles inline in the preallocated chain without another allocation or pointer chase"
)]
pub enum HostProcessor {
    /// A VST3 processor.
    Vst3(vst3_host::ProcessorHandle),
    /// A CLAP processor.
    Clap(clap_host::ProcessorHandle),
}

/// Allocation-free native processing failure, retaining the format and result code.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
pub enum HostProcessError {
    /// VST3 processing failed.
    #[error(transparent)]
    Vst3(#[from] vst3_host::ProcessError),
    /// CLAP processing failed.
    #[error(transparent)]
    Clap(#[from] clap_host::ProcessError),
}

/// Editor failure with the native cause preserved.
#[cfg(feature = "editor")]
#[derive(Debug, thiserror::Error)]
pub enum HostEditorError {
    /// VST3 editor failure.
    #[error(transparent)]
    Vst3(#[from] vst3_host::EditorError),
    /// CLAP editor failure.
    #[error(transparent)]
    Clap(#[from] clap_host::EditorError),
}

/// Main-thread-owned native editor for either format.
#[cfg(feature = "editor")]
pub enum HostEditor {
    /// A VST3 editor window.
    Vst3(vst3_host::EditorWindow),
    /// A CLAP editor window.
    Clap(clap_host::EditorWindow),
}

impl HostError {
    /// Whether capturing state should be retried after the next audio block.
    #[must_use]
    pub const fn is_edits_pending(&self) -> bool {
        matches!(
            self,
            Self::Vst3(vst3_host::InstanceError::EditsPending) | Self::Clap(clap_host::InstanceError::EditsPending)
        )
    }
}

impl HostState {
    /// Whether the slot has no persisted settings and should start from fresh defaults.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        match self {
            Self::Vst3(state) => state.component.is_empty() && state.controller.is_empty(),
            Self::Clap(state) => state.is_empty(),
        }
    }
}

impl From<vst3_host::PluginState> for HostState {
    fn from(state: vst3_host::PluginState) -> Self {
        Self::Vst3(state)
    }
}

impl From<vst3_host::PluginInstance> for HostInstance {
    fn from(instance: vst3_host::PluginInstance) -> Self {
        Self::Vst3(instance)
    }
}

impl From<clap_host::PluginInstance> for HostInstance {
    fn from(instance: clap_host::PluginInstance) -> Self {
        Self::Clap(instance)
    }
}

impl HostInstance {
    /// Loads and instantiates the exact format and identifier recorded by a scan.
    ///
    /// # Errors
    /// Returns the typed loading or instantiation error.
    pub fn load(
        ui: &UiThread,
        description: &PluginDescription,
        dispatcher: Option<UiDispatcher>,
    ) -> Result<Self, HostError> {
        match (description.format, &description.class_id) {
            (PluginFormat::Vst3, PluginId::Vst3(id)) => {
                let module = vst3_host::Module::load(ui, &description.bundle)?;
                Ok(Self::Vst3(vst3_host::PluginInstance::create(
                    ui, &module, *id, dispatcher,
                )?))
            }
            (PluginFormat::Clap, PluginId::Clap(id)) => {
                let module = clap_host::Module::load(ui, &description.bundle)?;
                Ok(Self::Clap(clap_host::PluginInstance::create(
                    ui, &module, id, dispatcher,
                )?))
            }
            _ => Err(HostError::InvalidIdentity),
        }
    }

    /// Recreates an instance after all processor handles and editors have retired.
    ///
    /// # Errors
    /// Returns the native restoration error or a mismatched state error.
    pub fn recreate(self, ui: &UiThread, state: &HostState) -> Result<Self, HostError> {
        match (self, state) {
            (Self::Vst3(instance), HostState::Vst3(state)) => Ok(Self::Vst3(instance.recreate(ui, state)?)),
            (Self::Clap(instance), HostState::Clap(state)) => Ok(Self::Clap(instance.recreate(ui, state)?)),
            _ => Err(HostError::InvalidIdentity),
        }
    }

    /// Whether audio still holds a processor and prevents reconfiguration.
    #[must_use]
    pub fn has_processor_handles(&self) -> bool {
        match self {
            Self::Vst3(instance) => instance.has_processor_handles(),
            Self::Clap(instance) => instance.has_processor_handles(),
        }
    }

    /// Negotiates the main audio buses while inactive.
    ///
    /// # Errors
    /// Returns the native layout error.
    pub fn configure_main_buses(&mut self, ui: &UiThread, channels: usize) -> Result<(), HostError> {
        match self {
            Self::Vst3(instance) => {
                instance.configure_main_buses(ui, channels)?;
            }
            Self::Clap(instance) => {
                instance.configure_main_buses(ui, channels)?;
            }
        }
        Ok(())
    }

    /// Selects realtime or offline processing before activation.
    ///
    /// # Errors
    /// Returns the native setup error.
    pub fn setup(&mut self, ui: &UiThread, mode: ProcessMode, sample_rate: f64, block: usize) -> Result<(), HostError> {
        match self {
            Self::Vst3(instance) => Ok(instance.setup(ui, mode, sample_rate, block)?),
            Self::Clap(instance) => Ok(instance.setup(
                ui,
                match mode {
                    ProcessMode::Realtime => clap_host::ProcessMode::Realtime,
                    ProcessMode::Offline => clap_host::ProcessMode::Offline,
                },
                sample_rate,
                block,
            )?),
        }
    }

    /// Activates prepared plugin resources on the main thread.
    ///
    /// # Errors
    /// Returns the native activation error.
    pub fn activate(&mut self, ui: &UiThread) -> Result<(), HostError> {
        match self {
            Self::Vst3(instance) => Ok(instance.activate(ui)?),
            Self::Clap(instance) => Ok(instance.activate(ui)?),
        }
    }

    /// Deactivates after every processor has returned to the main thread.
    ///
    /// # Errors
    /// Returns the native lifecycle error.
    pub fn deactivate(&mut self, ui: &UiThread) -> Result<(), HostError> {
        match self {
            Self::Vst3(instance) => Ok(instance.deactivate(ui)?),
            Self::Clap(instance) => Ok(instance.deactivate(ui)?),
        }
    }

    /// Takes exclusive audio processing ownership with all buffers prepared.
    ///
    /// # Errors
    /// Returns the native processor acquisition error.
    pub fn processor(&self, ui: &UiThread) -> Result<HostProcessor, HostError> {
        match self {
            Self::Vst3(instance) => Ok(HostProcessor::Vst3(instance.processor(ui)?)),
            Self::Clap(instance) => Ok(HostProcessor::Clap(instance.processor(ui)?)),
        }
    }

    /// Captures the plugin's opaque state on the main thread.
    ///
    /// # Errors
    /// Returns the native capture error.
    pub fn state(&self, ui: &UiThread) -> Result<HostState, HostError> {
        match self {
            Self::Vst3(instance) => Ok(HostState::Vst3(instance.state(ui)?)),
            Self::Clap(instance) => Ok(HostState::Clap(instance.state(ui)?)),
        }
    }

    /// Captures state while previewing, retrying when edits are pending.
    ///
    /// # Errors
    /// Returns the native capture error.
    pub fn live_state(&self, ui: &UiThread) -> Result<HostState, HostError> {
        match self {
            Self::Vst3(instance) => Ok(HostState::Vst3(instance.live_state(ui)?)),
            Self::Clap(instance) => Ok(HostState::Clap(instance.live_state(ui)?)),
        }
    }

    /// Restores only state belonging to this plugin's format.
    ///
    /// # Errors
    /// Returns the native restoration error or a format mismatch.
    pub fn set_state(&mut self, ui: &UiThread, state: &HostState) -> Result<bool, HostError> {
        match (self, state) {
            (Self::Vst3(instance), HostState::Vst3(state)) => Ok(instance.set_state(ui, state)?),
            (Self::Clap(instance), HostState::Clap(state)) => Ok(instance.set_state(ui, state)?),
            _ => Err(HostError::InvalidIdentity),
        }
    }

    /// Sets a parameter using its native format's value domain.
    /// VST3 values are normalized, CLAP values use the parameter's plain units.
    ///
    /// # Errors
    /// Returns the native validation or delivery failure.
    pub fn set_parameter(&self, ui: &UiThread, id: u32, value: f64) -> Result<(), HostError> {
        match self {
            Self::Vst3(instance) => Ok(instance.set_parameter(ui, id, value)?),
            Self::Clap(instance) => Ok(instance.set_parameter(ui, id, value)?),
        }
    }

    /// Returns the latency reported by the current active setup.
    #[must_use]
    pub fn latency_samples(&self, ui: &UiThread) -> u32 {
        match self {
            Self::Vst3(instance) => instance.latency_samples(ui),
            Self::Clap(instance) => instance.latency_samples(ui),
        }
    }

    /// Returns the tail, with the infinite sentinel preserved for chain policy.
    #[must_use]
    pub fn tail_samples(&self, ui: &UiThread) -> u32 {
        match self {
            Self::Vst3(instance) => instance.tail_samples(ui),
            Self::Clap(instance) => instance.tail_samples(ui),
        }
    }

    /// Takes and clears the plugin's settings-dirty notification.
    #[must_use]
    pub fn take_dirty(&self) -> bool {
        match self {
            Self::Vst3(instance) => instance.take_dirty(),
            Self::Clap(instance) => instance.take_dirty(),
        }
    }

    /// Takes a deferred native restart request, preserving the stronger reload action.
    #[must_use]
    pub fn take_restart_action(&self) -> Option<RestartAction> {
        match self {
            Self::Vst3(instance) => instance.take_restart_flags().action(),
            Self::Clap(instance) => {
                let flags = instance.take_restart_flags();
                // CLAP restart preserves the instance so deferred configuration can commit during deactivation.
                if flags.needs_rebuild() {
                    Some(RestartAction::Reconfigure)
                } else {
                    None
                }
            }
        }
    }

    /// Whether a CLAP plugin needs a bounded service call while transport is idle.
    #[must_use]
    pub fn needs_idle_service(&self) -> bool {
        matches!(self, Self::Clap(instance) if instance.needs_idle_service())
    }

    /// Services CLAP requests after the playback insert has retired its processor lease.
    ///
    /// # Errors
    /// Returns the native service failure without silently discarding the request.
    pub fn service_idle(&self, ui: &UiThread) -> Result<(), HostError> {
        if let Self::Clap(instance) = self {
            instance.service_idle(ui)?;
        }
        Ok(())
    }

    /// Services deferred CLAP main-thread callbacks and timers outside the audio callback.
    pub fn service_callbacks(&self, ui: &UiThread) {
        if let Self::Clap(_) = self {
            clap_host::service_main_thread(ui);
        }
    }
}

impl HostProcessor {
    /// Largest prepared block size.
    #[must_use]
    pub fn max_block_frames(&self) -> usize {
        match self {
            Self::Vst3(processor) => processor.max_block_frames(),
            Self::Clap(processor) => processor.max_block_frames(),
        }
    }
    /// Prepared sample rate.
    #[must_use]
    pub fn sample_rate(&self) -> f64 {
        match self {
            Self::Vst3(processor) => processor.sample_rate(),
            Self::Clap(processor) => processor.sample_rate(),
        }
    }
    /// Main input channel count.
    #[must_use]
    pub fn input_channels(&self) -> usize {
        match self {
            Self::Vst3(processor) => processor.input_channels(),
            Self::Clap(processor) => processor.input_channels(),
        }
    }
    /// Main output channel count.
    #[must_use]
    pub fn output_channels(&self) -> usize {
        match self {
            Self::Vst3(processor) => processor.output_channels(),
            Self::Clap(processor) => processor.output_channels(),
        }
    }
    /// Borrows a prepared input plane.
    pub fn input_channel_mut(&mut self, channel: usize) -> Option<&mut [f32]> {
        match self {
            Self::Vst3(processor) => processor.input_channel_mut(channel),
            Self::Clap(processor) => processor.input_channel_mut(channel),
        }
    }
    /// Borrows a prepared output plane.
    #[must_use]
    pub fn output_channel(&self, channel: usize) -> Option<&[f32]> {
        match self {
            Self::Vst3(processor) => processor.output_channel(channel),
            Self::Clap(processor) => processor.output_channel(channel),
        }
    }
    /// Processes owned planes without allocation, blocking, or formatted errors.
    ///
    /// # Errors
    /// Returns the format-tagged native process failure.
    pub fn process(&mut self, frames: usize) -> Result<(), HostProcessError> {
        match self {
            Self::Vst3(processor) => Ok(processor.process(frames)?),
            Self::Clap(processor) => Ok(processor.process(frames)?),
        }
    }
    /// Processes borrowed main planes through a preallocated native adapter.
    ///
    /// # Errors
    /// Returns the format-tagged native process failure.
    pub fn process_in_place(&mut self, channels: &mut [&mut [f32]], frames: usize) -> Result<(), HostProcessError> {
        match self {
            Self::Vst3(processor) => Ok(processor.process_in_place(channels, frames)?),
            Self::Clap(processor) => Ok(processor.process_in_place(channels, frames)?),
        }
    }
    /// Clears DSP history on the audio thread and sets the source position.
    pub fn reset(&mut self, position: u64) {
        match self {
            Self::Vst3(processor) => processor.reset(position),
            Self::Clap(processor) => processor.reset(i64::try_from(position).unwrap_or(i64::MAX)),
        }
    }
    /// Updates transport position without changing DSP history.
    pub fn set_position(&mut self, position: u64) {
        match self {
            Self::Vst3(processor) => processor.set_position(position),
            Self::Clap(processor) => processor.set_position(i64::try_from(position).unwrap_or(i64::MAX)),
        }
    }
}

#[cfg(feature = "editor")]
impl HostEditor {
    /// Opens the plugin editor on the UI thread.
    ///
    /// # Errors
    /// Returns the native editor failure.
    pub fn open(
        ui: &UiThread,
        instance: &HostInstance,
        title: &str,
        owner: Option<raw_window_handle::RawWindowHandle>,
        position: Option<vst3_host::WindowPosition>,
    ) -> Result<Self, HostEditorError> {
        match instance {
            HostInstance::Vst3(instance) => Ok(Self::Vst3(vst3_host::EditorWindow::open(
                ui, instance, title, owner, position,
            )?)),
            HostInstance::Clap(instance) => Ok(Self::Clap(clap_host::EditorWindow::open(
                ui, instance, title, owner, position,
            )?)),
        }
    }
    /// Whether the user requested closing the native window.
    #[must_use]
    pub fn is_closed(&self) -> bool {
        match self {
            Self::Vst3(editor) => editor.is_closed(),
            Self::Clap(editor) => editor.is_closed(),
        }
    }
    /// Last native position, also available after hiding.
    #[must_use]
    pub fn position(&self) -> Option<vst3_host::WindowPosition> {
        match self {
            Self::Vst3(editor) => editor.position(),
            Self::Clap(editor) => editor.position(),
        }
    }
    /// Shows and raises the native window.
    pub fn show(&self, ui: &UiThread) {
        match self {
            Self::Vst3(editor) => editor.show(ui),
            Self::Clap(editor) => editor.show(ui),
        }
    }
}
