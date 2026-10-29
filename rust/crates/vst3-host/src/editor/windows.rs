//! The Win32 window that shows a plugin editor on Windows.
//!
//! The editor creates its own child window inside this window's client area.
//! VST3 sizes are physical pixels on Windows, so the client area matches the view's size exactly,
//! and the view is told the display scale through `IPlugViewContentScaleSupport`.
//! The window procedure hides the window when the user closes it,
//! checks the user's resizes with the view, reports the new size to the view,
//! and passes a new display scale on when the window moves to another display.

#![expect(unsafe_code, reason = "creates a Win32 window and handles its messages")]

use std::ffi::c_void;
use std::rc::{Rc, Weak};
use std::sync::OnceLock;

use raw_window_handle::RawWindowHandle;
use vst3::Steinberg::{FIDString, kPlatformTypeHWND};
use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows_sys::Win32::Graphics::Gdi::{BLACK_BRUSH, GetStockObject};
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::UI::HiDpi::{AdjustWindowRectExForDpi, GetDpiForWindow};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    CS_HREDRAW, CS_VREDRAW, CW_USEDEFAULT, CreateWindowExW, DefWindowProcW, DestroyWindow, GWL_EXSTYLE, GWL_STYLE,
    GWLP_USERDATA, GetWindowLongPtrW, GetWindowLongW, IDC_ARROW, LoadCursorW, RegisterClassExW, SIZE_MINIMIZED,
    SW_HIDE, SW_SHOWNORMAL, SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOZORDER, SetForegroundWindow, SetWindowLongPtrW,
    SetWindowPos, ShowWindow, WM_CLOSE, WM_DPICHANGED, WM_NCDESTROY, WM_SIZE, WM_SIZING, WMSZ_BOTTOMLEFT, WMSZ_LEFT,
    WMSZ_TOP, WMSZ_TOPLEFT, WMSZ_TOPRIGHT, WNDCLASSEXW, WS_CAPTION, WS_CLIPCHILDREN, WS_MAXIMIZEBOX, WS_MINIMIZEBOX,
    WS_OVERLAPPED, WS_SYSMENU, WS_THICKFRAME,
};

use super::{EditorError, EditorShared, ViewSize};
use crate::thread::UiThread;

/// Name of the window class of editor windows.
const CLASS_NAME: &str = "AudioBatchPluginEditor";

/// Dots per inch of a display at 100 percent scale.
const BASE_DPI: f32 = 96.0;

/// The window of one editor.
pub struct NativeWindow {
    /// The window handle, destroyed when this is dropped.
    hwnd: HWND,
}

impl NativeWindow {
    /// Editors attach to an `HWND`.
    pub const PLATFORM_TYPE: FIDString = kPlatformTypeHWND;

    /// Creates a hidden window owned by `owner`, with a client area of `size` pixels.
    pub fn create(
        _ui: &UiThread,
        shared: &Rc<EditorShared>,
        title: &str,
        size: ViewSize,
        resizable: bool,
        owner: Option<RawWindowHandle>,
    ) -> Result<Self, EditorError> {
        let class = window_class()?;
        let mut style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        if resizable {
            style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
        }
        let owner = match owner {
            Some(RawWindowHandle::Win32(handle)) => {
                std::ptr::with_exposed_provenance_mut(handle.hwnd.get().cast_unsigned())
            }
            _ => std::ptr::null_mut(),
        };
        let title = wide(title);
        // SAFETY: the class is registered, the title is null-terminated and outlives the call,
        // and the owner is a window of this thread or null.
        let hwnd = unsafe {
            CreateWindowExW(
                0,
                class,
                title.as_ptr(),
                style,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                owner,
                std::ptr::null_mut(),
                GetModuleHandleW(std::ptr::null()),
                std::ptr::null(),
            )
        };
        if hwnd.is_null() {
            return Err(EditorError::CreateWindow {
                source: std::io::Error::last_os_error(),
            });
        }
        let state = Box::into_raw(Box::new(Rc::downgrade(shared)));
        // SAFETY: the window is valid, and the window procedure frees the state when the window is destroyed.
        unsafe { SetWindowLongPtrW(hwnd, GWLP_USERDATA, state.expose_provenance().cast_signed()) };
        let window = Self { hwnd };
        window.set_content_size(size);
        Ok(window)
    }

