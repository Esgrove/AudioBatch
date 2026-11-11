//! Native windows that show the editor of a plugin.
//!
//! A plugin draws its editor into a view or window that the host provides,
//! so each editor gets its own top-level window, an `NSWindow` on macOS and a Win32 window on Windows.
//! These are native windows rather than windows of the application's GUI framework,
//! because gpui draws its Windows content on top of child windows, which would hide the plugin's own window.
//! Sizes follow requests from both sides.
//! The plugin asks for a new size through `IPlugFrame::resizeView`,
//! and a resize by the user is checked with `checkSizeConstraint` and reported with `onSize`.
//! Closing the window only hides it and marks it closed,
//! so the owner can capture the plugin's settings and the window's position before it drops the editor.
//! A window opened at a remembered position is kept on a connected display.
//! An editor keeps the parts of its plugin instance alive, so the view never outlives its controller.

#![expect(unsafe_code, reason = "calls the IPlugView COM interface of plugin editors")]

mod plug_frame;

use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;

#[cfg(windows)]
pub use plugin_host_core::editor::WindowObservation;
pub use plugin_host_core::editor::WindowPosition;
use plugin_host_core::editor::{EditorCallbacks, NativeWindow, WindowError, WindowSize as ViewSize};
use raw_window_handle::RawWindowHandle;
use thiserror::Error;
use vst3::Steinberg::Vst::IEditControllerTrait as _;
use vst3::Steinberg::Vst::ViewType::kEditor;
use vst3::Steinberg::{IPlugFrame, IPlugView, IPlugViewContentScaleSupport, IPlugViewTrait as _, ViewRect};
use vst3::Steinberg::{IPlugViewContentScaleSupportTrait as _, kResultOk, kResultTrue};
use vst3::{ComPtr, ComWrapper};

use crate::instance::{InstanceCore, PluginInstance};
use crate::thread::UiThread;
use plug_frame::PlugFrame;

/// Native parent type expected by a VST3 editor.
#[cfg(target_os = "macos")]
const PLATFORM_TYPE: vst3::Steinberg::FIDString = vst3::Steinberg::kPlatformTypeNSView;

/// Native parent type expected by a VST3 editor.
#[cfg(windows)]
const PLATFORM_TYPE: vst3::Steinberg::FIDString = vst3::Steinberg::kPlatformTypeHWND;

/// No editor parent type exists on unsupported platforms.
#[cfg(not(any(target_os = "macos", windows)))]
const PLATFORM_TYPE: vst3::Steinberg::FIDString = c"unsupported".as_ptr();

/// Size of the window for an editor that does not report its size.
const FALLBACK_SIZE: ViewSize = ViewSize {
    width: 640,
    height: 480,
};

/// Why a plugin editor could not be opened.
#[derive(Debug, Error)]
pub enum EditorError {
    /// The controller or its editor view is absent.
    #[error("The plugin has no editor")]
    MissingEditor,
    /// The view does not support the native window type.
    #[error("The plugin's editor does not support the windows of this platform")]
    UnsupportedWindowType {
        /// The result returned by `isPlatformTypeSupported`.
        result: i32,
    },
    /// The host frame lacks its required interface.
    #[error("The plug frame does not implement IPlugFrame")]
    MissingPlugFrame,
    /// The view refused to attach.
    #[error("The plugin's editor could not be attached to its window")]
    Attach {
        /// The result returned by `attached`.
        result: i32,
    },
    /// `AppKit` windows must be opened on the main thread.
    #[error("Plugin editors can only be opened on the main thread")]
    MainThreadRequired,
    /// The operating system could not create the window.
    #[error("The editor window could not be created: {source}")]
    CreateWindow {
        /// The native window creation failure.
        #[source]
        source: std::io::Error,
    },
    /// The operating system could not register the window class.
    #[error("The editor window class could not be registered: {source}")]
    RegisterWindowClass {
        /// The native registration failure.
        #[source]
        source: std::io::Error,
    },
    /// This platform has no native editor implementation.
    #[error("Plugin editors are only supported on macOS and Windows")]
    UnsupportedPlatform,
}

