//! A plugin instance: its component, audio processor and edit controller, and their lifecycle.
//!
//! Creating an instance initializes the component, finds or creates its controller,
//! connects the two, and copies the component state into the controller.
//! Every lifecycle call takes a `UiThread` token, because VST3 allows those calls only on the UI thread.
//! Audio processing goes through `ProcessorHandle`s, which can run on any thread.
//! While a handle exists, calls that change the processing setup are refused,
//! so the setup a handle was built for cannot change under it.
//! Dropping the last reference tears the plugin down on the UI thread,
//! by posting the teardown there when the drop happens elsewhere.

#![expect(
    unsafe_code,
    reason = "calls the COM interfaces of plugin components and controllers"
)]

use std::mem::ManuallyDrop;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

use crossbeam_queue::ArrayQueue;
use thiserror::Error;
use vst3::Steinberg::Vst::{
    BusDirections_, IAudioProcessor, IAudioProcessorTrait as _, IComponent, IComponentHandler, IComponentTrait as _,
    IConnectionPoint, IConnectionPointTrait as _, IEditController, IEditControllerTrait as _, IParameterChanges,
    MediaTypes_, ProcessData, ProcessModes_, ProcessSetup, SpeakerArr, SpeakerArrangement, SymbolicSampleSizes_,
};
use vst3::Steinberg::{FUnknown, IBStream, IPluginBaseTrait as _, TUID, kResultOk, kResultTrue};
use vst3::{ComPtr, ComWrapper};

use crate::enums::to_i32;
use crate::factory::ClassId;
use crate::host_objects::{
    ComponentHandler, EDIT_QUEUE_CAPACITY, HostApplication, MemoryStream, ParameterChanges, ParameterEdit, RestartFlags,
};
use crate::module::{FactoryError, Module};
use crate::processor::ProcessorHandle;
use crate::thread::{UiDispatcher, UiThread};

/// Why an instance could not be created, configured, or controlled.
#[derive(Debug, Clone, Copy, PartialEq, Error)]
pub enum InstanceError {
    /// The module's factory could not create the component.
    #[error(transparent)]
    Factory(#[from] FactoryError),
    /// The component refused initialization.
    #[error("The plugin could not be initialized")]
    Initialize {
        /// The result returned by `initialize`.
        result: i32,
    },
    /// The component does not implement the audio processor interface.
    #[error("The plugin has no audio processor")]
    MissingProcessor,
    /// The plugin has no output bus.
    #[error("The plugin has no audio output")]
    MissingOutput,
    /// No supported arrangement can carry the signal.
    #[error("The plugin does not support {channels} channel audio")]
    UnsupportedChannels {
        /// The requested number of channels.
        channels: usize,
        /// The last arrangement result, when an arrangement was tried.
        result: Option<i32>,
    },
    /// The processor does not support the host's sample format.
    #[error("The plugin cannot process 32-bit samples")]
    UnsupportedSampleSize {
        /// The result returned by `canProcessSampleSize`.
        result: i32,
    },
    /// The processor refused the processing setup.
    #[error("The plugin does not support processing at {sample_rate} Hz")]
    Setup {
        /// The requested sample rate.
        sample_rate: f64,
        /// The result returned by `setupProcessing`.
        result: i32,
    },
    /// Activation requires a processing setup.
    #[error("The plugin must be set up before it is activated")]
    SetupBeforeActivation,
    /// The component refused activation.
    #[error("The plugin could not be activated")]
    Activate {
        /// The result returned by `setActive`.
        result: i32,
    },
    /// A parameter cannot be changed without a controller.
    #[error("The plugin has no edit controller")]
    MissingController,
    /// A parameter value is not finite.
    #[error("The value {normalized} for parameter {id} is not a finite number")]
    NonFiniteParameter {
        /// The parameter being changed.
        id: u32,
        /// The unmodified requested value.
        normalized: f64,
    },
    /// The controller refused a parameter change.
    #[error("The plugin refused the value {value} for parameter {id}")]
    SetParameter {
        /// The parameter being changed.
        id: u32,
        /// The clamped value passed to the controller.
        value: f64,
        /// The result returned by `setParamNormalized`.
        result: i32,
    },
    /// Processing requires a setup.
    #[error("The plugin must be set up before it can process audio")]
    SetupBeforeProcessing,
    /// Processing requires an active component.
    #[error("The plugin must be active before it can process audio")]
    ActivationBeforeProcessing,
    /// An active component cannot be reconfigured.
    #[error("The plugin must be inactive to change its setup")]
    Active,
    /// Live processor handles prevent reconfiguration.
    #[error("The plugin is in use by an audio processor")]
    InUse,
}

/// How a plugin is told it will be run.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProcessMode {
    /// Live playback, where each block must finish in time.
    Realtime,
    /// Rendering to a file, where a plugin may take as long as it needs and use its best quality.
    Offline,
}

/// The audio buses of a plugin and their channel counts. Bus 0 in each direction is the main bus.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BusLayout {
    /// Channel count of each input bus.
    pub inputs: Vec<usize>,
    /// Channel count of each output bus.
    pub outputs: Vec<usize>,
}

/// The saved state of a plugin: the component's blob and the controller's blob.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PluginState {
    /// State of the audio component.
    pub component: Vec<u8>,
    /// State of the edit controller, such as editor settings.
    pub controller: Vec<u8>,
}

/// The processing setup a processor handle is built for.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct ProcessingSetup {
    /// Realtime or offline.
    pub(crate) mode: ProcessMode,
    /// Sample rate in hertz.
    pub(crate) sample_rate: f64,
    /// Largest block passed to `process`.
    pub(crate) max_block_frames: usize,
}

/// The parts of a plugin instance, shared by the instance and its processor handles.
pub(crate) struct InstanceCore {
    /// The COM objects, taken out for teardown when the core is dropped.
    parts: ManuallyDrop<InstanceParts>,
    /// Queue of parameter edits from the controller, drained into `process`.
    edits: Arc<ArrayQueue<ParameterEdit>>,
    /// Runs the teardown on the UI thread when the core is dropped elsewhere.
    dispatcher: Option<UiDispatcher>,
    /// Whether `setActive(true)` is in effect.
    active: AtomicBool,
    /// Number of processor handles alive.
    live_processors: AtomicUsize,
    /// Whether a processor handle is inside a processing call.
    processing: AtomicBool,
}

