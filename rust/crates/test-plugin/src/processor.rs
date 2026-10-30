//! The component and audio processor of the fake plugins.
//!
//! The component state is the gain as eight little-endian bytes.
//! When connected to its controller, the processor sends it a message created through the host context,
//! which checks that the host can create messages.

#![expect(
    unsafe_code,
    reason = "implements the VST3 component and audio processor COM interfaces of the fake plugins"
)]

use std::array;
use std::collections::VecDeque;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Mutex, PoisonError};

use vst3::Steinberg::Vst::{
    AudioBusBuffers, BusDirections_, BusInfo, BusInfo_::BusFlags_, BusTypes_, IAttributeListTrait as _,
    IAudioProcessor, IAudioProcessorTrait, IComponent, IComponentTrait, IConnectionPoint, IConnectionPointTrait,
    IHostApplication, IHostApplicationTrait as _, IMessage, IMessageTrait as _, IParamValueQueueTrait as _,
    IParameterChanges, IParameterChangesTrait as _, IoMode, MediaTypes_, ProcessData, ProcessSetup, SpeakerArr,
    SpeakerArrangement, SymbolicSampleSizes_, kNoTail,
};
use vst3::Steinberg::{
    FUnknown, IBStream, IBStreamTrait as _, IPluginBaseTrait, TBool, TUID, kInvalidArgument, kNotImplemented,
    kResultFalse, kResultOk, kResultTrue, tresult,
};
use vst3::{Class, ComPtr, ComRef, Interface};
use vst3_host::strings::to_utf16;

use crate::{
    BULK_PARAMETER_COUNT, BULK_PARAMETER_START, DELAY_LATENCY, ECHO_DELAY, ECHO_LEVEL, FAILING_STATE_BYTES,
    FLUSH_FAILURE_PARAMETER, GAIN_PARAMETER, Kind, RELOAD_TRIGGER_PARAMETER, record,
};

/// Id of the message the processor sends to its controller when they are connected.
pub const CONNECTED_MESSAGE: &std::ffi::CStr = c"TestProcessorConnected";

/// The component and audio processor of a fake plugin.
pub struct TestProcessor {
    /// What the plugin does.
    kind: Kind,
    /// Linear gain, as `f64` bits.
    gain: AtomicU64,
    /// Independent parameter values included in bulk state snapshots.
    parameters: [AtomicU64; BULK_PARAMETER_COUNT],
    /// A reload is required, and re-preparing this object must not clear it.
    reload_required: AtomicBool,
    /// An injected processing failure, cleared by a subsequent parameter edit.
    flush_failure: AtomicBool,
    /// Arrangement of the main input bus.
    input_arrangement: AtomicU64,
    /// Arrangement of the main output bus.
    output_arrangement: AtomicU64,
    /// Delay lines, one per output channel, for the delay and echo kinds.
    delay_lines: Mutex<Vec<VecDeque<f32>>>,
    /// The host context from `initialize`.
    host: Mutex<Option<ComPtr<IHostApplication>>>,
}

impl Class for TestProcessor {
    type Interfaces = (IComponent, IAudioProcessor, IConnectionPoint);
}

impl TestProcessor {
    /// Creates the processor of a kind with unity gain.
    pub fn new(kind: Kind) -> Self {
        let arrangement = if kind == Kind::MonoOnly {
            SpeakerArr::kMono
        } else {
            SpeakerArr::kStereo
        };
        Self {
            kind,
            gain: AtomicU64::new(1.0_f64.to_bits()),
            parameters: array::from_fn(|_| AtomicU64::new(0)),
            reload_required: AtomicBool::new(false),
            flush_failure: AtomicBool::new(false),
            input_arrangement: AtomicU64::new(arrangement),
            output_arrangement: AtomicU64::new(arrangement),
            delay_lines: Mutex::new(Vec::new()),
            host: Mutex::new(None),
        }
    }

    /// Logs a call when this is a recorder.
    fn record(&self, name: &'static str) {
        record(self.kind == Kind::Recorder, name);
    }

    /// Returns the number of input buses: the main bus, and a sidechain for the echo.
    const fn input_bus_count(&self) -> i32 {
        if matches!(self.kind, Kind::Echo) { 2 } else { 1 }
    }