/// A native window showing the editor of a plugin.
/// It is not `Send`, so it stays on the UI thread that opened it and is destroyed there.
pub struct EditorWindow {
    /// The native window. Dropping it destroys the window.
    window: Rc<NativeWindow>,
    /// The frame the view reports its size requests to.
    frame: ComWrapper<PlugFrame>,
    /// The view and its flags.
    shared: Rc<EditorShared>,
    /// Keeps the controller that created the view alive until the view is released.
    _core: Arc<InstanceCore>,
}

/// The state that the window, its event handlers, and the plug frame share.
struct EditorShared {
    /// The plugin's editor view.
    view: ComPtr<IPlugView>,
    /// Whether the view is attached to the window, so it may be told about size and scale changes.
    attached: Cell<bool>,
    /// Whether the user closed the window, which then only hides.
    closed: Cell<bool>,
    /// Whether the window is being resized because the plugin asked for it,
    /// so the resize is not reported back to the plugin.
    resizing_for_plugin: Cell<bool>,
}

impl From<WindowError> for EditorError {
    fn from(error: WindowError) -> Self {
        match error {
            WindowError::MainThreadRequired => Self::MainThreadRequired,
            WindowError::CreateWindow { source } => Self::CreateWindow { source },
            WindowError::RegisterWindowClass { source } => Self::RegisterWindowClass { source },
            WindowError::UnsupportedPlatform => Self::UnsupportedPlatform,
        }
    }
}

impl EditorWindow {
    /// Opens the editor of a plugin instance in a new window and shows it.
    /// On Windows, the window is owned by `owner`, so it stays above it and minimizes with it.
    /// On macOS, the window is independent, because a child window would move together with the main window.
    /// With a `position`, the window opens there, moved onto a display when that place is no longer on one.
    /// Without one, the platform places it.
    ///
    /// # Errors
    ///
    /// Returns an error when the plugin has no editor, the editor does not support the platform's windows,
    /// the window cannot be created, or the editor refuses to attach to it.
    pub fn open(
        ui: &UiThread,
        instance: &PluginInstance,
        title: &str,
        owner: Option<RawWindowHandle>,
        position: Option<WindowPosition>,
    ) -> Result<Self, EditorError> {
        let core = Arc::clone(instance.core());
        let controller = core.controller().ok_or(EditorError::MissingEditor)?;
        // SAFETY: the controller is valid, and the view type is a null-terminated constant.
        let view = unsafe { ComPtr::from_raw(controller.createView(kEditor)) }.ok_or(EditorError::MissingEditor)?;
        // SAFETY: the view is valid, and the platform type is a null-terminated constant.
        let result = unsafe { view.isPlatformTypeSupported(PLATFORM_TYPE) };
        if result != kResultTrue {
            return Err(EditorError::UnsupportedWindowType { result });
        }
        let shared = Rc::new(EditorShared {
            view,
            attached: Cell::new(false),
            closed: Cell::new(false),
            resizing_for_plugin: Cell::new(false),
        });
        let size = shared.size().unwrap_or(FALLBACK_SIZE);
        let callbacks: Rc<dyn EditorCallbacks> = shared.clone();
        let window = Rc::new(NativeWindow::create(
            ui,
            &callbacks,
            title,
            size,
            shared.can_resize(),
            owner,
        )?);
        let frame = ComWrapper::new(PlugFrame::new(Rc::downgrade(&shared), Rc::downgrade(&window)));
        let editor = Self {
            window,
            frame,
            shared,
            _core: core,
        };
        editor.attach(position)?;
        Ok(editor)
    }

    /// Returns true when the user closed the window.
    #[must_use]
    pub fn is_closed(&self) -> bool {
        self.shared.closed.get()
    }

