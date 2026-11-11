//! An uninhabited native window for platforms without an editor implementation.

use std::ffi::c_void;
use std::rc::Rc;

use raw_window_handle::RawWindowHandle;

use super::{EditorCallbacks, WindowError, WindowPosition, WindowSize};
use crate::thread::UiThread;

/// A window that cannot be created.
pub enum NativeWindow {}

impl NativeWindow {
    /// Rejects native window creation on unsupported platforms.
    ///
    /// # Errors
    ///
    /// Always returns `WindowError::UnsupportedPlatform`.
    pub fn create(
        _ui: &UiThread,
        _callbacks: &Rc<dyn EditorCallbacks>,
        _title: &str,
        _size: WindowSize,
        _resizable: bool,
        _owner: Option<RawWindowHandle>,
    ) -> Result<Self, WindowError> {
        Err(WindowError::UnsupportedPlatform)
    }

    /// Never called because the window cannot exist.
    #[must_use]
    pub const fn parent(&self) -> *mut c_void {
        match *self {}
    }

    /// Never called because the window cannot exist.
    #[must_use]
    pub const fn raw_window_handle(&self) -> RawWindowHandle {
        match *self {}
    }

    /// Never called because the window cannot exist.
    pub const fn set_content_size(&self, _size: WindowSize) {
        match *self {}
    }

    /// Never called because the window cannot exist.
    #[must_use]
    pub const fn content_scale(&self) -> Option<f32> {
        match *self {}
    }

    /// Never called because the window cannot exist.
    pub const fn show(&self) {
        match *self {}
    }

    /// Never called because the window cannot exist.
    pub const fn hide(&self) {
        match *self {}
    }

    /// Never called because the window cannot exist.
    #[must_use]
    pub const fn position(&self) -> Option<WindowPosition> {
        match *self {}
    }

    /// Never called because the window cannot exist.
    pub const fn move_to(&self, _position: WindowPosition) {
        match *self {}
    }
}
