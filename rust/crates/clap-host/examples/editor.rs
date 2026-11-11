//! Bounded, hardware-free qualification of a trusted build-tree CLAP native editor.
//!
//! Run `cargo run -p clap-host --features editor --example clap_editor -- /absolute/plugin.clap`.
//! The bundle is executed in process, never discovered or installed, and no application storage is opened.
//! Native window structure, not screenshots or pixels, establishes attachment and cleanup.

use std::error::Error;
use std::ffi::OsString;
use std::io;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use clap_host::{EditorError, EditorWindow, Module, PluginInstance, UiThread, service_main_thread};
use plugin_host_core::pump_platform_events;

/// Default automatic close time for a short native smoke run.
const DEFAULT_DURATION_MS: u64 = 250;
/// Longest supported manual observation interval.
const MAX_DURATION_MS: u64 = 60_000;
/// Maximum time between native events and CLAP callback/timer servicing.
const SERVICE_INTERVAL: Duration = Duration::from_millis(10);
/// Command help is handled before UI registration, native initialization, or bundle loading.
const HELP: &str = "Qualify a trusted build-tree CLAP native editor without audio hardware.\n\n\
Usage: clap_editor <ABSOLUTE_BUNDLE.clap> [-d|--duration-ms <MILLISECONDS>]\n\n\
  -d, --duration-ms <MILLISECONDS>  Automatic close after 1..=60000 ms [default: 250]\n\
  -h, --help                        Print help without loading native code\n\n\
Pass only an explicitly authorized build-tree bundle. Native plugin code executes\n\
in process and can crash or hang. The duration bounds event pumping, not plugin calls.\n\
No discovery, installation, audio device, application storage, or screenshots are used.\n\
A missing native editor is a diagnostic failure, never a generic parameter window.";

/// Fallible example operations preserve native host errors without a new library error API.
type Result<T> = std::result::Result<T, Box<dyn Error>>;

/// Validated command that can be parsed without touching native platform state.
#[derive(Debug)]
struct Options {
    bundle: PathBuf,
    duration: Duration,
}

/// Native client geometry and actual attached child views/windows, in platform-native units.
#[derive(Debug)]
struct NativeState {
    width: f64,
    height: f64,
    children: usize,
    visible: bool,
}

/// Parses arguments before registering or initializing any native UI.
fn main() -> Result<()> {
    let Some(options) = parse_arguments(std::env::args_os().skip(1))? else {
        println!("{HELP}");
        return Ok(());
    };
    let ui = UiThread::register_current()?;
    let title = format!("AudioBatch CLAP editor qualification {}", std::process::id());
    // The application scope drains Cocoa autoreleases before the final native-window inspection.
    let result = {
        let _application = native::Application::start()?;
        let module = Module::load(&ui, &options.bundle)?;
        let class = module
            .classes(&ui)
            .iter()
            .find(|class| class.is_effect)
            .ok_or_else(|| io::Error::other("The selected bundle has no CLAP audio effect"))?;
        println!("Loaded {} ({}) from {}", class.name, class.id, options.bundle.display());
        let instance = PluginInstance::create(&ui, &module, &class.id, None)?;
        // Do not activate a processor or touch audio hardware just to qualify a native editor.
        let result = qualify_editor(&ui, &instance, &title, options.duration);
        drop(instance);
        service_main_thread(&ui);
        pump_platform_events(&ui, SERVICE_INTERVAL);
        service_main_thread(&ui);
        result
    };
    let cleanup = native::inspect(&title)?.is_none();
    println!("Cleanup: matching native window absent={cleanup}, instance retired on main thread");
    result?;
    if !cleanup {
        return Err(io::Error::other("The native editor window survived editor destruction").into());
    }
    println!("PASS: native editor attached, serviced, and destroyed without audio hardware");
    Ok(())
}

