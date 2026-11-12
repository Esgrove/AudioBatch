//! Bounded Win32 qualification of the packaged editor on its native owner thread.

#![expect(unsafe_code, reason = "exercises the packaged VST3 ABI and its owned Win32 child")]

use std::cell::{Cell, RefCell};
use std::ffi::c_void;
use std::path::Path;
use std::rc::{Rc, Weak};
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, ensure};
use image_control_plugin_rs::{COMPONENT_ID, GAIN_PARAMETER};
use plugin_host_core::{EditorCallbacks, NativeWindow, UiThread, WindowPosition, WindowSize, pump_platform_events};
use vst3::Steinberg::Vst::{
    IComponent, IComponentHandler, IComponentHandlerTrait, IEditController, IEditControllerTrait as _,
};
use vst3::Steinberg::{
    IPlugView, IPlugViewTrait as _, IPluginBaseTrait as _, IPluginFactory, IPluginFactoryTrait as _, ViewRect,
    kResultFalse, kResultOk, tresult,
};
use vst3::{Class, ComPtr, ComRef, ComWrapper, Interface};
use windows_sys::Win32::Foundation::{HWND, POINT};
use windows_sys::Win32::Graphics::Gdi::ClientToScreen;
use windows_sys::Win32::UI::WindowsAndMessaging::{
    GW_CHILD, GetCursorPos, GetWindow, HWND_TOPMOST, IsWindow, SWP_NOMOVE, SWP_NOSIZE, SendMessageW, SetCursorPos,
    SetForegroundWindow, SetWindowPos, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE, WindowFromPoint,
};

const PROCESS_TIMEOUT: Duration = Duration::from_secs(30);
const SETTLE_TIME: Duration = Duration::from_millis(180);

/// Keeps the initialized component alive until every view is removed and released.
struct Component {
    /// Initialized through the packaged factory, not an in-process test factory.
    component: ComPtr<IComponent>,
    /// The wrapper's same-object controller.
    controller: ComPtr<IEditController>,
}

/// Native callbacks outlive their window and do not retain the plugin.
struct Callbacks {
    /// Borrowed view stays live until removal, then is cleared before final release.
    view: Cell<*mut IPlugView>,
    /// Parent identity is valid while this callback state is retained by the editor.
    parent: Cell<HWND>,
}

/// Callback results survive native teardown for late-call checks.
#[derive(Default)]
struct Evidence {
    /// Number of real UI-originated gesture starts.
    begin: Cell<usize>,
    /// Number of real UI-originated parameter writes.
    perform: Cell<usize>,
    /// Number of gesture releases, including any teardown releases.
    end: Cell<usize>,
    /// Removal attempts while the native callback is active, regardless of the selected admission contract.
    removal_attempts: Cell<usize>,
    /// Errors are reported outside the COM boundary, never unwound through it.
    failure: RefCell<Option<String>>,
}

/// The plugin retains no owning path back to its native host.
struct Handler {
    /// Upgraded only for the duration of a callback.
    host: Weak<Callbacks>,
    /// Retained independently to detect callbacks after editor retirement.
    evidence: Rc<Evidence>,
    /// Separates ordinary rejection from the upstream nested-runtime qualification gate.
    nested_events: bool,
    /// Removes the child synchronously instead of exercising the historical retry contract.
    synchronous_removal: bool,
}

/// Retains the parent and view until successful synchronous removal.
struct Editor {
    /// Owned view reference is released before its parent window.
    view: ComPtr<IPlugView>,
    /// Owned parent belongs to the registered thread.
    window: NativeWindow,
    /// `NativeWindow` keeps only a weak callback reference.
    callbacks: Rc<Callbacks>,
    /// Tracks the successful attach/remove pair for error-path cleanup.
    attached: bool,
}

/// Fails the separate process if a native callback or worker join stops returning.
struct Watchdog {
    /// Completion cancels the timeout without sleeping.
    finished: mpsc::Sender<()>,
    /// Joined after all native and plugin resources are released.
    worker: Option<JoinHandle<()>>,
}

/// Restores the user's cursor after bounded input to the owned editor window.
struct CursorRestore(POINT);

impl Drop for CursorRestore {
    fn drop(&mut self) {
        // SAFETY: the saved desktop coordinates came from a successful native cursor query.
        unsafe {
            SetCursorPos(self.0.x, self.0.y);
        }
    }
}

