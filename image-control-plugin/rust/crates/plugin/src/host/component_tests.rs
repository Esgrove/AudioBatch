//! Headless VST3 latency notifications, raw callback aliasing, lifecycle admission, and detached state contracts.
//!
//! Bounded ABI and notification cases are Miri-compatible. Full FIR preparation is exercised natively.

#![expect(
    unsafe_code,
    reason = "exercises raw VST3 calls with live bounded sample and metadata allocations"
)]

use std::cell::{Cell, RefCell};
use std::rc::{Rc, Weak};
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::thread::{self, ThreadId};

use image_control_dsp::Imager;
use vst3::Steinberg::Vst::{
    AudioBusBuffers, AudioBusBuffers__type0, IAudioProcessorTrait as _, IComponentHandler, IComponentHandlerTrait,
    IComponentHandlerVtbl, IComponentTrait as _, IEditControllerTrait as _, ProcessData, ProcessModes_, ProcessSetup,
    RestartFlags_, SpeakerArr, SymbolicSampleSizes_,
};
use vst3::Steinberg::{
    FUnknown, FUnknownVtbl, IPluginBaseTrait as _, TUID, kInvalidArgument, kResultFalse, kResultOk, tresult,
};
use vst3::{Class, ComPtr, ComWrapper};

use super::super::audio::AudioState;
use super::super::enums::to_i32;
use super::Component;
use crate::params::GAIN_PARAMETER;

/// Builds fully allocated zero-initialized scalar callback metadata without invoking native DSP preparation.
fn process_data(bus: *mut AudioBusBuffers, frames: i32) -> ProcessData {
    // SAFETY: every field is an integer, float, raw pointer, or nullable C handle, for which zero is valid.
    let mut data: ProcessData = unsafe { std::mem::zeroed() };
    data.processMode = to_i32(ProcessModes_::kRealtime);
    data.symbolicSampleSize = to_i32(SymbolicSampleSizes_::kSample32);
    data.numSamples = frames;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = bus;
    data.outputs = bus;
    data
}

#[test]
#[cfg_attr(
    miri,
    ignore = "full FIR preparation and native worker creation are verified natively"
)]
fn mono_arrangement_after_setup_updates_the_prepared_audio_layout() {
    let component = Component::new();
    let mut setup = ProcessSetup {
        processMode: to_i32(ProcessModes_::kOffline),
        symbolicSampleSize: to_i32(SymbolicSampleSizes_::kSample32),
        maxSamplesPerBlock: 256,
        sampleRate: 48_000.0,
    };
    // JUCE hosts may prepare before publishing their final speaker arrangements.
    // SAFETY: setup and both arrangement slots remain live for the complete lifecycle calls.
    unsafe {
        assert_eq!(component.setupProcessing(&raw mut setup), kResultOk);
        let mut input = SpeakerArr::kMono;
        let mut output = SpeakerArr::kMono;
        assert_eq!(
            component.setBusArrangements(&raw mut input, 1, &raw mut output, 1),
            kResultOk
        );
        let pan = crate::params::band_parameter_id(0, 4).expect("Pan");
        for band in 0..4 {
            component.parameters.set(pan + band * 6, 1.0);
        }
        assert_eq!(component.setActive(1), kResultOk);
        assert_eq!(component.setProcessing(1), kResultOk);
        let mut samples = [0.0_f32; 256];
        let mut channels = [samples.as_mut_ptr()];
        let mut bus = AudioBusBuffers {
            numChannels: 1,
            silenceFlags: 0,
            __field0: AudioBusBuffers__type0 {
                channelBuffers32: channels.as_mut_ptr(),
            },
        };
        for block in 0..20 {
            samples.fill(0.0);
            if block == 0 {
                *samples.first_mut().expect("first sample") = 0.75;
            }
            let mut data = process_data(&raw mut bus, 256);
            data.processMode = to_i32(ProcessModes_::kOffline);
            assert_eq!(component.process(&raw mut data), kResultOk);
            for (offset, sample) in samples.iter().enumerate() {
                let expected: f32 = if block * 256 + offset == 4352 { 0.75 } else { 0.0 };
                assert_eq!(sample.to_bits(), expected.to_bits());
            }
        }
        assert_eq!(component.setProcessing(0), kResultOk);
        assert_eq!(component.setActive(0), kResultOk);
    }
}

