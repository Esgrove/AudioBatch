//! Shared hardware-free build-tree host, with UI-thread lifecycle and fresh marked callback threads.

#![cfg_attr(
    target_os = "macos",
    expect(unsafe_code, reason = "dispatches native AppKit events on the main thread")
)]

use std::hint::black_box;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, mpsc};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, bail, ensure};
use image_control_plugin_rs::{COMPONENT_ID, PARAMETER_COUNT, PLUGIN_NAME};
#[cfg(target_os = "macos")]
use objc2::{MainThreadMarker, msg_send, rc::Retained, runtime::ProtocolObject};
#[cfg(target_os = "macos")]
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSApplicationDelegate, NSEvent, NSEventMask, NSEventModifierFlags,
    NSEventType,
};
#[cfg(target_os = "macos")]
use objc2_foundation::{NSDate, NSDefaultRunLoopMode, NSPoint};
use vst3_host::{ClassId, EditorWindow, Module, PluginInstance, ProcessError, ProcessMode, ProcessorHandle, UiThread};

#[path = "artifacts.rs"]
mod artifacts;
#[path = "state_overlap.rs"]
mod state_overlap;

/// Internal wall-clock bound, including module loading, UI operations, and plugin teardown.
const CASE_TIMEOUT: Duration = Duration::from_secs(10);
/// Native host maximum and ordinary callback size.
const BLOCK_FRAMES: usize = 512;
/// Host rate used by independent latency and neutral-audio expectations.
const SAMPLE_RATE: f64 = 48_000.0;
/// Event-loop polling interval outside the measured callback.
const EVENT_WAIT: Duration = Duration::from_millis(5);
/// Minimum active interval for every UI/state lifecycle stage.
const STAGE_DURATION: Duration = Duration::from_millis(120);
/// Required observed callbacks per processing instance.
const MINIMUM_CALLBACKS: usize = 8;

/// One isolated detector control or actual packaged-plugin callback workload.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Case {
    CleanControl,
    AllocationControl,
    MutexControl,
    NativeColdAutomation,
    NativeReset,
    NativeStateInteraction,
    NativeEditorLifecycle,
}

/// Parsed explicit build-tree arguments. There is no installed-plugin discovery fallback.
pub struct Options {
    /// The isolated scenario selected by the caller.
    pub case: Case,
    /// Canonical absolute path to the build-tree VST3.
    pub bundle: PathBuf,
}

/// Cancels a fatal watchdog only after all native callbacks and teardown have returned.
struct Watchdog {
    /// Wakes the watchdog after successful or ordinary failed completion.
    finished: mpsc::Sender<()>,
    /// Joins the watchdog outside every marked region.
    worker: Option<JoinHandle<()>>,
}

/// Main-thread application ownership, including the delegate the plugin must not replace.
struct Application {
    /// Thread that owns the host application and all editor lifecycle calls.
    host_thread: thread::ThreadId,
    #[cfg(target_os = "macos")]
    /// Host application retained until all plugin editors have been removed.
    native: Retained<NSApplication>,
    #[cfg(target_os = "macos")]
    /// Host-owned delegate identity retained across editor close and reopen.
    delegate: Option<Retained<ProtocolObject<dyn NSApplicationDelegate>>>,
}

/// Cross-thread coordination, never read or changed inside the sanitizer marker.
struct Signals {
    /// Requests retirement between complete callback invocations.
    stop: AtomicBool,
    /// Counts successfully completed callbacks independently for two component instances.
    callbacks: [AtomicUsize; 2],
    /// Counts exercised reset boundaries.
    resets: AtomicUsize,
}

/// Returns processor ownership to the main thread even when processing reports an error.
struct CallbackOutcome {
    /// Prepared handles whose final destruction stays outside the measured region.
    processors: Vec<ProcessorHandle>,
    /// Deferred diagnostic, formatted only after the native callback returned.
    failure: Option<String>,
}

/// Stops and retires callback work before UI-owned plugin instances may be destroyed.
struct CallbackWorker {
    /// Shared bounded workload control and observed progress.
    signals: Arc<Signals>,
    /// Fresh callback thread, joined before instance deactivation.
    worker: Option<JoinHandle<CallbackOutcome>>,
}