    /// Returns true when the kind accepts an arrangement for both main buses.
    const fn accepts(&self, input: SpeakerArrangement, output: SpeakerArrangement) -> bool {
        match self.kind {
            Kind::MonoOnly => input == SpeakerArr::kMono && output == SpeakerArr::kMono,
            Kind::StereoOnly => input == SpeakerArr::kStereo && output == SpeakerArr::kStereo,
            Kind::Gain
            | Kind::Delay
            | Kind::Echo
            | Kind::NotFinite
            | Kind::Recorder
            | Kind::Failing
            | Kind::ManyParameters
            | Kind::FlushFailing
            | Kind::ReloadRequired => input == output && (input == SpeakerArr::kMono || input == SpeakerArr::kStereo),
        }
    }

    /// Fills the delay lines with silence for the current output channel count.
    fn reset_delay_lines(&self) {
        let length = match self.kind {
            Kind::Delay => DELAY_LATENCY,
            Kind::Echo => ECHO_DELAY,
            _ => 0,
        } as usize;
        let channels = self.output_arrangement.load(Ordering::Relaxed).count_ones() as usize;
        *self.delay_lines.lock().unwrap_or_else(PoisonError::into_inner) = (0..channels)
            .map(|_| std::iter::repeat_n(0.0, length).collect())
            .collect();
    }

    /// Applies the last gain value in the parameter changes.
    fn apply_parameter_changes(&self, changes: *mut IParameterChanges) {
        // SAFETY: the host passes a valid parameter changes object or null.
        let Some(changes) = (unsafe { ComRef::from_raw(changes) }) else {
            return;
        };
        // SAFETY: the parameter changes object is valid for the duration of `process`.
        let count = unsafe { changes.getParameterCount() };
        for index in 0..count {
            // SAFETY: the index is in range, and the queue lives as long as the changes object.
            let Some(queue) = (unsafe { ComRef::from_raw(changes.getParameterData(index)) }) else {
                continue;
            };
            // SAFETY: the queue is valid.
            let id = unsafe { queue.getParameterId() };
            let mut offset = 0;
            let mut value = 0.0;
            // SAFETY: the queue is valid.
            let points = unsafe { queue.getPointCount() };
            if points == 0 {
                continue;
            }
            // SAFETY: the queue is valid, the index is in range, and both result slots live for the call.
            if unsafe { queue.getPoint(points - 1, &raw mut offset, &raw mut value) } == kResultOk {
                if id == GAIN_PARAMETER {
                    self.gain.store(value.to_bits(), Ordering::Relaxed);
                } else if id == FLUSH_FAILURE_PARAMETER {
                    self.flush_failure.store(value > 0.5, Ordering::Relaxed);
                } else if id == RELOAD_TRIGGER_PARAMETER && self.kind == Kind::ReloadRequired {
                    self.reload_required.store(true, Ordering::Relaxed);
                } else if let Some(parameter) = id
                    .checked_sub(BULK_PARAMETER_START)
                    .and_then(|index| self.parameters.get(index as usize))
                {
                    parameter.store(value.to_bits(), Ordering::Relaxed);
                }
            }
        }
    }

    /// Processes one sample of one channel.
    fn process_sample(&self, delay_lines: &mut [VecDeque<f32>], channel: usize, input: f32, gain: f32) -> f32 {
        let delayed = |lines: &mut [VecDeque<f32>]| {
            lines.get_mut(channel).map_or(0.0, |line| {
                line.push_back(input);
                line.pop_front().unwrap_or_default()
            })
        };
        match self.kind {
            Kind::Gain
            | Kind::MonoOnly
            | Kind::StereoOnly
            | Kind::Recorder
            | Kind::Failing
            | Kind::ManyParameters
            | Kind::FlushFailing
            | Kind::ReloadRequired => input * gain,
            Kind::Delay => delayed(delay_lines) * gain,
            Kind::Echo => ECHO_LEVEL.mul_add(delayed(delay_lines), input) * gain,
            Kind::NotFinite => f32::NAN,
        }
    }

