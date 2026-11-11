//! Direct Iced/baseview child-window ownership, UI-only host callbacks, and bounded editor polling.
//!
//! The UI module supplies `create_program_with_notifier(parameters, shared, binding, window_size, notifier)`,
//! returning an `iced_program::Program<Renderer = iced_baseview::Renderer>`.
//! Its state owns its analysis worker. Its polling subscription uses `iced_baseview::poll_events()`
//! and rearms the notifier after each refresh. No parameter or audio callback sends messages into this bridge.

mod child;
mod native;

use baseview::dpi::NativeSize;
use baseview::host::{Host, HostCallbacks};
use baseview::{HandlerError, Window, WindowSize};
use fragile::Fragile;
use iced_baseview::shell::window::IcedMessageSender;
use iced_baseview::{IcedWindowSettings, PollSubNotifier};
use image_control_dsp::SharedState;
use std::cell::{Cell, RefCell};
use std::error::Error;
use std::ffi::c_void;
use std::fmt;
use std::rc::Rc;
use std::sync::Arc;
use std::time::Duration;

use crate::params::{EditorBinding, Parameters};

/// Main-thread polling cadence for host atomics and completed analysis, not a second timer thread.
const DISPLAY_INTERVAL: Duration = Duration::from_millis(16);

/// Failures at the native embedding boundary. None creates a replacement standalone window.
#[derive(Debug)]
pub enum EditorError {
    /// The native parent is null or is not owned by the calling window thread.
    InvalidParent,
    /// The host supplied a zero or unrepresentable native size.
    InvalidSize,
    /// The call is not on the native parent's UI thread.
    WrongThread,
    /// The host supplied a non-positive or non-finite content scale.
    InvalidScaleFactor,
    /// The window was destroyed and cannot accept further operations.
    Closed,
    /// Native removal must be retried after the current Iced callback returns.
    CallbackActive,
    /// The host declined an editor-originated resize.
    ResizeDenied,
    /// The editor's owned task worker could not be started.
    Executor(std::io::Error),
    /// Native window creation, GPU initialization, or a window operation failed.
    Native(baseview::Error),
}

impl fmt::Display for EditorError {
    /// Formats attachment errors outside the audio thread without installing a host-global logger.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidParent => formatter.write_str("the editor requires a live native host parent"),
            Self::InvalidSize => formatter.write_str("the editor dimensions must be positive native coordinates"),
            Self::WrongThread => formatter.write_str("the editor must be operated on its native UI thread"),
            Self::InvalidScaleFactor => formatter.write_str("the editor content scale must be positive and finite"),
            Self::Closed => formatter.write_str("the embedded editor has been closed"),
            Self::CallbackActive => formatter.write_str("retry editor removal after the active UI callback returns"),
            Self::ResizeDenied => formatter.write_str("the host declined the editor resize"),
            Self::Executor(error) => write!(formatter, "could not start editor tasks: {error}"),
            Self::Native(error) => write!(formatter, "could not operate the embedded Iced editor: {error}"),
        }
    }
}

impl Error for EditorError {
    /// Retains the native failure as a diagnostic source, never printing from plugin code.
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Native(error) => Some(error),
            Self::Executor(error) => Some(error),
            _ => None,
        }
    }
}

impl From<baseview::Error> for EditorError {
    /// Preserves native/GPU failures for the host wrapper's explicit attachment error path.
    fn from(error: baseview::Error) -> Self {
        Self::Native(error)
    }
}

