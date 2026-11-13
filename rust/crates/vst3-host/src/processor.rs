//! Running a plugin's audio processing on preallocated buffers.
//!
//! A `ProcessorHandle` owns a buffer for every channel of every audio bus, sized for the largest block,
//! and the parameter change objects passed to `process`.
//! The caller either fills and reads the owned buffers or lends main channels to `process_in_place`.
//! Auxiliary inputs get silence and auxiliary outputs are thrown away.
//! Parameter edits from the plugin's controller are delivered at the start of each block.
//! Nothing in `process` allocates or locks, so a handle can run inside an audio callback.

#![expect(unsafe_code, reason = "builds the raw process data a plugin's process call reads")]

use std::fmt;
use std::sync::Arc;

use thiserror::Error;
use vst3::Steinberg::Vst::{
    AudioBusBuffers, AudioBusBuffers__type0, IAudioProcessor, IAudioProcessorTrait as _, IParameterChanges,
    ProcessContext, ProcessContext_::StatesAndFlags_, ProcessData, SymbolicSampleSizes_,
};
use vst3::Steinberg::{kInvalidArgument, kResultFalse, kResultOk};
use vst3::{ComPtr, ComWrapper};

use crate::enums::{to_i32, to_u32};
use crate::host_objects::{EDIT_QUEUE_CAPACITY, ParameterChanges, ParameterEdit};
use crate::instance::{BusLayout, InstanceCore, ProcessingSetup, process_mode_value};

/// Tempo reported to plugins, which matters only to tempo-synced effects.
const TEMPO: f64 = 120.0;

/// Most main channels borrowed in place, matching the application's mono/stereo signal.
const MAXIMUM_IN_PLACE_CHANNELS: usize = 2;

/// A failed `process` call, with the result code the plugin returned.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
#[error("The plugin failed to process audio (result {0})")]
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
    /// Controller edits that did not fit into the parameter queues of a block, in the order they were made.
    /// They go first in the next block, so a newer edit of the same parameter is never applied before them.
    /// The capacity is reserved when the handle is created, so carrying edits never allocates.
    carried_edits: Vec<ParameterEdit>,
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

/// Temporarily binds the main buses to caller-owned channels and restores owned pointers on every exit.
struct MainBusBinding<'a> {
    /// All input descriptions passed to the plugin.
    inputs: &'a mut [AudioBusBuffers],
    /// All output descriptions passed to the plugin.
    outputs: &'a mut [AudioBusBuffers],
    /// The owned main input channel pointer array, when temporarily replaced.
    owned_input: Option<*mut *mut f32>,
    /// The owned main output channel pointer array, when temporarily replaced.
    owned_output: Option<*mut *mut f32>,
}