#[test]
fn identical_input_output_bus_and_sample_arrays_are_alias_safe() {
    let component = Component::new();
    // This non-prepared processor's direct neutral fallback exercises the real ABI sample access cheaply under Miri.
    component.maximum_frames.store(4, Ordering::Relaxed);
    component.active.store(true, Ordering::Relaxed);
    let mut left = [0.25_f32, -0.0, -0.75, 1.0];
    let mut right = [-0.5_f32, 0.0, 0.125, -1.0];
    let expected_left = left.map(f32::to_bits);
    let expected_right = right.map(f32::to_bits);
    let mut channels = [left.as_mut_ptr(), right.as_mut_ptr()];
    let mut bus = AudioBusBuffers {
        numChannels: 2,
        silenceFlags: 0,
        __field0: AudioBusBuffers__type0 {
            channelBuffers32: channels.as_mut_ptr(),
        },
    };
    let mut data = process_data(&raw mut bus, 4);
    // SAFETY: both bus pointers and channel buffers deliberately alias exactly as legal VST3 in-place buffers do.
    // All metadata and sample storage remain allocated and exclusively available for this callback.
    assert_eq!(unsafe { component.process(&raw mut data) }, kResultOk);
    assert_eq!(left.map(f32::to_bits), expected_left);
    assert_eq!(right.map(f32::to_bits), expected_right);
}

#[test]
fn silent_null_input_channels_do_not_get_dereferenced() {
    let component = Component::new();
    component.maximum_frames.store(4, Ordering::Relaxed);
    component.active.store(true, Ordering::Relaxed);
    let mut left = [1.0_f32; 4];
    let mut right = [1.0_f32; 4];
    let mut inputs = [std::ptr::null_mut(); 2];
    let mut outputs = [left.as_mut_ptr(), right.as_mut_ptr()];
    let mut input = AudioBusBuffers {
        numChannels: 2,
        silenceFlags: 3,
        __field0: AudioBusBuffers__type0 {
            channelBuffers32: inputs.as_mut_ptr(),
        },
    };
    let mut output = AudioBusBuffers {
        numChannels: 2,
        silenceFlags: 3,
        __field0: AudioBusBuffers__type0 {
            channelBuffers32: outputs.as_mut_ptr(),
        },
    };
    let mut data = process_data(&raw mut input, 4);
    data.outputs = &raw mut output;
    // SAFETY: silent flags permit null input channel samples, and writable output arrays cover the whole block.
    assert_eq!(unsafe { component.process(&raw mut data) }, kResultOk);
    assert!(left.into_iter().chain(right).all(|sample| sample.abs() < f32::EPSILON));
    assert_eq!(output.silenceFlags, 0);
    input.silenceFlags = 0;
    let mut data = process_data(&raw mut input, 4);
    data.outputs = &raw mut output;
    // SAFETY: the arrays remain live, and invalid non-silent null samples must be rejected before access.
    assert_eq!(unsafe { component.process(&raw mut data) }, kInvalidArgument);
}

#[test]
fn overlapping_lifecycle_reset_and_process_are_rejected_without_waiting() {
    let component = Component::new();
    component.maximum_frames.store(1, Ordering::Relaxed);
    component.active.store(true, Ordering::Relaxed);
    let mut samples = [0.25_f32; 1];
    let mut channels = [samples.as_mut_ptr(); 2];
    let mut bus = AudioBusBuffers {
        numChannels: 2,
        silenceFlags: 0,
        __field0: AudioBusBuffers__type0 {
            channelBuffers32: channels.as_mut_ptr(),
        },
    };
    let mut data = process_data(&raw mut bus, 1);
    component
        .audio
        .try_with(|_| {
            // SAFETY: all process storage is valid, but admission must reject access before any mutable DSP alias occurs.
            unsafe {
                assert_eq!(component.process(&raw mut data), kResultFalse);
                assert_eq!(component.setProcessing(1), kResultFalse);
                assert_eq!(component.setProcessing(0), kResultFalse);
            }
        })
        .expect("outer admission");
    // SAFETY: no outer admission is held, so reset can obtain exclusive DSP ownership.
    assert_eq!(unsafe { component.setProcessing(1) }, kResultOk);
}