impl Case {
    /// Returns the stable scenario name used by the sanitizer runner and completion protocol.
    pub const fn name(self) -> &'static str {
        match self {
            Self::CleanControl => "clean-control",
            Self::AllocationControl => "allocation-control",
            Self::MutexControl => "mutex-control",
            Self::NativeColdAutomation => "native-cold-automation",
            Self::NativeReset => "native-reset",
            Self::NativeStateInteraction => "native-state-interaction",
            Self::NativeEditorLifecycle => "native-editor-lifecycle",
        }
    }

    /// Parses one exact case name rather than accepting partial or unknown workloads.
    fn parse(name: &str) -> Result<Self> {
        match name {
            "clean-control" => Ok(Self::CleanControl),
            "allocation-control" => Ok(Self::AllocationControl),
            "mutex-control" => Ok(Self::MutexControl),
            "native-cold-automation" => Ok(Self::NativeColdAutomation),
            "native-reset" => Ok(Self::NativeReset),
            "native-state-interaction" => Ok(Self::NativeStateInteraction),
            "native-editor-lifecycle" => Ok(Self::NativeEditorLifecycle),
            _ => bail!("unknown case: {name}"),
        }
    }
}

impl Watchdog {
    /// Starts an internal process bound before module loading can block in third-party code.
    fn start() -> Result<Self> {
        let (finished, receiver) = mpsc::channel();
        let worker = thread::Builder::new()
            .name("packaged-host-watchdog".into())
            .spawn(move || {
                if receiver.recv_timeout(CASE_TIMEOUT) == Err(mpsc::RecvTimeoutError::Timeout) {
                    eprintln!("CASE_FAILED internal 10-second packaged-host deadline exceeded");
                    std::process::abort();
                }
            })?;
        Ok(Self {
            finished,
            worker: Some(worker),
        })
    }
}

