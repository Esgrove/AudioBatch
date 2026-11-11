//! Single-component VST3 lifecycle, block-rate automation, and alias-safe raw host sample access.
//!
//! Mutable DSP has exclusive atomic admission. Sound state uses independent atomics,
//! and detached window geometry uses a separate short lock that audio processing never enters.

#![expect(
    unsafe_code,
    reason = "implements VST3 lifecycle and the raw audio-buffer ABI boundary"
)]

use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use vst3::Steinberg::Vst::{
    BusDirections_, BusInfo, BusInfo_::BusFlags_, BusTypes_, IAudioProcessor, IAudioProcessorTrait, IComponent,
    IComponentHandler, IComponentTrait, IEditController, IParamValueQueueTrait as _, IParameterChanges,
    IParameterChangesTrait as _, IProcessContextRequirements, IProcessContextRequirementsTrait, IoMode, MediaTypes_,
    ProcessData, ProcessModes_, ProcessSetup, RoutingInfo, SpeakerArr, SpeakerArrangement, SymbolicSampleSizes_,
};
use vst3::Steinberg::{
    FUnknown, IBStream, IPluginBaseTrait, TBool, TUID, kInvalidArgument, kNotImplemented, kResultFalse, kResultOk,
    kResultTrue, tresult,
};
use vst3::{Class, ComPtr, ComRef};

use super::enums::{to_i32, to_u32};
use super::state;
use crate::runtime::Instance;
use crate::strings_utf16;
use notifications::LatencyNotifications;

#[path = "notifications.rs"]
mod notifications;

#[cfg(test)]
#[path = "component_tests.rs"]
mod component_tests;

/// Bounds queue traversal independently of malformed host counts.
const MAXIMUM_PARAMETER_QUEUES: i32 = 1024;

/// One COM object implements both processor and controller with a shared parameter allocation.
pub(super) struct Component {
    /// Format-independent sound, monitoring, geometry, and exclusive processing ownership.
    instance: Instance,
    /// UI-only shared COM owner. Arc copies execute no host reference-count callbacks under the lock.
    /// Installation, replacement, termination, and UI calls release COM owners outside this mutex.
    pub(super) handler: Arc<Mutex<Option<Arc<ComPtr<IComponentHandler>>>>>,
    /// Coalesced host restart requests, delivered only by SDK-defined UI lifecycle calls.
    latency_notifications: LatencyNotifications,
    /// Matching main input/output arrangement.
    arrangement: AtomicU64,
    /// Largest accepted host block, zero before setup.
    maximum_frames: AtomicI32,
    /// Prepared latency readable without entering DSP.
    latency_samples: AtomicU32,
    /// Prepared finite ringing beyond the latency-compensated endpoint.
    tail_samples: AtomicU32,
    /// Whether the host activated the audio component.
    active: AtomicBool,
}

impl std::ops::Deref for Component {
    type Target = Instance;

    /// Shares format-independent fields without duplicating parameter or DSP ownership.
    fn deref(&self) -> &Instance {
        &self.instance
    }
}

impl Class for Component {
    type Interfaces = (
        IComponent,
        IAudioProcessor,
        IEditController,
        IProcessContextRequirements,
    );
}

impl Component {
    /// Allocates transport and controls before exposing any COM interface to the host.
    pub(super) fn new() -> Self {
        Self {
            instance: Instance::new(),
            handler: Arc::new(Mutex::new(None)),
            latency_notifications: LatencyNotifications::new(),
            arrangement: AtomicU64::new(SpeakerArr::kStereo),
            maximum_frames: AtomicI32::new(0),
            latency_samples: AtomicU32::new(0),
            tail_samples: AtomicU32::new(0),
            active: AtomicBool::new(false),
        }
    }

    /// Flushes detached latency restart requests without retaining mutable DSP or handler-lock ownership.
    ///
    /// # Safety
    /// The caller must be a host UI-thread callback, as annotated in the native VST3 SDK.
    pub(super) unsafe fn flush_latency_notifications(&self) {
        // SAFETY: the caller provides the SDK UI-thread context. No mutable DSP admission is held here.
        unsafe { self.latency_notifications.flush(&self.handler) };
    }

