//! Native parent validation and raw-window-handle adaptation at the host's borrowed-pointer boundary.

use std::ffi::c_void;
use std::ptr::NonNull;

use raw_window_handle::{HandleError, HasWindowHandle, RawWindowHandle, WindowHandle};

#[cfg(target_os = "windows")]
use windows_sys::Win32::System::Threading::GetCurrentThreadId;
#[cfg(target_os = "windows")]
use windows_sys::Win32::UI::WindowsAndMessaging::GetWindowThreadProcessId;

use super::EditorError;

/// Dots per inch of a Windows display at 100 percent scale.
#[cfg(target_os = "windows")]
const BASE_DPI: f64 = 96.0;

/// A borrowed host parent, used only synchronously while baseview retains its platform parent handle.
/// The unsafe attachment caller guarantees the native object's type and lifetime.
pub(super) struct NativeParent {
    /// Non-null `NSView` or `HWND`, valid on its owning UI thread throughout attachment.
    pointer: NonNull<c_void>,
}

impl NativeParent {
    /// Resolves initial logical geometry against the live parent rather than a guessed creation-time DPI.
    #[cfg(target_os = "windows")]
    #[expect(unsafe_code, reason = "the validated native parent remains live on its UI thread")]
    pub(super) fn initial_size(&self, size: baseview::dpi::Size, fallback_scale: f64) -> baseview::dpi::Size {
        // SAFETY: NativeParent validates the caller-owned HWND and its owner thread before this query.
        let dpi = unsafe { windows_sys::Win32::UI::HiDpi::GetDpiForWindow(self.pointer.as_ptr()) };
        let scale = if dpi == 0 {
            fallback_scale
        } else {
            f64::from(dpi) / BASE_DPI
        };
        baseview::dpi::NativeSize::<u32>::from_size(size, scale).into()
    }

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

    #[cfg(target_os = "windows")]
    #[test]
    #[expect(unsafe_code, reason = "the test owns the native parent through its validated borrow")]
    fn initial_size_uses_parent_dpi_and_preserves_explicit_physical_pixels() {
        use plugin_host_core::{EditorCallbacks, NativeWindow, UiThread, WindowSize};
        use std::rc::Rc;

        struct Callbacks;
        impl EditorCallbacks for Callbacks {
            fn constrain(&self, size: WindowSize) -> WindowSize {
                size
            }
            fn resized(&self, _size: WindowSize) {}
            fn scale_changed(&self, _factor: f32) {}
            fn close_requested(&self) {}
        }

        let ui = UiThread::register_current().expect("register native owner");
        let callbacks: Rc<dyn EditorCallbacks> = Rc::new(Callbacks);
        let window = NativeWindow::create(
            &ui,
            &callbacks,
            "ImageControl initial geometry regression",
            WindowSize {
                width: 1000,
                height: 764,
            },
            false,
            None,
        )
        .expect("create owned native parent");
        // SAFETY: this test owns the live parent on its registered UI thread until after the borrow is dropped.
        let parent = unsafe { NativeParent::new(window.parent()) }.expect("validated parent");
        let scale = f64::from(window.content_scale().expect("native scale"));
        let logical = baseview::dpi::LogicalSize::new(1000.0, 764.0);
        let expected = baseview::dpi::NativeSize::<u32>::from_size(logical.into(), scale);
        assert_eq!(parent.initial_size(logical.into(), 0.5), expected.into());
        let physical = baseview::dpi::NativeSize::new(911_u32, 733_u32);
        assert_eq!(parent.initial_size(physical.into(), 0.5), physical.into());
    }

    #[test]
    #[expect(unsafe_code, reason = "A null pointer is rejected before any native API is called")]
    fn null_parent_never_reaches_native_object_apis() {
        // SAFETY: This is the explicitly rejected null case, so no object-lifetime requirement applies.
        let result = unsafe { NativeParent::new(std::ptr::null_mut()) };
        assert!(matches!(result, Err(EditorError::InvalidParent)));
    }
}