impl Drop for Watchdog {
    fn drop(&mut self) {
        let _ = self.finished.send(());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Application {
    /// Starts the native host on the executable's actual main thread, without an audio device.
    fn start() -> Result<Self> {
        ensure!(
            UiThread::current().is_some(),
            "register the host UI thread before application initialization"
        );
        let host_thread = thread::current().id();
        #[cfg(target_os = "macos")]
        {
            let marker = MainThreadMarker::new().context("the packaged host must run on the macOS main thread")?;
            let application = NSApplication::sharedApplication(marker);
            application.setActivationPolicy(NSApplicationActivationPolicy::Regular);
            application.finishLaunching();
            let delegate = application.delegate();
            Ok(Self {
                host_thread,
                native: application,
                delegate,
            })
        }
        #[cfg(not(target_os = "macos"))]
        {
            Ok(Self { host_thread })
        }
    }

    /// Requires the plugin to preserve application ownership through every editor lifecycle stage.
    fn verify_delegate(&self) -> Result<()> {
        ensure!(
            thread::current().id() == self.host_thread,
            "editor lifecycle left the host UI thread"
        );
        #[cfg(target_os = "macos")]
        ensure!(
            self.native.delegate() == self.delegate,
            "plugin replaced the host application delegate"
        );
        Ok(())
    }
}

impl CallbackWorker {
    /// Moves already prepared handles to a fresh thread without running an unmarked warmup callback.
    fn start(processors: Vec<ProcessorHandle>, reset: bool) -> Result<Self> {
        let signals = Arc::new(Signals {
            stop: AtomicBool::new(false),
            callbacks: std::array::from_fn(|_| AtomicUsize::new(0)),
            resets: AtomicUsize::new(0),
        });
        let worker_signals = Arc::clone(&signals);
        let worker = thread::Builder::new()
            .name("packaged-native-callback".into())
            .spawn(move || {
                let mut processors = processors;
                let failure = callback_loop(&mut processors, &worker_signals, reset)
                    .err()
                    .map(|error| error.to_string());
                CallbackOutcome { processors, failure }
            })?;
        Ok(Self {
            signals,
            worker: Some(worker),
        })
    }

    /// Reads completed-callback evidence outside the measured boundary.
    fn count(&self, instance: usize) -> usize {
        self.signals
            .callbacks
            .get(instance)
            .map_or(0, |count| count.load(Ordering::Acquire))
    }

    /// Stops between callbacks and checks both the worker status and each instance's actual progress.
    fn finish(mut self, instances: usize, reset: bool) -> Result<()> {
        self.signals.stop.store(true, Ordering::Release);
        let outcome = self
            .worker
            .take()
            .context("callback worker already retired")?
            .join()
            .map_err(|_| anyhow::anyhow!("native callback thread panicked"))?;
        let failure = outcome.failure;
        drop(outcome.processors);
        ensure!(
            failure.is_none(),
            "native callback failed: {}",
            failure.unwrap_or_default()
        );
        for instance in 0..instances {
            ensure!(
                self.count(instance) >= MINIMUM_CALLBACKS,
                "instance {instance} did not complete enough callbacks"
            );
        }
        if reset {
            ensure!(
                self.signals.resets.load(Ordering::Acquire) > 0,
                "no native reset boundary ran"
            );
        }
        Ok(())
    }
}

impl Drop for CallbackWorker {
    fn drop(&mut self) {
        self.signals.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            // Instance owners remain on the UI thread until this guard has retired callback work.
            // The watchdog still bounds a plugin that fails to return from process.
            let _ = worker.join();
        }
    }
}

/// Parses --bundle and --case before native initialization. --help has no build or plugin side effects.
pub fn arguments(default_case: Option<Case>) -> Result<Option<Options>> {
    let mut arguments = std::env::args_os().skip(1);
    let mut case = default_case;
    let mut bundle = std::env::var_os("IMAGE_CONTROL_OWN_BUNDLE").map(PathBuf::from);
    while let Some(argument) = arguments.next() {
        match argument.to_str() {
            Some("-h" | "--help") => {
                println!(
                    "Usage: packaged host --bundle <absolute-build-tree.vst3> --case <name>\n\
Cases: clean-control, allocation-control, mutex-control, native-cold-automation, native-reset,\n\
       native-state-interaction, native-editor-lifecycle\n\
IMAGE_CONTROL_OWN_BUNDLE supplies --bundle when omitted. No audio devices or installed plugins are used."
                );
                return Ok(None);
            }
            Some("--bundle") => {
                bundle = Some(PathBuf::from(
                    arguments.next().context("--bundle needs an absolute path")?,
                ));
            }
            Some("--case") => {
                let value = arguments.next().context("--case needs a name")?;
                case = Some(Case::parse(value.to_str().context("case name must be UTF-8")?)?);
            }
            _ => bail!("unknown argument: {}", argument.to_string_lossy()),
        }
    }
    let bundle = checked_bundle(&bundle.context("give --bundle or IMAGE_CONTROL_OWN_BUNDLE")?)?;
    Ok(Some(Options {
        case: case.context("give --case")?,
        bundle,
    }))
}

/// Requires a real absolute bundle within the repository or explicitly selected Cargo build root.
/// Canonical containment excludes symlink escapes and never searches installed locations.
pub fn checked_bundle(path: &Path) -> Result<PathBuf> {
    let path =
        artifacts::checked_artifact(path).with_context(|| format!("invalid build-tree bundle: {}", path.display()))?;
    ensure!(
        path.extension().is_some_and(|extension| extension == "vst3"),
        "bundle must end in .vst3"
    );
    ensure!(path.is_dir(), "the packaged VST3 must be a bundle directory");
    Ok(path)
}

/// Executes one bounded scenario and reports success only after callback verification and native teardown.
pub fn run(options: &Options) -> Result<()> {
    let watchdog = Watchdog::start()?;
    match options.case {
        Case::CleanControl => ensure!(clean_region(41) == 42, "clean detector control failed"),
        Case::AllocationControl => {
            ensure!(
                cfg!(audiobatch_rtsan),
                "allocation-control requires an RTSan-instrumented executable"
            );
            drop(rust_allocation_region());
            bail!("allocation-control returned without the required fatal detector finding");
        }
        Case::MutexControl => {
            ensure!(
                cfg!(audiobatch_rtsan),
                "mutex-control requires an RTSan-instrumented executable"
            );
            let mutex = Mutex::new(0);
            drop(
                mutex
                    .lock()
                    .map_err(|_| anyhow::anyhow!("mutex control initialization failed"))?,
            );
            mutex_region(&mutex);
            bail!("mutex-control returned without the required fatal detector finding");
        }
        case => native_case(&options.bundle, case)?,
    }
    drop(watchdog);
    println!("CASE_OK {}", options.case.name());
    Ok(())
}

/// Runs native setup, state, and editor work on the main/UI thread while marked audio runs elsewhere.
fn native_case(bundle: &Path, case: Case) -> Result<()> {
    let ui = UiThread::register_current()?;
    let application = Application::start()?;
    let module = Module::load(&ui, bundle).context("load actual packaged ImageControl VST3")?;
    let class_id = ClassId::from_tuid(&COMPONENT_ID);
    let class = module
        .class(class_id)
        .context("bundle does not export the independent primary component identity")?;
    ensure!(
        class.is_audio_effect() && class.name == PLUGIN_NAME,
        "bundle is not the primary ImageControl component"
    );
    if case == Case::NativeStateInteraction {
        state_overlap::run(bundle)?;
        handle_events(&ui, EVENT_WAIT);
        return application.verify_delegate();
    }
    let instances_count = if case == Case::NativeEditorLifecycle { 2 } else { 1 };
    let mut instances = Vec::with_capacity(instances_count);
    let mut processors = Vec::with_capacity(instances_count);
    for index in 0..instances_count {
        let mut instance = PluginInstance::create(&ui, &module, class_id, None)?;
        instance.configure_main_buses(&ui, 2)?;
        instance.setup(&ui, ProcessMode::Realtime, SAMPLE_RATE, BLOCK_FRAMES)?;
        instance.activate(&ui)?;
        // All automation is queued before the very first marked callback on its fresh thread.
        queue_automation(&ui, &instance, index)?;
        processors.push(instance.processor(&ui)?);
        instances.push(instance);
    }
    let worker = CallbackWorker::start(processors, case == Case::NativeReset)?;
    match case {
        Case::NativeEditorLifecycle => editor_lifecycle(&ui, &application, &instances, &worker)?,
        _ => run_stage(&ui, &worker, instances_count)?,
    }
    worker.finish(instances_count, case == Case::NativeReset)?;
    for instance in &mut instances {
        instance.deactivate(&ui)?;
    }
    drop(instances);
    handle_events(&ui, EVENT_WAIT);
    application.verify_delegate()
}

/// Queues every stable normalized ID, including all six independent controls in every band.
fn queue_automation(ui: &UiThread, instance: &PluginInstance, stage: usize) -> Result<()> {
    for id in 0..u32::try_from(PARAMETER_COUNT)? {
        let normalized = match id {
            0 => {
                if stage.is_multiple_of(2) {
                    0.6
                } else {
                    0.7
                }
            }
            1 => 0.2,
            2 => 0.5,
            3 => 0.8,
            4..=27 => match (id - 4) % 6 {
                0 => 0.75,
                1 => 0.8,
                2 => {
                    if stage.is_multiple_of(2) {
                        0.2
                    } else {
                        0.7
                    }
                }
                3 => 0.85,
                4 => {
                    if stage.is_multiple_of(2) {
                        0.3
                    } else {
                        0.7
                    }
                }
                _ => f64::from(stage % 4 == 3 && id == 9),
            },
            28 => f64::from(stage % 4 == 1),
            29 => f64::from(stage % 4 == 2),
            _ => f64::from(stage % 4 == 3),
        };
        instance.set_parameter(ui, id, normalized)?;
    }
    Ok(())
}

/// Runs callbacks until stopped, keeping synchronization, signal filling, output checks, and formatting unmarked.
fn callback_loop(processors: &mut [ProcessorHandle], signals: &Signals, reset: bool) -> Result<()> {
    let mut block = 0_usize;
    while !signals.stop.load(Ordering::Acquire) {
        for (index, processor) in processors.iter_mut().enumerate() {
            for channel in 0..2 {
                let samples = processor.input_channel_mut(channel).context("missing stereo input")?;
                for (frame, sample) in samples.iter_mut().enumerate() {
                    let phase = ((block * BLOCK_FRAMES + frame) % 48_000) as f32 / 48_000.0;
                    let side = if channel == 0 { 0.025 } else { -0.025 };
                    *sample = (phase * std::f32::consts::TAU * 1100.0)
                        .sin()
                        .mul_add(side, (phase * std::f32::consts::TAU * 440.0).sin() * 0.1);
                }
            }
            if reset && block.is_multiple_of(8) {
                native_reset_region(processor);
                signals.resets.fetch_add(1, Ordering::Release);
            }
            native_process_region(processor)?;
            for channel in 0..2 {
                ensure!(
                    processor
                        .output_channel(channel)
                        .context("missing stereo output")?
                        .iter()
                        .all(|sample| sample.is_finite()),
                    "non-finite native callback output"
                );
            }
            signals
                .callbacks
                .get(index)
                .context("unexpected component count")?
                .fetch_add(1, Ordering::Release);
        }
        block = block.wrapping_add(1);
        thread::park_timeout(Duration::from_millis(1));
    }
    Ok(())
}

/// Marks the complete production host processing call, including its native admission guard destruction.
/// No setup, synchronization, checks, or teardown is included in this boundary.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn native_process_region(processor: &mut ProcessorHandle) -> Result<(), ProcessError> {
    processor.process(BLOCK_FRAMES)
}