/// Opens and inspects the real native editor, then drops it before its instance even on failure.
fn qualify_editor(ui: &UiThread, instance: &PluginInstance, title: &str, duration: Duration) -> Result<()> {
    let editor = EditorWindow::open(ui, instance, title, None, None).map_err(|error| match error {
        EditorError::MissingEditor | EditorError::UnsupportedWindowType => io::Error::other(format!(
            "NO EDITOR: {error}. This qualification requires a plugin-provided native GUI, not host parameter controls"
        ))
        .into(),
        error => Box::<dyn Error>::from(error),
    })?;
    if editor.is_closed() {
        return Err(io::Error::other("The editor was already closed immediately after open").into());
    }
    let started = Instant::now();
    let mut attached = false;
    let mut ticks = 0_u64;
    while let Some(remaining) = duration.checked_sub(started.elapsed()) {
        if remaining.is_zero() {
            break;
        }
        pump_platform_events(ui, remaining.min(SERVICE_INTERVAL));
        service_main_thread(ui);
        ticks += 1;
        if let Some(error) = editor.take_error() {
            return Err(Box::new(error));
        }
        if editor.is_closed() {
            return Err(io::Error::other("The editor closed before the automatic qualification deadline").into());
        }
        if !attached
            && let Some(state) = native::inspect(title)?
            && state.width.is_finite()
            && state.height.is_finite()
            && state.width > 0.0
            && state.height > 0.0
            && state.children > 0
            && state.visible
        {
            println!(
                "Native editor: open=true, client={}x{}, attached children={}",
                state.width, state.height, state.children
            );
            attached = true;
        }
    }
    if !attached {
        return Err(io::Error::other("No positive-sized native editor with an attached child was observed").into());
    }
    println!(
        "Serviced {ticks} main-thread event/timer ticks for {} ms",
        duration.as_millis()
    );
    drop(editor);
    Ok(())
}

/// Parses the one explicitly authorized bundle and a bounded duration, with side-effect-free help.
fn parse_arguments(arguments: impl IntoIterator<Item = OsString>) -> Result<Option<Options>> {
    let arguments: Vec<_> = arguments.into_iter().collect();
    if arguments
        .iter()
        .any(|argument| argument == "--help" || argument == "-h")
    {
        return Ok(None);
    }
    let mut arguments = arguments.into_iter();
    let mut bundle = None;
    let mut duration_ms = DEFAULT_DURATION_MS;
    let mut duration_supplied = false;
    while let Some(argument) = arguments.next() {
        if argument == "--duration-ms" || argument == "-d" {
            if duration_supplied {
                return Err(io::Error::other("Supply --duration-ms only once").into());
            }
            duration_supplied = true;
            duration_ms = arguments
                .next()
                .and_then(|value| value.to_str().and_then(|value| value.parse().ok()))
                .filter(|value| (1..=MAX_DURATION_MS).contains(value))
                .ok_or_else(|| io::Error::other("--duration-ms requires an integer from 1 through 60000"))?;
        } else if argument.to_string_lossy().starts_with('-') || bundle.is_some() {
            return Err(io::Error::other(format!("Unexpected argument: {}", argument.to_string_lossy())).into());
        } else {
            bundle = Some(PathBuf::from(argument));
        }
    }
    let bundle = bundle.ok_or_else(|| io::Error::other("Supply an absolute build-tree .clap bundle, or --help"))?;
    if !bundle.is_absolute() || bundle.extension().is_none_or(|extension| extension != "clap") {
        return Err(io::Error::other("The explicitly authorized bundle must be an absolute .clap path").into());
    }
    Ok(Some(Options {
        bundle,
        duration: Duration::from_millis(duration_ms),
    }))
}