/// The COM objects of an instance.
struct InstanceParts {
    /// The audio component.
    component: ComPtr<IComponent>,
    /// The audio processor of the component.
    processor: ComPtr<IAudioProcessor>,
    /// The edit controller, which is the component itself for single-component plugins.
    controller: Option<ComPtr<IEditController>>,
    /// Whether the controller is a separate object that this host initialized.
    separate_controller: bool,
    /// The connection points of the component and the controller, when they were connected.
    connection: Option<(ComPtr<IConnectionPoint>, ComPtr<IConnectionPoint>)>,
    /// The component handler given to the controller.
    handler: ComWrapper<ComponentHandler>,
    /// The host context the component and controller were initialized with.
    _host: ComWrapper<HostApplication>,
    /// Keeps the module of the plugin loaded.
    _module: Arc<Module>,
}

/// A plugin instance.
pub struct PluginInstance {
    /// The parts shared with processor handles.
    core: Arc<InstanceCore>,
    /// The class this instance was created from.
    class_id: ClassId,
    /// The bus layout after `configure_main_buses`.
    layout: Option<BusLayout>,
    /// The setup after `setup`.
    setup: Option<ProcessingSetup>,
}

impl InstanceCore {
    /// Returns the audio processor.
    pub(crate) fn processor(&self) -> &ComPtr<IAudioProcessor> {
        &self.parts.processor
    }

    /// Returns the queue of parameter edits.
    pub(crate) const fn edits(&self) -> &Arc<ArrayQueue<ParameterEdit>> {
        &self.edits
    }

    /// Returns the edit controller, if the plugin has one.
    #[cfg_attr(
        not(any(feature = "editor", test)),
        expect(dead_code, reason = "only editor windows and tests reach the controller")
    )]
    pub(crate) fn controller(&self) -> Option<&ComPtr<IEditController>> {
        self.parts.controller.as_ref()
    }

    /// Records that a processor handle was created.
    pub(crate) fn processor_created(&self) {
        self.live_processors.fetch_add(1, Ordering::AcqRel);
    }

    /// Records that a processor handle was dropped.
    pub(crate) fn processor_dropped(&self) {
        self.live_processors.fetch_sub(1, Ordering::AcqRel);
    }

    /// Marks the instance as inside a processing call, and returns false when another handle already is.
    /// Lock-free, so it may run in an audio callback.
    pub(crate) fn try_begin_processing(&self) -> bool {
        self.processing
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
    }

    /// Clears the mark of `try_begin_processing`.
    pub(crate) fn end_processing(&self) {
        self.processing.store(false, Ordering::Release);
    }
}

impl Drop for InstanceCore {
    fn drop(&mut self) {
        // SAFETY: `parts` is taken exactly once, here, and never used afterwards.
        let parts = unsafe { ManuallyDrop::take(&mut self.parts) };
        let active = *self.active.get_mut();
        if let Some(ui) = UiThread::current() {
            parts.teardown(&ui, active);
            return;
        }
        // The parts stay in ManuallyDrop until they reach the UI thread.
        // Without a dispatcher, or when the UI thread has stopped and drops the job without running it,
        // they are leaked, since releasing them here would run plugin destructors on the wrong thread.
        let parts = ManuallyDrop::new(parts);
        if let Some(dispatcher) = self.dispatcher.take() {
            dispatcher.post(move |ui| ManuallyDrop::into_inner(parts).teardown(ui, active));
        }
    }
}

impl InstanceParts {
    /// Deactivates and terminates the plugin, in the reverse order of its creation.
    fn teardown(self, _ui: &UiThread, active: bool) {
        // SAFETY: every pointer is valid, and the calls follow the order the VST3 lifecycle requires.
        unsafe {
            if active {
                self.processor.setProcessing(0);
                self.component.setActive(0);
            }
            if let Some((component_point, controller_point)) = &self.connection {
                component_point.disconnect(controller_point.as_ptr());
                controller_point.disconnect(component_point.as_ptr());
            }
            if let Some(controller) = &self.controller {
                controller.setComponentHandler(std::ptr::null_mut());
                if self.separate_controller {
                    controller.terminate();
                }
            }
            self.component.terminate();
        }
    }
}

impl PluginInstance {
    /// Creates and initializes an instance of a class of the module.
    ///
    /// The dispatcher, when given, runs the teardown on the UI thread if the instance is dropped on another thread.
    ///
    /// # Errors
    ///
    /// Returns an error when the class cannot be created or initialized, or has no audio processor.
    pub fn create(
        ui: &UiThread,
        module: &Arc<Module>,
        class_id: ClassId,
        dispatcher: Option<UiDispatcher>,
    ) -> Result<Self, InstanceError> {
        let component = module.create::<IComponent>(ui, class_id)?;
        let host = HostApplication::create();
        let context = host.to_com_ptr::<FUnknown>();
        let context_pointer = context.as_ref().map_or(std::ptr::null_mut(), ComPtr::as_ptr);
        // SAFETY: the component is a fresh object and the context pointer is valid for the call.
        let result = unsafe { component.initialize(context_pointer) };
        if result != kResultOk {
            return Err(InstanceError::Initialize { result });
        }
        let Some(processor) = component.cast::<IAudioProcessor>() else {
            // SAFETY: the component was initialized above.
            unsafe { component.terminate() };
            return Err(InstanceError::MissingProcessor);
        };
        let (controller, separate_controller) = component.cast::<IEditController>().map_or_else(
            || (create_controller(ui, module, &component, context_pointer), true),
            |controller| (Some(controller), false),
        );
        let connection = if separate_controller {
            controller
                .as_ref()
                .and_then(|controller| connect(&component, controller))
        } else {
            None
        };
        let edits = Arc::new(ArrayQueue::new(EDIT_QUEUE_CAPACITY));
        let handler = ComWrapper::new(ComponentHandler::new(Arc::clone(&edits)));
        if let (Some(controller), Some(handler)) = (&controller, handler.to_com_ptr::<IComponentHandler>()) {
            // SAFETY: both objects are valid, and the controller takes its own reference to the handler.
            unsafe { controller.setComponentHandler(handler.as_ptr()) };
        }
        let parts = InstanceParts {
            component,
            processor,
            controller,
            separate_controller,
            connection,
            handler,
            _host: host,
            _module: Arc::clone(module),
        };
        let instance = Self {
            core: Arc::new(InstanceCore {
                parts: ManuallyDrop::new(parts),
                edits,
                dispatcher,
                active: AtomicBool::new(false),
                live_processors: AtomicUsize::new(0),
                processing: AtomicBool::new(false),
            }),
            class_id,
            layout: None,
            setup: None,
        };
        if separate_controller {
            let component_state = instance.component_state(ui);
            instance.load_controller_component_state(ui, &component_state);
        }
        Ok(instance)
    }

    /// Returns the class this instance was created from.
    #[must_use]
    pub const fn class_id(&self) -> ClassId {
        self.class_id
    }

    /// Returns true while the plugin is active.
    #[must_use]
    pub(crate) fn is_active(&self) -> bool {
        self.core.active.load(Ordering::Acquire)
    }