/// Measures the native setProcessing stop/start reset boundary without marking unrelated host lifecycle work.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn native_reset_region(processor: &mut ProcessorHandle) {
    processor.reset(0);
}

/// Proves a correctly marked allocation-free region returns while preserving an observable result.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn clean_region(value: usize) -> usize {
    black_box(usize::wrapping_add as fn(usize, usize) -> usize)(value, 1)
}

/// Positive detector control that cannot optimize its Rust allocation into an unobservable stack value.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn rust_allocation_region() -> Box<[u8; 4096]> {
    black_box(Box::new([black_box(7); 4096]))
}

/// Positive detector control whose native mutex storage is initialized outside this marked boundary.
#[cfg_attr(audiobatch_rtsan, sanitize(realtime = "nonblocking"))]
#[inline(never)]
fn mutex_region(mutex: &Mutex<usize>) {
    if let Ok(mut guard) = mutex.lock() {
        *guard += black_box(1);
    }
}

/// Pumps native UI events for a bounded active interval and requires each component to continue processing.
fn run_stage(ui: &UiThread, worker: &CallbackWorker, instances: usize) -> Result<()> {
    let before: Vec<_> = (0..instances).map(|index| worker.count(index)).collect();
    let deadline = Instant::now() + STAGE_DURATION;
    while Instant::now() < deadline {
        handle_events(ui, EVENT_WAIT);
    }
    for (index, before) in before.into_iter().enumerate() {
        ensure!(
            worker.count(index) > before,
            "instance {index} stopped processing during a UI/state stage"
        );
    }
    Ok(())
}