impl ProcessError {
    /// The error of a call made while another handle of the same instance was processing.
    pub(crate) const BUSY: Self = Self(kResultFalse);
}

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
            | to_u32(StatesAndFlags_::kContTimeValid)
            | to_u32(StatesAndFlags_::kProjectTimeMusicValid)
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
            carried_edits: Vec::with_capacity(EDIT_QUEUE_CAPACITY),
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
    #[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
    pub fn process(&mut self, frames: usize) -> Result<(), ProcessError> {
        self.process_buffers(frames, None)
    }

    /// Processes caller-owned mono/stereo main channels in place, without input or output copies.
    /// The input and output of each main channel share its buffer, as permitted by VST3.
    /// Output-only channels and auxiliary buses get silence before processing.
    /// Only the first `frames` samples are accessed, limited to `max_block_frames`.
    /// The owned main input and output buffers are unchanged and remain usable by `process`.
    ///
    /// # Errors
    ///
    /// Returns `kInvalidArgument` for main buses wider than stereo,
    /// when there are too few channels or any used channel is too short,
    /// or the result code when the plugin reports a failure.
    #[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
    pub fn process_in_place(&mut self, channels: &mut [&mut [f32]], frames: usize) -> Result<(), ProcessError> {
        let frames = frames.min(self.setup.max_block_frames);
        let main_channels = self.input_channels().max(self.output_channels());
        if main_channels > MAXIMUM_IN_PLACE_CHANNELS
            || channels.len() < main_channels
            || channels
                .iter()
                .take(main_channels)
                .any(|channel| channel.len() < frames)
        {
            return Err(ProcessError(kInvalidArgument));
        }
        self.process_buffers(frames, Some(channels))
    }

    /// Clears the plugin's internal buffers, such as delay lines, by stopping and restarting processing,
    /// and moves the reported position to `position` samples.
    /// Does nothing while another handle of the same instance is processing, since VST3 forbids overlapping calls.
    #[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
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
        self.context.continousTimeSamples = 0;
        self.set_position(position);
    }

    /// Moves the position reported to the plugin to `position` samples.
    pub fn set_position(&mut self, position: u64) {
        self.context.projectTimeSamples = i64::try_from(position).unwrap_or(i64::MAX);
        self.context.projectTimeMusic = project_time_music(self.context.projectTimeSamples, self.setup.sample_rate);
    }

    /// Processes owned or temporarily borrowed main buses with the same transport and parameter delivery.
    fn process_buffers(&mut self, frames: usize, main_channels: Option<&mut [&mut [f32]]>) -> Result<(), ProcessError> {
        let Some(_processing) = ProcessingGuard::begin(&self.core) else {
            return Err(ProcessError::BUSY);
        };
        let frames = frames.min(self.setup.max_block_frames);
        for bus in self.inputs.iter_mut().skip(1) {
            bus.clear(frames);
        }
        for bus in self.outputs.iter_mut().skip(usize::from(main_channels.is_some())) {
            bus.clear(frames);
        }
        for (buffers, bus) in self
            .inputs
            .iter_mut()
            .zip(&mut self.input_buses)
            .chain(self.outputs.iter_mut().zip(&mut self.output_buses))
        {
            *bus = buffers.bus();
        }
        let mut main_pointers = [std::ptr::null_mut(); MAXIMUM_IN_PLACE_CHANNELS];
        let in_place = main_channels.is_some();
        if let Some(channels) = main_channels {
            let inputs = self.inputs.first().map_or(0, |bus| bus.channels.len());
            let outputs = self.outputs.first().map_or(0, |bus| bus.channels.len());
            for channel in channels.iter_mut().take(outputs).skip(inputs) {
                channel.get_mut(..frames).expect("validated main channel").fill(0.0);
            }
            for (pointer, channel) in main_pointers.iter_mut().zip(channels.iter_mut()) {
                *pointer = channel.as_mut_ptr();
            }
        }
        let binding = MainBusBinding::new(
            &mut self.input_buses,
            &mut self.output_buses,
            in_place.then_some(main_pointers.as_mut_slice()),
        );
        self.input_changes.clear();
        self.output_changes.clear();
        // An edit fails only when every queue of the block is taken by other parameters,
        // so every later edit of the same parameter fails too, and the carried edits stay in order.
        let input_changes = &self.input_changes;
        self.carried_edits
            .retain(|edit| !input_changes.set(edit.id, edit.value));
        // A producer may keep editing during this block, so process only the finite entry snapshot.
        for _ in 0..self.core.edits().len().min(EDIT_QUEUE_CAPACITY) {
            let Some(edit) = self.core.edits().pop() else { break };
            if self.input_changes.set(edit.id, edit.value) {
                continue;
            }
            if self.carried_edits.len() == self.carried_edits.capacity() {
                let _ = self.core.edits().push(edit);
                break;
            }
            self.carried_edits.push(edit);
        }
        self.core.set_carried_edits(self.carried_edits.len());
        let mut data = ProcessData {
            processMode: process_mode_value(self.setup.mode),
            symbolicSampleSize: to_i32(SymbolicSampleSizes_::kSample32),
            numSamples: i32::try_from(frames).unwrap_or(0),
            numInputs: i32::try_from(binding.inputs.len()).unwrap_or(0),
            numOutputs: i32::try_from(binding.outputs.len()).unwrap_or(0),
            inputs: binding.inputs.as_mut_ptr(),
            outputs: binding.outputs.as_mut_ptr(),
            inputParameterChanges: changes_pointer(&self.input_changes),
            outputParameterChanges: changes_pointer(&self.output_changes),
            inputEvents: std::ptr::null_mut(),
            outputEvents: std::ptr::null_mut(),
            processContext: &raw mut self.context,
        };
        // SAFETY: the plugin is active, every bus points to buffers of at least `frames` samples per channel,
        // and the parameter changes and the context live for the call.
        let result = unsafe { self.core.processor().process(&raw mut data) };
        drop(binding);
        self.context.projectTimeSamples += i64::try_from(frames).unwrap_or(0);
        self.context.continousTimeSamples += i64::try_from(frames).unwrap_or(0);
        self.context.projectTimeMusic = project_time_music(self.context.projectTimeSamples, self.setup.sample_rate);
        if result == kResultOk {
            Ok(())
        } else {
            Err(ProcessError(result))
        }
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
    /// Hands the carried edits back to the instance, so the next handle or flush delivers them.
    fn drop(&mut self) {
        if !self.carried_edits.is_empty() {
            // Retirement is not an audio callback operation. Preserve newer queued values after the older carry.
            let newer: Vec<_> = (0..self.core.edits().len().min(EDIT_QUEUE_CAPACITY))
                .filter_map(|_| self.core.edits().pop())
                .collect();
            for edit in self.carried_edits.drain(..).chain(newer) {
                self.core.edits().force_push(edit);
            }
            self.core.set_carried_edits(0);
        }
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

    /// Refreshes raw pointers after mutable sample borrows and returns the bus description.
    fn bus(&mut self) -> AudioBusBuffers {
        for (pointer, channel) in self.pointers.iter_mut().zip(&mut self.channels) {
            *pointer = channel.as_mut_ptr();
        }
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

impl<'a> MainBusBinding<'a> {
    /// Borrows a channel pointer array for both main buses for the duration of this call.
    fn new(
        inputs: &'a mut [AudioBusBuffers],
        outputs: &'a mut [AudioBusBuffers],
        pointers: Option<&'a mut [*mut f32]>,
    ) -> Self {
        let pointers = pointers.map(<[_]>::as_mut_ptr);
        let bind = |bus: &mut AudioBusBuffers| {
            let pointers = pointers?;
            // SAFETY: the host always initializes this union with its binary32 channel pointer array.
            let owned = unsafe { bus.__field0.channelBuffers32 };
            bus.__field0 = AudioBusBuffers__type0 {
                channelBuffers32: pointers,
            };
            Some(owned)
        };
        let owned_input = inputs.first_mut().and_then(bind);
        let owned_output = outputs.first_mut().and_then(bind);
        Self {
            inputs,
            outputs,
            owned_input,
            owned_output,
        }
    }
}

impl Drop for MainBusBinding<'_> {
    fn drop(&mut self) {
        for (buses, owned) in [
            (&mut *self.inputs, self.owned_input),
            (&mut *self.outputs, self.owned_output),
        ] {
            if let Some(owned) = owned {
                buses.first_mut().expect("bound main bus").__field0 = AudioBusBuffers__type0 {
                    channelBuffers32: owned,
                };
            }
        }
    }
}

/// Converts a sample position to quarter-note musical beats at the reported tempo.
#[expect(
    clippy::cast_precision_loss,
    reason = "sample positions fit within IEEE-754 f64 precision for musical timeline reporting"
)]
fn project_time_music(samples: i64, sample_rate: f64) -> f64 {
    if sample_rate > 0.0 {
        (samples as f64 / sample_rate) * (TEMPO / 60.0)
    } else {
        0.0
    }
}

