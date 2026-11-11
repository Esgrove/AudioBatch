//! Same-component packaged VST3 state restores overlapping a fresh marked processing thread.
//!
//! This private host owns its COM interfaces and state streams, without bypassing the production host's lifecycle API.
//! Only the processor interface crosses threads.
//! Lifecycle, state, reference destruction, and library ownership stay on the main thread,
//! and the callback worker is joined before deactivation or unloading.

#![expect(
    unsafe_code,
    reason = "audits the packaged VST3 COM boundary with private, owned test interfaces"
)]

use std::ffi::c_void;
use std::io::{Cursor, Read as _, Seek as _, SeekFrom, Write as _};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, PoisonError};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, ensure};
use image_control_plugin_rs::{COMPONENT_ID, PARAMETER_COUNT};
use vst3::Steinberg::Vst::{
    AudioBusBuffers, AudioBusBuffers__type0, IAudioProcessor, IAudioProcessorTrait as _, IComponent,
    IComponentTrait as _, IEditController, IEditControllerTrait as _, ProcessData, ProcessSetup, SpeakerArr,
};
use vst3::Steinberg::{
    FUnknown, IBStream, IBStreamTrait, IPluginBaseTrait as _, IPluginFactory, IPluginFactoryTrait as _,
    kInvalidArgument, kResultFalse, kResultOk, tresult,
};
use vst3::{Class, ComPtr, ComWrapper, Interface};

use super::{BLOCK_FRAMES, MINIMUM_CALLBACKS, SAMPLE_RATE};

/// Bounds each progress fence independently of the outer process watchdog.
const PROGRESS_TIMEOUT: Duration = Duration::from_secs(2);
/// Number of different detached snapshots restored while callbacks remain active.
const RESTORE_COUNT: usize = 4;
/// Largest private test stream, including the plugin's state-length prefix.
const MAXIMUM_STREAM_BYTES: usize = 16_388;
/// VST3 real-time processing mode and 32-bit sample format both have SDK value zero.
const REALTIME_SAMPLE32: i32 = 0;

/// Main-thread component owner, whose processor and controller are queried from this exact COM object.
struct HostedComponent {
    /// Component kept alive until after the callback worker has joined.
    component: ComPtr<IComponent>,
    /// Same-object processor interface used for setup and transferred temporarily to the worker.
    processor: ComPtr<IAudioProcessor>,
    /// Same-object controller used only by the main thread.
    controller: ComPtr<IEditController>,
}

/// Callback evidence shared with the main-thread state stream, outside every marked region.
#[derive(Default)]
struct Progress {
    /// Requests retirement only between complete processing invocations.
    stop: AtomicBool,
    /// Completed, successful callbacks with verified finite output.
    callbacks: AtomicUsize,
}

/// Joined callback worker that returns its owned processor interface to the main thread.
struct Worker {
    /// Progress and retirement requests, never accessed inside the marker.
    progress: Arc<Progress>,
    /// Fresh thread, with all output validation outside the marked call.
    thread: Option<JoinHandle<(ComPtr<IAudioProcessor>, Result<()>)>>,
}

/// Private bounded state stream. Its optional read fence proves callbacks advance inside actual setState calls.
struct StateStream {
    /// Main-thread stream contents and seek position.
    bytes: Mutex<Cursor<Vec<u8>>>,
    /// Present only for restores while the same component's processor is running.
    progress: Option<Arc<Progress>>,
    /// Successful progress observations made from inside the plugin's state-read call stack.
    overlap_reads: AtomicUsize,
}