#[test]
fn detached_restore_and_snapshot_overlap_audio_ownership_without_dsp_aliases() {
    let component = Arc::new(Component::new());
    let audio = Arc::clone(&component);
    let worker = std::thread::spawn(move || {
        for _ in 0..8 {
            audio
                .audio
                .try_with(|imager| {
                    audio.parameters.publish(&audio.shared);
                    imager.begin_block();
                    for _ in 0..16 {
                        assert!(imager.process_frame([0.25, -0.25]).into_iter().all(f32::is_finite));
                    }
                })
                .expect("audio owner");
        }
    });
    for _ in 0..8 {
        let mut snapshot = component.parameters.snapshot();
        snapshot.gain = 0.25;
        assert!(component.parameters.restore(snapshot));
        assert!(component.parameters.snapshot().is_valid());
        assert_eq!(
            component.parameters.get(GAIN_PARAMETER).expect("gain").to_bits(),
            0.25_f64.to_bits()
        );
    }
    worker.join().expect("audio thread");
    assert!(component.audio.try_with(Imager::reset).is_some());
}

#[test]
fn admission_releases_after_nested_rejection_and_unwind() {
    let audio = AudioState::new(Imager::new());
    assert_eq!(audio.try_with(|_| audio.try_with(|_| ())), Some(None));
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        audio.try_with(|_| panic!("exercise admission guard unwinding"));
    }));
    assert!(result.is_err());
    assert!(audio.try_with(Imager::reset).is_some());
}

/// A host reentry hook is cloned outside its short evidence borrow before production code runs.
type RestartAction = Rc<dyn Fn(&Component)>;

/// One restart records the completely published lifecycle report and its actual callback thread.
#[derive(Debug)]
struct RestartEvent {
    /// Exact SDK restart bitmask received from the component.
    flags: i32,
    /// Published group delay at callback entry.
    latency: u32,
    /// Published finite tail at callback entry.
    tail: u32,
    /// Published maximum block size at callback entry.
    maximum_frames: i32,
    /// Host-designated UI callback thread, independently of component construction.
    thread: ThreadId,
}

/// Shared observations remain live after temporary COM owners and replaced handlers are released.
struct RestartEvidence {
    /// Completed callback-entry observations.
    events: RefCell<Vec<RestartEvent>>,
    /// Optional host reentry executed without retaining an evidence borrow.
    action: RefCell<Option<RestartAction>>,
    /// Number of host restart calls currently on the stack.
    depth: Cell<usize>,
    /// Largest nested restart depth observed.
    maximum_depth: Cell<usize>,
    /// Whether any callback encountered the plugin's handler mutex locked.
    locked: Cell<bool>,
    /// Whether any callback encountered mutable DSP admission held.
    audio_busy: Cell<bool>,
    /// Result returned to the plugin, including explicit host rejection.
    result: Cell<tresult>,
}

impl Default for RestartEvidence {
    /// Starts with a successful idle host and empty independent observations.
    fn default() -> Self {
        Self {
            events: RefCell::new(Vec::new()),
            action: RefCell::new(None),
            depth: Cell::new(0),
            maximum_depth: Cell::new(0),
            locked: Cell::new(false),
            audio_busy: Cell::new(false),
            result: Cell::new(kResultOk),
        }
    }
}

/// A real headless COM handler observes reports and can synchronously reenter host-facing lifecycle calls.
struct RecordingHandler {
    /// Weak ownership prevents a cycle through the component's retained handler.
    component: Weak<Component>,
    /// Independent host observations and reentry script.
    evidence: Rc<RestartEvidence>,
}

impl Class for RecordingHandler {
    type Interfaces = (IComponentHandler,);
}

impl IComponentHandlerTrait for RecordingHandler {
    unsafe fn beginEdit(&self, _: u32) -> tresult {
        kResultOk
    }

    unsafe fn performEdit(&self, _: u32, _: f64) -> tresult {
        kResultOk
    }

    unsafe fn endEdit(&self, _: u32) -> tresult {
        kResultOk
    }

