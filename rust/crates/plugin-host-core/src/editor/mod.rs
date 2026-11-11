//! Native top-level editor windows with format-neutral lifecycle callbacks.
//!
//! Windows hide on close, allowing the host to capture plugin state before dropping the editor.
//! Window operations and callbacks run on the registered UI thread.
//! `AppKit` additionally requires that thread to be the process main thread.

#[cfg(target_os = "macos")]
mod macos;
#[cfg(not(any(target_os = "macos", windows)))]
mod unsupported;
#[cfg(windows)]
mod windows;

use thiserror::Error;

#[cfg(target_os = "macos")]
pub use macos::NativeWindow;
#[cfg(not(any(target_os = "macos", windows)))]
pub use unsupported::NativeWindow;
#[cfg(windows)]
pub use windows::{NativeWindow, WindowObservation};

/// Why a native editor window could not be created.
#[derive(Debug, Error)]
pub enum WindowError {
    /// `AppKit` requires editor windows to be opened on the main thread.
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

/// Width and height of the content area, in points on macOS and physical pixels on Windows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WindowSize {
    /// Width of the editor content.
    pub width: i32,
    /// Height of the editor content.
    pub height: i32,
}

/// Top-left corner of the window frame, suitable for reopening the editor at the same position.
///
/// Coordinates are physical pixels from the virtual screen's top left on Windows,
/// and points from the main screen's bottom left on macOS.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WindowPosition {
    /// Horizontal coordinate of the top-left corner.
    pub x: i32,
    /// Vertical coordinate of the top-left corner.
    pub y: i32,
}

/// Receives native editor events on the UI thread without depending on a plugin format.
///
/// The host owns the callbacks, and the native window holds only a weak reference.
/// Implementations must not unwind through native event handlers.
pub trait EditorCallbacks {
    /// Returns the nearest accepted positive content size for a user resize.
    fn constrain(&self, size: WindowSize) -> WindowSize;

    /// Reports a new content size, including programmatic native resizes.
    /// The host suppresses plugin notifications while applying a plugin-requested size.
    fn resized(&self, size: WindowSize);

    /// Reports a new number of physical pixels per logical pixel.
    /// `AppKit` performs its own scaling, so this callback is only needed on Windows.
    fn scale_changed(&self, factor: f32);

    /// Reports a user close request after the native window has been hidden.
    fn close_requested(&self);
}