impl HostedComponent {
    /// Creates exactly one component through the actual library export, then queries both other interfaces from it.
    fn create(library: &libloading::Library) -> Result<Self> {
        // SAFETY: the loaded build-tree VST3 exports GetPluginFactory with the documented SDK signature.
        let export =
            unsafe { library.get::<unsafe extern "system" fn() -> *mut IPluginFactory>(b"GetPluginFactory\0") }?;
        // SAFETY: the retained library keeps this export live, and it transfers one factory reference.
        let factory = unsafe { ComPtr::from_raw(export()) }.context("packaged factory returned null")?;
        let mut pointer: *mut c_void = std::ptr::null_mut();
        // SAFETY: both IDs reference 16 bytes, and pointer is a live writable result slot.
        let result =
            unsafe { factory.createInstance(COMPONENT_ID.as_ptr(), IComponent::IID.as_ptr().cast(), &raw mut pointer) };
        ensure!(result == kResultOk, "packaged factory rejected the component: {result}");
        // SAFETY: a successful createInstance transfers one owned IComponent reference, or null is rejected.
        let component = unsafe { ComPtr::from_raw(pointer.cast::<IComponent>()) }
            .context("packaged factory returned a null component")?;
        let processor = component.cast::<IAudioProcessor>().context("same-object processor")?;
        let controller = component.cast::<IEditController>().context("same-object controller")?;
        let component_identity = component.cast::<FUnknown>().context("component identity")?;
        let processor_identity = processor.cast::<FUnknown>().context("processor identity")?;
        let controller_identity = controller.cast::<FUnknown>().context("controller identity")?;
        ensure!(
            component_identity.as_ptr() == processor_identity.as_ptr()
                && component_identity.as_ptr() == controller_identity.as_ptr(),
            "state and audio interfaces do not belong to the same component"
        );
        // SAFETY: initialization is on the main thread before setup, and this own-wrapper accepts a null host context.
        let result = unsafe { component.initialize(std::ptr::null_mut()) };
        ensure!(result == kResultOk, "initialize");
        Ok(Self {
            component,
            processor,
            controller,
        })
    }

    /// Prepares matching stereo buses before any callback runs, without a processing warmup.
    fn prepare(&self) -> Result<()> {
        let mut input = SpeakerArr::kStereo;
        let mut output = SpeakerArr::kStereo;
        let mut setup = ProcessSetup {
            processMode: REALTIME_SAMPLE32,
            symbolicSampleSize: REALTIME_SAMPLE32,
            maxSamplesPerBlock: i32::try_from(BLOCK_FRAMES)?,
            sampleRate: SAMPLE_RATE,
        };
        // SAFETY: lifecycle calls are serialized on the main thread, with live setup and arrangement slots.
        unsafe {
            ensure!(
                self.processor.setBusArrangements(&raw mut input, 1, &raw mut output, 1) == kResultOk,
                "stereo"
            );
            ensure!(
                self.processor.setupProcessing(&raw mut setup) == kResultOk,
                "setupProcessing"
            );
            ensure!(self.component.setActive(1) == kResultOk, "setActive");
            ensure!(self.processor.setProcessing(1) == kResultOk, "setProcessing");
        }
        Ok(())
    }

    /// Writes all stable parameters through this component's controller, never through mutable DSP.
    fn edit(&self, stage: usize) -> Result<Vec<f64>> {
        let mut expected = Vec::with_capacity(PARAMETER_COUNT);
        for id in 0..u32::try_from(PARAMETER_COUNT)? {
            let value = f64::from((id + u32::try_from(stage)? * 7) % 31 + 1) / 32.0;
            // SAFETY: the queried controller is live and parameter changes publish independent atomics only.
            let result = unsafe { self.controller.setParamNormalized(id, value) };
            ensure!(result == kResultOk, "parameter {id}");
            expected.push(value);
        }
        Ok(expected)
    }

    /// Captures state on the unmarked main thread while the processor may run independently.
    fn save(&self) -> Result<Vec<u8>> {
        let stream = StateStream::create(Vec::new(), None);
        let interface = stream.to_com_ptr::<IBStream>().context("save stream interface")?;
        // SAFETY: the same component and writable stream remain owned throughout this main-thread call.
        let result = unsafe { self.component.getState(interface.as_ptr()) };
        ensure!(result == kResultOk, "getState");
        let bytes = stream
            .bytes
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .get_ref()
            .clone();
        ensure!(!bytes.is_empty(), "native getState returned no bytes");
        Ok(bytes)
    }

