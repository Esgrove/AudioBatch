//! Main-thread `AppKit` parent and native-child assertions for the actual packaged CLAP Iced editor.

#![expect(
    unsafe_code,
    reason = "`AppKit` construction, bounded event dispatch, and the retained NSView-to-CLAP parent boundary"
)]

use std::path::Path;
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use anyhow::{Context as _, Result, ensure};
use clack_extensions::gui::{GuiApiType, GuiConfiguration, GuiSize, PluginGui, Window};
use objc2::{MainThreadMarker, MainThreadOnly as _, rc::Retained};
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSBackingStoreType, NSEventMask, NSView, NSWindow, NSWindowStyleMask,
};
use objc2_foundation::{NSDate, NSDefaultRunLoopMode, NSPoint, NSRect, NSSize, NSString};

#[path = "clap_editor_host.rs"]
mod host;

/// Covers loading, native attachment, event delivery, worker joins, and plugin/library destruction.
const PROCESS_TIMEOUT: Duration = Duration::from_secs(30);
/// Native state must become observable within this bounded event interval.
const EVENT_TIMEOUT: Duration = Duration::from_secs(2);
/// Flushes native teardown and pending timer work after each explicit lifecycle boundary.
const SETTLE_TIME: Duration = Duration::from_millis(120);
/// Ordinary direct CLAP hosts must receive an embedded Cocoa child, not a floating window.
const CONFIGURATION: GuiConfiguration<'static> = GuiConfiguration {
    api_type: GuiApiType::COCOA,
    is_floating: false,
};

/// Private application belongs to this executable and stays on `AppKit`'s real main thread.
struct Application(Retained<NSApplication>);

/// Bounded process guard remains armed until every native and plugin resource has been retired.
struct Watchdog {
    /// Normal completion, including ordinary errors, cancels the fatal timeout.
    finished: mpsc::Sender<()>,
    /// Joined only after native resources have been released.
    worker: Option<JoinHandle<()>>,
}

/// Host-owned `AppKit` parent outlives every CLAP GUI lifetime, including error teardown.
struct Parent {
    /// Retained native window never transfers ownership to the plugin.
    window: Retained<NSWindow>,
    /// Stable direct parent used for native child identity, visibility, and detachment assertions.
    view: Retained<NSView>,
}

/// Guard destroys the GUI before its borrowed native parent or owning Clack instance can be released.
struct Editor<'a> {
    /// Mutable instance borrow keeps all plugin calls on this single native owner thread.
    instance: &'a mut host::Instance,
    /// Extension remains valid through the borrowed instance's owned dynamic entry.
    gui: PluginGui,
    /// Borrowed parent enforces the native handle lifetime required by CLAP `set_parent`.
    parent: &'a Parent,
    /// Successful create is paired with destroy even when an assertion fails during attachment.
    created: bool,
}