/// Returns the COM pointer of a parameter changes object.
fn changes_pointer(changes: &ComWrapper<ParameterChanges>) -> *mut IParameterChanges {
    changes
        .as_com_ref::<IParameterChanges>()
        .map_or(std::ptr::null_mut(), |changes| changes.as_ptr())
}

#[cfg(test)]
mod test_in_place_processing {
    use audiobatch_test_plugin::{GAIN_PARAMETER, Kind};

    use super::*;
    #[cfg(audiobatch_rtsan)]
    use crate::instance::PluginInstance;
    use crate::test_helpers::{MAX_BLOCK, ProcessingFixture, active_instance, on_ui};

    /// Creates a half-gain plugin and its handle, keeping the instance alive for processing.
    fn processor(kind: Kind) -> ProcessingFixture {
        on_ui(move |ui| {
            let instance = active_instance(ui, kind, 2).expect("active test plugin");
            instance.set_parameter(ui, GAIN_PARAMETER, 0.5).expect("test gain");
            let processor = instance.processor(ui).expect("processor");
            ProcessingFixture { instance, processor }
        })
    }

    #[cfg(audiobatch_rtsan)]
    #[test]
    fn rtsan_gain_owned_and_borrowed_automation_reset() {
        let ProcessingFixture {
            instance,
            mut processor,
        } = on_ui(|ui| {
            let module = crate::test_helpers::test_module(ui);
            let mut instance = PluginInstance::create(
                ui,
                &module,
                crate::test_helpers::class_id(Kind::Gain),
                Some(crate::test_helpers::dispatcher().clone()),
            )
            .expect("realtime test instance");
            instance.configure_main_buses(ui, 2).expect("stereo buses");
            instance
                .setup(
                    ui,
                    crate::ProcessMode::Realtime,
                    crate::test_helpers::SAMPLE_RATE,
                    MAX_BLOCK,
                )
                .expect("explicit realtime setup");
            instance.activate(ui).expect("active realtime plugin");
            instance
                .set_parameter(ui, GAIN_PARAMETER, 0.5)
                .expect("first automation");
            let processor = instance.processor(ui).expect("realtime processor");
            ProcessingFixture { instance, processor }
        });
        assert_eq!(processor.setup.mode, crate::ProcessMode::Realtime);
        let mut left = [0.5; MAX_BLOCK];
        let mut right = [-0.25; MAX_BLOCK];
        std::thread::scope(|scope| {
            scope
                .spawn(|| {
                    processor.reset(17);
                    processor
                        .process_in_place(&mut [&mut left, &mut right], MAX_BLOCK)
                        .expect("borrowed");
                    processor.input_channel_mut(0).expect("input").fill(0.5);
                    processor.process(MAX_BLOCK).expect("owned");
                })
                .join()
                .expect("callback thread");
        });
        assert!(left.iter().all(|sample| (*sample - 0.25).abs() < f32::EPSILON));
        assert!(right.iter().all(|sample| (*sample + 0.125).abs() < f32::EPSILON));
        drop(processor);
        on_ui(move |_ui| drop(instance));
    }

