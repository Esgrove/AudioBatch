//! Native packaged-editor regression for synchronous host geometry and removal during real Iced parameter edits.
//! Run this executable in its own process. A panic, missing native event, or unsupported platform is a failure.

use std::path::PathBuf;

use anyhow::{Context as _, Result, bail, ensure};

#[path = "support/artifacts.rs"]
mod artifacts;

#[cfg(target_os = "macos")]
#[expect(
    unsafe_code,
    reason = "owns the private VST3 COM host and main-thread AppKit event boundary"
)]
mod native {
    use std::cell::{Cell, RefCell};
    use std::ffi::c_void;
    use std::io::{Cursor, Read as _, Seek as _, SeekFrom, Write as _};
    use std::path::Path;
    use std::ptr::NonNull;
    use std::rc::{Rc, Weak};
    use std::sync::mpsc;
    use std::thread::{self, JoinHandle};
    use std::time::{Duration, Instant};

    use anyhow::{Context as _, Result, ensure};
    use block2::RcBlock;
    use image_control_plugin_rs::{COMPONENT_ID, GAIN_PARAMETER};
    use objc2::{MainThreadMarker, MainThreadOnly as _, msg_send, rc::Retained, runtime::ProtocolObject};
    use objc2_app_kit::{
        NSApplication, NSApplicationActivationPolicy, NSBackingStoreType, NSEvent, NSEventMask, NSEventModifierFlags,
        NSEventType, NSView, NSViewFrameDidChangeNotification, NSWindow, NSWindowStyleMask,
    };
    use objc2_foundation::{
        NSDate, NSDefaultRunLoopMode, NSNotification, NSNotificationCenter, NSObjectProtocol, NSPoint, NSRect, NSSize,
        NSString,
    };
    use vst3::Steinberg::Vst::{
        IComponent, IComponentHandler, IComponentHandlerTrait, IComponentTrait as _, IEditController,
        IEditControllerTrait as _,
    };
    use vst3::Steinberg::{
        IBStream, IBStreamTrait, IPlugFrame, IPlugFrameTrait, IPlugView, IPlugViewContentScaleSupport,
        IPlugViewContentScaleSupportTrait as _, IPlugViewTrait as _, IPluginBaseTrait as _, IPluginFactory,
        IPluginFactoryTrait as _, ViewRect, kInvalidArgument, kResultFalse, kResultOk, tresult,
    };
    use vst3::{Class, ComPtr, ComRef, ComWrapper, Interface};

    /// Bounds native startup, event delivery, and teardown even when a dependency stops returning.
    const PROCESS_TIMEOUT: Duration = Duration::from_secs(20);
    /// Bounds observable editor progress independently of the fatal process watchdog.
    const EVENT_TIMEOUT: Duration = Duration::from_secs(2);
    /// Allows deferred geometry, window teardown, and timer callbacks to run without screenshots.
    const SETTLE_TIME: Duration = Duration::from_millis(180);
    /// Different native dimensions requested synchronously from the two parameter callbacks.
    const BEGIN_SIZE: (i32, i32) = (1072, 800);
    /// The final parameter callback must supersede geometry queued by beginEdit.
    const PERFORM_SIZE: (i32, i32) = (1120, 832);
    /// Geometry deliberately queued immediately before destroying the native child.
    const PENDING_SIZE: (i32, i32) = (1184, 864);
    /// Header output slider position in the packaged editor's initial logical coordinates.
    const OUTPUT_POINT: (f64, f64) = (850.0, 22.0);
    /// Maximum component payload and its four-byte length prefix.
    const MAXIMUM_STATE_STREAM_BYTES: usize = 4100;

    /// Selects which real native callback removes the child, if any.
    #[derive(Clone, Copy, Debug, Eq, PartialEq)]
    enum Mode {
        Resize,
        RemoveDuringEdit,
        RemoveDuringResize,
    }

    /// The application belongs to this executable, not to the packaged plugin.
    struct Application {
        /// All native operations use this actual main-thread application.
        native: Retained<NSApplication>,
    }

    /// Cancels the fatal timeout after all plugin teardown and event-loop assertions complete.
    struct Watchdog {
        /// Successful or ordinary failed completion wakes the timeout thread.
        finished: mpsc::Sender<()>,
        /// Joined outside native callbacks.
        worker: Option<JoinHandle<()>>,
    }

    /// Component and controller references are created only through `GetPluginFactory` from the bundle.
    struct Component {
        /// Owns the initialized component through editor close and reopen.
        component: ComPtr<IComponent>,
        /// Same-object controller used for handler installation and independent parameter readback.
        controller: ComPtr<IEditController>,
    }

    /// Bounded private state stream for exercising the packaged component's actual geometry codec.
    #[derive(Default)]
    struct StateStream(RefCell<Cursor<Vec<u8>>>);

    /// Observations remain live after destroying the host so late calls cannot masquerade as success.
    #[derive(Default)]
    struct Evidence {
        /// Native UI-originated beginEdit calls.
        begin: Cell<usize>,
        /// Native UI-originated performEdit calls.
        perform: Cell<usize>,
        /// Balanced gesture releases, including editor teardown.
        end: Cell<usize>,
        /// Host frame callbacks during attachment or native size negotiation.
        frame: Cell<usize>,
        /// Successful geometry and content-scale reentry from beginEdit.
        begin_reentry: Cell<usize>,
        /// Successful geometry and content-scale reentry from performEdit.
        perform_reentry: Cell<usize>,
        /// Native child removals completed synchronously inside the real performEdit callback.
        removed: Cell<usize>,
        /// Native frame-change notifications delivered synchronously by the real child's resize.
        geometry_notifications: Cell<usize>,
        /// Native removals completed inside the frame-change notification's active geometry call.
        geometry_removed: Cell<usize>,
        /// Repeated removals accepted inside the same frame-change callback.
        repeated_removed: Cell<usize>,
        /// Calls after the host has been retired or its weak reference expired.
        late: Cell<usize>,
        /// First callback failure, reported after returning across the COM boundary.
        failure: RefCell<Option<String>>,
    }

    /// Native window and non-owning COM pointer, accessed only by synchronous main-thread callbacks.
    struct Host {
        /// Retained native parent outlives the attached plugin child.
        window: Retained<NSWindow>,
        /// Stable host-owned `NSView` passed to `IPlugView::attached`.
        parent: Retained<NSView>,
        /// Borrowed pointer kept valid by `Editor::view` until removed and callbacks have returned.
        view: Cell<*mut IPlugView>,
        /// Latest accepted geometry must be visible through getSize before reentry returns.
        latest: Cell<(i32, i32)>,
        /// Suppresses access after the view has been removed.
        closed: Cell<bool>,
        /// A host may decline plugin-originated resizing without changing either parent or child.
        reject_resize: Cell<bool>,
    }

