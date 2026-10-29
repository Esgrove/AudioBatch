//! The `NSWindow` that shows a plugin editor on macOS.
//!
//! The editor attaches its own `NSView` to the window's content view.
//! A window delegate hides the window when the user closes it,
//! checks the user's resizes with the view, and reports the new size to the view.

#![expect(unsafe_code, reason = "creates an AppKit window and a delegate class for it")]

use std::ffi::c_void;
use std::rc::{Rc, Weak};

use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2::{DefinedClass as _, MainThreadMarker, MainThreadOnly, define_class, msg_send};
use objc2_app_kit::{NSBackingStoreType, NSView, NSWindow, NSWindowDelegate, NSWindowStyleMask};
use objc2_foundation::{NSNotification, NSObject, NSObjectProtocol, NSPoint, NSRect, NSSize, NSString};
use raw_window_handle::RawWindowHandle;
use vst3::Steinberg::{FIDString, kPlatformTypeNSView};

use super::{EditorError, EditorShared, ViewSize};
use crate::thread::UiThread;

define_class!(
    // SAFETY: `NSObject` has no subclassing requirements, and the delegate does not implement `Drop`.
    #[unsafe(super(NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = "AudioBatchPluginEditorDelegate"]
    #[ivars = Weak<EditorShared>]
    struct WindowDelegate;

    unsafe impl NSObjectProtocol for WindowDelegate {}

    unsafe impl NSWindowDelegate for WindowDelegate {
        #[unsafe(method(windowShouldClose:))]
        fn window_should_close(&self, sender: &NSWindow) -> bool {
            sender.orderOut(None);
            if let Some(shared) = self.ivars().upgrade() {
                shared.close_requested();
            }
            false
        }

        #[unsafe(method(windowWillResize:toSize:))]
        fn window_will_resize(&self, sender: &NSWindow, frame_size: NSSize) -> NSSize {
            let Some(shared) = self.ivars().upgrade() else {
                return frame_size;
            };
            let content = sender.contentRectForFrameRect(NSRect::new(sender.frame().origin, frame_size));
            let constrained = shared.constrain(view_size(content.size));
            sender
                .frameRectForContentRect(NSRect::new(content.origin, points(constrained)))
                .size
        }

        #[unsafe(method(windowDidResize:))]
        fn window_did_resize(&self, notification: &NSNotification) {
            let window = notification.object().and_then(|object| object.downcast::<NSWindow>().ok());
            if let (Some(shared), Some(window)) = (self.ivars().upgrade(), window) {
                shared.user_resized(view_size(window.contentRectForFrameRect(window.frame()).size));
            }
        }
    }
);

/// The window of one editor.
pub struct NativeWindow {
    /// The window, which is not released when it closes, because this owns it.
    window: Retained<NSWindow>,
    /// The window's delegate. The window only holds a weak reference to it.
    _delegate: Retained<WindowDelegate>,
}

impl WindowDelegate {
    /// Creates a delegate for the window of a view.
    fn new(marker: MainThreadMarker, shared: Weak<EditorShared>) -> Retained<Self> {
        let this = Self::alloc(marker).set_ivars(shared);
        // SAFETY: `NSObject`'s `init` has no requirements.
        unsafe { msg_send![super(this), init] }
    }
}

impl NativeWindow {
    /// Editors attach to an `NSView`.
    pub const PLATFORM_TYPE: FIDString = kPlatformTypeNSView;

    /// Creates a hidden window with a content view of `size` points in the middle of the screen.
    /// The owner is not used, because a child window would move together with the main window.
    pub fn create(
        _ui: &UiThread,
        shared: &Rc<EditorShared>,
        title: &str,
        size: ViewSize,
        resizable: bool,
        _owner: Option<RawWindowHandle>,
    ) -> Result<Self, EditorError> {
        let marker = MainThreadMarker::new().ok_or(EditorError::MainThreadRequired)?;
        let mut style = NSWindowStyleMask::Titled | NSWindowStyleMask::Closable | NSWindowStyleMask::Miniaturizable;
        if resizable {
            style |= NSWindowStyleMask::Resizable;
        }
        let content_rect = NSRect::new(NSPoint::new(0.0, 0.0), points(size));
        // SAFETY: the rectangle and the style are valid, and the window is created on the main thread.
        let window = unsafe {
            NSWindow::initWithContentRect_styleMask_backing_defer(
                NSWindow::alloc(marker),
                content_rect,
                style,
                NSBackingStoreType::Buffered,
                false,
            )
        };
        // SAFETY: the window is owned by this value, so closing it must not release it.
        unsafe { window.setReleasedWhenClosed(false) };
        window.setTitle(&NSString::from_str(title));
        let content = NSView::initWithFrame(NSView::alloc(marker), content_rect);
        window.setContentView(Some(&content));
        let delegate = WindowDelegate::new(marker, Rc::downgrade(shared));
        window.setDelegate(Some(ProtocolObject::from_ref(&*delegate)));
        window.center();
        Ok(Self {
            window,
            _delegate: delegate,
        })
    }

    /// Returns the content view the editor attaches to.
    pub fn parent(&self) -> *mut c_void {
        self.window
            .contentView()
            .map_or(std::ptr::null_mut(), |view| Retained::as_ptr(&view).cast_mut().cast())
    }

    /// Resizes the window so its content is `size` points.
    pub fn set_content_size(&self, size: ViewSize) {
        self.window.setContentSize(points(size));
    }

    /// Returns none, because `AppKit` scales views for the display itself.
    #[expect(
        clippy::unused_self,
        reason = "matches the Windows window, which reads its own display scale"
    )]
    pub const fn content_scale(&self) -> Option<f32> {
        None
    }

    /// Shows the window in front of the others and gives it the keyboard focus.
    pub fn show(&self) {
        self.window.makeKeyAndOrderFront(None);
    }
}

impl Drop for NativeWindow {
    fn drop(&mut self) {
        self.window.setDelegate(None);
        self.window.orderOut(None);
        self.window.close();
    }
}

/// Returns a size in points as an `AppKit` size.
fn points(size: ViewSize) -> NSSize {
    NSSize::new(f64::from(size.width), f64::from(size.height))
}

/// Returns an `AppKit` size as a view size in whole points.
#[expect(
    clippy::cast_possible_truncation,
    reason = "window sizes in points are far inside the range of i32"
)]
const fn view_size(size: NSSize) -> ViewSize {
    ViewSize {
        width: size.width.round() as i32,
        height: size.height.round() as i32,
    }
}
