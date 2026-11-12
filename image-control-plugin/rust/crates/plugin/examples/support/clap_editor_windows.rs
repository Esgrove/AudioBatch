//! Bounded Win32 child ownership checks against the explicitly selected packaged CLAP editor.

#![expect(
    unsafe_code,
    reason = "owned HWND queries and the documented CLAP native-parent boundary"
)]

use std::cell::{Cell, RefCell};
use std::path::Path;
use std::rc::Rc;
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, ensure};
use clack_extensions::gui::{GuiApiType, GuiConfiguration, GuiSize, PluginGui, Window};
use clack_extensions::params::PluginParams;
use clack_host::events::{
    event_types::{ParamGestureBeginEvent, ParamGestureEndEvent},
    io::{EventBuffer, InputEvents, OutputEvents},
};
use plugin_host_core::{EditorCallbacks, NativeWindow, UiThread, WindowPosition, WindowSize, pump_platform_events};
use windows_sys::Win32::Foundation::{HWND, POINT, RECT};
use windows_sys::Win32::Graphics::Gdi::ClientToScreen;
use windows_sys::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    GW_CHILD, GW_HWNDNEXT, GetClientRect, GetWindow, IsWindow, IsWindowVisible,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    GetCursorPos, HWND_TOPMOST, SWP_NOMOVE, SWP_NOSIZE, SendMessageW, SetCursorPos, SetForegroundWindow, SetWindowPos,
    WM_LBUTTONDOWN, WM_MOUSEMOVE, WindowFromPoint,
};

#[path = "clap_editor_host.rs"]
mod host;

/// A blocked native call must fail the process rather than hang a qualification run.
const PROCESS_TIMEOUT: Duration = Duration::from_secs(30);
/// One bounded native settling interval, independent of successful CLAP return values.
const SETTLE_TIME: Duration = Duration::from_millis(180);
/// Only embedded native HWND children are accepted.
const CONFIGURATION: GuiConfiguration<'static> = GuiConfiguration {
    api_type: GuiApiType::WIN32,
    is_floating: false,
};

/// Keeps host parent creation and hit testing in the same physical coordinate space as baseview.
struct DpiContext(DPI_AWARENESS_CONTEXT);

impl DpiContext {
    /// Selects per-monitor awareness only for this native host thread.
    fn enter() -> Result<Self> {
        // SAFETY: the documented awareness pseudo-handle is valid.
        // The previous context is restored on drop.
        let previous = unsafe { SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) };
        ensure!(!previous.is_null(), "set native host DPI context");
        Ok(Self(previous))
    }
}

impl Drop for DpiContext {
    /// Restores the caller's awareness after all native windows have been destroyed.
    fn drop(&mut self) {
        // SAFETY: this is the context returned by the successful owner-thread selection.
        unsafe {
            SetThreadDpiAwarenessContext(self.0);
        }
    }
}

/// Native host callbacks never modify plugin state during window dispatch.
struct Callbacks;

impl EditorCallbacks for Callbacks {
    /// Accepts the qualification driver's explicit client geometry unchanged.
    fn constrain(&self, size: WindowSize) -> WindowSize {
        size
    }
    /// Geometry is driven explicitly through CLAP rather than recursive native notifications.
    fn resized(&self, _size: WindowSize) {}
    /// This run checks native 100% dimensions.
    /// It does not simulate monitor DPI changes.
    fn scale_changed(&self, _factor: f32) {}
    /// The bounded driver owns the lifetime instead of closing inside native dispatch.
    fn close_requested(&self) {}
}

/// Fatal process guard covers plugin loading, ABI calls, native dispatch, and module release.
struct Watchdog {
    /// Signals ordinary completion, including an assertion failure.
    finished: mpsc::Sender<()>,
    /// Joined after all borrowed native and plugin ownership has ended.
    worker: Option<JoinHandle<()>>,
}