    /// Sends the connected message to the controller.
    fn send_connected_message(&self, other: &ComRef<'_, IConnectionPoint>) {
        let host = self.host.lock().unwrap_or_else(PoisonError::into_inner).clone();
        let Some(host) = host else {
            return;
        };
        let mut class_id: TUID = IMessage::IID.map(u8::cast_signed);
        let mut interface_id = class_id;
        let mut object: *mut c_void = std::ptr::null_mut();
        // SAFETY: the host context is valid, both ids live for the call, and the result slot is writable.
        let result = unsafe { host.createInstance(&raw mut class_id, &raw mut interface_id, &raw mut object) };
        if result != kResultOk {
            return;
        }
        // SAFETY: on success the host returned an owned IMessage reference.
        let Some(message) = (unsafe { ComPtr::from_raw(object.cast::<IMessage>()) }) else {
            return;
        };
        // SAFETY: the message, its attribute list, and the controller are valid for the calls.
        unsafe {
            message.setMessageID(CONNECTED_MESSAGE.as_ptr());
            if let Some(attributes) = ComRef::from_raw(message.getAttributes()) {
                attributes.setInt(c"latency".as_ptr(), i64::from(DELAY_LATENCY));
            }
            other.notify(message.as_ptr());
        }
    }
}

impl IPluginBaseTrait for TestProcessor {
    unsafe fn initialize(&self, context: *mut FUnknown) -> tresult {
        self.record("component.initialize");
        // SAFETY: the host passes a valid context or null.
        let host = unsafe { ComRef::from_raw(context) }.and_then(|context| context.cast::<IHostApplication>());
        *self.host.lock().unwrap_or_else(PoisonError::into_inner) = host;
        kResultOk
    }

    unsafe fn terminate(&self) -> tresult {
        self.record("component.terminate");
        *self.host.lock().unwrap_or_else(PoisonError::into_inner) = None;
        kResultOk
    }
}

impl IComponentTrait for TestProcessor {
    unsafe fn getControllerClassId(&self, class_id: *mut TUID) -> tresult {
        if class_id.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes a writable class id, checked for null above.
        unsafe { *class_id = self.kind.controller_id() };
        kResultOk
    }

    unsafe fn setIoMode(&self, _mode: IoMode) -> tresult {
        kNotImplemented
    }

    unsafe fn getBusCount(&self, media_type: i32, direction: i32) -> i32 {
        if i64::from(media_type) != i64::from(MediaTypes_::kAudio) {
            return 0;
        }
        if i64::from(direction) == i64::from(BusDirections_::kInput) {
            self.input_bus_count()
        } else {
            1
        }
    }

    unsafe fn getBusInfo(&self, media_type: i32, direction: i32, index: i32, bus: *mut BusInfo) -> tresult {
        // SAFETY: the host passes a valid bus counts query.
        if bus.is_null() || index < 0 || index >= unsafe { self.getBusCount(media_type, direction) } {
            return kInvalidArgument;
        }
        let input = i64::from(direction) == i64::from(BusDirections_::kInput);
        let (arrangement, name, bus_type) = match (input, index) {
            (true, 0) => (
                self.input_arrangement.load(Ordering::Relaxed),
                "Input",
                BusTypes_::kMain,
            ),
            (true, _) => (SpeakerArr::kMono, "Sidechain", BusTypes_::kAux),
            (false, _) => (
                self.output_arrangement.load(Ordering::Relaxed),
                "Output",
                BusTypes_::kMain,
            ),
        };
        // SAFETY: the host passes a writable BusInfo, checked for null above.
        let bus = unsafe { &mut *bus };
        bus.mediaType = media_type;
        bus.direction = direction;
        bus.channelCount = i32::try_from(arrangement.count_ones()).unwrap_or_default();
        to_utf16(name, &mut bus.name);
        bus.busType = i32::try_from(i64::from(bus_type)).unwrap_or_default();
        bus.flags = if index == 0 {
            u32::try_from(i64::from(BusFlags_::kDefaultActive)).unwrap_or_default()
        } else {
            0
        };
        kResultOk
    }

    unsafe fn getRoutingInfo(
        &self,
        _input: *mut vst3::Steinberg::Vst::RoutingInfo,
        _output: *mut vst3::Steinberg::Vst::RoutingInfo,
    ) -> tresult {
        kNotImplemented
    }

    unsafe fn activateBus(&self, _media_type: i32, _direction: i32, _index: i32, _state: TBool) -> tresult {
        kResultOk
    }

    unsafe fn setActive(&self, state: TBool) -> tresult {
        self.record(if state == 0 {
            "component.setActive(false)"
        } else {
            "component.setActive(true)"
        });
        if state != 0 {
            self.reset_delay_lines();
        }
        kResultOk
    }