impl Watchdog {
    /// Starts before native loading so even a blocked ABI call fails instead of hanging indefinitely.
    fn start() -> Result<Self> {
        let (finished, receiver) = mpsc::channel();
        let worker = thread::Builder::new()
            .name("clap-editor-watchdog".into())
            .spawn(move || {
                if receiver.recv_timeout(PROCESS_TIMEOUT) == Err(mpsc::RecvTimeoutError::Timeout) {
                    eprintln!("CASE_FAILED native-clap-editor exceeded its internal 30-second deadline");
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
    /// Cancels and joins after the main thread has completed native teardown or returned an ordinary error.
    fn drop(&mut self) {
        let _ = self.finished.send(());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Application {
    /// Initializes the host application without giving the plugin ownership of its delegate or event loop.
    fn start(marker: MainThreadMarker) -> Self {
        let application = NSApplication::sharedApplication(marker);
        application.setActivationPolicy(NSApplicationActivationPolicy::Regular);
        application.finishLaunching();
        application.activate();
        Self(application)
    }

    /// Dispatches one native event with a short bounded wait and a scoped autorelease pool.
    fn pump(&self) {
        objc2::rc::autoreleasepool(|_| {
            let until = NSDate::dateWithTimeIntervalSinceNow(0.005);
            // SAFETY: application, date, and run-loop mode stay live on `AppKit`'s main thread.
            if let Some(event) = unsafe {
                self.0.nextEventMatchingMask_untilDate_inMode_dequeue(
                    NSEventMask::Any,
                    Some(&until),
                    NSDefaultRunLoopMode,
                    true,
                )
            } {
                self.0.sendEvent(&event);
            }
            self.0.updateWindows();
        });
    }

    /// Runs deferred native operations and CLAP main-thread requests for a fixed bounded interval.
    fn settle(&self, instance: &mut host::Instance) -> Result<()> {
        let deadline = Instant::now() + SETTLE_TIME;
        while Instant::now() < deadline {
            self.pump();
            host::service_callbacks(instance)?;
        }
        Ok(())
    }

    /// Waits for actual native state, never merely trusting successful CLAP return values.
    fn until(&self, instance: &mut host::Instance, mut ready: impl FnMut() -> bool, description: &str) -> Result<()> {
        let deadline = Instant::now() + EVENT_TIMEOUT;
        while !ready() && Instant::now() < deadline {
            self.pump();
            host::service_callbacks(instance)?;
        }
        ensure!(ready(), "timed out waiting for {description}");
        Ok(())
    }
}

impl Parent {
    /// Creates one host-controlled native view without registering an installed plugin or using audio hardware.
    fn create(marker: MainThreadMarker) -> Self {
        let rectangle = NSRect::new(NSPoint::new(0.0, 0.0), NSSize::new(1000.0, 764.0));
        // SAFETY: frame, backing, and style are valid and construction is confined to the main thread.
        let window = unsafe {
            NSWindow::initWithContentRect_styleMask_backing_defer(
                NSWindow::alloc(marker),
                rectangle,
                NSWindowStyleMask::Titled | NSWindowStyleMask::Closable | NSWindowStyleMask::Resizable,
                NSBackingStoreType::Buffered,
                false,
            )
        };
        // SAFETY: this guard owns the window's retained reference, so close must not release it independently.
        unsafe { window.setReleasedWhenClosed(false) };
        window.setTitle(&NSString::from_str("ImageControl direct CLAP native editor smoke"));
        let view = NSView::initWithFrame(NSView::alloc(marker), rectangle);
        window.setContentView(Some(&view));
        window.center();
        window.makeKeyAndOrderFront(None);
        Self { window, view }
    }

    /// Mirrors host-accepted logical point dimensions in the real `AppKit` client area.
    fn resize(&self, size: GuiSize) {
        self.window
            .setContentSize(NSSize::new(f64::from(size.width), f64::from(size.height)));
    }

    /// Retains the actual single direct child so destruction can be verified without dereferencing a dead pointer.
    fn child(&self) -> Result<Retained<NSView>> {
        let children = self.view.subviews();
        ensure!(
            children.count() == 1,
            "expected exactly one direct packaged-editor NSView child"
        );
        Ok(children.objectAtIndex(0))
    }

    /// Requires a completely detached child immediately after destroy, before pumping any native events.
    fn detached(&self, child: &NSView) -> bool {
        // SAFETY: the retained child and host parent stay live on their `AppKit` owner thread throughout this query.
        self.view.subviews().count() == 0 && unsafe { child.superview() }.is_none() && child.window().is_none()
    }
}

impl Drop for Parent {
    /// Closes the retained host window only after all borrowed editor lifetimes have ended.
    fn drop(&mut self) {
        self.window.orderOut(None);
        self.window.close();
    }
}

impl<'a> Editor<'a> {
    /// Creates, sizes, and embeds a packaged GUI while keeping the parent alive on all failure paths.
    fn open(instance: &'a mut host::Instance, parent: &'a Parent, size: GuiSize) -> Result<Self> {
        let handle = instance.plugin_handle();
        let gui = handle
            .get_extension::<PluginGui>()
            .context("packaged CLAP GUI extension")?;
        ensure!(
            gui.is_api_supported(&handle, CONFIGURATION),
            "embedded Cocoa API not supported"
        );
        let preferred = gui.get_preferred_api(&handle).context("preferred native GUI API")?;
        ensure!(
            preferred.api_type == CONFIGURATION.api_type && !preferred.is_floating,
            "preferred GUI API is not embedded Cocoa"
        );
        gui.create(&handle, CONFIGURATION).context("gui.create")?;
        let mut editor = Self {
            instance,
            gui,
            parent,
            created: true,
        };
        {
            let handle = editor.instance.plugin_handle();
            ensure!(editor.gui.can_resize(&handle), "packaged GUI does not support resizing");
            editor.gui.set_size(&handle, size).context("pre-parent gui.set_size")?;
        }
        editor.verify_size(size)?;
        editor.parent.resize(size);
        let handle = editor.instance.plugin_handle();
        let pointer = Retained::as_ptr(&editor.parent.view).cast_mut().cast();
        // SAFETY: Editor borrows Parent until after gui.destroy and keeps all native calls on its main thread.
        // The pointer names the retained actual host NSView, not a dummy handle or standalone plugin window.
        unsafe {
            editor
                .gui
                .set_parent(&handle, Window::from_cocoa_nsview(pointer))
                .context("gui.set_parent")?;
        }
        Ok(editor)
    }

    /// Verifies synchronous CLAP size readback independently of eventual native child dimensions.
    fn verify_size(&mut self, expected: GuiSize) -> Result<()> {
        let actual = self
            .gui
            .get_size(&self.instance.plugin_handle())
            .context("gui.get_size")?;
        ensure!(
            actual.width == expected.width && actual.height == expected.height,
            "CLAP size readback mismatch: {actual:?}, expected {expected:?}"
        );
        Ok(())
    }

    /// Destroys synchronously while retaining the instance and native parent for immediate detachment assertions.
    fn destroy(&mut self) {
        if self.created {
            self.gui.destroy(&self.instance.plugin_handle());
            self.created = false;
        }
    }
}

impl Drop for Editor<'_> {
    /// Pairs every successful create with destroy before releasing the borrowed host parent or instance.
    fn drop(&mut self) {
        self.destroy();
    }
}

/// Checks real native point geometry with only the existing half-point rounding allowance, never pixels.
fn matches_size(child: &NSView, expected: GuiSize) -> bool {
    let size = child.frame().size;
    (size.width - f64::from(expected.width)).abs() < 0.5 && (size.height - f64::from(expected.height)).abs() < 0.5
}

/// Exercises show/hide without detaching or replacing the direct native child.
fn visibility(application: &Application, editor: &mut Editor<'_>, child: &NSView, visible: bool) -> Result<()> {
    let handle = editor.instance.plugin_handle();
    if visible {
        editor.gui.show(&handle).context("gui.show")?;
    } else {
        editor.gui.hide(&handle).context("gui.hide")?;
    }
    application.until(
        editor.instance,
        || child.isHidden() != visible,
        if visible {
            "native child shown"
        } else {
            "native child hidden"
        },
    )?;
    application.settle(editor.instance)?;
    ensure!(
        child.isHidden() != visible,
        "native visibility reverted after deferred work"
    );
    let current = editor.parent.child()?;
    ensure!(
        Retained::as_ptr(&current) == std::ptr::from_ref(child),
        "show/hide replaced the native child"
    );
    // SAFETY: both retained native views stay live on the main thread through this immediate hierarchy query.
    let superview = unsafe { child.superview() };
    ensure!(
        superview.as_deref() == Some(&*editor.parent.view),
        "child left its parent"
    );
    Ok(())
}

/// Runs an initial open and two independent reopen lifetimes on the same real packaged CLAP instance.
pub fn run(bundle: &Path) -> Result<()> {
    let watchdog = Watchdog::start()?;
    let bundle = host::checked_bundle(bundle)?;
    let marker = MainThreadMarker::new().context("clap_editor requires `AppKit`'s real main thread")?;
    let application = Application::start(marker);
    let delegate = application.0.delegate();
    let parent = Parent::create(marker);
    let mut instance = host::load(&bundle)?;
    println!("CASE_BEGIN native-clap-editor bundle={}", bundle.display());
    for iteration in 0..3 {
        let size = GuiSize {
            width: 1000 + iteration * 64,
            height: 764 + iteration * 32,
        };
        let resized = GuiSize {
            width: size.width + 48,
            height: size.height + 24,
        };
        ensure!(
            parent.view.subviews().count() == 0,
            "previous lifetime left a native child"
        );
        let mut editor = Editor::open(&mut instance, &parent, size)?;
        let child = parent.child()?;
        application.settle(editor.instance)?;
        let initially_hidden = child.isHidden();
        println!("native-clap-editor iteration={iteration} attached_hidden={initially_hidden}");
        ensure!(matches_size(&child, size), "native attachment ignored pre-parent size");
        visibility(&application, &mut editor, &child, true)?;
        visibility(&application, &mut editor, &child, false)?;
        visibility(&application, &mut editor, &child, true)?;
        parent.resize(resized);
        editor
            .gui
            .set_size(&editor.instance.plugin_handle(), resized)
            .context("visible gui.set_size")?;
        editor.verify_size(resized)?;
        application.until(
            editor.instance,
            || matches_size(&child, resized),
            "native child resized",
        )?;
        ensure!(!child.isHidden(), "resizing hid the native child");
        editor.destroy();
        ensure!(
            parent.detached(&child),
            "gui.destroy returned before native child detachment"
        );
        application.settle(editor.instance)?;
        ensure!(
            parent.detached(&child),
            "deferred native work reattached a destroyed child"
        );
        ensure!(
            application.0.delegate() == delegate,
            "plugin replaced the host application delegate"
        );
        println!(
            "native-clap-editor iteration={iteration} show=visible hide=hidden reshow=visible size={}x{} resize={}x{} destroy=detached",
            size.width, size.height, resized.width, resized.height
        );
    }
    drop(instance);
    drop(parent);
    application.pump();
    drop(application);
    drop(watchdog);
    println!("CASE_OK native-clap-editor opens=3 reopens=2");
    Ok(())
}