    /// Reads the current channel count of every audio bus.
    #[must_use]
    pub fn current_bus_layout(&self, ui: &UiThread) -> BusLayout {
        BusLayout {
            inputs: self
                .arrangements(ui, BusDirection::Input)
                .iter()
                .map(|arrangement| channel_count(*arrangement))
                .collect(),
            outputs: self
                .arrangements(ui, BusDirection::Output)
                .iter()
                .map(|arrangement| channel_count(*arrangement))
                .collect(),
        }
    }

    /// Sets the main input and output buses for a signal of `channels` channels and returns the resulting layout.
    ///
    /// The arrangements tried are the one for the signal's channel count, then stereo for a mono signal,
    /// and finally the plugin's own default if it has at least as many channels as the signal.
    /// Auxiliary buses keep their default arrangement.
    /// Auxiliary audio buses and event buses are deactivated, and ones a plugin refuses to deactivate get silence.
    ///
    /// # Errors
    ///
    /// Returns an error when the plugin is active, has no audio output, or accepts none of the arrangements.
    pub fn configure_main_buses(&mut self, ui: &UiThread, channels: usize) -> Result<BusLayout, InstanceError> {
        self.ensure_reconfigurable()?;
        let mut inputs = self.arrangements(ui, BusDirection::Input);
        let mut outputs = self.arrangements(ui, BusDirection::Output);
        if outputs.is_empty() {
            return Err(InstanceError::MissingOutput);
        }
        let default_input = inputs.first().copied();
        let default_output = outputs.first().copied().unwrap_or_default();
        let mut candidates = Vec::new();
        if let Some(arrangement) = arrangement_for(channels) {
            candidates.push(arrangement);
        }
        if channels == 1 {
            candidates.push(SpeakerArr::kStereo);
        }
        let mut accepted = false;
        let mut last_result = None;
        for candidate in candidates {
            if let Some(main) = inputs.first_mut() {
                *main = candidate;
            }
            if let Some(main) = outputs.first_mut() {
                *main = candidate;
            }
            // SAFETY: both arrays are valid for their lengths during the call.
            let result = unsafe {
                self.core.parts.processor.setBusArrangements(
                    inputs.as_mut_ptr(),
                    bus_count(inputs.len()),
                    outputs.as_mut_ptr(),
                    bus_count(outputs.len()),
                )
            };
            last_result = Some(result);
            if result == kResultTrue && self.main_arrangements_are(ui, candidate) {
                accepted = true;
                break;
            }
        }
        if !accepted {
            let input_channels = default_input.map_or(channels, channel_count);
            if input_channels < channels || channel_count(default_output) < channels {
                return Err(InstanceError::UnsupportedChannels {
                    channels,
                    result: last_result,
                });
            }
            if let Some(main) = inputs.first_mut() {
                *main = default_input.unwrap_or_default();
            }
            if let Some(main) = outputs.first_mut() {
                *main = default_output;
            }
            // SAFETY: both arrays are valid for their lengths during the call.
            unsafe {
                self.core.parts.processor.setBusArrangements(
                    inputs.as_mut_ptr(),
                    bus_count(inputs.len()),
                    outputs.as_mut_ptr(),
                    bus_count(outputs.len()),
                );
            }
        }
        self.activate_buses(ui);
        let layout = self.current_bus_layout(ui);
        self.layout = Some(layout.clone());
        Ok(layout)
    }

    /// Tells the plugin how it will be run.
    ///
    /// # Errors
    ///
    /// Returns an error when the plugin is active, cannot process 32-bit samples, or refuses the setup.
    pub fn setup(
        &mut self,
        _ui: &UiThread,
        mode: ProcessMode,
        sample_rate: f64,
        max_block_frames: usize,
    ) -> Result<(), InstanceError> {
        self.ensure_reconfigurable()?;
        let processor = &self.core.parts.processor;
        // SAFETY: the processor is valid.
        let result = unsafe { processor.canProcessSampleSize(to_i32(SymbolicSampleSizes_::kSample32)) };
        if result != kResultTrue {
            return Err(InstanceError::UnsupportedSampleSize { result });
        }
        let mut setup = ProcessSetup {
            processMode: process_mode_value(mode),
            symbolicSampleSize: to_i32(SymbolicSampleSizes_::kSample32),
            maxSamplesPerBlock: i32::try_from(max_block_frames).unwrap_or(i32::MAX),
            sampleRate: sample_rate,
        };
        // SAFETY: the processor is valid and the setup lives for the call.
        let result = unsafe { processor.setupProcessing(&raw mut setup) };
        if result != kResultOk {
            return Err(InstanceError::Setup { sample_rate, result });
        }
        self.setup = Some(ProcessingSetup {
            mode,
            sample_rate,
            max_block_frames,
        });
        Ok(())
    }

    /// Activates the plugin and starts processing.
    ///
    /// # Errors
    ///
    /// Returns an error when the plugin has not been set up or refuses to activate.
    pub fn activate(&mut self, _ui: &UiThread) -> Result<(), InstanceError> {
        if self.is_active() {
            return Ok(());
        }
        if self.setup.is_none() {
            return Err(InstanceError::SetupBeforeActivation);
        }
        let parts = &self.core.parts;
        // SAFETY: the component is valid and set up.
        let result = unsafe { parts.component.setActive(1) };
        if result != kResultOk {
            return Err(InstanceError::Activate { result });
        }
        // SAFETY: the processor is valid and its component is active.
        unsafe { parts.processor.setProcessing(1) };
        self.core.active.store(true, Ordering::Release);
        Ok(())
    }

    /// Stops processing and deactivates the plugin.
    ///
    /// # Errors
    ///
    /// Returns an error while processor handles exist.
    pub fn deactivate(&mut self, _ui: &UiThread) -> Result<(), InstanceError> {
        if !self.is_active() {
            return Ok(());
        }
        self.ensure_no_processors()?;
        let parts = &self.core.parts;
        // SAFETY: both objects are valid, and processing stops before the component is deactivated.
        unsafe {
            parts.processor.setProcessing(0);
            parts.component.setActive(0);
        }
        self.core.active.store(false, Ordering::Release);
        Ok(())
    }

    /// Returns the latency the plugin reports, in samples.
    #[must_use]
    pub fn latency_samples(&self, _ui: &UiThread) -> u32 {
        // SAFETY: the processor is valid.
        unsafe { self.core.parts.processor.getLatencySamples() }
    }

    /// Returns the tail length the plugin reports, in samples.
    /// `u32::MAX` means an infinite tail, which callers limit to a length of their own.
    #[must_use]
    pub fn tail_samples(&self, _ui: &UiThread) -> u32 {
        // SAFETY: the processor is valid.
        unsafe { self.core.parts.processor.getTailSamples() }
    }