    /// Restores a captured state with callback progress fenced from inside the plugin's `IBStream` reads.
    fn restore(&self, bytes: Vec<u8>, progress: &Arc<Progress>) -> Result<()> {
        let stream = StateStream::create(bytes, Some(Arc::clone(progress)));
        let interface = stream.to_com_ptr::<IBStream>().context("restore stream interface")?;
        // SAFETY: component and stream remain owned on this thread.
        // Only the separately queried processor runs elsewhere.
        // The own wrapper's state callback accesses parameter atomics, not audio-owned mutable history.
        let result = unsafe { self.component.setState(interface.as_ptr()) };
        ensure!(result == kResultOk, "concurrent setState");
        ensure!(
            stream.overlap_reads.load(Ordering::Acquire) > 0,
            "no callback completed inside setState"
        );
        Ok(())
    }

    /// Checks every restored value against independent controller edits, not merely a nonempty saved blob.
    fn verify(&self, expected: &[f64]) -> Result<()> {
        for (id, expected) in expected.iter().enumerate() {
            // SAFETY: main-thread readback uses this live component's controller with a validated stable ID.
            let actual = unsafe { self.controller.getParamNormalized(u32::try_from(id)?) };
            ensure!(
                actual.to_bits() == expected.to_bits(),
                "restored parameter {id} differs"
            );
        }
        Ok(())
    }
}

impl Drop for HostedComponent {
    fn drop(&mut self) {
        // SAFETY: run declares the callback guard after this owner, so it always joins before lifecycle teardown.
        // These calls stay on the main thread, and every interface remains live through termination.
        unsafe {
            let _ = self.processor.setProcessing(0);
            let _ = self.component.setActive(0);
            let _ = self.component.terminate();
        }
    }
}

impl Worker {
    /// Moves only a same-object processor reference to a fresh thread, without an unmarked first callback.
    fn start(processor: ComPtr<IAudioProcessor>) -> Result<Self> {
        let progress = Arc::new(Progress::default());
        let signals = Arc::clone(&progress);
        let thread = thread::Builder::new()
            .name("packaged-state-callback".into())
            .spawn(move || {
                let outcome = callback_loop(&processor, &signals);
                (processor, outcome)
            })?;
        Ok(Self {
            progress,
            thread: Some(thread),
        })
    }

    /// Stops processing and returns interface destruction to the main thread before reporting errors.
    fn finish(mut self) -> Result<()> {
        self.progress.stop.store(true, Ordering::Release);
        let (processor, outcome) = self
            .thread
            .take()
            .context("callback worker retired")?
            .join()
            .map_err(|_| anyhow::anyhow!("state callback thread panicked"))?;
        drop(processor);
        outcome?;
        ensure!(
            self.progress.callbacks.load(Ordering::Acquire) >= MINIMUM_CALLBACKS,
            "insufficient callbacks"
        );
        Ok(())
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        self.progress.stop.store(true, Ordering::Release);
        if let Some(worker) = self.thread.take() {
            // Returning the processor in the joined outcome keeps COM release on this main thread even after errors.
            drop(worker.join());
        }
    }
}

impl Class for StateStream {
    type Interfaces = (IBStream,);
}

impl StateStream {
    /// Owns detached stream bytes separately from the audio component and callback buffers.
    fn create(bytes: Vec<u8>, progress: Option<Arc<Progress>>) -> ComWrapper<Self> {
        ComWrapper::new(Self {
            bytes: Mutex::new(Cursor::new(bytes)),
            progress,
            overlap_reads: AtomicUsize::new(0),
        })
    }
}

