//! `AppKit` editor windows with weak, format-neutral event callbacks.
//!
//! A delegate hides the window on close and negotiates user resizes.
//! Remembered window positions keep the title bar on a connected screen.

#![expect(unsafe_code, reason = "creates an `AppKit` window and a delegate class for it")]

use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::{Rc, Weak};

use objc2::rc::Retained;
use objc2::runtime::ProtocolObject;
use objc2::{DefinedClass as _, MainThreadMarker, MainThreadOnly, define_class, msg_send};
use objc2_app_kit::{NSBackingStoreType, NSView, NSWindow, NSWindowDelegate, NSWindowStyleMask};
use objc2_foundation::{NSNotification, NSObject, NSObjectProtocol, NSPoint, NSRect, NSSize, NSString};
use raw_window_handle::{AppKitWindowHandle, RawWindowHandle};

use super::{EditorCallbacks, WindowError, WindowPosition, WindowSize};
use crate::thread::UiThread;

/// Objective-C delegate name, distinct in unit tests that also link the ordinary library.
#[cfg(not(test))]
macro_rules! delegate_class_name {
    () => {
        "AudioBatchPluginEditorDelegate"
    };
}

/// Objective-C delegate name in the unit test binary.
#[cfg(test)]
macro_rules! delegate_class_name {
    () => {
        "AudioBatchPluginEditorTestDelegate"
    };
}

define_class!(
    // SAFETY: `NSObject` has no subclassing requirements, and the delegate does not implement `Drop`.
    #[unsafe(super(NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = delegate_class_name!()]
    #[ivars = Weak<dyn EditorCallbacks>]
    struct WindowDelegate;

    unsafe impl NSObjectProtocol for WindowDelegate {}

    unsafe impl NSWindowDelegate for WindowDelegate {
        #[unsafe(method(windowShouldClose:))]
        fn window_should_close(&self, sender: &NSWindow) -> bool {
            sender.orderOut(None);
            if let Some(callbacks) = self.ivars().upgrade() {
                callbacks.close_requested();
            }
            false
        }

        #[unsafe(method(windowWillResize:toSize:))]
        fn window_will_resize(&self, sender: &NSWindow, frame_size: NSSize) -> NSSize {
            let Some(callbacks) = self.ivars().upgrade() else {
                return frame_size;
            };
            let content = sender.contentRectForFrameRect(NSRect::new(sender.frame().origin, frame_size));
            let constrained = callbacks.constrain(window_size(content.size));
            sender
                .frameRectForContentRect(NSRect::new(content.origin, points(constrained)))
                .size
        }

        #[unsafe(method(windowDidResize:))]
        fn window_did_resize(&self, notification: &NSNotification) {
            let window = notification.object().and_then(|object| object.downcast::<NSWindow>().ok());
            if let (Some(callbacks), Some(window)) = (self.ivars().upgrade(), window) {
                callbacks.resized(window_size(window.contentRectForFrameRect(window.frame()).size));
            }
        }
    }
);

/// A native editor window that stays on the UI thread and hides on a user close request.
///
/// The content view stays alive until the window is dropped.
pub struct NativeWindow {
    /// The window, which is not released when it closes because this owns it.
    window: Retained<NSWindow>,
    /// The content view exposed to the plugin for the full lifetime of the native window.
    content: Retained<NSView>,
    /// The window holds only a weak reference to its delegate.
    _delegate: Retained<WindowDelegate>,
}

impl WindowDelegate {
    /// Creates a delegate that does not keep the host's callbacks alive.
    fn new(marker: MainThreadMarker, callbacks: Weak<dyn EditorCallbacks>) -> Retained<Self> {
        let this = Self::alloc(marker).set_ivars(callbacks);
        // SAFETY: `NSObject`'s `init` has no requirements.
        unsafe { msg_send![super(this), init] }
    }
}