    /// Captures the component and controller state.
    ///
    /// A plugin that reports a failure, such as a stateless plugin that does not implement saving,
    /// gives whatever it wrote, often nothing, the way JUCE hosts treat it, so it can still be saved in a chain.
    ///
    /// # Errors
    ///
    /// Never fails at the moment, and keeps the `Result` so callers handle a future failure.
    pub fn state(&self, ui: &UiThread) -> Result<PluginState, InstanceError> {
        let component = self.component_state(ui);
        let controller = self.core.parts.controller.as_ref().map_or_else(Vec::new, |controller| {
            let stream = MemoryStream::create();
            let pointer = stream
                .to_com_ptr::<IBStream>()
                .map_or(std::ptr::null_mut(), |stream| stream.as_ptr());
            // SAFETY: the controller and the stream are valid for the call.
            if unsafe { controller.getState(pointer) } == kResultOk {
                stream.data()
            } else {
                Vec::new()
            }
        });
        Ok(PluginState { component, controller })
    }

    /// Restores a captured state into the component and the controller,
    /// and returns false when the component refused its state.
    /// Empty blobs are skipped, so an empty state keeps the plugin's defaults.
    /// A plugin that refuses a state keeps its current settings and stays usable, the way JUCE hosts treat it,
    /// since some plugins report a failure for a state they do not need, such as an empty one.
    ///
    /// # Errors
    ///
    /// Returns an error when the plugin is active.
    pub fn set_state(&mut self, ui: &UiThread, state: &PluginState) -> Result<bool, InstanceError> {
        self.ensure_reconfigurable()?;
        let mut accepted = true;
        if !state.component.is_empty() {
            let stream = MemoryStream::with_data(state.component.clone());
            let pointer = stream
                .to_com_ptr::<IBStream>()
                .map_or(std::ptr::null_mut(), |stream| stream.as_ptr());
            // SAFETY: the component and the stream are valid for the call.
            accepted = unsafe { self.core.parts.component.setState(pointer) } == kResultOk;
            if self.core.parts.separate_controller {
                self.load_controller_component_state(ui, &state.component);
            }
        }
        if let (Some(controller), false) = (&self.core.parts.controller, state.controller.is_empty()) {
            let stream = MemoryStream::with_data(state.controller.clone());
            let pointer = stream
                .to_com_ptr::<IBStream>()
                .map_or(std::ptr::null_mut(), |stream| stream.as_ptr());
            // SAFETY: the controller and the stream are valid for the call.
            unsafe { controller.setState(pointer) };
        }
        Ok(accepted)
    }

    /// Sets a parameter to a normalized value between 0 and 1, the way a host automates it.
    ///
    /// The controller takes the value at once.
    /// The processor gets it with the next block of a processor handle,
    /// or with `flush_parameter_edits` while the plugin is active and no handle exists.
    /// Values outside 0 to 1 are clamped.
    ///
    /// # Errors
    ///
    /// Returns an error when the value is not a finite number,
    /// or when the plugin has no edit controller or the controller refuses the value.
    pub fn set_parameter(&self, _ui: &UiThread, id: u32, normalized: f64) -> Result<(), InstanceError> {
        let Some(controller) = &self.core.parts.controller else {
            return Err(InstanceError::MissingController);
        };
        // Clamping keeps NaN as it is, so a value that is not a number would reach the plugin.
        if !normalized.is_finite() {
            return Err(InstanceError::NonFiniteParameter { id, normalized });
        }
        let value = normalized.clamp(0.0, 1.0);
        // SAFETY: the controller is valid for the call.
        let result = unsafe { controller.setParamNormalized(id, value) };
        if result != kResultOk {
            return Err(InstanceError::SetParameter { id, value, result });
        }
        self.core.edits.force_push(ParameterEdit { id, value });
        Ok(())
    }

    /// Delivers queued parameter edits to the processor with a zero-length `process` call,
    /// so that a following `state` includes them.
    ///
    /// Does nothing while the plugin is inactive, or while a processor handle exists,
    /// because the handle delivers the edits with its next block.
    pub fn flush_parameter_edits(&self, _ui: &UiThread) {
        if !self.is_active() || self.core.live_processors.load(Ordering::Acquire) > 0 || self.core.edits.is_empty() {
            return;
        }
        if !self.core.try_begin_processing() {
            return;
        }
        let changes = ParameterChanges::create();
        // Edits past the capacity of one call wait for the next flush or block instead of being lost.
        // The controller only edits on this thread, so they go back in their order once the call is done.
        let mut carried = Vec::new();
        while let Some(edit) = self.core.edits.pop() {
            if !changes.set(edit.id, edit.value) {
                carried.push(edit);
            }
        }
        let changes_pointer = changes
            .as_com_ref::<IParameterChanges>()
            .map_or(std::ptr::null_mut(), |changes| changes.as_ptr());
        let mut data = ProcessData {
            processMode: self.setup.map_or_else(
                || to_i32(ProcessModes_::kRealtime),
                |setup| process_mode_value(setup.mode),
            ),
            symbolicSampleSize: to_i32(SymbolicSampleSizes_::kSample32),
            numSamples: 0,
            numInputs: 0,
            numOutputs: 0,
            inputs: std::ptr::null_mut(),
            outputs: std::ptr::null_mut(),
            inputParameterChanges: changes_pointer,
            outputParameterChanges: std::ptr::null_mut(),
            inputEvents: std::ptr::null_mut(),
            outputEvents: std::ptr::null_mut(),
            processContext: std::ptr::null_mut(),
        };
        // SAFETY: the processor is active, the processing mark keeps any other call out,
        // and `data` and the parameter changes live for the call.
        unsafe { self.core.parts.processor.process(&raw mut data) };
        self.core.end_processing();
        for edit in carried {
            let _ = self.core.edits.push(edit);
        }
    }

    /// Returns and clears the restart requests the plugin made.
    #[must_use]
    pub fn take_restart_flags(&self) -> RestartFlags {
        self.core.parts.handler.take_restart_flags()
    }

    /// Returns and clears whether the plugin reported a change to its parameters or state,
    /// through a parameter edit, a dirty flag, or a restart request for changed parameter values.
    #[must_use]
    pub fn take_dirty(&self) -> bool {
        self.core.parts.handler.take_dirty()
    }

    /// Creates a handle that runs the plugin's audio processing on any one thread.
    ///
    /// # Errors
    ///
    /// Returns an error when the plugin is not active.
    pub fn processor(&self, ui: &UiThread) -> Result<ProcessorHandle, InstanceError> {
        let Some(setup) = self.setup else {
            return Err(InstanceError::SetupBeforeProcessing);
        };
        if !self.is_active() {
            return Err(InstanceError::ActivationBeforeProcessing);
        }
        let layout = self.layout.clone().unwrap_or_else(|| self.current_bus_layout(ui));
        Ok(ProcessorHandle::new(Arc::clone(&self.core), &layout, setup))
    }