    unsafe fn restartComponent(&self, flags: i32) -> tresult {
        let Some(component) = self.component.upgrade() else {
            return kResultFalse;
        };
        self.evidence.depth.set(self.evidence.depth.get() + 1);
        self.evidence
            .maximum_depth
            .set(self.evidence.maximum_depth.get().max(self.evidence.depth.get()));
        self.evidence
            .locked
            .set(self.evidence.locked.get() || component.handler.try_lock().is_err());
        self.evidence
            .audio_busy
            .set(self.evidence.audio_busy.get() || component.audio.try_with(|_| ()).is_none());
        // SAFETY: the component remains live on this host UI callback's stack, and report getters borrow no DSP.
        let (latency, tail) = unsafe { (component.getLatencySamples(), component.getTailSamples()) };
        self.evidence.events.borrow_mut().push(RestartEvent {
            flags,
            latency,
            tail,
            maximum_frames: component.maximum_frames.load(Ordering::Relaxed),
            thread: thread::current().id(),
        });
        let action = self.evidence.action.borrow().clone();
        if !self.evidence.locked.get()
            && !self.evidence.audio_busy.get()
            && let Some(action) = action
        {
            action(&component);
        }
        self.evidence.depth.set(self.evidence.depth.get() - 1);
        self.evidence.result.get()
    }
}

/// Creates a retained headless host handler without opening a view or requiring editor features.
fn recording_handler(component: &Rc<Component>, evidence: &Rc<RestartEvidence>) -> ComPtr<IComponentHandler> {
    ComWrapper::new(RecordingHandler {
        component: Rc::downgrade(component),
        evidence: Rc::clone(evidence),
    })
    .to_com_ptr::<IComponentHandler>()
    .expect("recording host handler interface")
}

/// Supplies a supported stopped offline setup, leaving native DSP allocation outside all realtime callbacks.
fn offline_setup(sample_rate: f64) -> ProcessSetup {
    ProcessSetup {
        processMode: to_i32(ProcessModes_::kOffline),
        symbolicSampleSize: to_i32(SymbolicSampleSizes_::kSample32),
        maxSamplesPerBlock: 256,
        sampleRate: sample_rate,
    }
}

/// Requires host callbacks to run after both DSP admission and the handler lock have been released.
fn assert_restart_safety(evidence: &RestartEvidence) {
    assert!(!evidence.locked.get(), "restart ran under the handler mutex");
    assert!(!evidence.audio_busy.get(), "restart ran under mutable DSP admission");
    assert!(
        evidence.maximum_depth.get() <= 1,
        "restart delivery recursively entered host code"
    );
    assert!(
        evidence.events.borrow().iter().all(|event| {
            event.flags == to_i32(RestartFlags_::kLatencyChanged) && event.thread == thread::current().id()
        }),
        "restart flags or host UI callback context differed"
    );
}

#[test]
#[cfg_attr(miri, ignore = "native FIR preparation is covered by stopped lifecycle tests")]
fn latency_notifications_cover_changed_unchanged_and_failed_preparation_without_an_editor() {
    let construction = thread::spawn(|| (Component::new(), thread::current().id()));
    let (component, construction_thread) = construction.join().expect("component construction worker");
    assert_ne!(construction_thread, thread::current().id());
    let component = Rc::new(component);
    let evidence = Rc::new(RestartEvidence::default());
    let handler = recording_handler(&component, &evidence);
    let mut setup = offline_setup(48_000.0);
    // SAFETY: this is the host-designated UI lifecycle thread and every setup and handler remains live.
    unsafe {
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert!(evidence.events.borrow().is_empty());
        assert_eq!(component.setupProcessing(&raw mut setup), kResultOk);
        assert_eq!(evidence.events.borrow().len(), 1);
        assert_eq!(component.setupProcessing(&raw mut setup), kResultOk);
        assert_eq!(evidence.events.borrow().len(), 1, "same latency must not restart");
        setup.sampleRate = 96_000.0;
        setup.maxSamplesPerBlock = 512;
        assert_eq!(component.setupProcessing(&raw mut setup), kResultOk);
        assert_eq!(evidence.events.borrow().len(), 2);
        setup.sampleRate = f64::NAN;
        assert_eq!(component.setupProcessing(&raw mut setup), kResultFalse);
        setup.sampleRate = 3_000_000.0;
        assert_eq!(component.setupProcessing(&raw mut setup), kResultFalse);
        assert_eq!(component.setupProcessing(std::ptr::null_mut()), kInvalidArgument);
        setup.sampleRate = 192_000.0;
        component
            .audio
            .try_with(|_| assert_eq!(component.setupProcessing(&raw mut setup), kResultFalse))
            .expect("exclusive outer DSP admission");
        assert_eq!(component.getLatencySamples(), 8448);
        assert_eq!(component.getTailSamples(), 8192);
        assert_eq!(component.setActive(1), kResultOk);
        assert_eq!(component.setupProcessing(&raw mut setup), kInvalidArgument);
        assert_eq!(component.setActive(0), kResultOk);
    }
    let events = evidence.events.borrow();
    assert_eq!(events.len(), 2, "rejected or failed setup must never notify");
    assert_eq!(
        (
            events.first().expect("first report").latency,
            events.first().expect("first report").tail
        ),
        (4352, 4096)
    );
    let changed = events.last().expect("changed report");
    assert_eq!(
        (changed.latency, changed.tail, changed.maximum_frames),
        (8448, 8192, 512)
    );
    drop(events);
    assert_restart_safety(&evidence);
}