    /// Applies each queue's final finite point at block rate, including zero-sample flushes.
    fn apply_automation(&self, changes: *mut IParameterChanges) {
        // SAFETY: VST3 provides a valid changes object for this callback, or null.
        let Some(changes) = (unsafe { ComRef::from_raw(changes) }) else {
            return;
        };
        // SAFETY: the changes object remains alive until process returns.
        let count = unsafe { changes.getParameterCount() }.clamp(0, MAXIMUM_PARAMETER_QUEUES);
        for index in 0..count {
            // SAFETY: the requested queue is within the host-reported count.
            let Some(queue) = (unsafe { ComRef::from_raw(changes.getParameterData(index)) }) else {
                continue;
            };
            // SAFETY: queue metadata is valid for this processing call.
            let (id, count) = unsafe { (queue.getParameterId(), queue.getPointCount()) };
            if count <= 0 {
                continue;
            }
            let mut offset = 0;
            let mut value = 0.0;
            // SAFETY: the final point index is in range and both output slots are writable.
            if unsafe { queue.getPoint(count - 1, &raw mut offset, &raw mut value) } == kResultOk {
                self.parameters.set(id, value);
            }
        }
    }
}

impl IProcessContextRequirementsTrait for Component {
    unsafe fn getProcessContextRequirements(&self) -> u32 {
        // Audio processing needs no transport, musical-position, or clock context.
        0
    }
}

impl IPluginBaseTrait for Component {
    unsafe fn initialize(&self, _context: *mut FUnknown) -> tresult {
        kResultOk
    }

    unsafe fn terminate(&self) -> tresult {
        self.active.store(false, Ordering::Release);
        self.shared.set_processing_active(false);
        self.shared.invalidate_monitoring();
        self.latency_notifications.clear();
        let old = self
            .handler
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .take();
        drop(old);
        kResultOk
    }
}

impl IComponentTrait for Component {
    unsafe fn getControllerClassId(&self, _class_id: *mut TUID) -> tresult {
        // The controller is obtained through queryInterface on this same object.
        kNotImplemented
    }

    unsafe fn setIoMode(&self, _mode: IoMode) -> tresult {
        kResultOk
    }

    unsafe fn getBusCount(&self, media: i32, direction: i32) -> i32 {
        i32::from(valid_bus(media, direction, 0))
    }

    unsafe fn getBusInfo(&self, media: i32, direction: i32, index: i32, bus: *mut BusInfo) -> tresult {
        if !valid_bus(media, direction, index) || bus.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides one writable bus description.
        let bus = unsafe { &mut *bus };
        bus.mediaType = media;
        bus.direction = direction;
        bus.channelCount = self.arrangement.load(Ordering::Relaxed).count_ones().cast_signed();
        strings_utf16(if direction == 0 { "Input" } else { "Output" }, &mut bus.name);
        bus.busType = to_i32(BusTypes_::kMain);
        bus.flags = to_u32(BusFlags_::kDefaultActive);
        kResultOk
    }

    unsafe fn getRoutingInfo(&self, _input: *mut RoutingInfo, _output: *mut RoutingInfo) -> tresult {
        kNotImplemented
    }

    unsafe fn activateBus(&self, media: i32, direction: i32, index: i32, _state: TBool) -> tresult {
        if valid_bus(media, direction, index) {
            kResultOk
        } else {
            kInvalidArgument
        }
    }

    unsafe fn setActive(&self, state: TBool) -> tresult {
        if state != 0 && self.maximum_frames.load(Ordering::Relaxed) <= 0 {
            return kResultFalse;
        }
        if state != 0
            && self
                .audio
                .try_with(|imager| {
                    self.parameters.publish(&self.shared);
                    imager.reset();
                })
                .is_none()
        {
            return kResultFalse;
        }
        self.active.store(state != 0, Ordering::Release);
        if state == 0 {
            self.shared.set_processing_active(false);
        }
        self.shared.invalidate_monitoring();
        kResultOk
    }

    unsafe fn setState(&self, stream: *mut IBStream) -> tresult {
        // SAFETY: the host supplies a live stream for this non-audio callback, or null.
        if unsafe { state::restore(stream, &self.parameters, &self.shared, &self.geometry) }.is_ok() {
            kResultOk
        } else {
            kResultFalse
        }
    }