/// `AppKit` introspection is confined to this main-thread-only qualification boundary.
#[cfg(target_os = "macos")]
mod native {
    #![expect(
        unsafe_code,
        reason = "inspects process-owned AppKit windows through the Objective-C runtime"
    )]

    use std::ffi::{CStr, c_char, c_void};
    use std::io;

    use super::{NativeState, Result};

    /// An unretained Objective-C object used only synchronously on the main thread.
    type Object = *mut c_void;

    #[link(name = "AppKit", kind = "framework")]
    unsafe extern "C" {}

    #[link(name = "objc")]
    unsafe extern "C" {
        fn objc_getClass(name: *const c_char) -> Object;
        fn sel_registerName(name: *const c_char) -> Object;
        fn objc_msgSend();
        #[cfg(target_arch = "x86_64")]
        fn objc_msgSend_stret();
        fn objc_autoreleasePoolPush() -> Object;
        fn objc_autoreleasePoolPop(pool: Object);
    }

    /// Calls a known Objective-C method with its exact ABI signature.
    macro_rules! send {
        ($receiver:expr, $selector:literal, $result:ty $(, $kind:ty => $argument:expr)*) => {{
            let function: unsafe extern "C" fn(Object, Object $(, $kind)*) -> $result =
                std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
            function($receiver, sel_registerName(concat!($selector, "\0").as_ptr().cast()) $(, $argument)*)
        }};
    }

    /// Cocoa rectangles have four `CGFloat` values on supported 64-bit macOS architectures.
    #[derive(Default)]
    #[repr(C)]
    struct Rect {
        x: f64,
        y: f64,
        width: f64,
        height: f64,
    }

    /// Keeps autoreleased objects bounded and releases inspection arrays before editor destruction.
    struct Pool(Object);

    /// Keeps native initialization and its autorelease scope on the process main thread.
    pub struct Application {
        _pool: Pool,
    }

    impl Pool {
        /// Starts one nested main-thread autorelease scope.
        fn new() -> Self {
            // SAFETY: Every caller runs synchronously on the process main thread.
            Self(unsafe { objc_autoreleasePoolPush() })
        }
    }

    impl Drop for Pool {
        fn drop(&mut self) {
            // SAFETY: Pools are popped on their creating thread in reverse creation order.
            unsafe { objc_autoreleasePoolPop(self.0) };
        }
    }

    impl Application {
        /// Initializes the shared native application without application data or audio initialization.
        pub fn start() -> Result<Self> {
            let pool = Pool::new();
            // SAFETY: main calls this on the process main thread, with a live autorelease pool.
            // Each selector below has the exact declared Objective-C signature.
            unsafe {
                let application = application();
                if application.is_null() {
                    return Err(io::Error::other("Could not initialize NSApplication").into());
                }
                let _ = send!(application, "setActivationPolicy:", bool, isize => 0);
                send!(application, "finishLaunching", ());
            }
            Ok(Self { _pool: pool })
        }
    }

    /// Finds only this process's uniquely titled host window and reads its actual content hierarchy.
    pub fn inspect(title: &str) -> Result<Option<NativeState>> {
        let _pool = Pool::new();
        // SAFETY: AppKit owns these objects for the synchronous main-thread inspection.
        // The windows array keeps each selected window alive until this nested pool is drained.
        // Selector signatures match AppKit, Foundation, and the supported 64-bit Cocoa ABI.
        unsafe {
            let windows = send!(application(), "windows", Object);
            let count = send!(windows, "count", usize);
            for index in 0..count {
                let window = send!(windows, "objectAtIndex:", Object, usize => index);
                let native_title = send!(window, "title", Object);
                let text = send!(native_title, "UTF8String", *const c_char);
                if text.is_null() || CStr::from_ptr(text).to_bytes() != title.as_bytes() {
                    continue;
                }
                let content = send!(window, "contentView", Object);
                if content.is_null() {
                    return Err(io::Error::other("The native window has no content view").into());
                }
                let bounds = bounds(content);
                let children = send!(send!(content, "subviews", Object), "count", usize);
                return Ok(Some(NativeState {
                    width: bounds.width,
                    height: bounds.height,
                    children,
                    visible: send!(window, "isVisible", bool),
                }));
            }
        }
        Ok(None)
    }

    /// Gets the process singleton without retaining it beyond `AppKit`'s ownership.
    unsafe fn application() -> Object {
        // SAFETY: The class and selector are known AppKit symbols with their exact native signature.
        unsafe { send!(objc_getClass(c"NSApplication".as_ptr()), "sharedApplication", Object) }
    }

    /// Reads a view's bounds using the architecture's required Objective-C struct-return convention.
    unsafe fn bounds(view: Object) -> Rect {
        #[cfg(target_arch = "aarch64")]
        // SAFETY: ARM64 returns this four-double Cocoa rectangle through the ordinary message ABI.
        unsafe {
            send!(view, "bounds", Rect)
        }
        #[cfg(target_arch = "x86_64")]
        // SAFETY: Intel returns this 32-byte Cocoa rectangle through an explicit output pointer.
        unsafe {
            let function: unsafe extern "C" fn(*mut Rect, Object, Object) =
                std::mem::transmute(objc_msgSend_stret as unsafe extern "C" fn());
            let mut result = Rect::default();
            function(&raw mut result, view, sel_registerName(c"bounds".as_ptr()));
            result
        }
    }
}