    unsafe fn setState(&self, state: *mut IBStream) -> tresult {
        self.record("component.setState");
        // SAFETY: the host passes a valid stream or null.
        let Some(stream) = (unsafe { ComRef::from_raw(state) }) else {
            return kInvalidArgument;
        };
        if self.kind == Kind::Failing {
            return kResultFalse;
        }
        let mut bytes = [0_u8; 8];
        let mut read = 0;
        // SAFETY: the stream is valid and the buffer holds the eight bytes requested.
        let result = unsafe { stream.read(bytes.as_mut_ptr().cast(), 8, &raw mut read) };
        if result != kResultOk || read != 8 {
            return kResultFalse;
        }
        self.gain.store(f64::from_le_bytes(bytes).to_bits(), Ordering::Relaxed);
        if self.kind == Kind::ManyParameters {
            for parameter in &self.parameters {
                // SAFETY: the stream is live and the buffer holds eight writable bytes.
                if unsafe { stream.read(bytes.as_mut_ptr().cast(), 8, &raw mut read) } != kResultOk || read != 8 {
                    return kResultFalse;
                }
                parameter.store(u64::from_le_bytes(bytes), Ordering::Relaxed);
            }
        }
        kResultOk
    }

    unsafe fn getState(&self, state: *mut IBStream) -> tresult {
        self.record("component.getState");
        // SAFETY: the host passes a valid stream or null.
        let Some(stream) = (unsafe { ComRef::from_raw(state) }) else {
            return kInvalidArgument;
        };
        let mut bytes = f64::from_bits(self.gain.load(Ordering::Relaxed)).to_le_bytes();
        if self.kind == Kind::Failing {
            let length = i32::try_from(FAILING_STATE_BYTES).unwrap_or_default();
            // SAFETY: the stream is valid and the buffer holds more than the bytes written.
            unsafe { stream.write(bytes.as_mut_ptr().cast(), length, std::ptr::null_mut()) };
            return kResultFalse;
        }
        // SAFETY: the stream is valid and the buffer holds the eight bytes written.
        let result = unsafe { stream.write(bytes.as_mut_ptr().cast(), 8, std::ptr::null_mut()) };
        if result != kResultOk {
            return result;
        }
        if self.kind == Kind::ManyParameters {
            for parameter in &self.parameters {
                bytes = parameter.load(Ordering::Relaxed).to_le_bytes();
                // SAFETY: the stream is live and the buffer contains eight readable bytes.
                let result = unsafe { stream.write(bytes.as_mut_ptr().cast(), 8, std::ptr::null_mut()) };
                if result != kResultOk {
                    return result;
                }
            }
        }
        kResultOk
    }
}

impl IAudioProcessorTrait for TestProcessor {
    unsafe fn setBusArrangements(
        &self,
        inputs: *mut SpeakerArrangement,
        input_count: i32,
        outputs: *mut SpeakerArrangement,
        output_count: i32,
    ) -> tresult {
        self.record("processor.setBusArrangements");
        if input_count != self.input_bus_count() || output_count != 1 || inputs.is_null() || outputs.is_null() {
            return kResultFalse;
        }
        // SAFETY: the host passes arrays with the counts checked above.
        let (input, output) = unsafe { (*inputs, *outputs) };
        if !self.accepts(input, output) {
            return kResultFalse;
        }
        self.input_arrangement.store(input, Ordering::Relaxed);
        self.output_arrangement.store(output, Ordering::Relaxed);
        kResultTrue
    }

    unsafe fn getBusArrangement(&self, direction: i32, index: i32, arrangement: *mut SpeakerArrangement) -> tresult {
        if arrangement.is_null() {
            return kInvalidArgument;
        }
        let input = i64::from(direction) == i64::from(BusDirections_::kInput);
        let value = match (input, index) {
            (true, 0) => self.input_arrangement.load(Ordering::Relaxed),
            (true, 1) if self.kind == Kind::Echo => SpeakerArr::kMono,
            (false, 0) => self.output_arrangement.load(Ordering::Relaxed),
            _ => return kInvalidArgument,
        };
        // SAFETY: the host passes a writable arrangement, checked for null above.
        unsafe { *arrangement = value };
        kResultOk
    }

    unsafe fn canProcessSampleSize(&self, symbolic_sample_size: i32) -> tresult {
        if i64::from(symbolic_sample_size) == i64::from(SymbolicSampleSizes_::kSample32) {
            kResultTrue
        } else {
            kResultFalse
        }
    }

    unsafe fn getLatencySamples(&self) -> u32 {
        if self.kind == Kind::Delay { DELAY_LATENCY } else { 0 }
    }