impl Component {
    fn create(library: &libloading::Library) -> Result<Self> {
        // SAFETY: this explicit ImageControl library exports the SDK factory ABI and outlives every COM reference.
        let export =
            unsafe { library.get::<unsafe extern "system" fn() -> *mut IPluginFactory>(b"GetPluginFactory\0") }?;
        // SAFETY: the live export transfers one factory reference.
        let factory = unsafe { ComPtr::from_raw(export()) }.context("packaged factory")?;
        let mut pointer: *mut c_void = std::ptr::null_mut();
        // SAFETY: the IDs are sixteen-byte SDK values and the output slot is writable.
        let result =
            unsafe { factory.createInstance(COMPONENT_ID.as_ptr(), IComponent::IID.as_ptr().cast(), &raw mut pointer) };
        ensure!(result == kResultOk, "createInstance failed: {result}");
        // SAFETY: successful creation transfers an owned component reference.
        let component = unsafe { ComPtr::from_raw(pointer.cast::<IComponent>()) }.context("component")?;
        let controller = component.cast::<IEditController>().context("same-object controller")?;
        ensure!(
            // SAFETY: the own wrapper accepts a null context and initializes on the registered native owner.
            unsafe { component.initialize(std::ptr::null_mut()) } == kResultOk,
            "initialize"
        );
        Ok(Self { component, controller })
    }
}

impl Drop for Component {
    fn drop(&mut self) {
        // SAFETY: every editor is dropped before this initialized component on its owner thread.
        unsafe {
            let _ = self.controller.setComponentHandler(std::ptr::null_mut());
            let _ = self.component.terminate();
        }
    }
}

impl EditorCallbacks for Callbacks {
    fn constrain(&self, size: WindowSize) -> WindowSize {
        size
    }
    fn resized(&self, _size: WindowSize) {}
    fn scale_changed(&self, _factor: f32) {}
    fn close_requested(&self) {}
}

impl Evidence {
    fn record(&self, result: Result<()>) -> tresult {
        if let Err(error) = result {
            self.failure.borrow_mut().get_or_insert_with(|| format!("{error:#}"));
            kResultFalse
        } else {
            kResultOk
        }
    }

    fn verify(&self) -> Result<()> {
        ensure!(
            self.failure.borrow().is_none(),
            "native callback failure: {:?}",
            self.failure.borrow()
        );
        Ok(())
    }

    const fn counts(&self) -> [usize; 4] {
        [
            self.begin.get(),
            self.perform.get(),
            self.end.get(),
            self.removal_attempts.get(),
        ]
    }
}

impl Handler {
    fn reenter(&self, remove: bool) -> Result<()> {
        let host = self.host.upgrade().context("callback reached retired native host")?;
        // SAFETY: the editor retains the borrowed view until synchronous removal and callback return.
        let view = unsafe { ComRef::from_raw(host.view.get()) }.context("callback reached retired view")?;
        let mut size = ViewRect {
            left: 0,
            top: 0,
            right: 1120,
            bottom: 832,
        };
        ensure!(
            // SAFETY: the view and writable rectangle are live on this native owner thread.
            unsafe { view.onSize(&raw mut size) } == kResultOk,
            "callback resize rejected"
        );
        let mut observed = ViewRect {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        ensure!(
            // SAFETY: this read-only reentry uses the same retained view and an independent output slot.
            unsafe { view.getSize(&raw mut observed) } == kResultOk,
            "callback getSize"
        );
        ensure!(
            observed.right == size.right && observed.bottom == size.bottom,
            "callback geometry readback"
        );
        let ui = UiThread::current().context("callback changed native owner thread")?;
        if self.nested_events {
            pump_platform_events(&ui, Duration::from_millis(5));
        }
        if remove && (self.synchronous_removal || !self.nested_events) {
            // SAFETY: these queries and removal attempts use the retained view and owned native parent.
            unsafe {
                let child = GetWindow(host.parent.get(), GW_CHILD);
                ensure!(!child.is_null(), "child missing before callback removal");
                for _ in 0..2 {
                    if self.synchronous_removal {
                        ensure!(view.removed() == kResultOk, "synchronous callback removal failed");
                        ensure!(GetWindow(host.parent.get(), GW_CHILD).is_null(), "child still attached");
                        ensure!(IsWindow(child) == 0, "native child survived synchronous removal");
                        self.evidence
                            .removal_attempts
                            .set(self.evidence.removal_attempts.get() + 1);
                        continue;
                    }
                    ensure!(
                        view.removed() == kResultFalse,
                        "callback removal must be rejected on Windows"
                    );
                    self.evidence
                        .removal_attempts
                        .set(self.evidence.removal_attempts.get() + 1);
                    ensure!(
                        GetWindow(host.parent.get(), GW_CHILD) == child && IsWindow(child) != 0,
                        "rejected removal changed native ownership"
                    );
                }
            }
        }
        Ok(())
    }
}

impl Class for Handler {
    type Interfaces = (IComponentHandler,);
}

impl IComponentHandlerTrait for Handler {
    unsafe fn beginEdit(&self, id: u32) -> tresult {
        eprintln!("win32-stage beginEdit id={id}");
        self.evidence.begin.set(self.evidence.begin.get() + 1);
        self.evidence.record((|| {
            ensure!(id == GAIN_PARAMETER, "pointer did not edit Output");
            self.reenter(false)
        })())
    }