/// Win32 introspection stays on this thread and never enumerates other processes' windows.
#[cfg(windows)]
mod native {
    #![expect(
        unsafe_code,
        reason = "inspects only thread-owned Win32 windows and their child handles"
    )]

    use std::ffi::c_void;
    use std::io;

    use super::{NativeState, Result};

    /// Win32's first-child and next-sibling traversal commands.
    const GW_CHILD: u32 = 5;
    /// Win32's next-sibling traversal command.
    const GW_HWNDNEXT: u32 = 2;

    /// Plain Win32 client rectangle with the platform ABI.
    #[derive(Default)]
    #[repr(C)]
    struct Rect {
        left: i32,
        top: i32,
        right: i32,
        bottom: i32,
    }

    /// Synchronous enumeration context, borrowed only for the duration of `EnumThreadWindows`.
    struct Search<'a> {
        title: &'a [u16],
        found: *mut c_void,
    }

    /// Win32 needs no process application object before a host window is created.
    pub struct Application;

    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentThreadId() -> u32;
    }

    #[link(name = "user32")]
    unsafe extern "system" {
        fn EnumThreadWindows(
            thread: u32,
            callback: unsafe extern "system" fn(*mut c_void, isize) -> i32,
            data: isize,
        ) -> i32;
        fn GetWindowTextLengthW(window: *mut c_void) -> i32;
        fn GetWindowTextW(window: *mut c_void, text: *mut u16, maximum: i32) -> i32;
        fn GetClientRect(window: *mut c_void, rectangle: *mut Rect) -> i32;
        fn IsWindowVisible(window: *mut c_void) -> i32;
        fn GetWindow(window: *mut c_void, command: u32) -> *mut c_void;
    }

    impl Application {
        /// Provides the same fallible startup interface as `AppKit` without accessing native state.
        #[expect(clippy::unnecessary_wraps, reason = "matches fallible AppKit initialization")]
        pub const fn start() -> Result<Self> {
            Ok(Self)
        }
    }

    /// Reads the current thread's uniquely titled editor and counts its real immediate child HWNDs.
    pub fn inspect(title: &str) -> Result<Option<NativeState>> {
        let title: Vec<_> = title.encode_utf16().collect();
        let mut search = Search {
            title: &title,
            found: std::ptr::null_mut(),
        };
        // SAFETY: The synchronous callback borrows search only during enumeration of this thread.
        unsafe {
            EnumThreadWindows(
                GetCurrentThreadId(),
                find_window,
                (&raw mut search).expose_provenance().cast_signed(),
            );
        }
        if search.found.is_null() {
            return Ok(None);
        }
        let mut rectangle = Rect::default();
        // SAFETY: The matching HWND belongs to this thread and no events run before inspection completes.
        if unsafe { GetClientRect(search.found, &raw mut rectangle) } == 0 {
            return Err(io::Error::last_os_error().into());
        }
        let mut children = 0;
        // SAFETY: Parent and child HWNDs remain live during this non-reentrant same-thread inspection.
        unsafe {
            let mut child = GetWindow(search.found, GW_CHILD);
            while !child.is_null() {
                children += 1;
                child = GetWindow(child, GW_HWNDNEXT);
            }
        }
        Ok(Some(NativeState {
            width: f64::from(rectangle.right - rectangle.left),
            height: f64::from(rectangle.bottom - rectangle.top),
            children,
            // SAFETY: The matching HWND still belongs to this thread and no event pumping intervenes.
            visible: unsafe { IsWindowVisible(search.found) } != 0,
        }))
    }

    /// Selects the exact title without mutating windows or invoking GUI automation.
    unsafe extern "system" fn find_window(window: *mut c_void, data: isize) -> i32 {
        // SAFETY: EnumThreadWindows receives the live exclusive Search pointer from inspect.
        let search = unsafe { &mut *std::ptr::with_exposed_provenance_mut::<Search<'_>>(data.cast_unsigned()) };
        // SAFETY: The enumerated HWND belongs to this thread and remains valid during the callback.
        let length = unsafe { GetWindowTextLengthW(window) };
        if usize::try_from(length).ok() != Some(search.title.len()) {
            return 1;
        }
        let mut title = vec![0; search.title.len() + 1];
        // SAFETY: The buffer has length+1 UTF-16 elements, including space for the terminating null.
        let written = unsafe { GetWindowTextW(window, title.as_mut_ptr(), length + 1) };
        if written == length && title.get(..search.title.len()) == Some(search.title) {
            search.found = window;
            return 0;
        }
        1
    }
}

