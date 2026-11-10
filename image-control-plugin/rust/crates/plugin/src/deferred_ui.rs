//! Cancellable, coalesced UI-thread work dispatched by the host's native run loop.
//!
//! This is not an executor or a real-time API. Construct, schedule, cancel, and drop it on the editor's UI thread.
//! Scheduling never calls the closure inline.
//! Native timers may fire in nested run loops, so callers must still defer again
//! when their own UI callback or polling guard is active.

use std::cell::{Cell, RefCell};
use std::io;
use std::rc::{Rc, Weak};
use std::thread::{self, ThreadId};

/// One reusable UI callback with at most one outstanding native timer.
/// `Rc` deliberately makes this neither `Send` nor `Sync`, including when its callback captures only thread-safe data.
/// The callback may reschedule this object, but should capture it weakly to avoid an ownership cycle.
pub struct Deferred {
    /// Strong ownership stays on the creating UI thread. Native callbacks retain only a weak reference.
    state: Rc<State<native::Timer>>,
    /// Windows UI threads need not be the process's main thread.
    creator: ThreadId,
}

/// Timer-independent scheduling state, also exercised with deterministic timer handles in unit tests.
struct State<Timer> {
    /// A handle is removed and dropped before entering user code or returning from cancellation.
    pending: RefCell<Option<(u64, Timer)>>,
    /// An invalidated callback must not consume work scheduled by a later request.
    generation: Cell<u64>,
    /// Retained between requests, so cancellation does not prevent a later schedule.
    callback: Box<dyn Fn()>,
}

impl Deferred {
    /// Retains a callback on the current UI thread without registering a native timer.
    /// On macOS, scheduling additionally requires the main thread.
    pub fn new(callback: impl Fn() + 'static) -> Self {
        Self {
            state: Rc::new(State::new(callback)),
            creator: thread::current().id(),
        }
    }

    /// Requests one future UI-run-loop callback, coalescing requests while one is already pending.
    /// Errors leave no outstanding work and allow retry. The host must continue pumping its native UI run loop.
    pub fn schedule(&self) -> io::Result<()> {
        if thread::current().id() != self.creator {
            return Err(io::Error::other("deferred UI work must stay on its creating thread"));
        }
        #[cfg(target_os = "macos")]
        native::check_thread()?;
        self.state.schedule_with(native::schedule)
    }

    /// Disarms pending native work without invoking the callback. Calling this repeatedly is harmless.
    /// Work already executing cannot be interrupted, but any work it has rescheduled is canceled.
    pub fn cancel(&self) {
        self.state.cancel();
    }
}

impl Drop for Deferred {
    /// Removes pending work before releasing the callback and its captured editor state.
    fn drop(&mut self) {
        self.cancel();
    }
}

impl<Timer> State<Timer> {
    /// Creates idle state without requiring a platform timer or running UI loop.
    fn new(callback: impl Fn() + 'static) -> Self {
        Self {
            pending: RefCell::new(None),
            generation: Cell::new(0),
            callback: Box::new(callback),
        }
    }

    /// Creates a timer only for the first outstanding request.
    /// `start` must not dispatch or pump the run loop before returning its owning timer handle.
    fn schedule_with(self: &Rc<Self>, start: impl FnOnce(Weak<Self>, u64) -> io::Result<Timer>) -> io::Result<()> {
        if self.pending.borrow().is_some() {
            return Ok(());
        }
        let generation = self
            .generation
            .get()
            .checked_add(1)
            .ok_or_else(|| io::Error::other("deferred UI timer generation exhausted"))?;
        self.generation.set(generation);
        let timer = start(Rc::downgrade(self), generation)?;
        *self.pending.borrow_mut() = Some((generation, timer));
        Ok(())
    }

    /// Releases the borrow before destroying the timer, because native teardown may release captured objects.
    fn cancel(&self) {
        let pending = self.pending.borrow_mut().take();
        drop(pending);
    }

    /// Ignores stale firings and disarms the matching timer before allowing recursive scheduling.
    fn fire(&self, generation: u64) {
        let pending = {
            let mut pending = self.pending.borrow_mut();
            if pending.as_ref().is_none_or(|(scheduled, _)| *scheduled != generation) {
                return;
            }
            pending.take()
        };
        drop(pending);
        (self.callback)();
    }
}