    unsafe fn getState(&self, stream: *mut IBStream) -> tresult {
        // SAFETY: the host supplies a writable stream for this non-audio callback, or null.
        if unsafe { state::save(stream, &self.parameters, &self.shared, &self.geometry) }.is_ok() {
            kResultOk
        } else {
            kResultFalse
        }
    }
}

impl IAudioProcessorTrait for Component {
    unsafe fn setBusArrangements(
        &self,
        inputs: *mut SpeakerArrangement,
        input_count: i32,
        outputs: *mut SpeakerArrangement,
        output_count: i32,
    ) -> tresult {
        if self.active.load(Ordering::Relaxed)
            || input_count != 1
            || output_count != 1
            || inputs.is_null()
            || outputs.is_null()
        {
            return kResultFalse;
        }
        // SAFETY: the host provides one readable arrangement in each non-null array.
        let (input, output) = unsafe { (inputs.read(), outputs.read()) };
        if input != output || ![SpeakerArr::kMono, SpeakerArr::kStereo].contains(&input) {
            return kResultFalse;
        }
        let channels = input.count_ones() as usize;
        if self.audio.try_with(|imager| imager.set_channels(channels)).is_none() {
            return kResultFalse;
        }
        self.arrangement.store(input, Ordering::Relaxed);
        kResultTrue
    }

    unsafe fn getBusArrangement(&self, direction: i32, index: i32, arrangement: *mut SpeakerArrangement) -> tresult {
        if !valid_bus(to_i32(MediaTypes_::kAudio), direction, index) || arrangement.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides one writable arrangement output.
        unsafe { arrangement.write(self.arrangement.load(Ordering::Relaxed)) };
        kResultOk
    }

    unsafe fn canProcessSampleSize(&self, size: i32) -> tresult {
        if size == to_i32(SymbolicSampleSizes_::kSample32) {
            kResultTrue
        } else {
            kResultFalse
        }
    }

    unsafe fn getLatencySamples(&self) -> u32 {
        self.latency_samples.load(Ordering::Relaxed)
    }

    unsafe fn setupProcessing(&self, setup: *mut ProcessSetup) -> tresult {
        if setup.is_null() || self.active.load(Ordering::Relaxed) {
            return kInvalidArgument;
        }
        // SAFETY: the host provides a readable setup for this lifecycle call.
        let setup = unsafe { &*setup };
        if !setup.sampleRate.is_finite()
            || setup.sampleRate <= 0.0
            || setup.maxSamplesPerBlock <= 0
            || setup.symbolicSampleSize != to_i32(SymbolicSampleSizes_::kSample32)
            || ![
                to_i32(ProcessModes_::kRealtime),
                to_i32(ProcessModes_::kPrefetch),
                to_i32(ProcessModes_::kOffline),
            ]
            .contains(&setup.processMode)
        {
            return kResultFalse;
        }
        // Preparation may allocate and design kernels, but occurs before real-time callbacks begin.
        let Some(prepared) = self.instance.prepare(
            setup.sampleRate,
            self.arrangement.load(Ordering::Relaxed).count_ones() as usize,
            setup.processMode == to_i32(ProcessModes_::kOffline),
            setup.maxSamplesPerBlock as u32,
        ) else {
            return kResultFalse;
        };
        let previous_latency = self.latency_samples.swap(prepared.latency_samples, Ordering::Relaxed);
        self.tail_samples.store(prepared.tail_samples, Ordering::Relaxed);
        self.maximum_frames.store(setup.maxSamplesPerBlock, Ordering::Relaxed);
        self.shared.set_maximum_block_frames(setup.maxSamplesPerBlock as u32);
        self.shared.set_processing_active(false);
        self.shared.invalidate_monitoring();
        if previous_latency != prepared.latency_samples {
            self.latency_notifications.request();
        }
        // SAFETY: ivstaudioprocessor.h annotates setupProcessing as UI-thread, matching restartComponent.
        // Preparation has released mutable DSP admission and every prepared report is published before reentry.
        unsafe { self.flush_latency_notifications() };
        kResultOk
    }

    unsafe fn setProcessing(&self, state: TBool) -> tresult {
        // Both transitions participate in admission so reset never aliases an overlapping callback.
        if self
            .audio
            .try_with(|imager| {
                if state != 0 {
                    self.parameters.publish(&self.shared);
                    imager.reset();
                }
            })
            .is_some()
        {
            self.shared.set_processing_active(state != 0);
            self.shared.invalidate_monitoring();
            kResultOk
        } else {
            kResultFalse
        }
    }

