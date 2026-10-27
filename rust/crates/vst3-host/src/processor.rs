//! Running a plugin's audio processing on preallocated buffers.
//!
//! A `ProcessorHandle` owns a buffer for every channel of every audio bus, sized for the largest block,
//! and the parameter change objects passed to `process`.
//! The caller fills the main input channels, calls `process`, and reads the main output channels.
//! Auxiliary inputs get silence and auxiliary outputs are thrown away.
//! Parameter edits from the plugin's controller are delivered at the start of each block.
//! Nothing in `process` allocates or locks, so a handle can run inside an audio callback.

#![expect(unsafe_code, reason = "builds the raw process data a plugin's process call reads")]

use std::fmt;
use std::sync::Arc;

use vst3::Steinberg::Vst::{
    AudioBusBuffers, AudioBusBuffers__type0, IAudioProcessor, IAudioProcessorTrait as _, IParameterChanges,
    ProcessContext, ProcessContext_::StatesAndFlags_, ProcessData, SymbolicSampleSizes_,
};
use vst3::Steinberg::{kResultFalse, kResultOk};
use vst3::{ComPtr, ComWrapper};

use crate::enums::{to_i32, to_u32};
use crate::host_objects::ParameterChanges;
use crate::instance::{BusLayout, InstanceCore, ProcessingSetup, process_mode_value};

/// Tempo reported to plugins, which matters only to tempo-synced effects.
const TEMPO: f64 = 120.0;

/// A failed `process` call, with the result code the plugin returned.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProcessError(pub i32);

/// Runs a plugin's audio processing on one thread at a time.
pub struct ProcessorHandle {
    /// The instance the handle belongs to.
    core: Arc<InstanceCore>,
    /// The setup the buffers are sized for.
    setup: ProcessingSetup,
    /// Buffers of the input buses.
    inputs: Vec<BusBuffers>,
    /// Buffers of the output buses.
    outputs: Vec<BusBuffers>,
    /// Bus descriptions passed to the plugin, pointing into `inputs`.
    input_buses: Vec<AudioBusBuffers>,
    /// Bus descriptions passed to the plugin, pointing into `outputs`.
    output_buses: Vec<AudioBusBuffers>,
    /// Parameter changes delivered to the plugin.
    input_changes: ComWrapper<ParameterChanges>,
    /// Parameter changes the plugin reports, which are thrown away.
    output_changes: ComWrapper<ParameterChanges>,
    /// Transport information passed to the plugin.
    context: ProcessContext,
}

/// Marks an instance as inside a processing call, and clears the mark when dropped.
/// Two handles of one instance may exist at once, such as while the preview swaps chains,
/// but VST3 forbids overlapping `process` calls, so only one of them can hold the mark.
struct ProcessingGuard<'a>(&'a InstanceCore);

/// The buffers of one audio bus.
struct BusBuffers {
    /// One buffer of `max_block_frames` samples per channel.
    channels: Vec<Vec<f32>>,
    /// Pointers to the channel buffers, as the plugin reads them.
    pointers: Vec<*mut f32>,
}

impl ProcessError {
    /// The error of a call made while another handle of the same instance was processing.
    pub(crate) const BUSY: Self = Self(kResultFalse);
}

impl fmt::Display for ProcessError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "The plugin failed to process audio (result {})", self.0)
    }
}

impl std::error::Error for ProcessError {}

