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
//! so the owner can capture the plugin's settings before it drops the editor.
//! An editor keeps the parts of its plugin instance alive, so the view never outlives its controller.

#![expect(unsafe_code, reason = "calls the IPlugView COM interface of plugin editors")]

#[cfg(target_os = "macos")]
mod macos;
mod plug_frame;
#[cfg(not(any(target_os = "macos", windows)))]
mod unsupported;
#[cfg(windows)]
mod windows;

use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;

use anyhow::{Context as _, Result, bail};
use raw_window_handle::RawWindowHandle;
use vst3::Steinberg::Vst::IEditControllerTrait as _;
use vst3::Steinberg::Vst::ViewType::kEditor;
use vst3::Steinberg::{IPlugFrame, IPlugView, IPlugViewContentScaleSupport, IPlugViewTrait as _, ViewRect};
use vst3::Steinberg::{IPlugViewContentScaleSupportTrait as _, kResultOk, kResultTrue};
use vst3::{ComPtr, ComWrapper};

use crate::instance::{InstanceCore, PluginInstance};
use crate::thread::UiThread;
#[cfg(target_os = "macos")]
use macos::NativeWindow;
use plug_frame::PlugFrame;
#[cfg(not(any(target_os = "macos", windows)))]
use unsupported::NativeWindow;
#[cfg(windows)]
use windows::NativeWindow;

/// Size of the window for an editor that does not report its size.
const FALLBACK_SIZE: ViewSize = ViewSize {
    width: 640,
    height: 480,
};

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

/// The width and height of an editor view, in the units of the platform.
/// These are points on macOS and physical pixels on Windows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct ViewSize {
    width: i32,
    height: i32,
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

impl EditorWindow {
    /// Opens the editor of a plugin instance in a new window and shows it.
    /// On Windows, the window is owned by `owner`, so it stays above it and minimizes with it.
    /// On macOS, the window is independent, because a child window would move together with the main window.
    ///
    /// # Errors
    ///
    /// Returns an error when the plugin has no editor, the editor does not support the platform's windows,
    /// the window cannot be created, or the editor refuses to attach to it.
    pub fn open(ui: &UiThread, instance: &PluginInstance, title: &str, owner: Option<RawWindowHandle>) -> Result<Self> {
        let core = Arc::clone(instance.core());
        let controller = core.controller().context("The plugin has no editor")?;
        // SAFETY: the controller is valid, and the view type is a null-terminated constant.
        let view = unsafe { ComPtr::from_raw(controller.createView(kEditor)) }.context("The plugin has no editor")?;
        // SAFETY: the view is valid, and the platform type is a null-terminated constant.
        if unsafe { view.isPlatformTypeSupported(NativeWindow::PLATFORM_TYPE) } != kResultTrue {
            bail!("The plugin's editor does not support the windows of this platform");
        }
        let shared = Rc::new(EditorShared {
            view,
            attached: Cell::new(false),
            closed: Cell::new(false),
            resizing_for_plugin: Cell::new(false),
        });
        let size = shared.size().unwrap_or(FALLBACK_SIZE);
        let window = Rc::new(NativeWindow::create(
            ui,
            &shared,
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
        editor.attach()?;
        Ok(editor)
    }

    /// Returns true when the user closed the window.
    #[must_use]
    pub fn is_closed(&self) -> bool {
        self.shared.closed.get()
    }

    /// Shows the window again after it was closed, and brings it to the front.
    pub fn show(&self, _ui: &UiThread) {
        self.shared.closed.set(false);
        self.window.show();
    }

    /// Gives the view its frame and window, sizes the window to the view, and shows it.
    fn attach(&self) -> Result<()> {
        let frame = self
            .frame
            .to_com_ptr::<IPlugFrame>()
            .context("The plug frame does not implement IPlugFrame")?;
        // SAFETY: the view and the frame are valid, and the view takes its own reference to the frame.
        unsafe { self.shared.view.setFrame(frame.as_ptr()) };
        if let Some(scale) = self.window.content_scale() {
            self.shared.set_content_scale(scale);
        }
        // SAFETY: the view is valid, the parent stays valid until the view is removed,
        // and the platform type is a null-terminated constant.
        if unsafe {
            self.shared
                .view
                .attached(self.window.parent(), NativeWindow::PLATFORM_TYPE)
        } != kResultOk
        {
            bail!("The plugin's editor could not be attached to its window");
        }
        self.shared.attached.set(true);
        if let Some(size) = self.shared.size() {
            self.shared.resizing_for_plugin.set(true);
            self.window.set_content_size(size);
            self.shared.resizing_for_plugin.set(false);
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

impl ViewSize {
    /// Returns the size of a view rectangle, treating a negative extent as 0.
    const fn from_rect(rect: &ViewRect) -> Self {
        let width = rect.right.saturating_sub(rect.left);
        let height = rect.bottom.saturating_sub(rect.top);
        Self {
            width: if width > 0 { width } else { 0 },
            height: if height > 0 { height } else { 0 },
        }
    }

    /// Returns a view rectangle with this size at the origin.
    const fn rect(self) -> ViewRect {
        ViewRect {
            left: 0,
            top: 0,
            right: self.width,
            bottom: self.height,
        }
    }
}

impl EditorShared {
    /// Returns the size the view asks for, or none when it does not report one.
    fn size(&self) -> Option<ViewSize> {
        let mut rect = ViewSize { width: 0, height: 0 }.rect();
        // SAFETY: the view is valid, and the rectangle lives for the call.
        let result = unsafe { self.view.getSize(&raw mut rect) };
        let size = ViewSize::from_rect(&rect);
        (result == kResultOk && size.width > 0 && size.height > 0).then_some(size)
    }

    /// Returns true when the view can be resized by the user.
    fn can_resize(&self) -> bool {
        // SAFETY: the view is valid.
        unsafe { self.view.canResize() == kResultTrue }
    }

    /// Returns the nearest size to `size` that the view accepts, or `size` when the view does not say.
    fn constrain(&self, size: ViewSize) -> ViewSize {
        let mut rect = size.rect();
        // SAFETY: the view is valid, and the rectangle lives for the call.
        let result = unsafe { self.view.checkSizeConstraint(&raw mut rect) };
        let constrained = ViewSize::from_rect(&rect);
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
        let mut rect = size.rect();
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
            ViewSize::from_rect(&rect),
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
        assert_eq!(ViewSize::from_rect(&inverted), ViewSize { width: 0, height: 0 });
        let size = ViewSize {
            width: 640,
            height: 480,
        };
        assert_eq!(ViewSize::from_rect(&size.rect()), size);
    }

    #[test]
    fn a_plugin_without_a_view_has_no_editor() {
        let error = on_ui(|ui| {
            let instance = active_instance(ui, Kind::Gain, 2).map_err(|error| error.to_string())?;
            EditorWindow::open(ui, &instance, "Gain", None)
                .map(|_| ())
                .map_err(|error| error.to_string())
        });
        assert_eq!(error, Err(String::from("The plugin has no editor")));
    }
}