    /// Returns the window the editor attaches to.
    pub const fn parent(&self) -> *mut c_void {
        self.hwnd
    }

    /// Resizes the window so its client area is `size` pixels.
    pub fn set_content_size(&self, size: ViewSize) {
        let (extra_width, extra_height) = frame_extent(self.hwnd);
        // SAFETY: the window is valid.
        unsafe {
            SetWindowPos(
                self.hwnd,
                std::ptr::null_mut(),
                0,
                0,
                size.width.saturating_add(extra_width),
                size.height.saturating_add(extra_height),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE,
            );
        }
    }

    /// Returns how many physical pixels make one logical pixel on the window's display.
    #[expect(
        clippy::unnecessary_wraps,
        reason = "matches the macOS window, which leaves scaling to AppKit"
    )]
    pub fn content_scale(&self) -> Option<f32> {
        // SAFETY: the window is valid.
        Some(dpi_scale(unsafe { GetDpiForWindow(self.hwnd) }))
    }

    /// Shows the window in front of the others and gives it the keyboard focus.
    pub fn show(&self) {
        // SAFETY: the window is valid.
        unsafe {
            ShowWindow(self.hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(self.hwnd);
        }
    }
}

impl Drop for NativeWindow {
    fn drop(&mut self) {
        // SAFETY: the window is valid and was created on this thread.
        unsafe { DestroyWindow(self.hwnd) };
    }
}

/// Returns the class of editor windows, registering it the first time.
fn window_class() -> Result<*const u16, EditorError> {
    static ATOM: OnceLock<u16> = OnceLock::new();
    if let Some(&atom) = ATOM.get() {
        return Ok(class_pointer(atom));
    }
    let name = wide(CLASS_NAME);
    let class = WNDCLASSEXW {
        cbSize: u32::try_from(size_of::<WNDCLASSEXW>()).unwrap_or(u32::MAX),
        style: CS_HREDRAW | CS_VREDRAW,
        lpfnWndProc: Some(window_procedure),
        cbClsExtra: 0,
        cbWndExtra: 0,
        // SAFETY: a null name returns the module of the executable.
        hInstance: unsafe { GetModuleHandleW(std::ptr::null()) },
        hIcon: std::ptr::null_mut(),
        // SAFETY: the arrow is a system cursor, which needs no module.
        hCursor: unsafe { LoadCursorW(std::ptr::null_mut(), IDC_ARROW) },
        // SAFETY: stock objects are always available.
        hbrBackground: unsafe { GetStockObject(BLACK_BRUSH) },
        lpszMenuName: std::ptr::null(),
        lpszClassName: name.as_ptr(),
        hIconSm: std::ptr::null_mut(),
    };
    // SAFETY: the class description and its name are valid for the call.
    let atom = unsafe { RegisterClassExW(&raw const class) };
    if atom == 0 {
        return Err(EditorError::RegisterWindowClass {
            source: std::io::Error::last_os_error(),
        });
    }
    Ok(class_pointer(*ATOM.get_or_init(|| atom)))
}

/// Returns a class atom in the form that stands for a class name.
fn class_pointer(atom: u16) -> *const u16 {
    std::ptr::without_provenance(usize::from(atom))
}

/// Handles the messages of an editor window.
unsafe extern "system" fn window_procedure(hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    // SAFETY: the user data is null or the state stored by `NativeWindow::create`.
    let state: *mut Weak<EditorShared> =
        std::ptr::with_exposed_provenance_mut(unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) }.cast_unsigned());
    if message == WM_NCDESTROY && !state.is_null() {
        // SAFETY: the state was created by `Box::into_raw`, and clearing the user data first frees it only once.
        unsafe {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            drop(Box::from_raw(state));
        }
    } else if !state.is_null()
        // SAFETY: the state lives until the window is destroyed.
        && let Some(shared) = unsafe { &*state }.upgrade()
        // SAFETY: the window and the message parameters come from the system.
        && let Some(result) = unsafe { handle_message(hwnd, &shared, message, wparam, lparam) }
    {
        return result;
    }
    // SAFETY: the parameters come from the system unchanged.
    unsafe { DefWindowProcW(hwnd, message, wparam, lparam) }
}