impl ProcessorHandle {
    /// Creates a handle with buffers for every bus of the layout.
    pub(crate) fn new(core: Arc<InstanceCore>, layout: &BusLayout, setup: ProcessingSetup) -> Self {
        let frames = setup.max_block_frames;
        let mut inputs: Vec<BusBuffers> = layout
            .inputs
            .iter()
            .map(|channels| BusBuffers::new(*channels, frames))
            .collect();
        let mut outputs: Vec<BusBuffers> = layout
            .outputs
            .iter()
            .map(|channels| BusBuffers::new(*channels, frames))
            .collect();
        let input_buses = inputs.iter_mut().map(BusBuffers::bus).collect();
        let output_buses = outputs.iter_mut().map(BusBuffers::bus).collect();
        // SAFETY: `ProcessContext` holds only integers and floats, for which all zero bits are valid.
        let mut context: ProcessContext = unsafe { std::mem::zeroed() };
        context.state = to_u32(StatesAndFlags_::kPlaying)
            | to_u32(StatesAndFlags_::kTempoValid)
            | to_u32(StatesAndFlags_::kTimeSigValid);
        context.sampleRate = setup.sample_rate;
        context.tempo = TEMPO;
        context.timeSigNumerator = 4;
        context.timeSigDenominator = 4;
        core.processor_created();
        Self {
            core,
            setup,
            inputs,
            outputs,
            input_buses,
            output_buses,
            input_changes: ParameterChanges::create(),
            output_changes: ParameterChanges::create(),
            context,
        }
    }

    /// Returns the largest number of frames one `process` call takes.
    #[must_use]
    pub const fn max_block_frames(&self) -> usize {
        self.setup.max_block_frames
    }

    /// Returns the sample rate the plugin was set up for.
    #[must_use]
    pub const fn sample_rate(&self) -> f64 {
        self.setup.sample_rate
    }

    /// Returns the number of channels of the main input bus, zero when the plugin has none.
    #[must_use]
    pub fn input_channels(&self) -> usize {
        self.inputs.first().map_or(0, |bus| bus.channels.len())
    }

    /// Returns the number of channels of the main output bus.
    #[must_use]
    pub fn output_channels(&self) -> usize {
        self.outputs.first().map_or(0, |bus| bus.channels.len())
    }

    /// Returns a channel of the main input bus for writing the next block, `max_block_frames` samples long.
    pub fn input_channel_mut(&mut self, channel: usize) -> Option<&mut [f32]> {
        self.inputs
            .first_mut()?
            .channels
            .get_mut(channel)
            .map(Vec::as_mut_slice)
    }

    /// Returns a channel of the main output bus after `process`, `max_block_frames` samples long.
    #[must_use]
    pub fn output_channel(&self, channel: usize) -> Option<&[f32]> {
        self.outputs.first()?.channels.get(channel).map(Vec::as_slice)
    }

    /// Processes `frames` samples of the main input channels into the main output channels.
    /// `frames` is limited to `max_block_frames`.
    ///
    /// # Errors
    ///
    /// Returns the result code when the plugin reports a failure.
    pub fn process(&mut self, frames: usize) -> Result<(), ProcessError> {
        let Some(_processing) = ProcessingGuard::begin(&self.core) else {
            return Err(ProcessError::BUSY);
        };
        let frames = frames.min(self.setup.max_block_frames);
        for bus in self.inputs.iter_mut().skip(1) {
            bus.clear(frames);
        }
        for bus in &mut self.outputs {
            bus.clear(frames);
        }
        self.input_changes.clear();
        self.output_changes.clear();
        while let Some(edit) = self.core.edits().pop() {
            // When every queue of this block is taken, the edit waits for the next block instead of being lost.
            if !self.input_changes.set(edit.id, edit.value) {
                let _ = self.core.edits().push(edit);
                break;
            }
        }
        for bus in self.input_buses.iter_mut().chain(self.output_buses.iter_mut()) {
            bus.silenceFlags = 0;
        }
        let mut data = ProcessData {
            processMode: process_mode_value(self.setup.mode),
            symbolicSampleSize: to_i32(SymbolicSampleSizes_::kSample32),
            numSamples: i32::try_from(frames).unwrap_or(0),
            numInputs: i32::try_from(self.input_buses.len()).unwrap_or(0),
            numOutputs: i32::try_from(self.output_buses.len()).unwrap_or(0),
            inputs: self.input_buses.as_mut_ptr(),
            outputs: self.output_buses.as_mut_ptr(),
            inputParameterChanges: changes_pointer(&self.input_changes),
            outputParameterChanges: changes_pointer(&self.output_changes),
            inputEvents: std::ptr::null_mut(),
            outputEvents: std::ptr::null_mut(),
            processContext: &raw mut self.context,
        };
        // SAFETY: the plugin is active, every bus points to buffers of at least `frames` samples per channel,
        // and the parameter changes and the context live for the call.
        let result = unsafe { self.processor().process(&raw mut data) };
        self.context.projectTimeSamples += i64::try_from(frames).unwrap_or(0);
        self.context.continousTimeSamples += i64::try_from(frames).unwrap_or(0);
        if result == kResultOk {
            Ok(())
        } else {
            Err(ProcessError(result))
        }
    }