impl IBStreamTrait for StateStream {
    unsafe fn read(&self, buffer: *mut c_void, count: i32, transferred: *mut i32) -> tresult {
        let Ok(count) = usize::try_from(count) else {
            return kInvalidArgument;
        };
        if buffer.is_null() || count > MAXIMUM_STREAM_BYTES {
            return kInvalidArgument;
        }
        if let Some(progress) = &self.progress {
            let before = progress.callbacks.load(Ordering::Acquire);
            if wait_for_callbacks(progress, before + 2).is_err() {
                return kResultFalse;
            }
            self.overlap_reads.fetch_add(1, Ordering::Release);
        }
        // SAFETY: IBStream's caller supplies count writable bytes, and this stream is never used by audio processing.
        let target = unsafe { std::slice::from_raw_parts_mut(buffer.cast::<u8>(), count) };
        let Ok(read) = self.bytes.lock().unwrap_or_else(PoisonError::into_inner).read(target) else {
            return kResultFalse;
        };
        if !transferred.is_null() {
            // SAFETY: a non-null IBStream count slot is writable for this call, and read is bounded by count.
            unsafe { transferred.write(i32::try_from(read).unwrap_or_default()) };
        }
        kResultOk
    }

    unsafe fn write(&self, buffer: *mut c_void, count: i32, transferred: *mut i32) -> tresult {
        let Ok(count) = usize::try_from(count) else {
            return kInvalidArgument;
        };
        if buffer.is_null() || count > MAXIMUM_STREAM_BYTES {
            return kInvalidArgument;
        }
        let mut stream = self.bytes.lock().unwrap_or_else(PoisonError::into_inner);
        if stream.position().saturating_add(count as u64) > MAXIMUM_STREAM_BYTES as u64 {
            return kResultFalse;
        }
        // SAFETY: IBStream's caller supplies count readable bytes for this main-thread state operation.
        let source = unsafe { std::slice::from_raw_parts(buffer.cast::<u8>(), count) };
        if stream.write_all(source).is_err() {
            return kResultFalse;
        }
        drop(stream);
        if !transferred.is_null() {
            // SAFETY: the optional transferred slot is writable and count is bounded by MAXIMUM_STREAM_BYTES.
            unsafe { transferred.write(i32::try_from(count).unwrap_or_default()) };
        }
        kResultOk
    }

    unsafe fn seek(&self, offset: i64, mode: i32, result: *mut i64) -> tresult {
        let origin = match mode {
            0 => match u64::try_from(offset) {
                Ok(offset) => SeekFrom::Start(offset),
                Err(_) => return kInvalidArgument,
            },
            1 => SeekFrom::Current(offset),
            2 => SeekFrom::End(offset),
            _ => return kInvalidArgument,
        };
        let mut stream = self.bytes.lock().unwrap_or_else(PoisonError::into_inner);
        let previous = stream.position();
        let Ok(position) = stream.seek(origin) else {
            return kResultFalse;
        };
        if position > MAXIMUM_STREAM_BYTES as u64 {
            stream.set_position(previous);
            return kResultFalse;
        }
        drop(stream);
        if !result.is_null() {
            // SAFETY: the optional seek-result slot is writable and the returned position is bounded.
            unsafe { result.write(i64::try_from(position).unwrap_or_default()) };
        }
        kResultOk
    }

    unsafe fn tell(&self, result: *mut i64) -> tresult {
        if result.is_null() {
            return kInvalidArgument;
        }
        let position = self.bytes.lock().unwrap_or_else(PoisonError::into_inner).position();
        // SAFETY: the non-null position slot is writable, and every stream operation bounds the position.
        unsafe { result.write(i64::try_from(position).unwrap_or_default()) };
        kResultOk
    }
}

/// Audits repeated live getState/setState on one packaged component, with no processing stop between restores.
pub(super) fn run(bundle: &Path) -> Result<()> {
    let path = library_path(bundle)?;
    // SAFETY: the enclosing host already initialized this exact build-tree module through the native VST3 loader.
    // This additional library reference is declared first, so every COM object and worker retires before it unloads.
    let library = unsafe { libloading::Library::new(&path) }.context("open packaged state-audit library")?;
    let component = HostedComponent::create(&library)?;
    component.prepare()?;
    let worker = Worker::start(component.processor.clone())?;
    wait_for_callbacks(&worker.progress, MINIMUM_CALLBACKS)?;
    for stage in 0..RESTORE_COUNT {
        let expected = component.edit(stage)?;
        let saved = component.save()?;
        component.edit(stage + RESTORE_COUNT)?;
        component.restore(saved.clone(), &worker.progress)?;
        let restored_at = worker.progress.callbacks.load(Ordering::Acquire);
        wait_for_callbacks(&worker.progress, restored_at + 2)?;
        component.verify(&expected)?;
        ensure!(component.save()? == saved, "live restored state did not round trip");
    }
    worker.finish()?;
    Ok(())
}