/// Owns an embedded Iced window and its message endpoint on the parent's native UI thread.
/// Drop it before releasing the host parent. This type is deliberately neither Send nor Sync.
pub struct Editor {
    /// Each native operation retains storage independently, so removal never invalidates its active borrow.
    window: RefCell<Option<Rc<Window>>>,
    /// Detaches the actual child without consuming native storage used by a reentrant resize.
    child: child::ChildLease,
    /// Closes open host gestures without waiting for a borrowed Iced state to drop.
    binding: Rc<dyn EditorBinding>,
    /// Joins analysis independently of a still-active Iced program update.
    analysis: Rc<RefCell<Option<image_control_dsp::AnalysisShutdown>>>,
    /// Disables observation independently of any active Iced callback retaining its editor state.
    shared: Arc<SharedState>,
    /// Retains the typed message endpoint without exposing UI messages or channels to audio callbacks.
    message_sender: RefCell<Option<IcedMessageSender<crate::editor::Message>>>,
    /// Authoritative lifecycle, independent of platform-specific `Window::is_open` behavior.
    open: Rc<Cell<bool>>,
    /// Closing resources is independent of unsolicited native destroyed notifications.
    closing: Cell<bool>,
    /// Captures the creator's identity for every host-facing operation.
    owner_thread: std::thread::ThreadId,
    /// Cancels and joins every subscription task after native runtime destruction.
    executor: RefCell<crate::iced_executor::Shutdown>,
}

impl Editor {
    /// Attaches a GPU-only Iced child to an existing host parent, without owning its application or run loop.
    /// The builder is synchronous on the native UI thread on both supported platforms.
    /// `Fragile` checks that ownership while satisfying baseview's platform-independent Send builder bound.
    /// It does not make the binding or its COM interfaces transferable between threads.
    ///
    /// # Errors
    /// Reports an invalid parent, dimensions, UI thread, native window, or GPU initialization failure.
    ///
    /// # Safety
    /// `parent` must be a live `AppKit` `NSView` on macOS or `HWND` on Windows.
    /// It must remain valid until the returned editor is dropped on the same native UI thread.
    /// The caller must not destroy that parent reentrantly during a host binding callback.
    #[expect(
        unsafe_code,
        reason = "The native host pointer is validated and encapsulated at this API boundary"
    )]
    pub unsafe fn attach(
        parent: *mut c_void,
        size: baseview::dpi::Size,
        fallback_scale: f64,
        parameters: Arc<Parameters>,
        shared: Arc<SharedState>,
        binding: Rc<dyn EditorBinding>,
    ) -> Result<Self, EditorError> {
        let logical = size.to_logical::<f64>(fallback_scale);
        if !logical.width.is_finite() || !logical.height.is_finite() || logical.width <= 0.0 || logical.height <= 0.0 {
            return Err(EditorError::InvalidSize);
        }
        // SAFETY: The caller guarantees the parent's native type and lifetime for this editor.
        let native_parent = unsafe { native::NativeParent::new(parent)? };
        // SAFETY: NativeParent validated the same host pointer and UI thread above.
        let children = unsafe { child::ChildrenBefore::capture(parent)? };
        let analysis = Rc::new(RefCell::new(None));
        let open = Rc::new(Cell::new(true));
        let notifier = PollSubNotifier::new();
        let settings = IcedWindowSettings::new()
            .with_title("ImageControl RS")
            .with_size(size)
            .with_parent(&native_parent)
            .with_resizable(true)
            .with_fallback_scale_factor(fallback_scale)
            .with_min_size(Some(baseview::dpi::LogicalSize::new(
                f64::from(crate::editor::MIN_EDITOR_SIZE.0),
                f64::from(crate::editor::MIN_EDITOR_SIZE.1),
            )))
            .with_max_size(Some(baseview::dpi::LogicalSize::new(
                f64::from(crate::editor_geometry::MAX_EDITOR_SIZE.0),
                f64::from(crate::editor_geometry::MAX_EDITOR_SIZE.1),
            )))
            .with_poll_interval(DISPLAY_INTERVAL)
            .with_always_redraw(true);
        let host = Host::new().with_callbacks(EditorHost {
            binding: Rc::clone(&binding),
            open: Rc::clone(&open),
        });

        // Baseview constructs the handler on this same thread on macOS and Windows.
        // Fragile supplies a checked boundary instead of an unsafe Send implementation for Rc.
        // Both successful extraction and early-error destruction occur on the owner thread.
        let ui_binding = Fragile::new((Rc::clone(&binding), Rc::clone(&analysis)));
        let build_notifier = notifier.clone();
        let editor_shared = Arc::clone(&shared);
        let executor = crate::iced_executor::Shutdown::new().map_err(EditorError::Executor)?;
        let (window, message_sender) = crate::iced_executor::with_executor(&executor, || {
            iced_baseview::create_window(
                settings,
                notifier,
                move |shared_window_size| {
                    let (binding, analysis) = ui_binding.into_inner();
                    let binding = Rc::new(LogicalBinding {
                        native: binding,
                        window_size: shared_window_size.clone(),
                    });
                    Ok(crate::editor::create_program_with_notifier(
                        parameters,
                        shared,
                        binding,
                        shared_window_size,
                        build_notifier,
                        analysis,
                    ))
                },
                Some(host),
            )
        })?;
        let editor = Self {
            window: RefCell::new(Some(Rc::new(window))),
            child: children.finish()?,
            binding,
            analysis,
            shared: editor_shared,
            message_sender: RefCell::new(Some(message_sender)),
            open,
            closing: Cell::new(false),
            owner_thread: std::thread::current().id(),
            executor: RefCell::new(executor),
        };
        editor.window()?.show()?;
        Ok(editor)
    }

    /// Applies a host-originated native resize without asking the host to resize itself again.
    /// macOS coordinates are logical points. Windows coordinates are physical pixels.
    ///
    /// # Errors
    /// Rejects invalid dimensions, the wrong thread, a closed editor, or a native resize failure.
    pub fn resize(&self, width: u32, height: u32) -> Result<(), EditorError> {
        validate_size(width, height)?;
        self.window()?.resize(NativeSize::new(width, height))?;
        Ok(())
    }

    /// Reads committed platform geometry without borrowing Iced or emitting another host request.
    pub(crate) fn native_size(&self) -> Result<NativeSize<u32>, EditorError> {
        Ok(self.window()?.size().into())
    }

    /// Reads native DPI without entering Iced or dispatching a native geometry callback.
    pub(crate) fn scale_factor(&self) -> Result<f64, EditorError> {
        Ok(self.window()?.size().scale_factor)
    }

    /// Suggests the host's DPI scale without applying a second user zoom on top of native scaling.
    /// Native platform DPI is authoritative where available, including `AppKit` backing scale.
    ///
    /// # Errors
    /// Rejects invalid factors, the wrong thread, a closed window, or a native operation failure.
    pub fn set_scale_factor(&self, scale_factor: f64) -> Result<(), EditorError> {
        if !scale_factor.is_finite() || scale_factor <= 0.0 {
            return Err(EditorError::InvalidScaleFactor);
        }
        self.window()?.suggest_fallback_scale_factor(scale_factor)?;
        Ok(())
    }

    /// Checks owner-thread and lifecycle preconditions before touching the native window.
    fn window(&self) -> Result<Rc<Window>, EditorError> {
        if self.owner_thread != std::thread::current().id() {
            return Err(EditorError::WrongThread);
        }
        if !self.open.get() {
            return Err(EditorError::Closed);
        }
        self.window.borrow().clone().ok_or(EditorError::Closed)
    }
}