    /// Returns where the window is on screen, also after the user closed it,
    /// or none when the platform cannot tell, such as while the window is minimized.
    #[must_use]
    pub fn position(&self) -> Option<WindowPosition> {
        self.window.position()
    }

    /// Reads the owned Win32 client's geometry, DPI, and visibility on its UI thread.
    ///
    /// # Errors
    ///
    /// Returns an error when the native window's client area or DPI cannot be queried.
    #[cfg(windows)]
    pub fn native_observation(&self, _ui: &UiThread) -> std::io::Result<WindowObservation> {
        self.window.observation()
    }

    /// Shows the window again after it was closed, and brings it to the front.
    pub fn show(&self, _ui: &UiThread) {
        self.shared.closed.set(false);
        self.window.show();
    }

    /// Gives the view its frame and window, sizes the window to the view,
    /// moves it to `position` when there is one, and shows it.
    /// The move comes after the final size, so keeping the window on a display accounts for its whole frame.
    fn attach(&self, position: Option<WindowPosition>) -> Result<(), EditorError> {
        let frame = self
            .frame
            .to_com_ptr::<IPlugFrame>()
            .ok_or(EditorError::MissingPlugFrame)?;
        // SAFETY: the view and the frame are valid, and the view takes its own reference to the frame.
        unsafe { self.shared.view.setFrame(frame.as_ptr()) };
        if let Some(scale) = self.window.content_scale() {
            self.shared.set_content_scale(scale);
        }
        // SAFETY: the view is valid, the parent stays valid until the view is removed,
        // and the platform type is a null-terminated constant.
        let result = unsafe { self.shared.view.attached(self.window.parent(), PLATFORM_TYPE) };
        if result != kResultOk {
            return Err(EditorError::Attach { result });
        }
        self.shared.attached.set(true);
        if let Some(size) = self.shared.size() {
            self.shared.resizing_for_plugin.set(true);
            self.window.set_content_size(size);
            self.shared.resizing_for_plugin.set(false);
        }
        if let Some(position) = position {
            self.window.move_to(position);
        }
        self.window.show();
        Ok(())
    }
}

impl Drop for EditorWindow {
    fn drop(&mut self) {
        // SAFETY: the view is valid, `removed` is only called after a successful `attached`,
        // and clearing the frame makes the view release its reference to it.
        unsafe {
            if self.shared.attached.replace(false) {
                self.shared.view.removed();
            }
            self.shared.view.setFrame(std::ptr::null_mut());
        }
    }
}

impl std::fmt::Debug for EditorWindow {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("EditorWindow")
            .field("closed", &self.shared.closed.get())
            .finish_non_exhaustive()
    }
}

impl EditorShared {
    /// Returns the size the view asks for, or none when it does not report one.
    fn size(&self) -> Option<ViewSize> {
        let mut rect = view_rect(ViewSize { width: 0, height: 0 });
        // SAFETY: the view is valid, and the rectangle lives for the call.
        let result = unsafe { self.view.getSize(&raw mut rect) };
        let size = view_size(&rect);
        (result == kResultOk && size.width > 0 && size.height > 0).then_some(size)
    }

    /// Returns true when the view can be resized by the user.
    fn can_resize(&self) -> bool {
        // SAFETY: the view is valid.
        unsafe { self.view.canResize() == kResultTrue }
    }

    /// Returns the nearest size to `size` that the view accepts, or `size` when the view does not say.
    fn constrain(&self, size: ViewSize) -> ViewSize {
        let mut rect = view_rect(size);
        // SAFETY: the view is valid, and the rectangle lives for the call.
        let result = unsafe { self.view.checkSizeConstraint(&raw mut rect) };
        let constrained = view_size(&rect);
        if result == kResultTrue && constrained.width > 0 && constrained.height > 0 {
            constrained
        } else {
            size
        }
    }

    /// Tells the view that the user resized the window, unless the plugin asked for the resize itself.
    fn user_resized(&self, size: ViewSize) {
        if self.attached.get() && !self.resizing_for_plugin.get() {
            self.tell_size(size);
        }
    }