#[test]
#[cfg_attr(miri, ignore = "native FIR preparation is covered by stopped lifecycle tests")]
fn absent_handler_coalesces_latency_changes_and_installation_flushes_once_without_an_editor() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(RestartEvidence::default());
    let handler = recording_handler(&component, &evidence);
    // SAFETY: all lifecycle calls run on the host UI context with live stack-owned setup metadata.
    unsafe {
        for rate in [48_000.0, 96_000.0, 192_000.0] {
            let mut setup = offline_setup(rate);
            assert_eq!(component.setupProcessing(&raw mut setup), kResultOk);
        }
        assert_eq!(component.setComponentHandler(std::ptr::null_mut()), kResultOk);
        assert!(evidence.events.borrow().is_empty());
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
    }
    let events = evidence.events.borrow();
    assert_eq!(events.len(), 1);
    assert_eq!(events.first().expect("coalesced report").latency, 16_640);
    drop(events);
    assert_restart_safety(&evidence);
}

#[test]
#[cfg_attr(miri, ignore = "native FIR preparation is covered by stopped lifecycle tests")]
fn reentrant_preparation_coalesces_changed_reports_without_recursive_restart() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(RestartEvidence::default());
    let handler = recording_handler(&component, &evidence);
    let reentered = Rc::new(Cell::new(false));
    let callback_reentered = Rc::clone(&reentered);
    *evidence.action.borrow_mut() = Some(Rc::new(move |component| {
        if callback_reentered.replace(true) {
            return;
        }
        // SAFETY: restartComponent reentry remains on the same host UI lifecycle thread.
        unsafe {
            for rate in [96_000.0, 192_000.0] {
                let mut setup = offline_setup(rate);
                assert_eq!(component.setupProcessing(&raw mut setup), kResultOk);
            }
        }
    }));
    let mut setup = offline_setup(48_000.0);
    // SAFETY: live host handler and stack setup belong to this host UI lifecycle context.
    unsafe {
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(component.setupProcessing(&raw mut setup), kResultOk);
    }
    let events = evidence.events.borrow();
    assert_eq!(events.len(), 2, "nested changes need only one follow-up restart");
    assert_eq!(events.first().expect("initial report").latency, 4352);
    assert_eq!(events.last().expect("coalesced report").latency, 16_640);
    drop(events);
    assert_restart_safety(&evidence);
}

#[test]
fn restart_reentry_replaces_the_handler_before_coalesced_follow_up() {
    let component = Rc::new(Component::new());
    let original = Rc::new(RestartEvidence::default());
    let replacement = Rc::new(RestartEvidence::default());
    let replacement_handler = recording_handler(&component, &replacement);
    let original_handler = recording_handler(&component, &original);
    *original.action.borrow_mut() = Some(Rc::new(move |component| {
        component.latency_notifications.request();
        component.latency_notifications.request();
        assert_eq!(
            // SAFETY: synchronous restart reentry remains in the host UI context and the retained handler is live.
            unsafe { component.setComponentHandler(replacement_handler.as_ptr()) },
            kResultOk
        );
    }));
    component.latency_notifications.request();
    assert_eq!(
        // SAFETY: installation is a host UI lifecycle call with a live handler.
        unsafe { component.setComponentHandler(original_handler.as_ptr()) },
        kResultOk
    );
    assert_eq!(original.events.borrow().len(), 1);
    assert_eq!(replacement.events.borrow().len(), 1);
    assert_restart_safety(&original);
    assert_restart_safety(&replacement);
}