    /// Returns the parts shared with processor handles and editor windows.
    #[cfg_attr(
        not(any(feature = "editor", test)),
        expect(dead_code, reason = "only editor windows and tests reach the controller")
    )]
    pub(crate) const fn core(&self) -> &Arc<InstanceCore> {
        &self.core
    }

    /// Returns an error while the plugin is active or processor handles exist.
    fn ensure_reconfigurable(&self) -> Result<(), InstanceError> {
        self.ensure_no_processors()?;
        if self.is_active() {
            return Err(InstanceError::Active);
        }
        Ok(())
    }

    /// Returns an error while processor handles exist.
    fn ensure_no_processors(&self) -> Result<(), InstanceError> {
        if self.core.live_processors.load(Ordering::Acquire) > 0 {
            return Err(InstanceError::InUse);
        }
        Ok(())
    }

    /// Captures the component state, which is whatever the plugin wrote, even when it reported a failure.
    fn component_state(&self, _ui: &UiThread) -> Vec<u8> {
        let stream = MemoryStream::create();
        let pointer = stream
            .to_com_ptr::<IBStream>()
            .map_or(std::ptr::null_mut(), |stream| stream.as_ptr());
        // SAFETY: the component and the stream are valid for the call.
        unsafe { self.core.parts.component.getState(pointer) };
        stream.data()
    }

    /// Gives a separate controller the component state, so its parameters match the processor.
    fn load_controller_component_state(&self, _ui: &UiThread, component_state: &[u8]) {
        if let Some(controller) = &self.core.parts.controller {
            let stream = MemoryStream::with_data(component_state.to_vec());
            let pointer = stream
                .to_com_ptr::<IBStream>()
                .map_or(std::ptr::null_mut(), |stream| stream.as_ptr());
            // SAFETY: the controller and the stream are valid for the call.
            unsafe { controller.setComponentState(pointer) };
        }
    }

    /// Reads the speaker arrangement of every audio bus in one direction.
    fn arrangements(&self, _ui: &UiThread, direction: BusDirection) -> Vec<SpeakerArrangement> {
        let parts = &self.core.parts;
        // SAFETY: the component is valid.
        let count = unsafe {
            parts
                .component
                .getBusCount(to_i32(MediaTypes_::kAudio), direction.value())
        };
        (0..count.max(0))
            .map(|index| {
                let mut arrangement: SpeakerArrangement = 0;
                // SAFETY: the processor is valid and the arrangement slot lives for the call.
                let result = unsafe {
                    parts
                        .processor
                        .getBusArrangement(direction.value(), index, &raw mut arrangement)
                };
                if result == kResultOk { arrangement } else { 0 }
            })
            .collect()
    }

    /// Returns true when both main buses now have the arrangement.
    fn main_arrangements_are(&self, ui: &UiThread, arrangement: SpeakerArrangement) -> bool {
        let inputs = self.arrangements(ui, BusDirection::Input);
        let outputs = self.arrangements(ui, BusDirection::Output);
        inputs.first().is_none_or(|main| *main == arrangement) && outputs.first() == Some(&arrangement)
    }

    /// Activates the main audio buses and deactivates the auxiliary audio buses and all event buses.
    fn activate_buses(&self, _ui: &UiThread) {
        let component = &self.core.parts.component;
        for direction in [BusDirection::Input, BusDirection::Output] {
            for (media, keep_main) in [(MediaTypes_::kAudio, true), (MediaTypes_::kEvent, false)] {
                let media = to_i32(media);
                // SAFETY: the component is valid.
                let count = unsafe { component.getBusCount(media, direction.value()) };
                for index in 0..count.max(0) {
                    let state = u8::from(keep_main && index == 0);
                    // SAFETY: the component is valid and the bus index is in range.
                    unsafe { component.activateBus(media, direction.value(), index, state) };
                }
            }
        }
    }
}

impl std::fmt::Debug for PluginInstance {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("PluginInstance")
            .field("class_id", &self.class_id)
            .field("active", &self.is_active())
            .field("layout", &self.layout)
            .finish_non_exhaustive()
    }
}

/// The direction of a bus.
#[derive(Debug, Clone, Copy)]
enum BusDirection {
    /// Audio into the plugin.
    Input,
    /// Audio out of the plugin.
    Output,
}

impl BusDirection {
    /// Returns the value the VST3 interfaces take.
    fn value(self) -> i32 {
        match self {
            Self::Input => to_i32(BusDirections_::kInput),
            Self::Output => to_i32(BusDirections_::kOutput),
        }
    }
}

/// Returns the number of channels in a speaker arrangement.
pub(crate) const fn channel_count(arrangement: SpeakerArrangement) -> usize {
    arrangement.count_ones() as usize
}

/// Returns a bus count as the integer type `setBusArrangements` takes.
fn bus_count(count: usize) -> i32 {
    i32::try_from(count).unwrap_or(i32::MAX)
}

/// Returns the speaker arrangement for a mono or stereo signal.
const fn arrangement_for(channels: usize) -> Option<SpeakerArrangement> {
    match channels {
        1 => Some(SpeakerArr::kMono),
        2 => Some(SpeakerArr::kStereo),
        _ => None,
    }
}

/// Returns the VST3 value of a process mode.
pub(crate) fn process_mode_value(mode: ProcessMode) -> i32 {
    match mode {
        ProcessMode::Realtime => to_i32(ProcessModes_::kRealtime),
        ProcessMode::Offline => to_i32(ProcessModes_::kOffline),
    }
}

/// Creates and initializes the separate controller a component names.
fn create_controller(
    ui: &UiThread,
    module: &Module,
    component: &ComPtr<IComponent>,
    context: *mut FUnknown,
) -> Option<ComPtr<IEditController>> {
    let mut controller_id: TUID = [0; 16];
    // SAFETY: the component is valid and the id slot lives for the call.
    if unsafe { component.getControllerClassId(&raw mut controller_id) } != kResultOk {
        return None;
    }
    let controller = module
        .create::<IEditController>(ui, ClassId::from_tuid(&controller_id))
        .ok()?;
    // SAFETY: the controller is a fresh object and the context pointer is valid for the call.
    (unsafe { controller.initialize(context) } == kResultOk).then_some(controller)
}

/// Connects the connection points of a component and its separate controller.
fn connect(
    component: &ComPtr<IComponent>,
    controller: &ComPtr<IEditController>,
) -> Option<(ComPtr<IConnectionPoint>, ComPtr<IConnectionPoint>)> {
    let component_point = component.cast::<IConnectionPoint>()?;
    let controller_point = controller.cast::<IConnectionPoint>()?;
    // SAFETY: both connection points are valid, and each takes its own reference to the other.
    unsafe {
        component_point.connect(controller_point.as_ptr());
        controller_point.connect(component_point.as_ptr());
    }
    Some((component_point, controller_point))
}