    #[test]
    fn in_place_matches_buffered_gain_delay_and_echo() {
        for kind in [Kind::Gain, Kind::Delay, Kind::Echo] {
            let ProcessingFixture {
                instance: _buffered_instance,
                processor: mut buffered,
            } = processor(kind);
            let ProcessingFixture {
                instance: _borrowed_instance,
                processor: mut borrowed,
            } = processor(kind);
            buffered.reset(17);
            borrowed.reset(17);
            for block in 0..3 {
                let mut channels = [
                    vec![if block == 0 { 0.5 } else { 0.0 }; MAX_BLOCK],
                    vec![if block == 0 { -0.25 } else { 0.0 }; MAX_BLOCK],
                ];
                for (channel, samples) in channels.iter().enumerate() {
                    buffered
                        .input_channel_mut(channel)
                        .expect("input")
                        .copy_from_slice(samples);
                }
                buffered.process(MAX_BLOCK).expect("buffered processing");
                borrowed
                    .process_in_place(&mut channels.each_mut().map(Vec::as_mut_slice), MAX_BLOCK)
                    .expect("in-place processing");
                for (channel, samples) in channels.iter().enumerate() {
                    assert_eq!(samples.as_slice(), buffered.output_channel(channel).expect("output"));
                }
            }
        }
    }

    #[test]
    fn short_blocks_preserve_suffixes_and_owned_buffers() {
        let ProcessingFixture {
            instance: _instance,
            mut processor,
        } = processor(Kind::Echo);
        for bus in &mut processor.inputs {
            for channel in &mut bus.channels {
                channel.fill(0.75);
            }
        }
        for bus in &mut processor.outputs {
            for channel in &mut bus.channels {
                channel.fill(-0.75);
            }
        }
        let mut left = [0.5; 8];
        let mut right = [-0.25; 8];
        processor
            .process_in_place(&mut [&mut left, &mut right], 3)
            .expect("short block");
        assert_eq!(left, [0.25, 0.25, 0.25, 0.5, 0.5, 0.5, 0.5, 0.5]);
        assert_eq!(right, [-0.125, -0.125, -0.125, -0.25, -0.25, -0.25, -0.25, -0.25]);
        assert_eq!(processor.input_channel_mut(0).expect("owned input"), &[0.75; MAX_BLOCK]);
        assert_eq!(processor.output_channel(0).expect("owned output"), &[-0.75; MAX_BLOCK]);
        let sidechain = processor
            .inputs
            .get(1)
            .expect("sidechain")
            .channels
            .first()
            .expect("sidechain channel");
        assert_eq!(sidechain.get(..3), Some([0.0; 3].as_slice()));
        processor.process(1).expect("owned buffers restored");
        assert_eq!(processor.output_channel(0).expect("owned output").first(), Some(&0.375));
    }