/// Handles a message the editor needs to know about, returning none for the default handling.
///
/// # Safety
///
/// The parameters must be those the system passed to the window procedure.
unsafe fn handle_message(
    hwnd: HWND,
    shared: &EditorShared,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> Option<LRESULT> {
    match message {
        WM_CLOSE => {
            // SAFETY: the window is valid.
            unsafe { ShowWindow(hwnd, SW_HIDE) };
            shared.close_requested();
            Some(0)
        }
        WM_SIZE => {
            if u32::try_from(wparam).is_ok_and(|kind| kind != SIZE_MINIMIZED) {
                shared.user_resized(ViewSize {
                    width: low_word(lparam.cast_unsigned()),
                    height: low_word(lparam.cast_unsigned() >> 16),
                });
            }
            Some(0)
        }
        WM_SIZING => {
            // SAFETY: for this message the system passes the proposed window rectangle.
            let rect = unsafe { &mut *std::ptr::with_exposed_provenance_mut::<RECT>(lparam.cast_unsigned()) };
            constrain_drag(hwnd, shared, u32::try_from(wparam).unwrap_or(0), rect);
            Some(1)
        }
        WM_DPICHANGED => {
            shared.set_content_scale(dpi_scale(u32::try_from(wparam & 0xFFFF).unwrap_or(0)));
            // SAFETY: for this message the system passes the suggested window rectangle.
            let suggested = unsafe { &*std::ptr::with_exposed_provenance::<RECT>(lparam.cast_unsigned()) };
            // SAFETY: the window is valid.
            unsafe {
                SetWindowPos(
                    hwnd,
                    std::ptr::null_mut(),
                    suggested.left,
                    suggested.top,
                    suggested.right - suggested.left,
                    suggested.bottom - suggested.top,
                    SWP_NOZORDER | SWP_NOACTIVATE,
                );
            }
            Some(0)
        }
        _ => None,
    }
}

/// Changes a window rectangle the user is dragging so its client area has a size the view accepts.
/// The edges being dragged move, and the opposite edges stay where they are.
fn constrain_drag(hwnd: HWND, shared: &EditorShared, edge: u32, rect: &mut RECT) {
    let (extra_width, extra_height) = frame_extent(hwnd);
    let proposed = ViewSize {
        width: (rect.right - rect.left - extra_width).max(1),
        height: (rect.bottom - rect.top - extra_height).max(1),
    };
    let accepted = shared.constrain(proposed);
    let width = accepted.width + extra_width;
    let height = accepted.height + extra_height;
    if matches!(edge, WMSZ_LEFT | WMSZ_TOPLEFT | WMSZ_BOTTOMLEFT) {
        rect.left = rect.right - width;
    } else {
        rect.right = rect.left + width;
    }
    if matches!(edge, WMSZ_TOP | WMSZ_TOPLEFT | WMSZ_TOPRIGHT) {
        rect.top = rect.bottom - height;
    } else {
        rect.bottom = rect.top + height;
    }
}

/// Returns how much wider and taller the window is than its client area, at the window's display scale.
fn frame_extent(hwnd: HWND) -> (i32, i32) {
    let mut rect = RECT {
        left: 0,
        top: 0,
        right: 0,
        bottom: 0,
    };
    // SAFETY: the window is valid, and the rectangle lives for the call.
    unsafe {
        let style = GetWindowLongW(hwnd, GWL_STYLE).cast_unsigned();
        let extended_style = GetWindowLongW(hwnd, GWL_EXSTYLE).cast_unsigned();
        AdjustWindowRectExForDpi(&raw mut rect, style, 0, extended_style, GetDpiForWindow(hwnd));
    }
    (rect.right - rect.left, rect.bottom - rect.top)
}

/// Returns the scale of a display with a number of dots per inch, 1 when it is unknown.
fn dpi_scale(dpi: u32) -> f32 {
    u16::try_from(dpi)
        .ok()
        .filter(|&dpi| dpi > 0)
        .map_or(1.0, |dpi| f32::from(dpi) / BASE_DPI)
}

/// Returns the low 16 bits of a message parameter as a size.
fn low_word(value: usize) -> i32 {
    i32::from(u16::try_from(value & 0xFFFF).unwrap_or(0))
}

/// Returns text as a null-terminated UTF-16 string.
fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}