    unsafe fn performEdit(&self, id: u32, value: f64) -> tresult {
        eprintln!("win32-stage performEdit id={id} value={value}");
        self.evidence.perform.set(self.evidence.perform.get() + 1);
        self.evidence.record((|| {
            ensure!(id == GAIN_PARAMETER && value.is_finite(), "invalid Output edit");
            self.reenter(true)
        })())
    }

    unsafe fn endEdit(&self, id: u32) -> tresult {
        eprintln!("win32-stage endEdit id={id}");
        self.evidence.end.set(self.evidence.end.get() + 1);
        self.evidence.record((|| {
            ensure!(id == GAIN_PARAMETER, "unexpected gesture release");
            let host = self.host.upgrade().context("release reached retired host")?;
            ensure!(!host.view.get().is_null(), "release reached retired view");
            Ok(())
        })())
    }

    unsafe fn restartComponent(&self, _flags: i32) -> tresult {
        self.evidence
            .record(self.host.upgrade().context("restart reached retired host").map(|_| ()))
    }
}

impl Editor {
    fn open(
        ui: &UiThread,
        component: &Component,
        evidence: &Rc<Evidence>,
        nested_events: bool,
        synchronous_removal: bool,
    ) -> Result<Self> {
        // SAFETY: the initialized controller and static view name remain live on their owner thread.
        let view = unsafe { ComPtr::from_raw(component.controller.createView(c"editor".as_ptr())) }
            .context("packaged editor view")?;
        let mut initial = ViewRect {
            left: 0,
            top: 0,
            right: 1000,
            bottom: 764,
        };
        ensure!(
            // SAFETY: this live unattached view accepts an explicit host size before native input coordinates are chosen.
            unsafe { view.onSize(&raw mut initial) } == kResultOk,
            "initial host geometry"
        );
        let mut rectangle = ViewRect {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        ensure!(
            // SAFETY: the retained view and writable rectangle are live on their owner thread.
            unsafe { view.getSize(&raw mut rectangle) } == kResultOk,
            "getSize before attach"
        );
        let callbacks = Rc::new(Callbacks {
            view: Cell::new(view.as_ptr()),
            parent: Cell::new(std::ptr::null_mut()),
        });
        let native_callbacks: Rc<dyn EditorCallbacks> = callbacks.clone();
        let size = WindowSize {
            width: rectangle.right - rectangle.left,
            height: rectangle.bottom - rectangle.top,
        };
        let window = NativeWindow::create(ui, &native_callbacks, "ImageControl Win32 reentry", size, true, None)?;
        callbacks.parent.set(window.parent().cast());
        let handler = ComWrapper::new(Handler {
            host: Rc::downgrade(&callbacks),
            evidence: Rc::clone(evidence),
            nested_events,
            synchronous_removal,
        });
        let interface = handler
            .to_com_ptr::<IComponentHandler>()
            .context("component handler interface")?;
        ensure!(
            // SAFETY: the initialized controller retains the interface and callbacks borrow only the live view.
            unsafe { component.controller.setComponentHandler(interface.as_ptr()) } == kResultOk,
            "install handler"
        );
        ensure!(
            // SAFETY: the parent HWND is owned by this thread and outlives the attached child.
            unsafe { view.attached(window.parent(), c"HWND".as_ptr()) } == kResultOk,
            "attach HWND"
        );
        let editor = Self {
            view,
            window,
            callbacks,
            attached: true,
        };
        editor.window.show();
        editor.window.move_to(WindowPosition { x: 64, y: 64 });
        Ok(editor)
    }

    fn child(&self) -> Result<HWND> {
        // SAFETY: the query uses this thread's owned, live parent HWND.
        let child = unsafe { GetWindow(self.window.parent().cast(), GW_CHILD) };
        ensure!(!child.is_null(), "attached native child is missing");
        Ok(child)
    }

    fn close(&mut self) -> Result<()> {
        if self.attached {
            ensure!(
                // SAFETY: removal is outside native dispatch and the retained view and parent are live.
                unsafe { self.view.removed() } == kResultOk,
                "remove outside native callback"
            );
            self.attached = false;
            self.callbacks.view.set(std::ptr::null_mut());
        }
        Ok(())
    }
}

impl Drop for Editor {
    fn drop(&mut self) {
        if let Err(error) = self.close() {
            eprintln!("CASE_FAILED native-editor-reentry cleanup: {error:#}");
            std::process::abort();
        }
    }
}

impl Watchdog {
    fn start() -> Result<Self> {
        let (finished, receiver) = mpsc::channel();
        let worker = thread::Builder::new()
            .name("win32-editor-watchdog".into())
            .spawn(move || {
                if receiver.recv_timeout(PROCESS_TIMEOUT) == Err(mpsc::RecvTimeoutError::Timeout) {
                    eprintln!("CASE_FAILED native-editor-reentry exceeded its 30-second deadline");
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

fn settle(ui: &UiThread) {
    let deadline = Instant::now() + SETTLE_TIME;
    while Instant::now() < deadline {
        pump_platform_events(ui, Duration::from_millis(5));
    }
}

fn native_output_drag(ui: &UiThread, editor: &Editor) -> Result<()> {
    let child = editor.child()?;
    // SAFETY: the live child belongs to this thread.
    // Its actual DPI determines the physical input coordinates.
    let dpi = unsafe { windows_sys::Win32::UI::HiDpi::GetDpiForWindow(child) };
    eprintln!("win32-stage input child={child:p} dpi={dpi}");
    ensure!(dpi != 0, "native input DPI unavailable");
    let dpi = i32::try_from(dpi)?;
    let coordinates = |x: i32, y: i32| -> Result<isize> {
        let x = i16::try_from(x * dpi / 96)?;
        let y = i16::try_from(y * dpi / 96)?;
        Ok(isize::try_from(
            u32::from(x.cast_unsigned()) | (u32::from(y.cast_unsigned()) << 16),
        )?)
    };
    let start = coordinates(850, 22)?;
    let end = coordinates(890, 22)?;
    let mut previous = POINT { x: 0, y: 0 };
    ensure!(
        // SAFETY: the output point is valid writable memory.
        unsafe { GetCursorPos(&raw mut previous) } != 0,
        "query cursor before native input"
    );
    let _restore = CursorRestore(previous);
    // SAFETY: this probe may activate only its own live parent before delivering input.
    unsafe {
        ensure!(
            SetWindowPos(
                editor.callbacks.parent.get(),
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE,
            ) != 0,
            "raise owned input window"
        );
        SetForegroundWindow(editor.callbacks.parent.get());
    }
    let mut screen = POINT {
        x: i32::try_from(start & 0xFFFF)?,
        y: i32::try_from(start >> 16)?,
    };
    // SAFETY: the child remains owned, and only its client point is converted and targeted.
    unsafe {
        ensure!(ClientToScreen(child, &raw mut screen) != 0, "convert owned input point");
        ensure!(SetCursorPos(screen.x, screen.y) != 0, "position cursor in owned child");
    }
    settle(ui);
    // SAFETY: this diagnostic only queries the pointer target at the converted client point.
    let target = unsafe { WindowFromPoint(screen) };
    eprintln!(
        "win32-stage target={target:p} child={child:p} screen={},{}",
        screen.x, screen.y
    );
    ensure!(target == child, "owned child is not under the native pointer");
    // SAFETY: only this thread's actual Iced child receives normal Win32 pointer messages.
    unsafe {
        SendMessageW(child, WM_MOUSEMOVE, 0, start);
    }
    settle(ui);
    eprintln!("win32-stage pointer-down");
    // SAFETY: the pointer and live child remain at the start of this bounded gesture.
    unsafe {
        SendMessageW(child, WM_LBUTTONDOWN, 1, start);
    }
    settle(ui);
    // SAFETY: the retained parent is live.
    // Callback removal may have detached or destroyed its child.
    if unsafe { GetWindow(editor.callbacks.parent.get(), GW_CHILD) } != child {
        return Ok(());
    }
    eprintln!("win32-stage pointer-drag");
    let mut screen_end = POINT {
        x: i32::try_from(end & 0xFFFF)?,
        y: i32::try_from(end >> 16)?,
    };
    // SAFETY: the end point belongs to the same retained native child.
    unsafe {
        ensure!(ClientToScreen(child, &raw mut screen_end) != 0, "convert drag end");
        ensure!(SetCursorPos(screen_end.x, screen_end.y) != 0, "position drag end");
    }
    // SAFETY: the child remains attached before this dispatch.
    // The callback may synchronously remove it.
    unsafe {
        SendMessageW(child, WM_MOUSEMOVE, 1, end);
    }
    settle(ui);
    // SAFETY: this identity query prevents pointer-up delivery through a retired native child handle.
    if unsafe { GetWindow(editor.callbacks.parent.get(), GW_CHILD) } != child {
        return Ok(());
    }
    eprintln!("win32-stage pointer-up");
    // SAFETY: the native parent still owns this child after the last callback dispatch.
    unsafe {
        SendMessageW(child, WM_LBUTTONUP, 0, end);
    }
    settle(ui);
    Ok(())
}

/// Exercises explicit packaged attachment and native retirement before unloading the module.
pub fn run(bundle: &Path, nested_events: bool, synchronous_removal: bool) -> Result<()> {
    let _watchdog = Watchdog::start()?;
    let ui = UiThread::register_current()?;
    let executable = bundle.join("Contents/x86_64-win/ImageControl RS.vst3");
    ensure!(
        executable.is_file(),
        "missing packaged module: {}",
        executable.display()
    );
    // SAFETY: the exact build-tree module remains loaded until every native child and COM reference is gone.
    let library = unsafe { libloading::Library::new(executable) }.context("load packaged Win32 VST3")?;
    let component = Component::create(&library)?;
    for iteration in 0..3 {
        eprintln!("win32-stage iteration={iteration} reset");
        ensure!(
            // SAFETY: this reset uses the initialized controller between distinct native view lifetimes.
            unsafe { component.controller.setParamNormalized(GAIN_PARAMETER, 0.5) } == kResultOk,
            "reset Output"
        );
        let evidence = Rc::new(Evidence::default());
        eprintln!("win32-stage iteration={iteration} open");
        let mut editor = Editor::open(&ui, &component, &evidence, nested_events, synchronous_removal)?;
        eprintln!("win32-stage iteration={iteration} attached");
        settle(&ui);
        // SAFETY: independent readback uses the initialized controller on its owner thread.
        let before = unsafe { component.controller.getParamNormalized(GAIN_PARAMETER) };
        let child = editor.child()?;
        native_output_drag(&ui, &editor)?;
        evidence.verify()?;
        ensure!(
            evidence.begin.get() > 0
                && evidence.perform.get() > 0
                && (nested_events && !synchronous_removal || evidence.removal_attempts.get() > 0),
            "native pointer did not produce the required callback reentry: {:?}",
            evidence.counts()
        );
        ensure!(evidence.begin.get() == evidence.end.get(), "unbalanced native gesture");
        // SAFETY: this separate controller read occurs after the native gesture has completed.
        let after = unsafe { component.controller.getParamNormalized(GAIN_PARAMETER) };
        ensure!(
            before.to_bits() != after.to_bits(),
            "native edit did not change authoritative Output state"
        );
        eprintln!("win32-stage iteration={iteration} final-removal");
        editor.close()?;
        eprintln!("win32-stage iteration={iteration} removed");
        ensure!(
            // SAFETY: the owned parent remains live after synchronous child removal.
            unsafe { GetWindow(editor.window.parent().cast(), GW_CHILD) }.is_null(),
            "child still attached"
        );
        settle(&ui);
        ensure!(
            // SAFETY: IsWindow checks the captured identity without dereferencing any retired window storage.
            unsafe { IsWindow(child) } == 0,
            "native child survived successful removal"
        );
        drop(editor);
        let counts = evidence.counts();
        settle(&ui);
        evidence.verify()?;
        ensure!(counts == evidence.counts(), "late callbacks after native retirement");
        println!("win32-editor-reentry iteration={iteration} callbacks={counts:?} child=retired");
    }
    drop(component);
    settle(&ui);
    drop(library);
    let case = if synchronous_removal {
        "synchronous-removal"
    } else if nested_events {
        "nested-dispatch"
    } else {
        "removal-retry"
    };
    println!("CASE_OK native-editor-reentry windows-pointer-{case}");
    Ok(())
}