/// Selects only the known primary package's executable, never installed locations or an in-process factory.
fn library_path(bundle: &Path) -> Result<PathBuf> {
    #[cfg(target_os = "macos")]
    let path = bundle.join("Contents/MacOS/ImageControl RS");
    #[cfg(all(windows, target_arch = "x86_64"))]
    let path = bundle.join("Contents/x86_64-win/ImageControl RS.vst3");
    #[cfg(all(windows, target_arch = "aarch64"))]
    let path = bundle.join("Contents/arm64-win/ImageControl RS.vst3");
    ensure!(
        path.is_file(),
        "packaged state-audit executable is missing: {}",
        path.display()
    );
    Ok(path)
}

/// Requires actual callback progress while the native state callback remains on the caller's stack.
fn wait_for_callbacks(progress: &Progress, minimum: usize) -> Result<()> {
    let deadline = Instant::now() + PROGRESS_TIMEOUT;
    while progress.callbacks.load(Ordering::Acquire) < minimum {
        ensure!(
            !progress.stop.load(Ordering::Acquire),
            "callback worker stopped during state operation"
        );
        ensure!(
            Instant::now() < deadline,
            "callbacks stalled during concurrent state operation"
        );
        thread::yield_now();
    }
    Ok(())
}

/// Owns all sample arrays, channel pointers, and metadata exclusively on the fresh processing thread.
fn callback_loop(processor: &ComPtr<IAudioProcessor>, progress: &Progress) -> Result<()> {
    let mut left = [0.0_f32; BLOCK_FRAMES];
    let mut right = [0.0_f32; BLOCK_FRAMES];
    let mut block = 0_usize;
    while !progress.stop.load(Ordering::Acquire) {
        for (frame, (left, right)) in left.iter_mut().zip(&mut right).enumerate() {
            let phase = ((block * BLOCK_FRAMES + frame) % 48_000) as f32 / 48_000.0;
            let mid = (phase * std::f32::consts::TAU * 440.0).sin() * 0.1;
            let side = (phase * std::f32::consts::TAU * 1100.0).sin() * 0.025;
            *left = mid + side;
            *right = mid - side;
        }
        let mut channels = [left.as_mut_ptr(), right.as_mut_ptr()];
        let mut bus = AudioBusBuffers {
            numChannels: 2,
            silenceFlags: 0,
            __field0: AudioBusBuffers__type0 {
                channelBuffers32: channels.as_mut_ptr(),
            },
        };
        // SAFETY: ProcessData contains only scalars and nullable raw pointers, for which zero is valid.
        let mut data: ProcessData = unsafe { std::mem::zeroed() };
        data.processMode = REALTIME_SAMPLE32;
        data.symbolicSampleSize = REALTIME_SAMPLE32;
        data.numSamples = i32::try_from(BLOCK_FRAMES)?;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &raw mut bus;
        data.outputs = &raw mut bus;
        let result = native_process_region(processor, &mut data);
        if result != kResultOk {
            progress.stop.store(true, Ordering::Release);
            anyhow::bail!("same-component native process failed: {result}");
        }
        if !left.iter().chain(&right).all(|sample| sample.is_finite()) {
            progress.stop.store(true, Ordering::Release);
            anyhow::bail!("non-finite concurrent state callback output");
        }
        progress.callbacks.fetch_add(1, Ordering::Release);
        block = block.wrapping_add(1);
    }
    Ok(())
}

/// Marks only the actual packaged component's process call, excluding state streams, coordination, and assertions.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn native_process_region(processor: &ComPtr<IAudioProcessor>, data: &mut ProcessData) -> tresult {
    // SAFETY: callback_loop owns the live metadata, channel pointers, and in-place samples until this call returns.
    // The component is prepared and active, and only this worker calls process or touches its audio history.
    unsafe { processor.process(data) }
}