impl Editor {
    /// Detaches the host's child and joins both worker owners even during a native geometry callback.
    /// An active native call may retain its window storage until it returns, without retaining the host parent.
    /// Eventual Iced state destruction is separate from synchronous worker retirement.
    pub fn close(&self) -> Result<(), EditorError> {
        if self.closing.replace(true) {
            return Ok(());
        }
        self.open.set(false);
        self.shared.set_correlation_enabled(false);
        self.shared.set_analysis_enabled(false);
        let detached = self.child.detach();
        self.binding.close();
        let analysis = self.analysis.borrow_mut().take();
        if let Some(analysis) = analysis {
            analysis.shutdown();
        }
        let sender = self.message_sender.borrow_mut().take();
        drop(sender);
        if let Err(payload) = self.executor.borrow_mut().shutdown() {
            std::panic::resume_unwind(payload);
        }
        let window = self.window.borrow_mut().take();
        drop(window);
        detached
    }
}

impl Drop for Editor {
    /// No owned worker depends on a future UI turn or on destruction of an active Iced handler.
    /// The upstream widget-cache cleanup and arbitrary module unloading remain separate limitations.
    fn drop(&mut self) {
        let _ = self.close();
    }
}

/// Gives Iced controls logical resize coordinates while preserving native coordinates at the VST3 boundary.
/// The shared size is read without borrowing either the active Iced state or the baseview window.
struct LogicalBinding {
    /// Host binding takes platform-native dimensions and owns balanced parameter gestures.
    native: Rc<dyn EditorBinding>,
    /// Native DPI follows the live child, rather than an unrelated host fallback or backing-pixel guess.
    window_size: iced_baseview::shell::SharedWindowSize,
}