    unsafe fn process(&self, data: *mut ProcessData) -> tresult {
        if data.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: the host provides process metadata valid until callback return.
        let data = unsafe { &*data };
        if data.numSamples < 0
            || data.numSamples > self.maximum_frames.load(Ordering::Relaxed)
            || data.symbolicSampleSize != to_i32(SymbolicSampleSizes_::kSample32)
        {
            return kInvalidArgument;
        }
        self.apply_automation(data.inputParameterChanges);
        if data.numSamples == 0 {
            return kResultOk;
        }
        if !self.active.load(Ordering::Relaxed)
            || data.numInputs != 1
            || data.numOutputs != 1
            || data.inputs.is_null()
            || data.outputs.is_null()
        {
            return kInvalidArgument;
        }
        // Copy bus metadata by value, creating no Rust references that could alias in-place bus writes.
        // SAFETY: the host supplies one initialized input and output bus. They may be the same allocation.
        let (input, output) = unsafe { (data.inputs.read(), data.outputs.read()) };
        let channels = self.arrangement.load(Ordering::Relaxed).count_ones() as usize;
        if input.numChannels != i32::try_from(channels).unwrap_or_default()
            || output.numChannels != i32::try_from(channels).unwrap_or_default()
        {
            return kInvalidArgument;
        }
        // SAFETY: validated sample32 buses select the channelBuffers32 union member.
        let (inputs, outputs) = unsafe { (input.__field0.channelBuffers32, output.__field0.channelBuffers32) };
        if inputs.is_null() || outputs.is_null() {
            return kInvalidArgument;
        }
        // SAFETY: each channel array contains at least the declared first channel.
        let (left_input, left_output) = unsafe { (inputs.read(), outputs.read()) };
        let (right_input, right_output) = if channels == 2 {
            // SAFETY: stereo arrays contain exactly two or more channel entries.
            unsafe { (inputs.add(1).read(), outputs.add(1).read()) }
        } else {
            (std::ptr::null_mut(), std::ptr::null_mut())
        };
        let left_silent = input.silenceFlags & 1 != 0;
        let right_silent = input.silenceFlags & 2 != 0;
        if left_output.is_null()
            || (!left_silent && left_input.is_null())
            || (channels == 2 && (right_output.is_null() || (!right_silent && right_input.is_null())))
        {
            return kInvalidArgument;
        }
        if self
            .audio
            .try_with(|imager| {
                self.parameters.publish(&self.shared);
                imager.begin_block();
                for index in 0..data.numSamples as usize {
                    let left = if left_silent {
                        0.0
                    } else {
                        // SAFETY: non-silent channel buffers cover numSamples readable samples.
                        unsafe { left_input.add(index).read() }
                    };
                    let right = if channels == 2 && !right_silent {
                        // SAFETY: the second non-silent channel covers numSamples readable samples.
                        unsafe { right_input.add(index).read() }
                    } else {
                        left
                    };
                    // Read both inputs before writing either output, supporting legal in-place sample aliasing.
                    let [left, right] = imager.process_frame([
                        left,
                        if channels == 1 {
                            left
                        } else if right_silent {
                            0.0
                        } else {
                            right
                        },
                    ]);
                    // SAFETY: output channels cover numSamples writable samples, with no live Rust sample references.
                    unsafe {
                        left_output.add(index).write(left);
                        if channels == 2 {
                            right_output.add(index).write(right);
                        }
                    }
                }
            })
            .is_none()
        {
            return kResultFalse;
        }
        // SAFETY: the host grants writable output bus metadata, and bus copies hold no references into it.
        unsafe { (*data.outputs).silenceFlags = 0 };
        kResultOk
    }

    unsafe fn getTailSamples(&self) -> u32 {
        self.tail_samples.load(Ordering::Relaxed)
    }
}

/// Validates supported main audio buses and rejects MIDI, auxiliary, and invalid direction queries.
fn valid_bus(media: i32, direction: i32, index: i32) -> bool {
    media == to_i32(MediaTypes_::kAudio)
        && index == 0
        && (direction == to_i32(BusDirections_::kInput) || direction == to_i32(BusDirections_::kOutput))
}