    unsafe fn setupProcessing(&self, setup: *mut ProcessSetup) -> tresult {
        self.record("processor.setupProcessing");
        if setup.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes a readable processing setup, checked for null above.
        let rate = unsafe { (*setup).sampleRate };
        if rate.is_finite() && rate > 0.0 {
            kResultOk
        } else {
            kResultFalse
        }
    }

    unsafe fn setProcessing(&self, state: TBool) -> tresult {
        self.record(if state == 0 {
            "processor.setProcessing(false)"
        } else {
            "processor.setProcessing(true)"
        });
        if state != 0 {
            self.reset_delay_lines();
        }
        kResultOk
    }

    unsafe fn process(&self, data: *mut ProcessData) -> tresult {
        if data.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host passes valid process data, checked for null above.
        let data = unsafe { &*data };
        self.record(if data.numSamples == 0 {
            "processor.process(0)"
        } else {
            "processor.process"
        });
        self.apply_parameter_changes(data.inputParameterChanges);
        let frames = usize::try_from(data.numSamples).unwrap_or_default();
        if self.kind == Kind::FlushFailing
            || self.flush_failure.load(Ordering::Relaxed)
            || (frames > 0 && (self.kind == Kind::Failing || self.reload_required.load(Ordering::Relaxed)))
        {
            return kResultFalse;
        }
        if frames == 0 || data.numOutputs < 1 || data.outputs.is_null() {
            return kResultOk;
        }
        // SAFETY: the host passes at least one output bus, checked above.
        let output = unsafe { &*data.outputs };
        let input = if data.numInputs > 0 && !data.inputs.is_null() {
            // SAFETY: the host passes at least one input bus, checked above.
            Some(unsafe { &*data.inputs })
        } else {
            None
        };
        let gain = f64::from_bits(self.gain.load(Ordering::Relaxed)) as f32;
        let mut delay_lines = self.delay_lines.lock().unwrap_or_else(PoisonError::into_inner);
        let output_channels = usize::try_from(output.numChannels).unwrap_or_default();
        let input_channels = input.map_or(0, |bus| usize::try_from(bus.numChannels).unwrap_or_default());
        for channel in 0..output_channels {
            // SAFETY: the bus has `numChannels` channel pointers, each to at least `frames` samples.
            let output_samples = unsafe { channel_samples(output, channel, frames) };
            let input_samples = input
                .filter(|_| input_channels > 0)
                // SAFETY: as above, for the input bus, with the channel index clamped to its channel count.
                .map(|bus| unsafe { channel_samples(bus, channel.min(input_channels - 1), frames) });
            for index in 0..frames {
                let sample = input_samples
                    .as_ref()
                    .and_then(|samples| samples.get(index).copied())
                    .unwrap_or_default();
                let processed = self.process_sample(&mut delay_lines, channel, sample, gain);
                if let Some(target) = output_samples.get_mut(index) {
                    *target = processed;
                }
            }
        }
        drop(delay_lines);
        kResultOk
    }

    unsafe fn getTailSamples(&self) -> u32 {
        if self.kind == Kind::Echo { ECHO_DELAY } else { kNoTail }
    }
}

impl IConnectionPointTrait for TestProcessor {
    unsafe fn connect(&self, other: *mut IConnectionPoint) -> tresult {
        self.record("component.connect");
        // SAFETY: the host passes a valid connection point or null.
        let Some(other) = (unsafe { ComRef::from_raw(other) }) else {
            return kInvalidArgument;
        };
        self.send_connected_message(&other);
        kResultOk
    }

    unsafe fn disconnect(&self, _other: *mut IConnectionPoint) -> tresult {
        self.record("component.disconnect");
        kResultOk
    }

    unsafe fn notify(&self, _message: *mut IMessage) -> tresult {
        kResultOk
    }
}

/// Returns the samples of one channel of a bus.
///
/// # Safety
///
/// The bus must have more than `channel` channel pointers,
/// each pointing to at least `frames` samples that nothing else accesses while the slice lives.
unsafe fn channel_samples<'a>(bus: &AudioBusBuffers, channel: usize, frames: usize) -> &'a mut [f32] {
    // SAFETY: the caller guarantees the channel pointer array and the channel buffer are valid.
    unsafe {
        let pointer = *bus.__field0.channelBuffers32.add(channel);
        std::slice::from_raw_parts_mut(pointer, frames)
    }
}