impl EditorBinding for LogicalBinding {
    fn is_open(&self) -> bool {
        self.native.is_open()
    }

    fn begin_edit(&self, id: u32) {
        self.native.begin_edit(id);
    }

    fn set_value(&self, id: u32, normalized: f64) {
        self.native.set_value(id, normalized);
    }

    fn end_edit(&self, id: u32) {
        self.native.end_edit(id);
    }

    fn mark_dirty(&self) {
        self.native.mark_dirty();
    }

    fn close(&self) {
        self.native.close();
    }

    /// Converts a user grip's logical dimensions once, before asking the host to resize its frame.
    fn request_resize(&self, width: u32, height: u32) -> bool {
        let Some((width, height)) =
            logical_resize_size(width, height, self.window_size.get().scale_factor, cfg!(windows))
        else {
            return false;
        };
        self.native.request_resize(width, height)
    }
}

/// Bounds logical user geometry before DPI conversion. macOS host coordinates remain points at every backing scale.
fn logical_resize_size(width: u32, height: u32, scale: f64, physical: bool) -> Option<(u32, u32)> {
    if !scale.is_finite() || scale <= 0.0 || width == 0 || height == 0 {
        return None;
    }
    let factor = if physical { scale } else { 1.0 };
    let convert = |dimension: u32, minimum: f32, maximum: u32| {
        let native = f64::from(dimension.clamp(minimum as u32, maximum)) * factor;
        (native.round() <= f64::from(i32::MAX)).then_some(native.round() as u32)
    };
    Some((
        convert(
            width,
            crate::editor::MIN_EDITOR_SIZE.0,
            crate::editor_geometry::MAX_EDITOR_SIZE.0,
        )?,
        convert(
            height,
            crate::editor::MIN_EDITOR_SIZE.1,
            crate::editor_geometry::MAX_EDITOR_SIZE.1,
        )?,
    ))
}

/// Main-thread host callbacks use the same Rc as parameter gestures, never an Arc/Send COM proxy.
struct EditorHost {
    /// The host wrapper owns the actual UI-thread interface implementation.
    binding: Rc<dyn EditorBinding>,
    /// Suppresses late requests when the native window is being destroyed.
    open: Rc<Cell<bool>>,
}

impl HostCallbacks for EditorHost {
    /// Negotiates editor-originated dimensions in native units before baseview commits its resize.
    fn request_resize(&mut self, new_size: WindowSize) -> Result<(), HandlerError> {
        if !self.open.get() {
            return Err(EditorError::Closed.into());
        }
        let size = NativeSize::<u32>::from(new_size);
        validate_size(size.width, size.height)?;
        if self.binding.request_resize(size.width, size.height) {
            Ok(())
        } else {
            Err(EditorError::ResizeDenied.into())
        }
    }

    /// Marks an unexpectedly destroyed child closed without dispatching callbacks into a removed editor.
    fn destroyed(&mut self) {
        self.open.set(false);
    }
}