/// Opens two independent native Iced editors and closes/reopens them while both component callbacks remain active.
fn editor_lifecycle(
    ui: &UiThread,
    application: &Application,
    instances: &[PluginInstance],
    worker: &CallbackWorker,
) -> Result<()> {
    let first = instances.first().context("first editor instance")?;
    let second = instances.get(1).context("second editor instance")?;
    let first_editor = EditorWindow::open(ui, first, "ImageControl RS packaged host A", None, None)?;
    let second_editor = EditorWindow::open(ui, second, "ImageControl RS packaged host B", None, None)?;
    application.verify_delegate()?;
    run_stage(ui, worker, 2)?;
    #[cfg(target_os = "macos")]
    exercise_monitoring_taps(ui, first, worker)?;
    ensure!(
        !first_editor.is_closed() && !second_editor.is_closed(),
        "editor closed before lifecycle observation"
    );
    drop(first_editor);
    run_stage(ui, worker, 2)?;
    application.verify_delegate()?;
    drop(second_editor);
    run_stage(ui, worker, 2)?;
    application.verify_delegate()?;
    for stage in 0..2 {
        queue_automation(ui, first, stage + 2)?;
        queue_automation(ui, second, stage + 3)?;
        let first_editor = EditorWindow::open(ui, first, "ImageControl RS reopened A", None, None)?;
        let second_editor = EditorWindow::open(ui, second, "ImageControl RS reopened B", None, None)?;
        application.verify_delegate()?;
        run_stage(ui, worker, 2)?;
        ensure!(
            !first_editor.is_closed() && !second_editor.is_closed(),
            "reopened editor closed prematurely"
        );
        drop(second_editor);
        drop(first_editor);
        run_stage(ui, worker, 2)?;
        application.verify_delegate()?;
    }
    Ok(())
}