impl Watchdog {
    /// Arms the deadline before the first dynamic-library operation.
    fn start() -> Result<Self> {
        let (finished, receiver) = mpsc::channel();
        let worker = thread::Builder::new()
            .name("win32-clap-watchdog".into())
            .spawn(move || {
                if receiver.recv_timeout(PROCESS_TIMEOUT) == Err(mpsc::RecvTimeoutError::Timeout) {
                    eprintln!("CASE_FAILED native-clap-editor exceeded its internal deadline");
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
    /// Cancels the deadline only after ordinary lifecycle cleanup has run.
    fn drop(&mut self) {
        let _ = self.finished.send(());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

/// Keeps the native parent and owning Clack instance alive until synchronous GUI destruction.
struct Editor<'a> {
    /// Exclusive main-thread access owns the dynamically loaded plugin lifetime.
    instance: &'a mut host::Instance,
    /// Extension pointer is valid through the borrowed plugin instance.
    gui: PluginGui,
    /// Host parent is never transferred to the plugin.
    parent: &'a NativeWindow,
    /// Every successful create is paired with destroy on assertion failures too.
    created: bool,
}

impl Editor<'_> {
    /// Gets the single actual child, rejecting accidental floating or duplicate editor windows.
    fn child(&self) -> Result<HWND> {
        // SAFETY: this guard retains the host parent on its registered UI thread.
        let child = unsafe { GetWindow(self.parent.parent().cast(), GW_CHILD) };
        ensure!(!child.is_null(), "CLAP did not attach a direct HWND child");
        ensure!(
            // SAFETY: child is the live first child obtained from the owned parent above.
            unsafe { GetWindow(child, GW_HWNDNEXT) }.is_null(),
            "duplicate native children"
        );
        Ok(child)
    }

    /// Verifies native physical dimensions separately from the CLAP size readback.
    fn verify_size(&mut self, child: HWND, expected: GuiSize) -> Result<()> {
        let actual = self
            .gui
            .get_size(&self.instance.plugin_handle())
            .context("gui.get_size")?;
        ensure!(
            actual.width == expected.width && actual.height == expected.height,
            "CLAP size mismatch"
        );
        let mut rectangle = RECT::default();
        ensure!(
            // SAFETY: the live editor owns this child and the output rectangle is writable.
            unsafe { GetClientRect(child, &raw mut rectangle) } != 0,
            "GetClientRect"
        );
        ensure!(
            rectangle.right == i32::try_from(expected.width)? && rectangle.bottom == i32::try_from(expected.height)?,
            "native size mismatch: {}x{}",
            rectangle.right,
            rectangle.bottom
        );
        Ok(())
    }

    /// Destroys before the host may retire its native parent or loaded module.
    fn destroy(&mut self) {
        if self.created {
            self.created = false;
            self.gui.destroy(&self.instance.plugin_handle());
        }
    }
}

impl Drop for Editor<'_> {
    /// Retires a successfully created GUI even when a later assertion returns an error.
    fn drop(&mut self) {
        self.destroy();
    }
}

/// Runs bounded Win32 dispatch and deferred CLAP main-thread work without audio hardware.
fn settle(ui: &UiThread, instance: &mut host::Instance) -> Result<()> {
    let deadline = Instant::now() + SETTLE_TIME;
    while Instant::now() < deadline {
        pump_platform_events(ui, Duration::from_millis(5));
        host::service_callbacks(instance)?;
    }
    Ok(())
}

/// Requires embedded Win32 support without accepting a floating or foreign-platform fallback.
fn checked_gui(instance: &mut host::Instance) -> Result<PluginGui> {
    let handle = instance.plugin_handle();
    let gui = handle
        .get_extension::<PluginGui>()
        .context("packaged Win32 CLAP GUI extension is disabled")?;
    ensure!(
        gui.is_api_supported(&handle, CONFIGURATION),
        "embedded Win32 unsupported"
    );
    ensure!(
        !gui.is_api_supported(
            &handle,
            GuiConfiguration {
                is_floating: true,
                ..CONFIGURATION
            }
        ),
        "floating GUI advertised"
    );
    let preferred = gui.get_preferred_api(&handle).context("preferred GUI API")?;
    ensure!(
        preferred.api_type == GuiApiType::WIN32 && !preferred.is_floating,
        "wrong preferred API"
    );
    Ok(gui)
}

/// Exercises native hidden attachment, visibility, resize, pending work, destroy, and two reopens.
pub fn run(bundle: &Path) -> Result<()> {
    let _watchdog = Watchdog::start()?;
    let _dpi = DpiContext::enter()?;
    let ui = UiThread::register_current()?;
    let mut instance = host::load(bundle)?;
    let gui = checked_gui(&mut instance)?;
    let initial = GuiSize {
        width: 1000,
        height: 764,
    };
    let callbacks: Rc<dyn EditorCallbacks> = Rc::new(Callbacks);
    let parent = NativeWindow::create(
        &ui,
        &callbacks,
        "ImageControl direct CLAP native editor",
        WindowSize {
            width: i32::try_from(initial.width)?,
            height: i32::try_from(initial.height)?,
        },
        true,
        None,
    )?;
    parent.show();
    for iteration in 0..3 {
        gui.create(&instance.plugin_handle(), CONFIGURATION)
            .context("gui.create")?;
        let mut editor = Editor {
            instance: &mut instance,
            gui,
            parent: &parent,
            created: true,
        };
        let handle = editor.instance.plugin_handle();
        ensure!(
            editor.gui.create(&handle, CONFIGURATION).is_err(),
            "duplicate create accepted"
        );
        editor.gui.set_scale(&handle, 1.0).context("gui.set_scale")?;
        editor.gui.set_size(&handle, initial).context("pre-parent size")?;
        // SAFETY: Editor borrows the actual host-owned HWND until after gui.destroy returns.
        unsafe {
            editor
                .gui
                .set_parent(&handle, Window::from_win32_hwnd(parent.parent().cast()))
                .context("gui.set_parent")?;
        }
        let child = editor.child()?;
        settle(&ui, editor.instance)?;
        editor.verify_size(child, initial)?;
        for visible in [false, true, false, true] {
            let handle = editor.instance.plugin_handle();
            if visible {
                editor.gui.show(&handle)?;
            } else {
                editor.gui.hide(&handle)?;
            }
            settle(&ui, editor.instance)?;
            ensure!(
                // SAFETY: the created editor retains its original child during visibility-only changes.
                (unsafe { IsWindowVisible(child) } != 0) == visible,
                "native visibility mismatch"
            );
            ensure!(editor.child()? == child, "visibility replaced the child");
        }
        let resized = GuiSize {
            width: 1120,
            height: 832,
        };
        parent.set_content_size(WindowSize {
            width: i32::try_from(resized.width)?,
            height: i32::try_from(resized.height)?,
        });
        editor.gui.set_size(&editor.instance.plugin_handle(), resized)?;
        settle(&ui, editor.instance)?;
        editor.verify_size(child, resized)?;
        editor.gui.set_size(&editor.instance.plugin_handle(), initial)?;
        editor.gui.hide(&editor.instance.plugin_handle())?;
        editor.destroy();
        ensure!(
            // SAFETY: IsWindow checks the retired identity.
            // The parent is still owned and live.
            unsafe { IsWindow(child) } == 0,
            "CLAP destroy returned with a live HWND"
        );
        ensure!(
            // SAFETY: the host parent remains valid until after this editor guard is dropped.
            unsafe { GetWindow(parent.parent().cast(), GW_CHILD) }.is_null(),
            "child survived destroy"
        );
        settle(&ui, editor.instance)?;
        println!("win32-clap-editor iteration={iteration} child=retired pending-work=cancelled");
    }
    drop(parent);
    drop(instance);
    for iteration in 0..3 {
        reentrant_destroy(&ui, bundle)?;
        println!("win32-clap-editor reentry-iteration={iteration}");
    }
    pump_platform_events(&ui, Duration::from_millis(10));
    println!("CASE_OK native-clap-editor win32 create-show-hide-resize-destroy-reopen");
    Ok(())
}

/// Restores the user's cursor after the bounded gesture, including ordinary assertion failures.
struct CursorRestore(POINT);

impl Drop for CursorRestore {
    /// Uses the saved desktop point without interacting with another application's controls.
    fn drop(&mut self) {
        // SAFETY: the saved coordinates contain no borrowed native storage.
        unsafe {
            SetCursorPos(self.0.x, self.0.y);
        }
    }
}

/// Owns error cleanup without retaining a host-instance borrow across native callback dispatch.
struct ReentryEditor {
    /// The loaded module stays alive until after this guard retires the GUI.
    instance: Rc<RefCell<host::Instance>>,
    /// Native GUI extension belongs to the retained plugin instance.
    gui: PluginGui,
    /// Set before destruction to prevent recursive host callbacks from closing twice.
    destroyed: Rc<Cell<bool>>,
}

impl Drop for ReentryEditor {
    /// Completes native teardown when the pointer or a qualification assertion fails early.
    fn drop(&mut self) {
        if !self.destroyed.replace(true) {
            self.gui.destroy(&self.instance.borrow_mut().plugin_handle());
        }
    }
}

/// Destroys from an actual `request_flush` callback during Iced's parameter gesture and checks queued ends.
fn reentrant_destroy(ui: &UiThread, bundle: &Path) -> Result<()> {
    let instance = Rc::new(RefCell::new(host::load(bundle)?));
    let callbacks: Rc<dyn EditorCallbacks> = Rc::new(Callbacks);
    let parent = NativeWindow::create(
        ui,
        &callbacks,
        "ImageControl CLAP callback destruction",
        WindowSize {
            width: 1000,
            height: 764,
        },
        true,
        None,
    )?;
    parent.move_to(WindowPosition { x: 64, y: 64 });
    parent.show();
    let gui = instance
        .borrow_mut()
        .plugin_handle()
        .get_extension::<PluginGui>()
        .context("reentry GUI extension")?;
    gui.create(&instance.borrow_mut().plugin_handle(), CONFIGURATION)?;
    let destroyed = Rc::new(Cell::new(false));
    let _editor = ReentryEditor {
        instance: Rc::clone(&instance),
        gui,
        destroyed: Rc::clone(&destroyed),
    };
    {
        let mut instance = instance.borrow_mut();
        let handle = instance.plugin_handle();
        gui.set_size(
            &handle,
            GuiSize {
                width: 1000,
                height: 764,
            },
        )?;
        // SAFETY: the owned parent outlives the error-cleanup guard and every explicit destroy call.
        unsafe {
            gui.set_parent(&handle, Window::from_win32_hwnd(parent.parent().cast()))?;
        }
        gui.show(&handle)?;
    }
    settle(ui, &mut instance.borrow_mut())?;
    // SAFETY: this live, owned parent has completed its native attachment.
    let child = unsafe { GetWindow(parent.parent().cast(), GW_CHILD) };
    ensure!(!child.is_null(), "reentry child missing");
    let weak = Rc::downgrade(&instance);
    let callback_destroyed = Rc::clone(&destroyed);
    let _callback = host::on_flush(Rc::new(move || {
        if callback_destroyed.replace(true) {
            return;
        }
        if let Some(instance) = weak.upgrade() {
            if let Some(ui) = UiThread::current() {
                pump_platform_events(&ui, Duration::from_millis(5));
            }
            gui.destroy(&instance.borrow_mut().plugin_handle());
        }
    }));
    native_press(ui, &instance, &parent, child)?;
    ensure!(destroyed.get(), "native gesture did not enter request_flush");
    ensure!(
        // SAFETY: this checks only the captured identity after synchronous destruction.
        unsafe { IsWindow(child) } == 0,
        "reentrant CLAP destroy retained its HWND"
    );
    let mut instance = instance.borrow_mut();
    let params = instance
        .plugin_handle()
        .get_extension::<PluginParams>()
        .context("reentry params")?;
    let mut events = EventBuffer::new();
    params.flush(
        &mut instance.inactive_plugin_handle().context("inactive GUI host")?,
        &InputEvents::empty(),
        &mut OutputEvents::from_buffer(&mut events),
    );
    let input = InputEvents::from_buffer(&events);
    ensure!(
        input.len() == 2,
        "destroy must preserve exactly one gesture begin/end pair"
    );
    ensure!(
        input
            .get(0)
            .and_then(|event| event.as_event::<ParamGestureBeginEvent>())
            .is_some(),
        "missing queued begin"
    );
    ensure!(
        input
            .get(1)
            .and_then(|event| event.as_event::<ParamGestureEndEvent>())
            .is_some(),
        "missing queued end"
    );
    settle(ui, &mut instance)?;
    println!("win32-clap-editor callback-destroy=retired queued-gesture=balanced");
    Ok(())
}

/// Delivers native pointer input without retaining a host-instance borrow during dispatch.
fn native_press(ui: &UiThread, instance: &RefCell<host::Instance>, parent: &NativeWindow, child: HWND) -> Result<()> {
    let mut previous = POINT::default();
    // SAFETY: previous is writable, and activation targets only the driver's owned parent.
    unsafe {
        ensure!(GetCursorPos(&raw mut previous) != 0, "get cursor");
        SetWindowPos(
            parent.parent().cast(),
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE,
        );
        SetForegroundWindow(parent.parent().cast());
    }
    let _restore = CursorRestore(previous);
    let mut position = POINT { x: 850, y: 22 };
    // SAFETY: the owned parent retains this live child until the armed callback destroys it.
    unsafe {
        ensure!(
            ClientToScreen(child, &raw mut position) != 0,
            "convert gesture position"
        );
        ensure!(SetCursorPos(position.x, position.y) != 0, "position native cursor");
    }
    settle(ui, &mut instance.borrow_mut())?;
    wait_for_pointer_target(ui, parent, child, position)?;
    let mut rectangle = RECT::default();
    // SAFETY: all queried HWNDs belong to this driver's live parent/child hierarchy.
    unsafe {
        GetClientRect(child, &raw mut rectangle);
        eprintln!(
            "clap-input parent={:p} child={child:p} visible={} parent-visible={} size={}x{} dpi={} screen={},{}",
            parent.parent(),
            IsWindowVisible(child),
            IsWindowVisible(parent.parent().cast()),
            rectangle.right,
            rectangle.bottom,
            windows_sys::Win32::UI::HiDpi::GetDpiForWindow(child),
            position.x,
            position.y,
        );
    }
    let point = (0x16_isize << 16) | 0x0352;
    // SAFETY: the target remains live before the gesture and the pointer is over the actual control.
    unsafe {
        SendMessageW(child, WM_MOUSEMOVE, 0, point);
    }
    settle(ui, &mut instance.borrow_mut())?;
    // SAFETY: no host instance borrow is held across native input.
    // Destruction may occur synchronously.
    unsafe {
        SendMessageW(child, WM_LBUTTONDOWN, 1, point);
    }
    let deadline = Instant::now() + SETTLE_TIME;
    while Instant::now() < deadline {
        pump_platform_events(ui, Duration::from_millis(5));
        host::service_callbacks(&mut instance.borrow_mut())?;
    }
    Ok(())
}

/// Waits for real native stacking to expose the owned child without sending input to an obstruction.
fn wait_for_pointer_target(ui: &UiThread, parent: &NativeWindow, child: HWND, position: POINT) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        // SAFETY: queries use desktop coordinates and only the driver's live parent is raised.
        unsafe {
            if WindowFromPoint(position) == child {
                ensure!(
                    SetCursorPos(position.x, position.y) != 0,
                    "position visible native cursor"
                );
                return Ok(());
            }
            if Instant::now() >= deadline {
                use windows_sys::Win32::UI::Input::KeyboardAndMouse::IsWindowEnabled;
                use windows_sys::Win32::UI::WindowsAndMessaging::{GetClassNameW, GetWindowRect};
                let target = WindowFromPoint(position);
                let mut class = [0_u16; 128];
                let length = usize::try_from(GetClassNameW(target, class.as_mut_ptr(), 128))?;
                for (name, window) in [("parent", parent.parent().cast()), ("child", child), ("target", target)] {
                    let mut rectangle = RECT::default();
                    GetWindowRect(window, &raw mut rectangle);
                    eprintln!(
                        "clap-hit {name}={window:p} enabled={} visible={} rect={},{},{},{}",
                        IsWindowEnabled(window),
                        IsWindowVisible(window),
                        rectangle.left,
                        rectangle.top,
                        rectangle.right,
                        rectangle.bottom
                    );
                }
                anyhow::bail!(
                    "CLAP child obstructed after bounded stacking wait: target class={}",
                    String::from_utf16_lossy(class.get(..length).context("native class length")?)
                );
            }
            ensure!(
                SetWindowPos(
                    parent.parent().cast(),
                    HWND_TOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE | SWP_NOSIZE
                ) != 0,
                "raise owned CLAP window"
            );
        }
        pump_platform_events(ui, Duration::from_millis(5));
    }
}
