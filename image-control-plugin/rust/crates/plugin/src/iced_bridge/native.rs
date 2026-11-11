//! Native parent validation and raw-window-handle adaptation at the host's borrowed-pointer boundary.

use std::ffi::c_void;
use std::ptr::NonNull;

use raw_window_handle::{HandleError, HasWindowHandle, RawWindowHandle, WindowHandle};

#[cfg(target_os = "windows")]
use windows_sys::Win32::System::Threading::GetCurrentThreadId;
#[cfg(target_os = "windows")]
use windows_sys::Win32::UI::WindowsAndMessaging::GetWindowThreadProcessId;

use super::EditorError;

/// A borrowed host parent, used only synchronously while baseview retains its platform parent handle.
/// The unsafe attachment caller guarantees the native object's type and lifetime.
pub(super) struct NativeParent {
    /// Non-null `NSView` or `HWND`, valid on its owning UI thread throughout attachment.
    pointer: NonNull<c_void>,
}

impl NativeParent {
    /// Rejects null parents and off-thread attachment before baseview can touch native object APIs.
    ///
    /// # Safety
    /// The non-null pointer must identify a live `NSView` on macOS or `HWND` on Windows.
    /// It must remain alive until the editor and all baseview-owned callbacks are destroyed.
    #[expect(
        unsafe_code,
        reason = "Native parent validation is the narrow borrowed host-pointer boundary"
    )]
    pub(super) unsafe fn new(pointer: *mut c_void) -> Result<Self, EditorError> {
        let pointer = NonNull::new(pointer).ok_or(EditorError::InvalidParent)?;
        #[cfg(target_os = "macos")]
        if objc2::MainThreadMarker::new().is_none() {
            return Err(EditorError::WrongThread);
        }
        #[cfg(target_os = "windows")]
        {
            // SAFETY: The caller supplies a valid HWND. Null asks not to retrieve its process id.
            let parent_thread = unsafe { GetWindowThreadProcessId(pointer.as_ptr(), std::ptr::null_mut()) };
            if parent_thread == 0 {
                return Err(EditorError::InvalidParent);
            }
            // SAFETY: GetCurrentThreadId has no pointer arguments or initialization preconditions.
            if parent_thread != unsafe { GetCurrentThreadId() } {
                return Err(EditorError::WrongThread);
            }
        }
        Ok(Self { pointer })
    }
}

impl HasWindowHandle for NativeParent {
    /// Borrows the native handle only while the checked host-parent adapter remains in scope.
    #[expect(
        unsafe_code,
        reason = "The attachment caller guarantees the native pointer's type and lifetime"
    )]
    fn window_handle(&self) -> Result<WindowHandle<'_>, HandleError> {
        #[cfg(target_os = "macos")]
        let raw = RawWindowHandle::AppKit(raw_window_handle::AppKitWindowHandle::new(self.pointer));
        #[cfg(target_os = "windows")]
        let raw = {
            let handle = std::num::NonZeroIsize::new(self.pointer.as_ptr() as isize).ok_or(HandleError::Unavailable)?;
            RawWindowHandle::Win32(raw_window_handle::Win32WindowHandle::new(handle))
        };
        // SAFETY: NativeParent exists only inside attach,
        // whose caller guarantees this native object remains live through the editor's destruction.
        // This borrow is shorter than that lifetime.
        Ok(unsafe { WindowHandle::borrow_raw(raw) })
    }
}

#[cfg(test)]
mod tests {
    use super::{EditorError, NativeParent};

    #[test]
    #[expect(unsafe_code, reason = "A null pointer is rejected before any native API is called")]
    fn null_parent_never_reaches_native_object_apis() {
        // SAFETY: This is the explicitly rejected null case, so no object-lifetime requirement applies.
        let result = unsafe { NativeParent::new(std::ptr::null_mut()) };
        assert!(matches!(result, Err(EditorError::InvalidParent)));
    }
}