#[test]
fn continuous_reentry_is_bounded_and_preserves_pending_work_for_the_next_ui_call() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(RestartEvidence::default());
    let handler = recording_handler(&component, &evidence);
    *evidence.action.borrow_mut() = Some(Rc::new(|component| {
        component.latency_notifications.request();
        assert_eq!(
            // SAFETY: the recursive UI entry must defer notification instead of recursively calling the host.
            unsafe { component.setComponentHandler(std::ptr::null_mut()) },
            kResultOk
        );
    }));
    component.latency_notifications.request();
    // SAFETY: all installs remain on the host UI context with live handler references.
    unsafe {
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(evidence.events.borrow().len(), 1);
        *evidence.action.borrow_mut() = Some(Rc::new(|component| component.latency_notifications.request()));
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(
            evidence.events.borrow().len(),
            3,
            "host-induced changes must have a bounded drain"
        );
        *evidence.action.borrow_mut() = None;
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(
            evidence.events.borrow().len(),
            4,
            "bounded drain must preserve remaining work"
        );
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(
            evidence.events.borrow().len(),
            4,
            "completed drain must not notify again"
        );
    }
    assert_restart_safety(&evidence);
}

/// Stack-owned COM storage permits `AddRef` and `Release` reentry without a fixture lifetime cycle.
#[repr(C)]
struct ReentrantReferenceHandler {
    /// First-field ABI interface remains live until the test clears every plugin reference.
    interface: IComponentHandler,
    /// Component remains live while any fixture callback executes.
    component: Rc<Component>,
    /// Host-owned initial reference plus the plugin's explicit COM owner.
    references: Cell<u32>,
    /// Whether a reference-count callback found the handler lock held.
    locked: Cell<bool>,
    /// Whether a reference-count callback found mutable DSP admitted.
    audio_busy: Cell<bool>,
    /// Whether restart detaches itself, making dispatch release the last plugin-owned COM reference.
    remove_on_restart: Cell<bool>,
    /// Number of restart calls received while the raw handler remained current.
    restarts: Cell<usize>,
}

impl ReentrantReferenceHandler {
    /// Nonblocking probes keep regressions bounded before a reference-count callback removes the current handler.
    fn reenter(&self) {
        self.locked
            .set(self.locked.get() || self.component.handler.try_lock().is_err());
        self.audio_busy
            .set(self.audio_busy.get() || self.component.audio.try_with(|_| ()).is_none());
        if !self.locked.get() && !self.audio_busy.get() {
            // SAFETY: reference-count callbacks are synchronous in this fixture's host UI lifecycle calls.
            unsafe { self.component.setComponentHandler(std::ptr::null_mut()) };
        }
    }
}

/// Borrows the fixture through the first repr(C) interface, whose storage the test retains throughout every call.
unsafe fn reference_fixture<'a>(this: *mut FUnknown) -> &'a ReentrantReferenceHandler {
    // SAFETY: this vtable is installed only in the live stack-owned fixture below.
    unsafe { &*this.cast::<ReentrantReferenceHandler>() }
}

/// Rejects unsupported interface queries without manufacturing any retained reference.
unsafe extern "system" fn reference_query(
    _: *mut FUnknown,
    _: *const TUID,
    output: *mut *mut std::ffi::c_void,
) -> tresult {
    if !output.is_null() {
        // SAFETY: queryInterface supplies a writable output slot.
        unsafe { output.write(std::ptr::null_mut()) };
    }
    kResultFalse
}

/// Takes one explicit COM reference and reenters before installation can acquire the handler mutex.
unsafe extern "system" fn reference_add(this: *mut FUnknown) -> u32 {
    // SAFETY: the installed vtable belongs to the test's live fixture.
    let host = unsafe { reference_fixture(this) };
    host.references.set(host.references.get() + 1);
    host.reenter();
    host.references.get()
}

/// Releases one explicit COM reference and removes any newly installed handler from the reentrant callback.
unsafe extern "system" fn reference_release(this: *mut FUnknown) -> u32 {
    // SAFETY: the installed vtable belongs to the test's live fixture.
    let host = unsafe { reference_fixture(this) };
    host.references.set(host.references.get() - 1);
    host.reenter();
    host.references.get()
}