#[cfg(test)]
mod argument_tests {
    use super::*;

    /// Builds OS-native argument vectors without filesystem or plugin dependencies.
    fn arguments(values: &[&str]) -> Vec<OsString> {
        values.iter().map(OsString::from).collect()
    }

    /// Uses a syntactically absolute path suitable for each supported platform.
    fn bundle_path() -> &'static str {
        if cfg!(windows) {
            r"C:\build\example.clap"
        } else {
            "/build/example.clap"
        }
    }

    #[test]
    fn help_bypasses_bundle_and_duration_validation() {
        assert!(
            parse_arguments(arguments(&["--duration-ms", "invalid", "--help"]))
                .expect("help parses")
                .is_none()
        );
    }

    #[test]
    fn defaults_to_short_automatic_close() {
        let options = parse_arguments(arguments(&[bundle_path()]))
            .expect("absolute bundle parses")
            .expect("run options");
        assert_eq!(options.duration, Duration::from_millis(DEFAULT_DURATION_MS));
    }

    #[test]
    fn duration_is_bounded_and_accepts_short_alias() {
        let options = parse_arguments(arguments(&["-d", "500", bundle_path()]))
            .expect("short option parses")
            .expect("run options");
        assert_eq!(options.duration, Duration::from_millis(500));
        for duration in ["0", "60001", "-1", "invalid"] {
            assert!(parse_arguments(arguments(&[bundle_path(), "--duration-ms", duration])).is_err());
        }
    }

    #[test]
    fn rejects_implicit_or_ambiguous_bundle_authorization() {
        assert!(parse_arguments(arguments(&[])).is_err());
        assert!(parse_arguments(arguments(&["relative.clap"])).is_err());
        assert!(parse_arguments(arguments(&[bundle_path(), bundle_path()])).is_err());
        assert!(parse_arguments(arguments(&[bundle_path(), "-d", "1", "-d", "2"])).is_err());
    }
}