#[cfg(test)]
mod test_lifecycle {
    use audiobatch_test_plugin::{Kind, take_recorded_calls};

    use super::*;
    use crate::test_helpers::{
        MAX_BLOCK, active_instance, class_id, dispatcher, lock_recorder_log, on_ui, test_module,
    };

    #[test]
    fn lifecycle_calls_run_on_the_ui_thread_in_order() {
        let _log = lock_recorder_log();
        let (instance, ui_thread) = on_ui(|ui| {
            drop(take_recorded_calls());
            (active_instance(ui, Kind::Recorder, 2), std::thread::current().id())
        });
        let mut instance = instance.expect("recorder instance");
        let mut processor = on_ui_processor(&instance);
        processor.process(MAX_BLOCK).expect("process succeeds");
        drop(processor);
        let deactivated = on_ui(move |ui| {
            let result = instance.deactivate(ui).map_err(|error| error.to_string());
            drop(instance);
            result
        });
        assert_eq!(deactivated, Ok(()));
        let calls = take_recorded_calls();
        let names: Vec<&str> = calls.iter().map(|call| call.name).collect();
        assert_eq!(
            names,
            [
                "component.initialize",
                "controller.initialize",
                "component.connect",
                "controller.notify",
                "controller.connect",
                "controller.setComponentHandler",
                "component.getState",
                "controller.setComponentState",
                "processor.setBusArrangements",
                "processor.setupProcessing",
                "component.setActive(true)",
                "processor.setProcessing(true)",
                "processor.process",
                "processor.setProcessing(false)",
                "component.setActive(false)",
                "component.disconnect",
                "controller.disconnect",
                "controller.setComponentHandler",
                "controller.terminate",
                "component.terminate",
            ]
        );
        let test_thread = std::thread::current().id();
        for call in calls {
            let expected = if call.name == "processor.process" {
                test_thread
            } else {
                ui_thread
            };
            assert_eq!(call.thread, expected, "{} ran on the wrong thread", call.name);
        }
    }

    #[test]
    fn setup_changes_are_refused_while_a_processor_exists() {
        let mut instance = on_ui(|ui| active_instance(ui, Kind::Gain, 2)).expect("gain instance");
        let processor = on_ui_processor(&instance);
        let (instance_back, refused) = on_ui(move |ui| {
            let deactivate = instance.deactivate(ui).map_err(|error| error.to_string());
            let set_state = instance
                .set_state(ui, &PluginState::default())
                .map(|_| ())
                .map_err(|error| error.to_string());
            (instance, [deactivate, set_state])
        });
        instance = instance_back;
        for result in refused {
            assert_eq!(result, Err("The plugin is in use by an audio processor".to_owned()));
        }
        drop(processor);
        let deactivated = on_ui(move |ui| instance.deactivate(ui).map_err(|error| error.to_string()));
        assert_eq!(deactivated, Ok(()));
    }

    #[test]
    fn processing_needs_an_active_plugin() {
        let messages = on_ui(|ui| {
            let module = test_module(ui);
            let mut instance =
                PluginInstance::create(ui, &module, class_id(Kind::Gain), None).map_err(|error| error.to_string())?;
            let before_setup = instance.processor(ui).map(drop).map_err(|error| error.to_string());
            let activate_before_setup = instance.activate(ui).map_err(|error| error.to_string());
            instance
                .setup(ui, ProcessMode::Realtime, 44_100.0, 64)
                .map_err(|error| error.to_string())?;
            let before_activation = instance.processor(ui).map(drop).map_err(|error| error.to_string());
            Ok::<_, String>([before_setup, activate_before_setup, before_activation])
        })
        .expect("gain instance");
        assert_eq!(
            messages,
            [
                Err("The plugin must be set up before it can process audio".to_owned()),
                Err("The plugin must be set up before it is activated".to_owned()),
                Err("The plugin must be active before it can process audio".to_owned()),
            ]
        );
    }

    #[test]
    fn dropping_off_the_ui_thread_tears_down_on_the_ui_thread() {
        let _log = lock_recorder_log();
        let (instance, ui_thread) = on_ui(|ui| {
            let instance = active_instance(ui, Kind::Recorder, 1);
            drop(take_recorded_calls());
            (instance, std::thread::current().id())
        });
        let instance = instance.expect("recorder instance");
        assert!(instance.is_active());
        drop(instance);
        // The dispatcher runs jobs in order, so the posted teardown has run once this call returns.
        on_ui(|_| ());
        let calls = take_recorded_calls();
        let names: Vec<&str> = calls.iter().map(|call| call.name).collect();
        assert_eq!(
            names,
            [
                "processor.setProcessing(false)",
                "component.setActive(false)",
                "component.disconnect",
                "controller.disconnect",
                "controller.setComponentHandler",
                "controller.terminate",
                "component.terminate",
            ]
        );
        assert_ne!(ui_thread, std::thread::current().id());
        for call in calls {
            assert_eq!(call.thread, ui_thread, "{} ran on the wrong thread", call.name);
        }
        assert!(dispatcher().post(|_| ()));
    }

    /// Creates a processor handle for an instance on the UI thread.
    fn on_ui_processor(instance: &PluginInstance) -> ProcessorHandle {
        let core = Arc::clone(&instance.core);
        let layout = instance.layout.clone().unwrap_or_default();
        let setup = instance.setup.expect("instance is set up");
        on_ui(move |_| ProcessorHandle::new(core, &layout, setup))
    }
}

#[cfg(test)]
mod test_plugin_failures {
    use audiobatch_test_plugin::{FAILING_STATE_BYTES, INITIAL_CONTROLLER_STATE, Kind};
    use vst3::Steinberg::{kInvalidArgument, kResultFalse};

    use super::*;
    use crate::processor::ProcessError;
    use crate::test_helpers::{MAX_BLOCK, active_instance, on_ui};

    #[test]
    fn a_failing_process_call_returns_the_result_of_the_plugin() {
        assert_eq!(size_of::<ProcessError>(), size_of::<i32>());
        let (_instance, mut processor) = on_ui(|ui| {
            let instance = active_instance(ui, Kind::Failing, 2).map_err(|error| error.to_string())?;
            let processor = instance.processor(ui).map_err(|error| error.to_string())?;
            Ok::<_, String>((instance, processor))
        })
        .expect("failing instance with a processor");
        let error = processor.process(MAX_BLOCK).expect_err("process fails");
        assert_eq!(error, ProcessError(kResultFalse));
        assert_eq!(
            error.to_string(),
            format!("The plugin failed to process audio (result {kResultFalse})")
        );
        assert_eq!(processor.process(0), Ok(()));
    }