/// Keeps panics inside Rust rather than unwinding through an Objective-C block or Win32 timer procedure.
/// Upgrading here keeps the closure alive even when it drops the last owning `Deferred` during its invocation.
fn dispatch<Timer>(state: &Weak<State<Timer>>, generation: u64) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if let Some(state) = state.upgrade() {
            state.fire(generation);
        }
    }));
}

#[cfg(target_os = "macos")]
mod native {
    use std::io;
    use std::ptr::NonNull;
    use std::rc::Weak;

    use block2::RcBlock;
    use objc2::MainThreadMarker;
    use objc2::rc::Retained;
    use objc2_foundation::{NSRunLoop, NSRunLoopCommonModes, NSTimer};

    use super::{State, dispatch};

    /// A positive delay prevents synchronous delivery and avoids busy-spinning through repeated deferrals.
    const DEFER_SECONDS: f64 = 0.01;

    /// A private main-run-loop timer, never exposed to other threads or manually fired.
    pub struct Timer {
        /// Foundation and this handle retain the timer. Its block retains only a weak scheduling state.
        timer: Retained<NSTimer>,
    }

    /// Rejects worker-thread use before creating or touching Foundation run-loop objects.
    pub fn check_thread() -> io::Result<()> {
        MainThreadMarker::new()
            .map(|_| ())
            .ok_or_else(|| io::Error::other("deferred UI work requires the macOS main thread"))
    }

    /// Installs a one-shot timer in common modes so tracking and modal UI loops can service it.
    #[expect(
        unsafe_code,
        reason = "The private Foundation timer is confined to the main run loop"
    )]
    pub fn schedule(state: Weak<State<Timer>>, generation: u64) -> io::Result<Timer> {
        check_thread()?;
        let block = RcBlock::new(move |_: NonNull<NSTimer>| {
            // Keep the weak capture independent of the block while dispatch invalidates its owning timer.
            let state = state.clone();
            dispatch(&state, generation);
        });
        // SAFETY: This private timer is created, registered, invalidated, and released only on the main thread.
        // Its block cannot be called or destroyed on another thread because the timer is never exposed or moved.
        // The retained block owns only a Weak, so retaining it cannot keep the editor or scheduling state alive.
        let timer = unsafe { NSTimer::timerWithTimeInterval_repeats_block(DEFER_SECONDS, false, &block) };
        // SAFETY: The timer and run loop belong exclusively to this main thread.
        // Common modes is Foundation's process-lifetime run-loop mode constant, and adding a timer does not fire it.
        unsafe { NSRunLoop::mainRunLoop().addTimer_forMode(&timer, NSRunLoopCommonModes) };
        Ok(Timer { timer })
    }

    impl Drop for Timer {
        /// Invalidates before releasing the strong retain, including when the callback reschedules itself.
        fn drop(&mut self) {
            self.timer.invalidate();
        }
    }
}

#[cfg(target_os = "windows")]
mod native {
    use std::ffi::c_void;
    use std::io;
    use std::ptr;
    use std::rc::Weak;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::time::{Duration, Instant};