    #[test]
    fn invalid_buffers_are_rejected_without_touching_audio() {
        let ProcessingFixture {
            instance: _instance,
            mut processor,
        } = processor(Kind::Gain);
        let mut left = [0.5; 4];
        let mut right = [-0.25; 2];
        assert_eq!(
            processor.process_in_place(&mut [&mut left], 3),
            Err(ProcessError(kInvalidArgument))
        );
        assert_eq!(
            processor.process_in_place(&mut [&mut left, &mut right], 3),
            Err(ProcessError(kInvalidArgument))
        );
        assert_eq!(left, [0.5; 4]);
        assert_eq!(right, [-0.25; 2]);
        processor
            .process_in_place(&mut [&mut left, &mut right], 0)
            .expect("parameter flush");
        processor
            .process_in_place(&mut [&mut left, &mut right], 2)
            .expect("valid block");
        assert_eq!(left, [0.25, 0.25, 0.5, 0.5]);
    }

    #[test]
    fn failed_processing_restores_owned_bus_descriptions() {
        let ProcessingFixture {
            instance: _instance,
            mut processor,
        } = processor(Kind::Failing);
        // SAFETY: the host initializes these descriptions with binary32 channel pointer arrays.
        let (input, output) = unsafe {
            (
                processor
                    .input_buses
                    .first()
                    .expect("main input")
                    .__field0
                    .channelBuffers32,
                processor
                    .output_buses
                    .first()
                    .expect("main output")
                    .__field0
                    .channelBuffers32,
            )
        };
        let mut left = [0.5; 4];
        let mut right = [-0.25; 4];
        assert!(processor.process_in_place(&mut [&mut left, &mut right], 4).is_err());
        // SAFETY: failure preserves the initialized binary32 union member.
        unsafe {
            assert_eq!(
                processor
                    .input_buses
                    .first()
                    .expect("main input")
                    .__field0
                    .channelBuffers32,
                input
            );
            assert_eq!(
                processor
                    .output_buses
                    .first()
                    .expect("main output")
                    .__field0
                    .channelBuffers32,
                output
            );
        }
        processor.process(0).expect("buffered flush after failure");
    }

    #[test]
    fn binding_aliases_main_channels_and_restores_bus_descriptions() {
        let mut input = BusBuffers::new(1, 8);
        let mut output = BusBuffers::new(2, 8);
        let owned_input = input.pointers.as_mut_ptr();
        let owned_output = output.pointers.as_mut_ptr();
        let mut input_bus = input.bus();
        let mut output_bus = output.bus();
        let mut left = [0.5; 8];
        let mut right = [-0.25; 8];
        let mut pointers = [left.as_mut_ptr(), right.as_mut_ptr()];
        let shared = pointers.as_mut_ptr();
        {
            let binding = MainBusBinding::new(
                std::slice::from_mut(&mut input_bus),
                std::slice::from_mut(&mut output_bus),
                Some(&mut pointers),
            );
            // SAFETY: the binding initializes both descriptions with binary32 channel pointer arrays.
            unsafe {
                assert_eq!(binding.inputs.first().expect("input").__field0.channelBuffers32, shared);
                assert_eq!(
                    binding.outputs.first().expect("output").__field0.channelBuffers32,
                    shared
                );
            }
        }
        assert_eq!(left, [0.5; 8]);
        assert_eq!(right, [-0.25; 8]);
        // SAFETY: the descriptions retain their initialized binary32 union member after restoration.
        unsafe {
            assert_eq!(input_bus.__field0.channelBuffers32, owned_input);
            assert_eq!(output_bus.__field0.channelBuffers32, owned_output);
        }
    }
}