    #[test]
    fn parameter_failures_keep_the_result_code_and_requested_value() {
        let (refused, non_finite) = on_ui(|ui| {
            let instance = active_instance(ui, Kind::Gain, 2)?;
            Ok::<_, anyhow::Error>((
                instance.set_parameter(ui, u32::MAX, 0.5),
                instance.set_parameter(ui, 0, f64::INFINITY),
            ))
        })
        .expect("gain instance");
        let refused = refused.expect_err("unknown parameter");
        let non_finite = non_finite.expect_err("infinite value");
        assert!(matches!(
            refused,
            InstanceError::SetParameter {
                id: u32::MAX,
                result,
                ..
            } if result == kInvalidArgument
        ));
        assert_eq!(
            refused.to_string(),
            format!("The plugin refused the value 0.5 for parameter {}", u32::MAX)
        );
        assert!(matches!(
            non_finite,
            InstanceError::NonFiniteParameter {
                id: 0,
                normalized: f64::INFINITY
            }
        ));
        assert_eq!(
            non_finite.to_string(),
            "The value inf for parameter 0 is not a finite number"
        );
    }

    #[test]
    fn a_failing_get_state_keeps_what_the_plugin_wrote() {
        let state = on_ui(|ui| {
            let instance = active_instance(ui, Kind::Failing, 2).map_err(|error| error.to_string())?;
            instance.state(ui).map_err(|error| error.to_string())
        })
        .expect("state of the failing plugin");
        assert_eq!(
            state.component,
            1.0_f64.to_le_bytes().get(..FAILING_STATE_BYTES).expect("prefix")
        );
        assert_eq!(state.controller, INITIAL_CONTROLLER_STATE);
    }

    #[test]
    fn a_refused_state_keeps_the_current_settings() {
        let results = on_ui(|ui| {
            let mut gain = active_instance(ui, Kind::Gain, 2).map_err(|error| error.to_string())?;
            gain.deactivate(ui).map_err(|error| error.to_string())?;
            let short = PluginState {
                component: vec![0; 3],
                controller: Vec::new(),
            };
            let gain_result = gain.set_state(ui, &short).map_err(|error| error.to_string());
            let gain_state = gain.state(ui).map_err(|error| error.to_string())?;
            gain.activate(ui).map_err(|error| error.to_string())?;

            let mut failing = active_instance(ui, Kind::Failing, 2).map_err(|error| error.to_string())?;
            failing.deactivate(ui).map_err(|error| error.to_string())?;
            let full = PluginState {
                component: 0.5_f64.to_le_bytes().to_vec(),
                controller: Vec::new(),
            };
            let failing_result = failing.set_state(ui, &full).map_err(|error| error.to_string());
            let failing_state = failing.state(ui).map_err(|error| error.to_string())?;
            failing.activate(ui).map_err(|error| error.to_string())?;
            Ok::<_, String>((gain_result, gain_state, failing_result, failing_state))
        })
        .expect("instances that refuse a state");
        let (gain_result, gain_state, failing_result, failing_state) = results;
        assert_eq!(gain_result, Ok(false));
        assert_eq!(gain_state.component, 1.0_f64.to_le_bytes());
        assert_eq!(failing_result, Ok(false));
        assert_eq!(
            failing_state.component,
            1.0_f64.to_le_bytes().get(..FAILING_STATE_BYTES).expect("prefix")
        );
    }
}

#[cfg(test)]
mod test_buses {
    use audiobatch_test_plugin::Kind;

    use super::*;
    use crate::test_helpers::{class_id, on_ui, test_module};

    /// Configures a fresh instance of a kind for `channels` channels.
    fn configure(kind: Kind, channels: usize) -> Result<BusLayout, String> {
        on_ui(move |ui| {
            let module = test_module(ui);
            let mut instance =
                PluginInstance::create(ui, &module, class_id(kind), None).map_err(|error| error.to_string())?;
            instance
                .configure_main_buses(ui, channels)
                .map_err(|error| error.to_string())
        })
    }

    /// Returns a layout with the channel counts of each bus.
    fn layout(inputs: &[usize], outputs: &[usize]) -> BusLayout {
        BusLayout {
            inputs: inputs.to_vec(),
            outputs: outputs.to_vec(),
        }
    }

    #[test]
    fn mono_and_stereo_signals_get_matching_buses() {
        assert_eq!(configure(Kind::Gain, 1), Ok(layout(&[1], &[1])));
        assert_eq!(configure(Kind::Gain, 2), Ok(layout(&[2], &[2])));
        assert_eq!(configure(Kind::MonoOnly, 1), Ok(layout(&[1], &[1])));
    }

    #[test]
    fn a_mono_signal_runs_through_a_stereo_only_plugin_in_stereo() {
        assert_eq!(configure(Kind::StereoOnly, 1), Ok(layout(&[2], &[2])));
    }

    #[test]
    fn plugins_with_fewer_channels_than_the_signal_are_refused() {
        assert_eq!(
            configure(Kind::MonoOnly, 2),
            Err("The plugin does not support 2 channel audio".to_owned())
        );
        assert_eq!(
            configure(Kind::Gain, 3),
            Err("The plugin does not support 3 channel audio".to_owned())
        );
    }

    #[test]
    fn auxiliary_buses_keep_their_arrangement() {
        assert_eq!(configure(Kind::Echo, 1), Ok(layout(&[1, 1], &[1])));
        assert_eq!(configure(Kind::Echo, 2), Ok(layout(&[2, 1], &[2])));
    }
}

#[cfg(test)]
mod test_processing {
    use audiobatch_test_plugin::{
        DELAY_LATENCY, ECHO_DELAY, ECHO_LEVEL, EDIT_TRIGGER_PARAMETER, GAIN_PARAMETER, INITIAL_CONTROLLER_STATE, Kind,
    };

    use super::*;
    use crate::test_helpers::{MAX_BLOCK, active_instance, on_ui};

    /// Processes one block holding an impulse through every input channel, and returns the main output channels.
    fn process_impulse(processor: &mut ProcessorHandle, frames: usize) -> Vec<Vec<f32>> {
        for channel in 0..processor.input_channels() {
            let input = processor.input_channel_mut(channel).expect("input channel");
            input.fill(0.0);
            if let Some(first) = input.first_mut() {
                *first = 1.0;
            }
        }
        processor.process(frames).expect("process succeeds");
        let frames = frames.min(processor.max_block_frames());
        (0..processor.output_channels())
            .map(|channel| {
                processor
                    .output_channel(channel)
                    .expect("output channel")
                    .get(..frames)
                    .expect("frames")
                    .to_vec()
            })
            .collect()
    }