    /// Clears the plugin's internal buffers, such as delay lines, by stopping and restarting processing,
    /// and moves the reported position to `position` samples.
    /// Does nothing while another handle of the same instance is processing, since VST3 forbids overlapping calls.
    pub fn reset(&mut self, position: u64) {
        {
            let Some(_processing) = ProcessingGuard::begin(&self.core) else {
                return;
            };
            let processor = self.processor();
            // SAFETY: the plugin is active, no other handle is in a processing call,
            // and VST3 allows `setProcessing` on the processing thread.
            unsafe {
                processor.setProcessing(0);
                processor.setProcessing(1);
            }
        }
        self.set_position(position);
    }

    /// Moves the position reported to the plugin to `position` samples.
    pub fn set_position(&mut self, position: u64) {
        self.context.projectTimeSamples = i64::try_from(position).unwrap_or(i64::MAX);
    }

    /// Returns the audio processor.
    fn processor(&self) -> &ComPtr<IAudioProcessor> {
        self.core.processor()
    }
}

#[expect(
    clippy::non_send_fields_in_send_ty,
    reason = "the pointer fields point into buffers the handle owns and are only used by the thread holding it"
)]
// SAFETY: the raw pointers point into buffers the handle owns, which move with it,
// and VST3 allows `process` and `setProcessing` on any one thread at a time.
unsafe impl Send for ProcessorHandle {}

impl Drop for ProcessorHandle {
    fn drop(&mut self) {
        self.core.processor_dropped();
    }
}

impl fmt::Debug for ProcessorHandle {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ProcessorHandle")
            .field("setup", &self.setup)
            .field("inputs", &self.inputs.len())
            .field("outputs", &self.outputs.len())
            .finish_non_exhaustive()
    }
}

impl<'a> ProcessingGuard<'a> {
    /// Marks the instance as processing, or returns `None` when another handle already holds the mark.
    fn begin(core: &'a InstanceCore) -> Option<Self> {
        core.try_begin_processing().then_some(Self(core))
    }
}

impl Drop for ProcessingGuard<'_> {
    fn drop(&mut self) {
        self.0.end_processing();
    }
}

impl BusBuffers {
    /// Allocates the buffers of a bus.
    fn new(channels: usize, frames: usize) -> Self {
        let mut channels: Vec<Vec<f32>> = (0..channels).map(|_| vec![0.0; frames]).collect();
        let pointers = channels.iter_mut().map(Vec::as_mut_ptr).collect();
        Self { channels, pointers }
    }

    /// Returns the description of the bus that the plugin reads.
    fn bus(&mut self) -> AudioBusBuffers {
        AudioBusBuffers {
            numChannels: i32::try_from(self.channels.len()).unwrap_or(0),
            silenceFlags: 0,
            __field0: AudioBusBuffers__type0 {
                channelBuffers32: self.pointers.as_mut_ptr(),
            },
        }
    }

    /// Sets the first `frames` samples of every channel to zero.
    fn clear(&mut self, frames: usize) {
        for channel in &mut self.channels {
            if let Some(samples) = channel.get_mut(..frames) {
                samples.fill(0.0);
            }
        }
    }
}

/// Returns the COM pointer of a parameter changes object.
fn changes_pointer(changes: &ComWrapper<ParameterChanges>) -> *mut IParameterChanges {
    changes
        .as_com_ref::<IParameterChanges>()
        .map_or(std::ptr::null_mut(), |changes| changes.as_ptr())
}