    /// Tells the view its new size.
    fn tell_size(&self, size: ViewSize) {
        let mut rect = view_rect(size);
        // SAFETY: the view is valid, and the rectangle lives for the call.
        unsafe { self.view.onSize(&raw mut rect) };
    }

    /// Tells a view that supports it how many physical pixels make one logical pixel.
    fn set_content_scale(&self, factor: f32) {
        if let Some(scale_support) = self.view.cast::<IPlugViewContentScaleSupport>() {
            // SAFETY: the interface pointer is valid for the call.
            unsafe { scale_support.setContentScaleFactor(factor) };
        }
    }

    /// Marks the window closed after the user closed it.
    fn close_requested(&self) {
        self.closed.set(true);
    }
}

impl EditorCallbacks for EditorShared {
    fn constrain(&self, size: ViewSize) -> ViewSize {
        self.constrain(size)
    }

    fn resized(&self, size: ViewSize) {
        self.user_resized(size);
    }

    fn scale_changed(&self, factor: f32) {
        self.set_content_scale(factor);
    }

    fn close_requested(&self) {
        self.close_requested();
    }
}

/// Returns the size of a VST3 rectangle, ignoring its origin and clamping negative extents to zero.
const fn view_size(rect: &ViewRect) -> ViewSize {
    let width = rect.right.saturating_sub(rect.left);
    let height = rect.bottom.saturating_sub(rect.top);
    ViewSize {
        width: if width > 0 { width } else { 0 },
        height: if height > 0 { height } else { 0 },
    }
}

/// Returns a VST3 view rectangle with this content size at the origin.
const fn view_rect(size: ViewSize) -> ViewRect {
    ViewRect {
        left: 0,
        top: 0,
        right: size.width,
        bottom: size.height,
    }
}

#[cfg(test)]
mod test_editor {
    use audiobatch_test_plugin::Kind;

    use super::*;
    use crate::test_helpers::{active_instance, on_ui};

    #[test]
    fn view_sizes_ignore_the_origin_and_negative_extents() {
        let rect = ViewRect {
            left: 10,
            top: 20,
            right: 410,
            bottom: 320,
        };
        assert_eq!(
            view_size(&rect),
            ViewSize {
                width: 400,
                height: 300
            }
        );
        let inverted = ViewRect {
            left: 50,
            top: 50,
            right: 10,
            bottom: 10,
        };
        assert_eq!(view_size(&inverted), ViewSize { width: 0, height: 0 });
        let size = ViewSize {
            width: 640,
            height: 480,
        };
        assert_eq!(view_size(&view_rect(size)), size);
    }

    #[test]
    fn native_window_errors_preserve_vst3_error_variants_and_sources() {
        let main_thread = EditorError::from(WindowError::MainThreadRequired);
        assert!(matches!(main_thread, EditorError::MainThreadRequired));
        let unsupported = EditorError::from(WindowError::UnsupportedPlatform);
        assert!(matches!(unsupported, EditorError::UnsupportedPlatform));
        let create = EditorError::from(WindowError::CreateWindow {
            source: std::io::Error::from_raw_os_error(5),
        });
        assert!(matches!(create, EditorError::CreateWindow { ref source } if source.raw_os_error() == Some(5)));
        let register = EditorError::from(WindowError::RegisterWindowClass {
            source: std::io::Error::from_raw_os_error(6),
        });
        assert!(
            matches!(register, EditorError::RegisterWindowClass { ref source } if source.raw_os_error() == Some(6))
        );
    }

    #[test]
    fn a_plugin_without_a_view_has_no_editor() {
        let error = on_ui(|ui| {
            let instance = active_instance(ui, Kind::Gain, 2).map_err(|error| error.to_string())?;
            EditorWindow::open(ui, &instance, "Gain", None, None)
                .map(|_| ())
                .map_err(|error| error.to_string())
        });
        assert_eq!(error, Err(String::from("The plugin has no editor")));
    }
}
