//! The editor window on platforms without one, which always fails to open.
//!
//! Plugin editors are only supported on macOS and Windows.

use std::ffi::c_void;
use std::rc::Rc;

use raw_window_handle::RawWindowHandle;
use vst3::Steinberg::{FIDString, kPlatformTypeX11EmbedWindowID};

use super::{EditorError, EditorShared, ViewSize};
use crate::thread::UiThread;

/// A window that cannot be created.
pub enum NativeWindow {}

impl NativeWindow {
    /// The platform type editors are asked about before the window fails to open.
    pub const PLATFORM_TYPE: FIDString = kPlatformTypeX11EmbedWindowID;

    /// Returns an error, because this platform has no editor windows.
    pub fn create(
        _ui: &UiThread,
        _shared: &Rc<EditorShared>,
        _title: &str,
        _size: ViewSize,
        _resizable: bool,
        _owner: Option<RawWindowHandle>,
    ) -> Result<Self, EditorError> {
        Err(EditorError::UnsupportedPlatform)
    }

    /// Never called, because the window cannot exist.
    pub const fn parent(&self) -> *mut c_void {
        match *self {}
    }

    /// Never called, because the window cannot exist.
    pub const fn set_content_size(&self, _size: ViewSize) {
        match *self {}
    }

    /// Never called, because the window cannot exist.
    pub const fn content_scale(&self) -> Option<f32> {
        match *self {}
    }

    /// Never called, because the window cannot exist.
    pub const fn show(&self) {
        match *self {}
    }
}