    /// Private automation host, without a strong COM view or native-window ownership cycle.
    struct Handler {
        /// Upgraded only for a single callback, never retained by the plugin view.
        host: Weak<Host>,
        /// Survives host destruction to record illegal late callbacks.
        evidence: Rc<Evidence>,
        /// Removes the native child from the first performEdit while the Iced update is still active.
        remove_during_edit: bool,
    }

    /// Private frame uses the same weak native owner as the component handler.
    struct Frame {
        /// No strong back-reference to the view retaining this frame.
        host: Weak<Host>,
        /// Retained independently for post-destruction assertions.
        evidence: Rc<Evidence>,
    }

    /// Owns one synchronous native frame observer and unregisters it before native host destruction.
    struct GeometryObserver {
        /// Default notification center retains the registered block until token removal.
        center: Retained<NSNotificationCenter>,
        /// Opaque registration identity, removed exactly once on the main thread.
        token: Retained<ProtocolObject<dyn NSObjectProtocol>>,
        /// Retains the observed child until its notification registration is removed.
        child: Retained<NSView>,
        /// Restores the original notification policy after unregistering.
        posted_frame_changes: bool,
    }

    /// Attachment guard removes the native child before releasing its host or COM view.
    struct Editor {
        /// Owns the view for every use of `Host::view`'s borrowed pointer.
        view: ComPtr<IPlugView>,
        /// Owns the native window until removal has finished.
        host: Rc<Host>,
        /// Keeps the controller live for detaching the automation handler during teardown.
        controller: ComPtr<IEditController>,
        /// Allows explicit close and Drop to share the same idempotent lifecycle.
        attached: bool,
    }

    impl Application {
        /// Starts `AppKit` on the executable's main thread without installing a plugin-owned delegate.
        fn start(marker: MainThreadMarker) -> Self {
            let native = NSApplication::sharedApplication(marker);
            native.setActivationPolicy(NSApplicationActivationPolicy::Regular);
            native.finishLaunching();
            native.activate();
            Self { native }
        }

        /// Dispatches one bounded native event, then lets `AppKit` update its real windows.
        fn pump(&self) {
            let until = NSDate::dateWithTimeIntervalSinceNow(0.005);
            // SAFETY: the application, date, and run-loop mode stay live on the main thread.
            if let Some(event) = unsafe {
                self.native.nextEventMatchingMask_untilDate_inMode_dequeue(
                    NSEventMask::Any,
                    Some(&until),
                    NSDefaultRunLoopMode,
                    true,
                )
            } {
                self.native.sendEvent(&event);
            }
            self.native.updateWindows();
        }

        /// Runs asynchronous geometry and teardown callbacks for a fixed bounded interval.
        fn settle(&self) {
            let deadline = Instant::now() + SETTLE_TIME;
            while Instant::now() < deadline {
                self.pump();
            }
        }

        /// Requires an observable native result rather than silently skipping unavailable event delivery.
        fn until(&self, mut ready: impl FnMut() -> bool, description: &str) -> Result<()> {
            let deadline = Instant::now() + EVENT_TIMEOUT;
            while !ready() && Instant::now() < deadline {
                self.pump();
            }
            ensure!(ready(), "timed out waiting for {description}");
            Ok(())
        }
    }