/// Bounds native dimensions before casting into the host's signed rectangle coordinates.
const fn validate_size(width: u32, height: u32) -> Result<(), EditorError> {
    if width == 0 || height == 0 || width > i32::MAX as u32 || height > i32::MAX as u32 {
        Err(EditorError::InvalidSize)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use super::{EditorBinding, HostCallbacks, NativeSize, Rc, WindowSize};
    use super::{EditorError, EditorHost, validate_size};

    /// Records only UI callbacks, allowing lifecycle checks without a native window or audio device.
    #[derive(Default)]
    struct RecordingBinding {
        /// Native host dimensions requested by the child.
        requests: RefCell<Vec<(u32, u32)>>,
        /// Explicit response from the host frame.
        accept_resize: bool,
    }

    impl EditorBinding for RecordingBinding {
        /// No parameter gesture is needed for a pure host-window lifecycle contract.
        fn begin_edit(&self, _id: u32) {}

        /// No parameter value is changed by native size negotiation.
        fn set_value(&self, _id: u32, _normalized: f64) {}

        /// No parameter gesture is opened by native size negotiation.
        fn end_edit(&self, _id: u32) {}

        /// Records the native dimensions before returning the scripted host response.
        fn request_resize(&self, width: u32, height: u32) -> bool {
            self.requests.borrow_mut().push((width, height));
            self.accept_resize
        }
    }

    #[test]
    fn host_resize_uses_native_coordinates_and_stops_after_close() {
        let binding = Rc::new(RecordingBinding {
            accept_resize: true,
            ..RecordingBinding::default()
        });
        let open = Rc::new(std::cell::Cell::new(true));
        let mut host = EditorHost {
            binding: binding.clone(),
            open: Rc::clone(&open),
        };
        let size = WindowSize::from_logical(baseview::dpi::LogicalSize::new(640.0, 480.0), 2.0);
        let native = NativeSize::<u32>::from(size);
        assert!(host.request_resize(size).is_ok());
        assert_eq!(*binding.requests.borrow(), [(native.width, native.height)]);
        host.destroyed();
        assert!(!open.get());
        assert!(host.request_resize(size).is_err());
        assert_eq!(binding.requests.borrow().len(), 1);
    }

    #[test]
    fn host_resize_denial_is_not_silently_accepted() {
        let binding = Rc::new(RecordingBinding::default());
        let mut host = EditorHost {
            binding: binding.clone(),
            open: Rc::new(std::cell::Cell::new(true)),
        };
        let size = WindowSize::from_logical(baseview::dpi::LogicalSize::new(640.0, 480.0), 1.0);
        assert!(host.request_resize(size).is_err());
        assert_eq!(*binding.requests.borrow(), [(640, 480)]);
    }

    #[test]
    fn native_dimensions_reject_zero_and_signed_overflow() {
        for (width, height) in [(0, 760), (1000, 0), (u32::MAX, 760), (1000, u32::MAX)] {
            assert!(matches!(validate_size(width, height), Err(EditorError::InvalidSize)));
        }
        assert!(validate_size(1000, 760).is_ok());
        assert!(validate_size(i32::MAX as u32, i32::MAX as u32).is_ok());
    }

    #[test]
    fn logical_grip_dimensions_are_constrained_before_windows_dpi_conversion() {
        let minimum = crate::editor::MIN_EDITOR_SIZE;
        for scale in [1.0, 1.25, 1.5, 2.0] {
            assert_eq!(super::logical_resize_size(1000, 764, scale, false), Some((1000, 764)));
            assert_eq!(
                super::logical_resize_size(1000, 764, scale, true),
                Some(((1000.0 * scale).round() as u32, (764.0 * scale).round() as u32))
            );
            assert_eq!(
                super::logical_resize_size(1, 1, scale, true),
                Some((
                    (f64::from(minimum.0) * scale).round() as u32,
                    (f64::from(minimum.1) * scale).round() as u32
                ))
            );
        }
        assert_eq!(
            super::logical_resize_size(u32::MAX, u32::MAX, 2.0, true),
            Some((8192, 8192))
        );
        for scale in [0.0, -1.0, f64::NAN, f64::INFINITY, f64::MAX] {
            assert!(super::logical_resize_size(1000, 764, scale, true).is_none());
        }
        assert!(super::logical_resize_size(0, 764, 1.0, false).is_none());
    }

    #[test]
    fn ui_binding_transport_checks_thread_without_an_unsafe_send_implementation() {
        let binding = std::rc::Rc::new(std::cell::Cell::new(42));
        let transport = fragile::Fragile::new(std::rc::Rc::clone(&binding));
        let transport = std::thread::spawn(move || {
            assert!(transport.try_get().is_err());
            transport
        })
        .join()
        .expect("bounded thread-affinity check");
        assert_eq!(transport.into_inner().get(), 42);
        assert_eq!(std::rc::Rc::strong_count(&binding), 1);
    }
}