/// Switches actual packaged tap buttons while marked callbacks and correlation continue, then verifies saved readback.
#[cfg(target_os = "macos")]
fn exercise_monitoring_taps(ui: &UiThread, instance: &PluginInstance, worker: &CallbackWorker) -> Result<()> {
    let marker = MainThreadMarker::new().context("monitoring input requires the host UI thread")?;
    let application = NSApplication::sharedApplication(marker);
    let windows = application.windows();
    let window = windows
        .iter()
        .find(|window| window.title().to_string() == "ImageControl RS packaged host A")
        .context("owned monitoring test window")?;
    let parent = window.contentView().context("host content view")?;
    for (x, expected) in [(201.0, "pre"), (243.0, "post")] {
        let bounds = parent.bounds();
        let local = NSPoint::new(
            x,
            if parent.isFlipped() {
                22.0
            } else {
                bounds.size.height - 22.0
            },
        );
        let position = parent.convertPoint_toView(local, None);
        let target = parent.hitTest(local).context("native tap target")?;
        let make_event = |kind, pressure| {
            NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                kind, position, NSEventModifierFlags::empty(), 0.0, window.windowNumber(), None, 1, 1, pressure,
            ).context("native tap mouse event")
        };
        let moved = make_event(NSEventType::MouseMoved, 0.0)?;
        let down = make_event(NSEventType::LeftMouseDown, 1.0)?;
        let up = make_event(NSEventType::LeftMouseUp, 0.0)?;
        // SAFETY: hit testing retains the actual live responder in this owned host window on the UI thread.
        unsafe {
            let _: () = msg_send![&*target, mouseMoved: &*moved];
            let _: () = msg_send![&*target, mouseDown: &*down];
            let _: () = msg_send![&*target, mouseUp: &*up];
        }
        run_stage(ui, worker, 2)?;
        let deadline = Instant::now() + STAGE_DURATION;
        let state = loop {
            match instance.live_state(ui) {
                Ok(state) => break state,
                Err(vst3_host::InstanceError::EditsPending) if Instant::now() < deadline => {
                    handle_events(ui, EVENT_WAIT);
                }
                Err(error) => return Err(error.into()),
            }
        };
        let bytes = state.component;
        let payload = bytes.get(4..).context("bounded state envelope")?;
        let decoded: serde_json::Value = serde_json::from_slice(payload)?;
        ensure!(
            decoded
                .get("editor")
                .and_then(|editor| editor.get("tap"))
                .and_then(serde_json::Value::as_str)
                == Some(expected),
            "native tap click did not publish {expected}"
        );
    }
    Ok(())
}

/// Dispatches actual `AppKit` input and display events on the executable's main thread.
#[cfg(target_os = "macos")]
fn handle_events(_ui: &UiThread, timeout: Duration) {
    let Some(marker) = MainThreadMarker::new() else {
        return;
    };
    let application = NSApplication::sharedApplication(marker);
    let until = NSDate::dateWithTimeIntervalSinceNow(timeout.as_secs_f64());
    // SAFETY: the application and default mode are valid, and event retrieval and dispatch stay on the main thread.
    if let Some(event) = unsafe {
        application.nextEventMatchingMask_untilDate_inMode_dequeue(
            NSEventMask::Any,
            Some(&until),
            NSDefaultRunLoopMode,
            true,
        )
    } {
        application.sendEvent(&event);
    }
    application.updateWindows();
}

/// Pumps the Win32 host event queue while all editor ownership stays on this thread.
#[cfg(not(target_os = "macos"))]
fn handle_events(ui: &UiThread, timeout: Duration) {
    vst3_host::pump_platform_events(ui, timeout);
}