    use windows_sys::Win32::Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM};
    use windows_sys::Win32::System::LibraryLoader::{
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, GetModuleHandleExW,
    };
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        CREATESTRUCTW, CreateWindowExW, DefWindowProcW, DestroyWindow, GWLP_USERDATA, GetWindowLongPtrW, HWND_MESSAGE,
        KillTimer, RegisterClassW, SetTimer, SetWindowLongPtrW, UnregisterClassW, WM_NCCREATE, WM_NCDESTROY, WM_TIMER,
        WNDCLASSW,
    };

    use super::{State, dispatch};

    /// Matches Win32's minimum timer interval and bounds repeated deferrals during nested dispatch.
    const DEFER_MILLISECONDS: u32 = 10;
    /// Every timer has a private window, so it needs only one window-local timer identifier.
    const TIMER_ID: usize = 1;
    /// Class names are unique across editors and threads without thread-local storage or destructor registration.
    static NEXT_CLASS_ID: AtomicU64 = AtomicU64::new(0);

    /// An owning box keeps this address stable until its private window has finished destruction.
    struct Registration {
        /// The window borrows this weak reference rather than retaining editor state.
        state: Weak<State<Timer>>,
        /// Identifies the outstanding request within its state.
        generation: u64,
        /// A queued message for a reused HWND cannot deliver a new request before its own delay.
        not_before: Instant,
    }

    /// A private class whose procedure must be unregistered before the plugin can unload.
    struct WindowClass {
        /// Null-terminated name includes the procedure address and a module-local monotonic id.
        name: Vec<u16>,
        /// The loaded module containing the procedure. The host owns its lifetime through cancellation.
        instance: HINSTANCE,
    }

    /// Owns a message-only window, its callback data, and its uniquely registered window class on the UI thread.
    pub struct Timer {
        /// Destroyed synchronously before either the class or borrowed registration can be released.
        window: HWND,
        /// Outlives all window messages, including reentrant destruction during the firing callback.
        registration: Box<Registration>,
        /// The class remains registered until the window has been destroyed.
        class: WindowClass,
    }

    impl WindowClass {
        /// Registers a class in the procedure's module without retaining that module or touching a host window.
        #[expect(unsafe_code, reason = "Registers the plugin's own private message-window procedure")]
        fn new() -> io::Result<Self> {
            let serial = NEXT_CLASS_ID
                .try_update(Ordering::Relaxed, Ordering::Relaxed, |id| id.checked_add(1))
                .map_err(|_| io::Error::other("deferred UI window class identifiers exhausted"))?;
            let procedure = window_proc as *const ();
            let name: Vec<u16> = format!("ImageControlDeferred_{procedure:p}_{serial}")
                .encode_utf16()
                .chain(Some(0))
                .collect();
            let mut instance = ptr::null_mut();
            // SAFETY: FROM_ADDRESS interprets the pointer as an address, not a string.
            // The procedure is part of this loaded module, which the host keeps loaded through UI teardown.
            if unsafe {
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    procedure.cast(),
                    &raw mut instance,
                )
            } == 0
            {
                return Err(io::Error::last_os_error());
            }
            let specification = WNDCLASSW {
                lpfnWndProc: Some(window_proc),
                hInstance: instance,
                lpszClassName: name.as_ptr(),
                ..WNDCLASSW::default()
            };
            // SAFETY: The class description and its terminated name are valid throughout registration.
            // Only the class returned on success owns the registration and can create its private window.
            if unsafe { RegisterClassW(&raw const specification) } == 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(Self { name, instance })
        }
    }

    impl Drop for WindowClass {
        /// Removes the plugin procedure from the OS class table after its only window has been destroyed.
        #[expect(unsafe_code, reason = "Unregisters this timer's uniquely owned native window class")]
        fn drop(&mut self) {
            // SAFETY: Timer destroys its window before dropping this class, on the same creating UI thread.
            // Failed window creation also leaves no live window referencing this class.
            unsafe { UnregisterClassW(self.name.as_ptr(), self.instance) };
        }
    }

    /// Creates a window-owned timer with no TIMERPROC pointer in queued messages and no thread-local registry.
    #[expect(unsafe_code, reason = "Creates and arms an exclusively owned message-only window")]
    pub fn schedule(state: Weak<State<Timer>>, generation: u64) -> io::Result<Timer> {
        let class = WindowClass::new()?;
        let registration = Box::new(Registration {
            state,
            generation,
            not_before: Instant::now() + Duration::from_millis(u64::from(DEFER_MILLISECONDS)),
        });
        // SAFETY: The class is registered in this module and the boxed creation argument has a stable address.
        // WM_NCCREATE borrows that address. On success Timer owns it through synchronous DestroyWindow.
        // On failure CreateWindowExW finishes all destruction messages before returning, so the box can be dropped.
        let window = unsafe {
            CreateWindowExW(
                0,
                class.name.as_ptr(),
                ptr::null(),
                0,
                0,
                0,
                0,
                0,
                HWND_MESSAGE,
                ptr::null_mut(),
                class.instance,
                ptr::from_ref(registration.as_ref()).cast::<c_void>(),
            )
        };
        if window.is_null() {
            return Err(io::Error::last_os_error());
        }
        let timer = Timer {
            window,
            registration,
            class,
        };
        // SAFETY: This thread owns the private window. None sends ordinary WM_TIMER messages with zero lParam,
        // so a queued message never retains a plugin function pointer after cancellation or module unload.
        if unsafe { SetTimer(timer.window, TIMER_ID, DEFER_MILLISECONDS, None) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(timer)
    }

    /// Borrows creation data only during window lifetime and clones weak work before reentrant destruction.
    #[expect(
        unsafe_code,
        reason = "Win32 dispatches the private window procedure with native message arguments"
    )]
    unsafe extern "system" fn window_proc(window: HWND, message: u32, parameter: WPARAM, data: LPARAM) -> LRESULT {
        match message {
            WM_NCCREATE => {
                // SAFETY: CreateWindowExW supplies a live CREATESTRUCTW during this synchronous message.
                // Its creation pointer refers to the boxed Registration owned by schedule and then Timer.
                let creation = unsafe { &*ptr::with_exposed_provenance::<CREATESTRUCTW>(data.cast_unsigned()) };
                // SAFETY: The exclusively owned window reserves its user-data slot for this stable pointer.
                unsafe {
                    SetWindowLongPtrW(
                        window,
                        GWLP_USERDATA,
                        creation.lpCreateParams.expose_provenance().cast_signed(),
                    )
                };
                1
            }
            WM_NCDESTROY => {
                // SAFETY: Clear the borrowed pointer before the owning Timer releases its box.
                unsafe { SetWindowLongPtrW(window, GWLP_USERDATA, 0) };
                // SAFETY: This forwards Win32's original native message without retaining its parameters.
                unsafe { DefWindowProcW(window, message, parameter, data) }
            }
            WM_TIMER if parameter == TIMER_ID && data == 0 => {
                // SAFETY: Only WM_NCCREATE installs this pointer and WM_NCDESTROY removes it before box release.
                let address = unsafe { GetWindowLongPtrW(window, GWLP_USERDATA) }.cast_unsigned();
                let registration = ptr::with_exposed_provenance::<Registration>(address);
                // SAFETY: A non-null registration is borrowed only until the clone completes.
                // dispatch may destroy this window and free the box, so no reference is used afterward.
                let work = unsafe { registration.as_ref() }
                    .filter(|registration| Instant::now() >= registration.not_before)
                    .map(|registration| (registration.state.clone(), registration.generation));
                if let Some((state, generation)) = work {
                    dispatch(&state, generation);
                }
                0
            }
            _ => {
                // SAFETY: This forwards Win32's original native message without retaining its parameters.
                unsafe { DefWindowProcW(window, message, parameter, data) }
            }
        }
    }

    impl Drop for Timer {
        /// Removes every OS path to plugin code before releasing registration data and unregistering the class.
        #[expect(
            unsafe_code,
            reason = "Synchronously destroys this thread's private timer and message window"
        )]
        fn drop(&mut self) {
            // Keep the ownership fields explicit: both must remain live until DestroyWindow has returned.
            let _ = (&self.registration, &self.class);
            // SAFETY: This thread exclusively owns the window and its timer.
            // Killing a timer alone does not purge queued messages. Destroying the window removes their target,
            // and zero lParam means DispatchMessage cannot bypass the destroyed window through a TIMERPROC pointer.
            unsafe {
                KillTimer(self.window, TIMER_ID);
                DestroyWindow(self.window);
            }
        }
    }

    #[cfg(test)]
    mod tests {
        use std::cell::Cell;
        use std::rc::Rc;

        use windows_sys::Win32::UI::WindowsAndMessaging::{
            DispatchMessageW, GetClassInfoW, IsWindow, MSG, WM_TIMER, WNDCLASSW,
        };

        use super::{State, TIMER_ID, schedule};

        /// Native firing destroys its target before user code, which can immediately create another private timer.
        #[test]
        #[expect(
            unsafe_code,
            reason = "Delivers a due window timer message synchronously without polling or sleeping"
        )]
        fn firing_destroys_window_before_recursive_scheduling() {
            let calls = Rc::new(Cell::new(0));
            let previous_window = Rc::new(Cell::new(std::ptr::null_mut()));
            let state: Rc<State<super::Timer>> = Rc::new_cyclic(|weak: &std::rc::Weak<State<super::Timer>>| {
                State::new({
                    let weak = weak.clone();
                    let calls = Rc::clone(&calls);
                    let previous_window = Rc::clone(&previous_window);
                    move || {
                        // SAFETY: IsWindow accepts the handle of the just-destroyed private timer window.
                        assert_eq!(unsafe { IsWindow(previous_window.get()) }, 0);
                        calls.set(calls.get() + 1);
                        weak.upgrade()
                            .expect("callback state remains owned")
                            .schedule_with(schedule)
                            .expect("recursive native timer schedules");
                    }
                })
            });
            state.schedule_with(schedule).expect("first native timer schedules");
            let window = {
                let mut pending = state.pending.borrow_mut();
                let (_, timer) = pending.as_mut().expect("timer remains pending");
                // Make this specific registration due without waiting for time to pass.
                timer.registration.not_before = std::time::Instant::now();
                timer.window
            };
            previous_window.set(window);
            let message = MSG {
                hwnd: window,
                message: WM_TIMER,
                wParam: TIMER_ID,
                lParam: 0,
                ..MSG::default()
            };
            // SAFETY: This models a due ordinary WM_TIMER message for the live timer's private window.
            unsafe { DispatchMessageW(&raw const message) };
            assert_eq!(calls.get(), 1);
            assert!(state.pending.borrow().is_some());
            state.cancel();
        }

        /// Pending native messages have no callback pointer and teardown removes their window and class.
        #[test]
        #[expect(
            unsafe_code,
            reason = "Tests the owned native window's destruction without waiting or polling"
        )]
        fn cancellation_removes_window_and_class_before_stale_dispatch() {
            let calls = Rc::new(Cell::new(0));
            let state = Rc::new(State::new({
                let calls = Rc::clone(&calls);
                move || calls.set(calls.get() + 1)
            }));
            let timer = schedule(Rc::downgrade(&state), 1).expect("private timer schedules");
            let window = timer.window;
            let name = timer.class.name.clone();
            let instance = timer.class.instance;
            let queued_message = MSG {
                hwnd: window,
                message: WM_TIMER,
                wParam: TIMER_ID,
                lParam: 0,
                ..MSG::default()
            };
            // SAFETY: The timer owns this live window on the current thread.
            assert_ne!(unsafe { IsWindow(window) }, 0);
            drop(timer);
            // SAFETY: IsWindow accepts stale handles and GetClassInfoW receives an owned terminated class name.
            assert_eq!(unsafe { IsWindow(window) }, 0);
            let mut class = WNDCLASSW::default();
            // SAFETY: The output class structure and terminated input name are valid for this synchronous query.
            assert_eq!(unsafe { GetClassInfoW(instance, name.as_ptr(), &raw mut class) }, 0);
            // SAFETY: This faithfully models an already retrieved ordinary timer message, with no TIMERPROC.
            // DispatchMessageW handles its destroyed target without calling the unregistered plugin procedure.
            unsafe { DispatchMessageW(&raw const queued_message) };
            assert_eq!(calls.get(), 0);
        }
    }
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;
    use std::io;
    use std::rc::Rc;

    use super::{State, dispatch};

    /// Deterministic handle whose destruction stands in for native timer cancellation.
    struct Timer(Rc<Cell<usize>>);

    impl Drop for Timer {
        /// Counts disarming so tests can assert teardown precedes callback execution.
        fn drop(&mut self) {
            self.0.set(self.0.get() + 1);
        }
    }

    /// Repeated requests share one timer, which is removed before the callback sees the state.
    #[test]
    fn requests_coalesce_and_fire_only_once() {
        let calls = Rc::new(Cell::new(0));
        let drops = Rc::new(Cell::new(0));
        let state = Rc::new(State::new({
            let calls = Rc::clone(&calls);
            let drops = Rc::clone(&drops);
            move || {
                assert_eq!(drops.get(), 1);
                calls.set(calls.get() + 1);
            }
        }));
        state
            .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
            .expect("first timer schedules");
        state
            .schedule_with(|_, _| Err(io::Error::other("coalesced requests must not create a timer")))
            .expect("second request coalesces");
        let generation = state.generation.get();
        state.fire(generation);
        state.fire(generation);
        assert_eq!(calls.get(), 1);
        assert!(state.pending.borrow().is_none());
    }

    /// Cancellation is idempotent and a queued firing cannot consume a later scheduling generation.
    #[test]
    fn cancellation_rejects_stale_callbacks_and_allows_retry() {
        let calls = Rc::new(Cell::new(0));
        let drops = Rc::new(Cell::new(0));
        let state = Rc::new(State::new({
            let calls = Rc::clone(&calls);
            move || calls.set(calls.get() + 1)
        }));
        state
            .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
            .expect("first timer schedules");
        let canceled = state.generation.get();
        state.cancel();
        state.cancel();
        state.fire(canceled);
        assert_eq!(drops.get(), 1);
        assert_eq!(calls.get(), 0);
        state
            .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
            .expect("new timer schedules");
        state.fire(canceled);
        assert!(state.pending.borrow().is_some());
        assert_eq!(calls.get(), 0);
        state.fire(state.generation.get());
        assert_eq!(calls.get(), 1);
        assert_eq!(drops.get(), 2);
    }

    /// Clearing and disarming before user code lets the callback schedule another distinct timer.
    #[test]
    fn callback_can_recursively_reschedule() {
        let drops = Rc::new(Cell::new(0));
        let calls = Rc::new(Cell::new(0));
        let state: Rc<State<Timer>> = Rc::new_cyclic(|weak: &std::rc::Weak<State<Timer>>| {
            State::new({
                let weak = weak.clone();
                let drops = Rc::clone(&drops);
                let calls = Rc::clone(&calls);
                move || {
                    calls.set(calls.get() + 1);
                    assert_eq!(drops.get(), calls.get());
                    weak.upgrade()
                        .expect("callback state remains owned")
                        .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
                        .expect("recursive timer schedules");
                }
            })
        });
        state
            .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
            .expect("first timer schedules");
        let first = state.generation.get();
        state.fire(first);
        assert!(state.pending.borrow().is_some());
        state.fire(first);
        assert_eq!(calls.get(), 1);
        state.fire(state.generation.get());
        assert_eq!(calls.get(), 2);
        state.cancel();
        assert_eq!(drops.get(), 3);
    }

    /// Timer creation failure does not strand the coalescing flag or prevent a subsequent request.
    #[test]
    fn failed_creation_leaves_state_idle() {
        let drops = Rc::new(Cell::new(0));
        let state = Rc::new(State::new(|| {}));
        assert!(
            state
                .schedule_with(|_, _| Err(io::Error::other("injected failure")))
                .is_err()
        );
        assert!(state.pending.borrow().is_none());
        state
            .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
            .expect("retry schedules");
        state.cancel();
        assert_eq!(drops.get(), 1);
    }

    /// Native blocks and registry entries cannot retain an abandoned callback or its captured objects.
    #[test]
    fn dropping_owner_releases_timer_and_callback_captures() {
        let drops = Rc::new(Cell::new(0));
        let capture = Rc::new(());
        let captured = Rc::downgrade(&capture);
        let state = Rc::new(State::new(move || {
            let _ = &capture;
        }));
        let weak = Rc::downgrade(&state);
        state
            .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
            .expect("timer schedules");
        let generation = state.generation.get();
        drop(state);
        assert_eq!(drops.get(), 1);
        assert!(captured.upgrade().is_none());
        dispatch(&weak, generation);
    }

    /// The public owner releases captures even when it is canceled repeatedly without scheduling native work.
    #[test]
    fn deferred_drop_releases_callback_captures() {
        let capture = Rc::new(());
        let captured = Rc::downgrade(&capture);
        let deferred = super::Deferred::new(move || {
            let _ = &capture;
        });
        deferred.cancel();
        deferred.cancel();
        assert!(captured.upgrade().is_some());
        drop(deferred);
        assert!(captured.upgrade().is_none());
    }

    /// Native dispatch retains the executing closure when that closure destroys its last external owner.
    #[test]
    fn callback_can_drop_last_owner() {
        let owner = Rc::new(std::cell::RefCell::new(None));
        let calls = Rc::new(Cell::new(0));
        let drops = Rc::new(Cell::new(0));
        let state = Rc::new(State::new({
            let owner = Rc::clone(&owner);
            let calls = Rc::clone(&calls);
            move || {
                owner.borrow_mut().take();
                calls.set(calls.get() + 1);
            }
        }));
        state
            .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
            .expect("timer schedules");
        let generation = state.generation.get();
        let weak = Rc::downgrade(&state);
        *owner.borrow_mut() = Some(state);
        dispatch(&weak, generation);
        assert_eq!(calls.get(), 1);
        assert_eq!(drops.get(), 1);
        assert!(weak.upgrade().is_none());
    }

    /// A callback panic stays inside the native boundary and does not leave its completed timer armed.
    #[test]
    fn callback_panic_is_contained_after_disarming() {
        let drops = Rc::new(Cell::new(0));
        let state = Rc::new(State::new(|| {
            assert!(std::hint::black_box(false), "injected callback panic");
        }));
        state
            .schedule_with(|_, _| Ok(Timer(Rc::clone(&drops))))
            .expect("timer schedules");
        dispatch(&Rc::downgrade(&state), state.generation.get());
        assert!(state.pending.borrow().is_none());
        assert_eq!(drops.get(), 1);
    }
}