    impl Watchdog {
        /// Starts before library loading so a blocked native callback still fails the separate process.
        fn start() -> Result<Self> {
            let (finished, receiver) = mpsc::channel();
            let worker = thread::Builder::new()
                .name("editor-reentry-watchdog".into())
                .spawn(move || {
                    if receiver.recv_timeout(PROCESS_TIMEOUT) == Err(mpsc::RecvTimeoutError::Timeout) {
                        eprintln!("CASE_FAILED native-editor-reentry exceeded its 20-second deadline");
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

    impl Component {
        /// Loads no in-process factory and creates the actual packaged component through its export.
        fn create(library: &libloading::Library) -> Result<Self> {
            // SAFETY: the supplied primary VST3 library exports the SDK's GetPluginFactory signature.
            let export =
                unsafe { library.get::<unsafe extern "system" fn() -> *mut IPluginFactory>(b"GetPluginFactory\0") }?;
            // SAFETY: the retained library keeps the function live, and it transfers one factory reference.
            let factory = unsafe { ComPtr::from_raw(export()) }.context("null packaged factory")?;
            let mut pointer: *mut c_void = std::ptr::null_mut();
            // SAFETY: the IDs reference sixteen bytes and pointer is a live writable output slot.
            let result = unsafe {
                factory.createInstance(COMPONENT_ID.as_ptr(), IComponent::IID.as_ptr().cast(), &raw mut pointer)
            };
            ensure!(result == kResultOk, "createInstance failed: {result}");
            // SAFETY: successful creation transfers one owned component reference, checked for null.
            let component = unsafe { ComPtr::from_raw(pointer.cast::<IComponent>()) }.context("null component")?;
            let controller = component.cast::<IEditController>().context("same-object controller")?;
            // SAFETY: this primary wrapper accepts a null context and is initialized on the main thread.
            let result = unsafe { component.initialize(std::ptr::null_mut()) };
            ensure!(result == kResultOk, "initialize failed");
            Ok(Self { component, controller })
        }

        /// Captures the actual detached host state without touching the native child or pumping its event loop.
        fn save(&self) -> Result<Vec<u8>> {
            let stream = ComWrapper::new(StateStream::default());
            let interface = stream.to_com_ptr::<IBStream>().context("state stream")?;
            // SAFETY: the initialized component and writable stream remain owned on this non-audio thread.
            let result = unsafe { self.component.getState(interface.as_ptr()) };
            ensure!(result == kResultOk, "save geometry");
            Ok(stream.0.borrow().get_ref().clone())
        }

        /// Restores detached state through the packaged ABI, without making a GUI resize call.
        fn restore(&self, bytes: Vec<u8>) -> Result<()> {
            let stream = ComWrapper::new(StateStream(RefCell::new(Cursor::new(bytes))));
            let interface = stream.to_com_ptr::<IBStream>().context("state stream")?;
            // SAFETY: the initialized component and readable stream remain owned throughout this state call.
            let result = unsafe { self.component.setState(interface.as_ptr()) };
            ensure!(result == kResultOk, "restore geometry");
            Ok(())
        }

        /// Reads authoritative controller state independently of the UI callback's value argument.
        fn gain(&self) -> f64 {
            // SAFETY: the same-object controller is initialized and read only on its UI owner.
            unsafe { self.controller.getParamNormalized(GAIN_PARAMETER) }
        }
    }

    impl Drop for Component {
        fn drop(&mut self) {
            // SAFETY: Editor guards are dropped first and all component operations stay on the main thread.
            unsafe {
                let _ = self.controller.setComponentHandler(std::ptr::null_mut());
                let _ = self.component.terminate();
            }
        }
    }

    impl Class for StateStream {
        type Interfaces = (IBStream,);
    }

    impl IBStreamTrait for StateStream {
        unsafe fn read(&self, buffer: *mut c_void, count: i32, transferred: *mut i32) -> tresult {
            let Ok(count) = usize::try_from(count) else {
                return kInvalidArgument;
            };
            if buffer.is_null() || count > MAXIMUM_STATE_STREAM_BYTES {
                return kInvalidArgument;
            }
            // SAFETY: the component supplies count writable bytes and this private stream bounds the count.
            let bytes = unsafe { std::slice::from_raw_parts_mut(buffer.cast::<u8>(), count) };
            let Ok(read) = self.0.borrow_mut().read(bytes) else {
                return kResultFalse;
            };
            if !transferred.is_null() {
                // SAFETY: the optional transferred slot is writable and read is bounded by count.
                unsafe { transferred.write(i32::try_from(read).unwrap_or_default()) };
            }
            kResultOk
        }

        unsafe fn write(&self, buffer: *mut c_void, count: i32, transferred: *mut i32) -> tresult {
            let Ok(count) = usize::try_from(count) else {
                return kInvalidArgument;
            };
            let mut stream = self.0.borrow_mut();
            if buffer.is_null() || stream.position().saturating_add(count as u64) > MAXIMUM_STATE_STREAM_BYTES as u64 {
                return kInvalidArgument;
            }
            // SAFETY: the component supplies count readable bytes and this private stream bounds the count.
            let bytes = unsafe { std::slice::from_raw_parts(buffer.cast::<u8>(), count) };
            if stream.write_all(bytes).is_err() {
                return kResultFalse;
            }
            if !transferred.is_null() {
                // SAFETY: the optional transferred slot is writable and count is bounded by the stream limit.
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
            let mut stream = self.0.borrow_mut();
            let previous = stream.position();
            let Ok(position) = stream.seek(origin) else {
                return kResultFalse;
            };
            if position > MAXIMUM_STATE_STREAM_BYTES as u64 {
                stream.set_position(previous);
                return kResultFalse;
            }
            if !result.is_null() {
                // SAFETY: the optional result is writable and every stream position is bounded.
                unsafe { result.write(i64::try_from(position).unwrap_or_default()) };
            }
            kResultOk
        }

        unsafe fn tell(&self, result: *mut i64) -> tresult {
            if result.is_null() {
                return kInvalidArgument;
            }
            // SAFETY: the non-null result is writable and every private stream position is bounded.
            unsafe { result.write(i64::try_from(self.0.borrow().position()).unwrap_or_default()) };
            kResultOk
        }
    }

    impl Evidence {
        /// Defers errors until after returning through COM, without unwinding across a native ABI.
        fn record(&self, outcome: Result<()>) -> tresult {
            if let Err(error) = outcome {
                let mut failure = self.failure.borrow_mut();
                if failure.is_none() {
                    *failure = Some(format!("{error:#}"));
                }
                kResultFalse
            } else {
                kResultOk
            }
        }

        /// Requires all native callback checks to have succeeded, including readback during reentry.
        fn verify(&self) -> Result<()> {
            ensure!(
                self.failure.borrow().is_none(),
                "host callback failed: {:?}",
                self.failure.borrow()
            );
            ensure!(
                self.late.get() == 0,
                "{} callbacks reached a retired host",
                self.late.get()
            );
            Ok(())
        }

        /// Captures every callback count before pumping events after native host destruction.
        const fn counts(&self) -> [usize; 9] {
            [
                self.begin.get(),
                self.perform.get(),
                self.end.get(),
                self.frame.get(),
                self.removed.get(),
                self.geometry_notifications.get(),
                self.geometry_removed.get(),
                self.repeated_removed.get(),
                self.late.get(),
            ]
        }
    }

    impl Host {
        /// Creates a real host window and parent `NSView` on `AppKit`'s main thread.
        fn create(marker: MainThreadMarker) -> Rc<Self> {
            let rectangle = NSRect::new(NSPoint::new(0.0, 0.0), NSSize::new(1000.0, 764.0));
            // SAFETY: the frame and style are valid and construction runs on the main thread.
            let window = unsafe {
                NSWindow::initWithContentRect_styleMask_backing_defer(
                    NSWindow::alloc(marker),
                    rectangle,
                    NSWindowStyleMask::Titled | NSWindowStyleMask::Closable | NSWindowStyleMask::Resizable,
                    NSBackingStoreType::Buffered,
                    false,
                )
            };
            // SAFETY: this Rc owns the window, so closing must not release its retained reference.
            unsafe { window.setReleasedWhenClosed(false) };
            window.setTitle(&NSString::from_str("ImageControl native editor reentry regression"));
            let parent = NSView::initWithFrame(NSView::alloc(marker), rectangle);
            window.setContentView(Some(&parent));
            window.center();
            Rc::new(Self {
                window,
                parent,
                view: Cell::new(std::ptr::null_mut()),
                latest: Cell::new((1000, 764)),
                closed: Cell::new(false),
                reject_resize: Cell::new(false),
            })
        }

        /// Reenters geometry and scale synchronously while the actual Iced update is still on the stack.
        fn reenter(&self, size: (i32, i32), scale: f32) -> Result<()> {
            ensure!(!self.closed.get(), "host already closed");
            // SAFETY: Editor owns this COM view throughout the synchronous callback and attachment.
            let view = unsafe { ComRef::from_raw(self.view.get()) }.context("missing attached view")?;
            let content_scale = view
                .cast::<IPlugViewContentScaleSupport>()
                .context("content scale support")?;
            let mut requested = rectangle(size);
            // SAFETY: COM interfaces and rectangle stay live on this UI thread throughout reentry.
            unsafe {
                ensure!(
                    view.onSize(&raw mut requested) == kResultOk,
                    "reentrant onSize rejected {size:?}"
                );
                ensure!(
                    content_scale.setContentScaleFactor(scale) == kResultOk,
                    "reentrant content scale rejected {scale}"
                );
            }
            self.latest.set(size);
            self.verify_size()
        }

        /// Checks the view's latest size immediately, before deferred native geometry can be applied.
        fn verify_size(&self) -> Result<()> {
            // SAFETY: Editor holds the owning COM reference until after this call returns.
            let view = unsafe { ComRef::from_raw(self.view.get()) }.context("missing live view")?;
            let mut actual = rectangle((0, 0));
            // SAFETY: actual is a writable ViewRect and the queried view is live on the main thread.
            ensure!(unsafe { view.getSize(&raw mut actual) } == kResultOk, "getSize failed");
            ensure!(
                (actual.left, actual.top, actual.right, actual.bottom)
                    == (0, 0, self.latest.get().0, self.latest.get().1),
                "getSize did not expose the latest rectangle"
            );
            Ok(())
        }

        /// Observes actual native child geometry, not merely the COM rectangle or an image.
        fn child_size(&self) -> Result<(f64, f64)> {
            let children = self.parent.subviews();
            ensure!(children.count() == 1, "expected exactly one embedded native child");
            let size = children.objectAtIndex(0).frame().size;
            Ok((size.width, size.height))
        }
    }

    impl Drop for Host {
        fn drop(&mut self) {
            self.window.orderOut(None);
            self.window.close();
        }
    }

    impl Handler {
        /// Obtains a short-lived host reference and records attempted access after native teardown.
        fn live_host(&self) -> Result<Rc<Host>> {
            if let Some(host) = self.host.upgrade()
                && !host.closed.get()
            {
                return Ok(host);
            }
            self.evidence.late.set(self.evidence.late.get() + 1);
            anyhow::bail!("component callback reached a destroyed host")
        }
    }

    impl Class for Handler {
        type Interfaces = (IComponentHandler,);
    }

    impl IComponentHandlerTrait for Handler {
        unsafe fn beginEdit(&self, id: u32) -> tresult {
            self.evidence.begin.set(self.evidence.begin.get() + 1);
            self.evidence.record((|| {
                ensure!(id == GAIN_PARAMETER, "native pointer did not edit Output");
                self.live_host()?.reenter(BEGIN_SIZE, 1.25)?;
                self.evidence.begin_reentry.set(self.evidence.begin_reentry.get() + 1);
                Ok(())
            })())
        }

        unsafe fn performEdit(&self, id: u32, value: f64) -> tresult {
            self.evidence.perform.set(self.evidence.perform.get() + 1);
            self.evidence.record((|| {
                ensure!(id == GAIN_PARAMETER && value.is_finite(), "invalid native Output edit");
                let host = self.live_host()?;
                host.reenter(PERFORM_SIZE, 1.5)?;
                self.evidence
                    .perform_reentry
                    .set(self.evidence.perform_reentry.get() + 1);
                if self.remove_during_edit {
                    ensure!(
                        self.evidence.removed.get() == 0,
                        "parameter write continued after reentrant removal"
                    );
                    // SAFETY: Editor owns this view throughout the active host callback and removal.
                    let view = unsafe { ComRef::from_raw(host.view.get()) }.context("missing removal view")?;
                    // SAFETY: the COM view and its native parent remain owned on the main thread.
                    let result = unsafe { view.removed() };
                    ensure!(result == kResultOk, "reentrant removed failed: {result}");
                    ensure!(
                        host.parent.subviews().count() == 0,
                        "reentrant removed returned before native child removal"
                    );
                    self.evidence.removed.set(1);
                }
                Ok(())
            })())
        }

        unsafe fn endEdit(&self, id: u32) -> tresult {
            self.evidence.end.set(self.evidence.end.get() + 1);
            self.evidence.record((|| {
                ensure!(id == GAIN_PARAMETER, "unexpected native gesture release");
                let _host = self.live_host()?;
                Ok(())
            })())
        }

        unsafe fn restartComponent(&self, _flags: i32) -> tresult {
            self.evidence.record(self.live_host().map(|_| ()))
        }
    }

    impl Class for Frame {
        type Interfaces = (IPlugFrame,);
    }

    impl IPlugFrameTrait for Frame {
        unsafe fn resizeView(&self, view: *mut IPlugView, size: *mut ViewRect) -> tresult {
            self.evidence.frame.set(self.evidence.frame.get() + 1);
            if size.is_null() {
                return kInvalidArgument;
            }
            if self.host.upgrade().is_some_and(|host| host.reject_resize.get()) {
                return kResultFalse;
            }
            self.evidence.record((|| {
                let host = self
                    .host
                    .upgrade()
                    .context("frame callback reached destroyed host")
                    .inspect_err(|_| {
                        self.evidence.late.set(self.evidence.late.get() + 1);
                    })?;
                ensure!(
                    !host.closed.get() && view == host.view.get(),
                    "frame callback has no live view"
                );
                // getSize must remain authoritative and unchanged before the host acknowledges the request.
                host.verify_size()?;
                // SAFETY: resizeView's caller supplies one readable and writable rectangle for this callback.
                let size = unsafe { &mut *size };
                ensure!(
                    size.right > size.left && size.bottom > size.top,
                    "invalid frame rectangle"
                );
                let dimensions = (size.right - size.left, size.bottom - size.top);
                let old_frame = host.window.frame();
                host.window
                    .setContentSize(NSSize::new(f64::from(dimensions.0), f64::from(dimensions.1)));
                let new_frame = host.window.frame();
                host.window.setFrameOrigin(NSPoint::new(
                    old_frame.origin.x,
                    old_frame.origin.y + old_frame.size.height - new_frame.size.height,
                ));
                // SAFETY: This owning host follows VST3's synchronous resizeView -> onSize acknowledgment contract.
                let view = unsafe { ComRef::from_raw(view) }.context("missing frame view")?;
                // SAFETY: The view and writable rectangle remain owned on the main thread.
                ensure!(unsafe { view.onSize(size) } == kResultOk, "frame onSize rejected");
                host.latest.set(dimensions);
                host.verify_size()
            })())
        }
    }

    impl GeometryObserver {
        /// Registers a main-thread-only, synchronous observer on this exact native child before queuing geometry.
        fn install(host: &Rc<Host>, evidence: &Rc<Evidence>) -> Result<Self> {
            let children = host.parent.subviews();
            ensure!(
                children.count() == 1,
                "geometry observer requires exactly one native child"
            );
            let child = children.objectAtIndex(0);
            let posted_frame_changes = child.postsFrameChangedNotifications();
            child.setPostsFrameChangedNotifications(true);
            let weak_host = Rc::downgrade(host);
            let observed = Rc::clone(evidence);
            let attempted = Cell::new(false);
            let block = RcBlock::new(move |_notification: NonNull<NSNotification>| {
                // This registration observes only a UI-owned NSView with no dispatch queue.
                // AppKit posts its frame changes synchronously on the main thread that owns these Rc values.
                if MainThreadMarker::new().is_none() {
                    std::process::abort();
                }
                observed
                    .geometry_notifications
                    .set(observed.geometry_notifications.get() + 1);
                if attempted.replace(true) {
                    return;
                }
                observed.record((|| {
                    let host = weak_host
                        .upgrade()
                        .context("geometry observer reached a destroyed host")?;
                    ensure!(!host.closed.get(), "geometry observer reached a closed host");
                    ensure!(
                        observed.perform.get() > 0,
                        "native resize ran before performEdit queued geometry"
                    );
                    ensure!(
                        close_size(host.child_size()?, PERFORM_SIZE),
                        "notification was not the requested native child resize"
                    );
                    // SAFETY: Editor owns the view and native parent throughout synchronous notification dispatch.
                    let view = unsafe { ComRef::from_raw(host.view.get()) }.context("missing geometry-removal view")?;
                    // SAFETY: the live COM view is removed on its owner while native resize remains on the stack.
                    let result = unsafe { view.removed() };
                    ensure!(result == kResultOk, "geometry-reentrant removed failed: {result}");
                    ensure!(
                        host.parent.subviews().count() == 0,
                        "geometry-reentrant removed returned before native child detachment"
                    );
                    observed.geometry_removed.set(observed.geometry_removed.get() + 1);
                    // SAFETY: the same owning view reference remains live after its first, successful removal.
                    let repeated = unsafe { view.removed() };
                    ensure!(
                        repeated == kResultOk,
                        "repeated geometry-reentrant removed was not idempotent"
                    );
                    ensure!(
                        host.parent.subviews().count() == 0,
                        "repeated removal restored a native child"
                    );
                    observed.repeated_removed.set(observed.repeated_removed.get() + 1);
                    Ok(())
                })());
            });
            let center = NSNotificationCenter::defaultCenter();
            // SAFETY: this exact NSView can post frame notifications only on its AppKit owner thread.
            // A nil queue invokes the copied block synchronously there, so its non-Send captures never cross threads.
            // This guard removes the registration before releasing the observed child or destroying the host.
            let token = unsafe {
                center.addObserverForName_object_queue_usingBlock(
                    Some(NSViewFrameDidChangeNotification),
                    Some(&child),
                    None,
                    &block,
                )
            };
            Ok(Self {
                center,
                token,
                child,
                posted_frame_changes,
            })
        }
    }

    impl Drop for GeometryObserver {
        fn drop(&mut self) {
            // SAFETY: token is the exact live registration returned by this notification center.
            // Removal and release both stay on the main thread before the host or observed child is destroyed.
            unsafe { self.center.removeObserver((*self.token).as_ref()) };
            self.child.setPostsFrameChangedNotifications(self.posted_frame_changes);
        }
    }

    impl Editor {
        /// Installs private weak handlers and attaches a newly created packaged COM view to a real `NSView`.
        fn open(
            component: &Component,
            marker: MainThreadMarker,
            evidence: &Rc<Evidence>,
            remove_during_edit: bool,
        ) -> Result<Self> {
            let host = Host::create(marker);
            let handler = ComWrapper::new(Handler {
                host: Rc::downgrade(&host),
                evidence: Rc::clone(evidence),
                remove_during_edit,
            })
            .to_com_ptr::<IComponentHandler>()
            .context("private component handler")?;
            // SAFETY: the initialized controller and handler stay live on the main thread.
            let result = unsafe { component.controller.setComponentHandler(handler.as_ptr()) };
            ensure!(result == kResultOk, "set handler");
            // SAFETY: createView transfers one owned view reference from this initialized controller.
            let view = unsafe { ComPtr::from_raw(component.controller.createView(c"editor".as_ptr())) }
                .context("packaged controller did not create an editor")?;
            host.view.set(view.as_ptr());
            let mut initial = rectangle((0, 0));
            ensure!(
                // SAFETY: The newly created view and writable rectangle are owned on this UI thread.
                unsafe { view.getSize(&raw mut initial) } == kResultOk,
                "initial getSize failed"
            );
            host.latest.set((initial.right, initial.bottom));
            host.window
                .setContentSize(NSSize::new(f64::from(initial.right), f64::from(initial.bottom)));
            let frame = ComWrapper::new(Frame {
                host: Rc::downgrade(&host),
                evidence: Rc::clone(evidence),
            })
            .to_com_ptr::<IPlugFrame>()
            .context("private plug frame")?;
            let mut editor = Self {
                view,
                host,
                controller: component.controller.clone(),
                attached: false,
            };
            // SAFETY: frame and parent remain live throughout attachment, and all calls use the UI owner.
            unsafe {
                ensure!(editor.view.setFrame(frame.as_ptr()) == kResultOk, "setFrame failed");
                let parent = Retained::as_ptr(&editor.host.parent).cast_mut().cast::<c_void>();
                ensure!(
                    editor.view.attached(parent, c"NSView".as_ptr()) == kResultOk,
                    "native attachment failed"
                );
            }
            editor.attached = true;
            editor.host.window.makeKeyAndOrderFront(None);
            Ok(editor)
        }

        /// Removes synchronously without giving deferred resize or timer work a run-loop opportunity.
        fn close(&mut self) -> Result<()> {
            if !self.attached {
                return Ok(());
            }
            // SAFETY: the native parent and view are owned until removed returns on the UI thread.
            unsafe {
                ensure!(self.view.removed() == kResultOk, "removed failed");
                self.attached = false;
                ensure!(
                    self.view.setFrame(std::ptr::null_mut()) == kResultOk,
                    "clear frame failed"
                );
                ensure!(
                    self.controller.setComponentHandler(std::ptr::null_mut()) == kResultOk,
                    "clear handler failed"
                );
            }
            self.host.closed.set(true);
            self.host.view.set(std::ptr::null_mut());
            ensure!(
                self.host.parent.subviews().count() == 0,
                "removed left a native child attached"
            );
            Ok(())
        }
    }

    impl Drop for Editor {
        fn drop(&mut self) {
            let _ = self.close();
        }
    }

    /// Executes real native event delivery, reentrant COM calls, close, and reopen against the packaged bundle.
    pub fn run(bundle: &Path) -> Result<()> {
        let watchdog = Watchdog::start()?;
        let marker = MainThreadMarker::new().context("native editor regression requires the macOS main thread")?;
        let application = Application::start(marker);
        let delegate = application.native.delegate();
        let executable = bundle.join("Contents/MacOS/ImageControl RS");
        ensure!(
            executable.is_file(),
            "packaged executable is missing: {}",
            executable.display()
        );
        // SAFETY: only the explicit packaged primary VST3 is loaded and it outlives all COM/native resources.
        let library = unsafe { libloading::Library::new(executable) }.context("load packaged ImageControl VST3")?;
        let component = Component::create(&library)?;
        exercise_user_resize(&application, &component, &library, marker)?;
        let independent = Component::create(&library)?;
        let independent_evidence = Rc::new(Evidence::default());
        let mut independent_editor = Editor::open(&independent, marker, &independent_evidence, false)?;
        application.settle();
        ensure!(
            independent_editor.host.latest.get() == (1000, 764)
                && close_size(independent_editor.host.child_size()?, (1000, 764)),
            "resizing one component changed a new component's default geometry"
        );
        independent_editor.close()?;
        drop(independent_editor);
        drop(independent);
        for (iteration, mode) in [
            Mode::Resize,
            Mode::Resize,
            Mode::RemoveDuringEdit,
            Mode::RemoveDuringResize,
        ]
        .into_iter()
        .enumerate()
        {
            exercise_editor(&application, &component, marker, iteration, mode)?;
            ensure!(
                application.native.delegate() == delegate,
                "plugin changed the host application delegate"
            );
        }
        drop(component);
        application.settle();
        drop(library);
        drop(watchdog);
        println!("CASE_OK native-editor-reentry");
        Ok(())
    }

    /// Exercises the embedded grip with native pointer events, host rejection, constraints, DPI, and host border geometry.
    fn exercise_user_resize(
        application: &Application,
        component: &Component,
        library: &libloading::Library,
        marker: MainThreadMarker,
    ) -> Result<()> {
        let evidence = Rc::new(Evidence::default());
        let mut editor = Editor::open(component, marker, &evidence, false)?;
        application.settle();
        let initial = editor.host.latest.get();
        verify_resize_constraints(&editor)?;
        editor.host.verify_size()?;
        let calls = evidence.frame.get();
        native_resize_drag(application, &editor.host, (96.0, 64.0))?;
        let enlarged = (initial.0 + 96, initial.1 + 64);
        application.until(
            || evidence.frame.get() > calls && editor.host.latest.get() == enlarged,
            "pointer-driven resizeView acknowledgement",
        )?;
        application.until(
            || editor.host.child_size().is_ok_and(|size| close_size(size, enlarged)),
            "grip child resize",
        )?;
        ensure!(
            close_size(
                (
                    editor.host.parent.bounds().size.width,
                    editor.host.parent.bounds().size.height
                ),
                enlarged
            ),
            "grip did not resize host"
        );

        let accepted_state = component.save()?;
        editor.host.reject_resize.set(true);
        let calls = evidence.frame.get();
        native_resize_drag(application, &editor.host, (40.0, 32.0))?;
        application.until(|| evidence.frame.get() > calls, "rejected native resize request")?;
        application.settle();
        ensure!(
            editor.host.latest.get() == enlarged,
            "rejected resize changed COM dimensions"
        );
        ensure!(
            close_size(editor.host.child_size()?, enlarged),
            "rejected resize changed native child"
        );
        editor.host.verify_size()?;
        ensure!(
            component.save()? == accepted_state,
            "rejected native resize changed saved geometry"
        );
        editor.host.reject_resize.set(false);

        // The host's content-scale suggestion must not double macOS point geometry on a Retina display.
        editor.host.reenter(enlarged, 2.0)?;
        application.settle();
        native_resize_drag(application, &editor.host, (32.0, 24.0))?;
        let scaled = (enlarged.0 + 32, enlarged.1 + 24);
        application.until(|| editor.host.latest.get() == scaled, "DPI-correct native grip")?;
        application.until(
            || editor.host.child_size().is_ok_and(|size| close_size(size, scaled)),
            "DPI-correct child",
        )?;

        // A host border drives the same production constraint/onSize path without an editor-originated frame call.
        let calls = evidence.frame.get();
        let border = (initial.0 + 48, initial.1 + 40);
        let mut constrained = rectangle(border);
        ensure!(
            // SAFETY: The retained live view and rectangle stay on the owner thread.
            unsafe { editor.view.checkSizeConstraint(&raw mut constrained) } == kResultOk,
            "border constraints failed"
        );
        editor
            .host
            .window
            .setContentSize(NSSize::new(f64::from(border.0), f64::from(border.1)));
        editor.host.reenter(border, 1.0)?;
        application.until(
            || editor.host.child_size().is_ok_and(|size| close_size(size, border)),
            "host border child resize",
        )?;
        ensure!(evidence.frame.get() == calls, "host resize echoed back to resizeView");
        ensure!(
            evidence.begin.get() == 0 && evidence.end.get() == 0,
            "resizing opened a sound gesture"
        );
        evidence.verify()?;
        editor.close()?;
        let saved = component.save()?;
        let counts = evidence.counts();
        drop(editor);
        application.settle();
        ensure!(counts == evidence.counts(), "late callbacks after grip teardown");
        evidence.verify()?;
        verify_reopened_geometry(application, component, library, marker, border, &saved)?;
        println!(
            "native-editor-user-resize initial={initial:?} grip={enlarged:?} dpi={scaled:?} border={border:?} rejection=preserved"
        );
        Ok(())
    }

    /// Requires remembered and serialized logical geometry to control native attachment across distinct view lifetimes.
    fn verify_reopened_geometry(
        application: &Application,
        component: &Component,
        library: &libloading::Library,
        marker: MainThreadMarker,
        border: (i32, i32),
        saved: &[u8],
    ) -> Result<()> {
        let reopened_evidence = Rc::new(Evidence::default());
        let mut reopened = Editor::open(component, marker, &reopened_evidence, false)?;
        application.settle();
        ensure!(
            reopened.host.latest.get() == border,
            "new COM view forgot the host-accepted geometry"
        );
        ensure!(
            close_size(reopened.host.child_size()?, border),
            "native attachment ignored the component's remembered logical size"
        );
        reopened.close()?;
        drop(reopened);
        application.settle();
        reopened_evidence.verify()?;
        let restored = Component::create(library)?;
        restored.restore(saved.to_owned())?;
        ensure!(
            restored.save()? == saved,
            "new component did not preserve saved logical geometry"
        );
        let restored_evidence = Rc::new(Evidence::default());
        let mut restored_editor = Editor::open(&restored, marker, &restored_evidence, false)?;
        application.settle();
        ensure!(
            restored_editor.host.latest.get() == border,
            "restored COM view forgot logical geometry"
        );
        ensure!(
            close_size(restored_editor.host.child_size()?, border),
            "restored native editor did not attach at the saved logical size"
        );
        restored_editor.close()?;
        drop(restored_editor);
        application.settle();
        restored_evidence.verify()?;
        Ok(())
    }

    /// Checks the real packaged COM constraints before any native user gesture can change accepted geometry.
    fn verify_resize_constraints(editor: &Editor) -> Result<()> {
        // SAFETY: The retained editor view and writable constraint rectangles remain live on the main thread.
        unsafe {
            ensure!(editor.view.canResize() == kResultOk, "embedded editor is not resizable");
            let mut minimum = rectangle((1, 1));
            ensure!(
                editor.view.checkSizeConstraint(&raw mut minimum) == kResultOk,
                "minimum constraint failed"
            );
            ensure!(minimum.right > 1 && minimum.bottom > 1, "missing minimum constraints");
            let mut maximum = rectangle((i32::MAX, i32::MAX));
            ensure!(
                editor.view.checkSizeConstraint(&raw mut maximum) == kResultOk,
                "maximum constraint failed"
            );
            ensure!(
                maximum.right == 4096 && maximum.bottom == 4096,
                "missing maximum constraints"
            );
            let mut invalid = rectangle((1, 1));
            ensure!(
                editor.view.onSize(&raw mut invalid) == kResultFalse,
                "invalid geometry was accepted"
            );
        }
        Ok(())
    }

    /// Dispatches ordinary `AppKit` pointer events to the actual bottom-right embedded child, never an Iced test message.
    fn native_resize_drag(application: &Application, host: &Host, delta: (f64, f64)) -> Result<()> {
        let bounds = host.parent.bounds();
        let local = NSPoint::new(
            bounds.size.width - 4.0,
            if host.parent.isFlipped() {
                bounds.size.height - 4.0
            } else {
                4.0
            },
        );
        let start = host.parent.convertPoint_toView(local, None);
        let target = host
            .parent
            .hitTest(local)
            .context("native resize grip target missing")?;
        ensure!(
            Retained::as_ptr(&target) != Retained::as_ptr(&host.parent),
            "grip hit only host parent"
        );
        let start = host.window.convertPointToScreen(start);
        let end = NSPoint::new(start.x + delta.0, start.y - delta.1);
        let event = |kind, screen, pressure| {
            let location = host.window.convertPointFromScreen(screen);
            NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                kind, location, NSEventModifierFlags::empty(), 0.0, host.window.windowNumber(), None, 1, 1, pressure,
            ).context("native resize event missing")
        };
        let moved = event(NSEventType::MouseMoved, start, 0.0)?;
        let down = event(NSEventType::LeftMouseDown, start, 1.0)?;
        let midpoint = NSPoint::new(delta.0.mul_add(0.5, start.x), delta.1.mul_add(-0.5, start.y));
        // SAFETY: Native hitTest retains the actual responder and every event references its owned NSWindow.
        unsafe {
            let _: () = msg_send![&*target, mouseMoved: &*moved];
            let _: () = msg_send![&*target, mouseDown: &*down];
        }
        application.settle();
        let first_drag = event(NSEventType::LeftMouseDragged, midpoint, 1.0)?;
        // SAFETY: Give Iced one UI turn to acquire the in-bounds pointer before moving beyond the current child.
        unsafe {
            let _: () = msg_send![&*target, mouseDragged: &*first_drag];
        }
        application.settle();
        let drag = event(NSEventType::LeftMouseDragged, end, 1.0)?;
        // SAFETY: The active grip and target remain owned across the first acknowledged native resize.
        unsafe {
            let _: () = msg_send![&*target, mouseDragged: &*drag];
        }
        application.settle();
        let up = event(NSEventType::LeftMouseUp, end, 0.0)?;
        // SAFETY: This normal resize keeps the actual native child and parent alive through the pointer release.
        unsafe {
            let _: () = msg_send![&*target, mouseUp: &*up];
        }
        application.settle();
        Ok(())
    }

    /// Sends a real native gesture and verifies both synchronous readback and deferred native child geometry.
    fn exercise_editor(
        application: &Application,
        component: &Component,
        marker: MainThreadMarker,
        iteration: usize,
        mode: Mode,
    ) -> Result<()> {
        let evidence = Rc::new(Evidence::default());
        let mut editor = Editor::open(component, marker, &evidence, mode == Mode::RemoveDuringEdit)?;
        let host_identity = Rc::downgrade(&editor.host);
        application.settle();
        evidence.verify()?;
        let stage = i32::try_from(iteration)?;
        let baseline = (1000 + stage * 16, 764 + stage * 8);
        editor
            .host
            .window
            .setContentSize(NSSize::new(f64::from(baseline.0), f64::from(baseline.1)));
        editor.host.reenter(baseline, 1.0)?;
        application.until(
            || editor.host.child_size().is_ok_and(|size| close_size(size, baseline)),
            "distinct native reentry baseline",
        )?;
        let initial_size = editor.host.child_size()?;
        let initial_gain = component.gain();
        let observer = if mode == Mode::RemoveDuringResize {
            Some(GeometryObserver::install(&editor.host, &evidence)?)
        } else {
            None
        };
        native_gesture(application, &editor.host, &evidence, iteration, mode)?;
        application.until(
            || evidence.begin.get() > 0 && evidence.perform.get() > 0,
            "native Output gesture",
        )?;
        application.until(
            || evidence.end.get() == evidence.begin.get(),
            "balanced native gesture release",
        )?;
        evidence.verify()?;
        ensure!(
            evidence.begin_reentry.get() > 0 && evidence.perform_reentry.get() > 0,
            "both reentry paths must run"
        );
        ensure!(
            (component.gain() - initial_gain).abs() > 0.001,
            "native gesture did not change the controller parameter"
        );
        let resized = verify_native_result(application, &editor.host, &evidence, mode, initial_size)?;
        editor.host.verify_size()?;
        ensure!(
            editor.host.latest.get() == PERFORM_SIZE,
            "latest host rectangle was overwritten"
        );
        if mode == Mode::Resize {
            // Do not pump between this queued geometry and removal. Pending native work must be cancelled.
            editor.host.reenter(PENDING_SIZE, 1.75)?;
        }
        drop(observer);
        editor.close()?;
        evidence.verify()?;
        let closed_counts = evidence.counts();
        drop(editor);
        ensure!(
            host_identity.upgrade().is_none(),
            "private host retained through a COM ownership cycle"
        );
        application.settle();
        evidence.verify()?;
        ensure!(
            evidence.counts() == closed_counts,
            "callbacks continued after native host destruction"
        );
        println!(
            "native-editor-reentry iteration={iteration} mode={mode:?} begin={} perform={} end={} begin_reentry={} perform_reentry={} removed={} geometry_notifications={} geometry_removed={} repeated_removed={} child={initial_size:?}->{resized:?} late={}",
            evidence.begin.get(),
            evidence.perform.get(),
            evidence.end.get(),
            evidence.begin_reentry.get(),
            evidence.perform_reentry.get(),
            evidence.removed.get(),
            evidence.geometry_notifications.get(),
            evidence.geometry_removed.get(),
            evidence.repeated_removed.get(),
            evidence.late.get(),
        );
        Ok(())
    }

    /// Checks the actual child outcome for ordinary resize and both distinct synchronous removal call stacks.
    fn verify_native_result(
        application: &Application,
        host: &Host,
        evidence: &Evidence,
        mode: Mode,
        initial_size: (f64, f64),
    ) -> Result<Option<(f64, f64)>> {
        if mode == Mode::Resize {
            application.until(
                || host.child_size().is_ok_and(|size| close_size(size, PERFORM_SIZE)),
                "deferred native child resize",
            )?;
            ensure!(
                !close_size(initial_size, PERFORM_SIZE),
                "native dimensions did not change"
            );
            return host.child_size().map(Some);
        }
        if mode == Mode::RemoveDuringEdit {
            ensure!(
                evidence.removed.get() == 1,
                "performEdit did not remove the native child"
            );
        } else {
            ensure!(
                evidence.geometry_notifications.get() > 0,
                "no native frame notification ran"
            );
            ensure!(
                evidence.geometry_removed.get() == 1,
                "native geometry did not remove the child"
            );
            ensure!(
                evidence.repeated_removed.get() == 1,
                "repeated geometry removal was not exercised"
            );
        }
        ensure!(
            evidence.begin.get() == 1 && evidence.perform.get() == 1 && evidence.end.get() == 1,
            "reentrant removal did not complete exactly one balanced gesture"
        );
        ensure!(
            host.parent.subviews().count() == 0,
            "native child returned after reentrant removal"
        );
        Ok(None)
    }

    /// Delivers `NSEvent` input to the real deepest hit-tested native child, never a synthetic Iced message sender.
    fn native_gesture(
        application: &Application,
        host: &Host,
        evidence: &Evidence,
        iteration: usize,
        mode: Mode,
    ) -> Result<()> {
        let bounds = host.parent.bounds();
        let x = bounds.size.width - (1000.0 - OUTPUT_POINT.0)
            + match iteration {
                0 => 0.0,
                1 => -30.0,
                2 => 20.0,
                _ => -10.0,
            };
        let y = if host.parent.isFlipped() {
            OUTPUT_POINT.1
        } else {
            bounds.size.height - OUTPUT_POINT.1
        };
        let local = NSPoint::new(x, y);
        let window_point = host.parent.convertPoint_toView(local, None);
        let target = host.parent.hitTest(local).context("native Output target was not hit")?;
        ensure!(
            Retained::as_ptr(&target) != Retained::as_ptr(&host.parent),
            "pointer hit only the host parent"
        );
        let make_event = |kind, point, pressure| {
            NSEvent::mouseEventWithType_location_modifierFlags_timestamp_windowNumber_context_eventNumber_clickCount_pressure(
                kind, point, NSEventModifierFlags::empty(), 0.0, host.window.windowNumber(), None, 1, 1, pressure,
            ).context("AppKit did not create a native mouse event")
        };
        let moved = make_event(NSEventType::MouseMoved, window_point, 0.0)?;
        let down = make_event(NSEventType::LeftMouseDown, window_point, 1.0)?;
        let dragged_point = NSPoint::new(window_point.x + 8.0, window_point.y);
        let dragged = make_event(NSEventType::LeftMouseDragged, dragged_point, 1.0)?;
        let up = make_event(NSEventType::LeftMouseUp, dragged_point, 0.0)?;
        // SAFETY: hitTest retained the actual live NSView responder. NSEvents reference this owned NSWindow.
        // Each native event handler runs synchronously on AppKit's main thread, just as NSApplication dispatch does.
        unsafe {
            let _: () = msg_send![&*target, mouseMoved: &*moved];
            let _: () = msg_send![&*target, mouseDown: &*down];
        }
        if mode != Mode::Resize {
            application.until(
                || match mode {
                    Mode::RemoveDuringEdit => evidence.perform.get() > 0,
                    Mode::RemoveDuringResize => evidence.geometry_notifications.get() > 0,
                    Mode::Resize => false,
                },
                "native reentrant removal callback",
            )?;
            evidence.verify()?;
            if host.parent.subviews().count() == 0 {
                // No mouseUp is delivered. Program teardown must balance this genuinely held native gesture.
                return Ok(());
            }
            anyhow::bail!("native child survived reentrant removal in {mode:?}");
        }
        // SAFETY: ordinary gesture delivery retains its live target and parent until after mouseUp.
        unsafe {
            let _: () = msg_send![&*target, mouseDragged: &*dragged];
            let _: () = msg_send![&*target, mouseUp: &*up];
        }
        Ok(())
    }

    /// Uses independent integer host expectations with a point-rounding allowance, never pixel comparisons.
    fn close_size(actual: (f64, f64), expected: (i32, i32)) -> bool {
        (actual.0 - f64::from(expected.0)).abs() < 0.5 && (actual.1 - f64::from(expected.1)).abs() < 0.5
    }

    /// Constructs origin-zero host rectangles without borrowing native editor state.
    const fn rectangle(size: (i32, i32)) -> ViewRect {
        ViewRect {
            left: 0,
            top: 0,
            right: size.0,
            bottom: size.1,
        }
    }
}

/// Parses the explicit packaged path before touching `AppKit` or loading a plugin. Help has no native side effects.
fn arguments() -> Result<Option<PathBuf>> {
    let mut arguments = std::env::args_os().skip(1);
    let mut bundle = None;
    while let Some(argument) = arguments.next() {
        match argument.to_str() {
            Some("-h" | "--help") => {
                println!(
                    "Usage: editor_reentry --bundle <absolute-build-tree.vst3>\n\
Runs native-editor-reentry against the actual packaged ImageControl RS editor.\n\
Requires macOS native event delivery. No installed plugins, audio devices, or screenshots are used."
                );
                return Ok(None);
            }
            Some("--bundle") => bundle = Some(PathBuf::from(arguments.next().context("--bundle needs a path")?)),
            _ => bail!("unknown argument: {}", argument.to_string_lossy()),
        }
    }
    let bundle = bundle.context("give --bundle <absolute-build-tree.vst3>")?;
    let bundle = artifacts::checked_artifact(&bundle).context("invalid packaged build-tree bundle")?;
    ensure!(
        bundle.is_dir() && bundle.extension().is_some_and(|extension| extension == "vst3"),
        "expected a packaged .vst3 directory"
    );
    Ok(Some(bundle))
}

/// Runs one fail-closed native UI regression process, not an audio or `RealtimeSanitizer` scenario.
fn main() -> Result<()> {
    let Some(bundle) = arguments()? else {
        return Ok(());
    };
    #[cfg(target_os = "macos")]
    return native::run(&bundle);
    #[cfg(not(target_os = "macos"))]
    {
        let _ = bundle;
        bail!("native-editor-reentry is unsupported on this platform, native HWND event delivery is not implemented")
    }
}