impl NativeWindow {
    /// Creates a hidden window with a content area of `size` points in the middle of the screen.
    /// The owner is ignored because an `AppKit` child window would move with the main window.
    /// The host must retain `callbacks` while native events should be delivered.
    ///
    /// # Errors
    ///
    /// Returns `WindowError::MainThreadRequired` outside the process main thread.
    pub fn create(
        _ui: &UiThread,
        callbacks: &Rc<dyn EditorCallbacks>,
        title: &str,
        size: WindowSize,
        resizable: bool,
        _owner: Option<RawWindowHandle>,
    ) -> Result<Self, WindowError> {
        let marker = MainThreadMarker::new().ok_or(WindowError::MainThreadRequired)?;
        let mut style = NSWindowStyleMask::Titled | NSWindowStyleMask::Closable | NSWindowStyleMask::Miniaturizable;
        if resizable {
            style |= NSWindowStyleMask::Resizable;
        }
        let content_rect = NSRect::new(NSPoint::new(0.0, 0.0), points(size));
        // SAFETY: the rectangle and style are valid, and creation happens on the main thread.
        let window = unsafe {
            NSWindow::initWithContentRect_styleMask_backing_defer(
                NSWindow::alloc(marker),
                content_rect,
                style,
                NSBackingStoreType::Buffered,
                false,
            )
        };
        // SAFETY: this value owns the window, so closing it must not release it.
        unsafe { window.setReleasedWhenClosed(false) };
        window.setTitle(&NSString::from_str(title));
        let content = NSView::initWithFrame(NSView::alloc(marker), content_rect);
        window.setContentView(Some(&content));
        let delegate = WindowDelegate::new(marker, Rc::downgrade(callbacks));
        window.setDelegate(Some(ProtocolObject::from_ref(&*delegate)));
        window.center();
        Ok(Self {
            window,
            content,
            _delegate: delegate,
        })
    }

    /// Returns the content view the plugin attaches to, valid until this window is dropped.
    #[must_use]
    pub fn parent(&self) -> *mut c_void {
        Retained::as_ptr(&self.content).cast_mut().cast()
    }

    /// Returns the `NSView` handle used to embed the plugin editor.
    /// The handle is valid only while this window is alive and must only be used on its UI thread.
    #[must_use]
    pub fn raw_window_handle(&self) -> RawWindowHandle {
        let content = NonNull::from(&*self.content).cast();
        RawWindowHandle::AppKit(AppKitWindowHandle::new(content))
    }

    /// Resizes the content area to `size` points.
    pub fn set_content_size(&self, size: WindowSize) {
        self.window.setContentSize(points(size));
    }

    /// Returns none because `AppKit` scales views for the display itself.
    #[must_use]
    pub const fn content_scale(&self) -> Option<f32> {
        None
    }

    /// Shows the window in front of the others and gives it keyboard focus.
    pub fn show(&self) {
        self.window.makeKeyAndOrderFront(None);
    }

    /// Hides the native parent without destroying the plugin's embedded view.
    pub fn hide(&self) {
        self.window.orderOut(None);
    }

    /// Returns the frame's top-left corner in screen points, or none while minimized into the Dock.
    #[must_use]
    pub fn position(&self) -> Option<WindowPosition> {
        if self.window.isMiniaturized() {
            return None;
        }
        let frame = self.window.frame();
        Some(WindowPosition {
            x: whole_points(frame.origin.x),
            y: whole_points(frame.origin.y + frame.size.height),
        })
    }

    /// Moves the frame's top-left corner to `position`, keeping its title bar on a screen.
    /// A position on no connected screen centers the window on the main screen instead.
    pub fn move_to(&self, position: WindowPosition) {
        self.window
            .setFrameTopLeftPoint(NSPoint::new(f64::from(position.x), f64::from(position.y)));
        if let Some(screen) = self.window.screen() {
            let constrained = self
                .window
                .constrainFrameRect_toScreen(self.window.frame(), Some(&screen));
            self.window.setFrame_display(constrained, false);
        } else {
            self.window.center();
        }
    }
}

impl Drop for NativeWindow {
    fn drop(&mut self) {
        self.window.setDelegate(None);
        self.window.orderOut(None);
        self.window.close();
    }
}

/// Returns a content size as an `AppKit` size in points.
fn points(size: WindowSize) -> NSSize {
    NSSize::new(f64::from(size.width), f64::from(size.height))
}

/// Returns a screen coordinate in whole points.
#[expect(clippy::cast_possible_truncation, reason = "screen coordinates fit in i32")]
const fn whole_points(value: f64) -> i32 {
    value.round() as i32
}

/// Returns an `AppKit` size in whole points.
#[expect(clippy::cast_possible_truncation, reason = "window dimensions fit in i32")]
const fn window_size(size: NSSize) -> WindowSize {
    WindowSize {
        width: size.width.round() as i32,
        height: size.height.round() as i32,
    }
}