    /// Asserts that every channel holds the expected samples, within a rounding error.
    fn assert_channels(actual: &[Vec<f32>], expected: &[Vec<f32>]) {
        let matches = actual.len() == expected.len()
            && actual.iter().zip(expected).all(|(channel, wanted)| {
                channel.len() == wanted.len()
                    && channel
                        .iter()
                        .zip(wanted)
                        .all(|(sample, wanted_sample)| (sample - wanted_sample).abs() < 1e-6)
            });
        assert!(matches, "{actual:?} is not {expected:?}");
    }

    /// Creates an active instance and a processor handle for it.
    fn instance_and_processor(kind: Kind, channels: usize) -> (PluginInstance, ProcessorHandle) {
        on_ui(move |ui| {
            let instance = active_instance(ui, kind, channels).map_err(|error| error.to_string())?;
            let processor = instance.processor(ui).map_err(|error| error.to_string())?;
            Ok::<_, String>((instance, processor))
        })
        .expect("active instance with a processor")
    }

    #[test]
    fn saved_state_round_trips_and_sets_the_gain() {
        let gain_state = PluginState {
            component: 0.5_f64.to_le_bytes().to_vec(),
            controller: b"saved".to_vec(),
        };
        let expected = gain_state.clone();
        let (default_state, restored, _instance, mut processor) = on_ui(move |ui| {
            let mut instance = active_instance(ui, Kind::Gain, 2).map_err(|error| error.to_string())?;
            let default_state = instance.state(ui).map_err(|error| error.to_string())?;
            instance.deactivate(ui).map_err(|error| error.to_string())?;
            instance.set_state(ui, &gain_state).map_err(|error| error.to_string())?;
            instance.activate(ui).map_err(|error| error.to_string())?;
            let restored = instance.state(ui).map_err(|error| error.to_string())?;
            let processor = instance.processor(ui).map_err(|error| error.to_string())?;
            Ok::<_, String>((default_state, restored, instance, processor))
        })
        .expect("state round trip");
        assert_eq!(default_state.component, 1.0_f64.to_le_bytes());
        assert_eq!(default_state.controller, INITIAL_CONTROLLER_STATE);
        assert_eq!(restored, expected);
        let output = process_impulse(&mut processor, 4);
        assert_channels(&output, &[vec![0.5, 0.0, 0.0, 0.0], vec![0.5, 0.0, 0.0, 0.0]]);
    }

    #[test]
    fn delay_reports_its_latency_and_delays_the_signal() {
        let (instance, mut processor) = instance_and_processor(Kind::Delay, 1);
        let latency = on_ui(move |ui| (instance.latency_samples(ui), instance));
        assert_eq!(latency.0, DELAY_LATENCY);
        let frames = DELAY_LATENCY as usize + 1;
        let output = process_impulse(&mut processor, frames);
        let channel = output.first().expect("one channel");
        assert_eq!(
            channel.iter().position(|sample| *sample != 0.0),
            Some(DELAY_LATENCY as usize)
        );
    }

    #[test]
    fn echo_reports_its_tail_and_its_sidechain_gets_silence() {
        let (instance, mut processor) = instance_and_processor(Kind::Echo, 2);
        let tail = on_ui(move |ui| (instance.tail_samples(ui), instance));
        assert_eq!(tail.0, ECHO_DELAY);
        let frames = ECHO_DELAY as usize + 1;
        let output = process_impulse(&mut processor, frames);
        for channel in output {
            let ends = channel.first().copied().zip(channel.last().copied());
            assert!(
                ends.is_some_and(|(first, last)| (first - 1.0).abs() < 1e-6 && (last - ECHO_LEVEL).abs() < 1e-6),
                "{ends:?}"
            );
        }
    }

    #[test]
    fn controller_edits_reach_the_next_block() {
        let (instance, mut processor) = instance_and_processor(Kind::Gain, 1);
        let (instance, dirty) = on_ui(move |_| {
            if let Some(controller) = instance.core().controller() {
                // SAFETY: the controller is valid and this runs on the UI thread.
                unsafe { controller.setParamNormalized(EDIT_TRIGGER_PARAMETER, 0.25) };
            }
            let dirty = instance.take_dirty();
            (instance, dirty)
        });
        assert!(dirty);
        assert_channels(&process_impulse(&mut processor, 2), &[vec![0.25, 0.0]]);
        drop(processor);
        drop(instance);
    }

    #[test]
    fn a_flush_delivers_edits_without_a_processor() {
        let state = on_ui(|ui| {
            let instance = active_instance(ui, Kind::Gain, 2).map_err(|error| error.to_string())?;
            if let Some(controller) = instance.core().controller() {
                // SAFETY: the controller is valid and this runs on the UI thread.
                unsafe { controller.setParamNormalized(EDIT_TRIGGER_PARAMETER, 0.75) };
            }
            instance.flush_parameter_edits(ui);
            instance.state(ui).map_err(|error| error.to_string())
        })
        .expect("state after the flush");
        assert_eq!(state.component, 0.75_f64.to_le_bytes());
    }

    #[test]
    fn a_host_parameter_change_reaches_the_state_after_a_flush() {
        let state = on_ui(|ui| {
            let instance = active_instance(ui, Kind::Gain, 2).map_err(|error| error.to_string())?;
            instance
                .set_parameter(ui, GAIN_PARAMETER, 0.25)
                .map_err(|error| error.to_string())?;
            instance.flush_parameter_edits(ui);
            instance.state(ui).map_err(|error| error.to_string())
        })
        .expect("state after the parameter change");
        assert_eq!(state.component, 0.25_f64.to_le_bytes());
    }

    #[test]
    fn a_host_parameter_change_reaches_the_next_block_and_is_clamped() {
        let (instance, mut processor) = instance_and_processor(Kind::Gain, 1);
        let (instance, result) = on_ui(move |ui| {
            let result = instance
                .set_parameter(ui, GAIN_PARAMETER, 1.5)
                .and_then(|()| instance.set_parameter(ui, GAIN_PARAMETER, -1.0))
                .map_err(|error| error.to_string());
            (instance, result)
        });
        assert_eq!(result, Ok(()));
        assert_channels(&process_impulse(&mut processor, 2), &[vec![0.0, 0.0]]);
        drop(processor);
        drop(instance);
    }

    #[test]
    fn non_finite_output_and_oversized_blocks_pass_through() {
        let (_instance, mut processor) = instance_and_processor(Kind::NotFinite, 2);
        assert_eq!(processor.max_block_frames(), MAX_BLOCK);
        let output = process_impulse(&mut processor, MAX_BLOCK * 4);
        assert_eq!(output.len(), 2);
        assert!(
            output
                .iter()
                .all(|channel| channel.len() == MAX_BLOCK && channel.iter().all(|sample| sample.is_nan()))
        );
    }
}