/// Sound gestures are unrelated to this stopped lifecycle-only host fixture.
unsafe extern "system" fn reference_edit(_: *mut IComponentHandler, _: u32) -> tresult {
    kResultOk
}

/// Parameter values are unrelated to this stopped lifecycle-only host fixture.
unsafe extern "system" fn reference_perform(_: *mut IComponentHandler, _: u32, _: f64) -> tresult {
    kResultOk
}

/// Optionally removes the current handler so dispatch must release its final COM owner outside the lock.
unsafe extern "system" fn reference_restart(this: *mut IComponentHandler, _: i32) -> tresult {
    // SAFETY: the handler is the first repr(C) fixture field and remains retained through callback return.
    let host = unsafe { reference_fixture(this.cast::<FUnknown>()) };
    host.restarts.set(host.restarts.get() + 1);
    if host.remove_on_restart.get() {
        host.reenter();
    }
    kResultOk
}

/// Raw host vtable permits reference-count behavior that the automatic Rust COM wrapper cannot script.
const REENTRANT_REFERENCE_VTABLE: IComponentHandlerVtbl = IComponentHandlerVtbl {
    base: FUnknownVtbl {
        queryInterface: reference_query,
        addRef: reference_add,
        release: reference_release,
    },
    beginEdit: reference_edit,
    performEdit: reference_perform,
    endEdit: reference_edit,
    restartComponent: reference_restart,
};

#[test]
fn handler_reference_callbacks_replace_safely_and_pending_notifications_follow_the_current_handler() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(RestartEvidence::default());
    let replacement = recording_handler(&component, &evidence);
    let mut host = ReentrantReferenceHandler {
        interface: IComponentHandler {
            vtbl: &REENTRANT_REFERENCE_VTABLE,
        },
        component: Rc::clone(&component),
        references: Cell::new(1),
        locked: Cell::new(false),
        audio_busy: Cell::new(false),
        remove_on_restart: Cell::new(false),
        restarts: Cell::new(0),
    };
    // SAFETY: host storage outlives every plugin reference, which these serialized UI calls clear explicitly.
    unsafe {
        assert_eq!(component.setComponentHandler(&raw mut host.interface), kResultOk);
        assert_eq!(host.references.get(), 2);
        component.latency_notifications.request();
        assert_eq!(component.setComponentHandler(replacement.as_ptr()), kResultOk);
        assert_eq!(host.references.get(), 1);
        assert!(
            evidence.events.borrow().is_empty(),
            "Release removed the replacement before delivery"
        );
        assert_eq!(component.setComponentHandler(replacement.as_ptr()), kResultOk);
        assert_eq!(
            evidence.events.borrow().len(),
            1,
            "pending notification must follow the current handler"
        );
        assert_eq!(component.setComponentHandler(std::ptr::null_mut()), kResultOk);
        host.remove_on_restart.set(true);
        component.latency_notifications.request();
        assert_eq!(component.setComponentHandler(&raw mut host.interface), kResultOk);
        assert_eq!(host.restarts.get(), 1);
        assert_eq!(
            host.references.get(),
            1,
            "dispatch must release its final plugin-owned COM reference"
        );
        assert!(component.handler.try_lock().expect("unlocked handler").is_none());
    }
    assert!(!host.locked.get(), "host AddRef or Release ran under the handler mutex");
    assert!(
        !host.audio_busy.get(),
        "host AddRef or Release ran under mutable DSP admission"
    );
    assert_restart_safety(&evidence);
}

#[test]
fn rejected_restart_is_not_retried_and_termination_clears_pending_notifications() {
    let component = Rc::new(Component::new());
    let evidence = Rc::new(RestartEvidence::default());
    evidence.result.set(kResultFalse);
    let handler = recording_handler(&component, &evidence);
    component.latency_notifications.request();
    // SAFETY: the handler and component remain live through serialized UI lifecycle callbacks.
    unsafe {
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(evidence.events.borrow().len(), 1);
        assert_eq!(component.setComponentHandler(std::ptr::null_mut()), kResultOk);
        component.latency_notifications.request();
        assert_eq!(component.terminate(), kResultOk);
        assert_eq!(component.setComponentHandler(handler.as_ptr()), kResultOk);
        assert_eq!(
            evidence.events.borrow().len(),
            1,
            "termination must discard pending restarts"
        );
    }
    assert_restart_safety(&evidence);
}
