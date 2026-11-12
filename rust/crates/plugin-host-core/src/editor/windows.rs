//! Win32 editor windows with format-neutral size, scale, and close callbacks.
//!
//! The client area uses physical pixels.
//! User resizes are constrained by the host, and display changes report the new content scale.
//! Closing hides the window so the host can capture state before destroying it.

#![expect(unsafe_code, reason = "creates a Win32 window and handles its messages")]

use std::ffi::c_void;
use std::num::NonZeroIsize;
use std::rc::{Rc, Weak};
use std::sync::OnceLock;

use raw_window_handle::{RawWindowHandle, Win32WindowHandle};
use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows_sys::Win32::Graphics::Gdi::{
    BLACK_BRUSH, GetMonitorInfoW, GetStockObject, MONITOR_DEFAULTTONEAREST, MONITORINFO, MonitorFromPoint,
};
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::UI::HiDpi::{AdjustWindowRectExForDpi, GetDpiForWindow};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    CS_HREDRAW, CS_VREDRAW, CW_USEDEFAULT, CreateWindowExW, DefWindowProcW, DestroyWindow, GWL_EXSTYLE, GWL_STYLE,
    GWLP_USERDATA, GetClientRect, GetWindowLongPtrW, GetWindowLongW, GetWindowRect, IDC_ARROW, IsIconic,
    IsWindowVisible, LoadCursorW, RegisterClassExW, SIZE_MINIMIZED, SW_HIDE, SW_SHOWNORMAL, SWP_NOACTIVATE, SWP_NOMOVE,
    SWP_NOSIZE, SWP_NOZORDER, SetForegroundWindow, SetWindowLongPtrW, SetWindowPos, ShowWindow, WM_CLOSE,
    WM_DPICHANGED, WM_NCDESTROY, WM_SIZE, WM_SIZING, WMSZ_BOTTOMLEFT, WMSZ_LEFT, WMSZ_TOP, WMSZ_TOPLEFT, WMSZ_TOPRIGHT,
    WNDCLASSEXW, WS_CAPTION, WS_CLIPCHILDREN, WS_MAXIMIZEBOX, WS_MINIMIZEBOX, WS_OVERLAPPED, WS_SYSMENU, WS_THICKFRAME,
};

use super::{EditorCallbacks, WindowError, WindowPosition, WindowSize};
use crate::thread::UiThread;

/// Name of the window class of editor windows.
const CLASS_NAME: &str = "AudioBatchPluginEditor";
/// Dots per inch of a display at 100 percent scale.
const BASE_DPI: f32 = 96.0;

/// Read-only observations of one owned Win32 editor, queried on its UI thread.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WindowObservation {
    /// Client dimensions in physical pixels, without the title bar or frame.
    pub physical_size: WindowSize,
    /// Actual physical pixels per logical unit reported for this window.
    pub content_scale: f64,
    /// Whether the window has the visible style. It may still be occluded.
    pub visible: bool,
    /// Whether the top-level window is minimized.
    pub minimized: bool,
}

/// A native editor window that stays on the UI thread and hides on a user close request.
pub struct NativeWindow {
    /// The window handle, destroyed when this is dropped.
    hwnd: HWND,
    /// Nonzero form of the handle, validated once when the window is created.
    handle: NonZeroIsize,
}

impl NativeWindow {
    /// Creates a hidden window owned by `owner`, with a client area of `size` pixels.
    /// The host must retain `callbacks` while native events should be delivered.
    ///
    /// # Errors
    ///
    /// Returns an error when the window class cannot be registered or the window cannot be created.
    pub fn create(
        _ui: &UiThread,
        callbacks: &Rc<dyn EditorCallbacks>,
        title: &str,
        size: WindowSize,
        resizable: bool,
        owner: Option<RawWindowHandle>,
    ) -> Result<Self, WindowError> {
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
        // SAFETY: the class is registered, the null-terminated title outlives the call,
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
        let Some(handle) = NonZeroIsize::new(hwnd.expose_provenance().cast_signed()) else {
            return Err(WindowError::CreateWindow {
                source: std::io::Error::last_os_error(),
            });
        };
        let state = Box::into_raw(Box::new(Rc::downgrade(callbacks)));
        // SAFETY: the window procedure frees this state when the valid window is destroyed.
        unsafe { SetWindowLongPtrW(hwnd, GWLP_USERDATA, state.expose_provenance().cast_signed()) };
        let window = Self { hwnd, handle };
        window.set_content_size(size);
        Ok(window)
    }

    /// Returns the HWND the plugin attaches to, valid until this window is dropped.
    #[must_use]
    pub const fn parent(&self) -> *mut c_void {
        self.hwnd
    }

    /// Returns the HWND handle used to embed the plugin editor.
    /// The handle is valid only while this window is alive and must only be used on its UI thread.
    #[must_use]
    pub fn raw_window_handle(&self) -> RawWindowHandle {
        RawWindowHandle::Win32(Win32WindowHandle::new(self.handle))
    }

    /// Resizes the client area to `size` pixels.
    pub fn set_content_size(&self, size: WindowSize) {
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

    /// Returns the frame's top-left corner in physical pixels, or none while minimized.
    #[must_use]
    pub fn position(&self) -> Option<WindowPosition> {
        // SAFETY: the window is valid.
        if unsafe { IsIconic(self.hwnd) } != 0 {
            return None;
        }
        let rect = window_rect(self.hwnd)?;
        Some(WindowPosition {
            x: rect.left,
            y: rect.top,
        })
    }

    /// Moves the frame's top-left corner, keeping the window in the nearest monitor's work area.
    pub fn move_to(&self, position: WindowPosition) {
        let Some(rect) = window_rect(self.hwnd) else {
            return;
        };
        let placed = RECT {
            left: position.x,
            top: position.y,
            right: position.x + (rect.right - rect.left),
            bottom: position.y + (rect.bottom - rect.top),
        };
        let corner = POINT {
            x: position.x,
            y: position.y,
        };
        // SAFETY: the monitor handle comes from the system, and the information lives for the call.
        let work_area = unsafe {
            let monitor = MonitorFromPoint(corner, MONITOR_DEFAULTTONEAREST);
            let mut info = MONITORINFO {
                cbSize: u32::try_from(size_of::<MONITORINFO>()).unwrap_or(u32::MAX),
                rcMonitor: placed,
                rcWork: placed,
                dwFlags: 0,
            };
            (GetMonitorInfoW(monitor, &raw mut info) != 0).then_some(info.rcWork)
        };
        let target = work_area.map_or(position, |area| keep_inside(&placed, &area));
        // SAFETY: the window is valid.
        unsafe {
            SetWindowPos(
                self.hwnd,
                std::ptr::null_mut(),
                target.x,
                target.y,
                0,
                0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE,
            );
        }
    }

    /// Returns the number of physical pixels per logical pixel on this window's display.
    #[must_use]
    pub fn content_scale(&self) -> Option<f32> {
        // SAFETY: the window is valid.
        Some(dpi_scale(unsafe { GetDpiForWindow(self.hwnd) }))
    }

    /// Queries actual client geometry, DPI, and visibility without changing the window.
    ///
    /// # Errors
    ///
    /// Returns an error if Windows cannot query the owned window's client area or DPI.
    pub fn observation(&self) -> std::io::Result<WindowObservation> {
        let mut rect = RECT {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        // SAFETY: the owned window and output rectangle remain valid throughout the query.
        if unsafe { GetClientRect(self.hwnd, &raw mut rect) } == 0 {
            return Err(std::io::Error::last_os_error());
        }
        // SAFETY: the owned window remains valid on its UI thread.
        let dpi = unsafe { GetDpiForWindow(self.hwnd) };
        if dpi == 0 {
            return Err(std::io::Error::other("The editor window DPI is unavailable"));
        }
        Ok(WindowObservation {
            physical_size: WindowSize {
                width: rect.right - rect.left,
                height: rect.bottom - rect.top,
            },
            content_scale: f64::from(dpi) / f64::from(BASE_DPI),
            // SAFETY: these read-only queries use the live owned window on its UI thread.
            visible: unsafe { IsWindowVisible(self.hwnd) } != 0,
            // SAFETY: the live top-level window belongs to this UI thread.
            minimized: unsafe { IsIconic(self.hwnd) } != 0,
        })
    }

    /// Shows the window in front of the others and gives it keyboard focus.
    pub fn show(&self) {
        // SAFETY: the window is valid.
        unsafe {
            ShowWindow(self.hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(self.hwnd);
        }
    }
    /// Hides the native parent while retaining the embedded plugin window.
    pub fn hide(&self) {
        // SAFETY: The window remains owned by this UI-thread-only value.
        unsafe { ShowWindow(self.hwnd, SW_HIDE) };
    }
}

impl Drop for NativeWindow {
    fn drop(&mut self) {
        // SAFETY: the window is valid and was created on this thread.
        unsafe { DestroyWindow(self.hwnd) };
    }
}

/// Returns the editor window class, registering it on first use.
fn window_class() -> Result<*const u16, WindowError> {
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
        // SAFETY: a null name returns the executable's module.
        hInstance: unsafe { GetModuleHandleW(std::ptr::null()) },
        hIcon: std::ptr::null_mut(),
        // SAFETY: the arrow is a system cursor and needs no module.
        hCursor: unsafe { LoadCursorW(std::ptr::null_mut(), IDC_ARROW) },
        // SAFETY: stock objects are always available.
        hbrBackground: unsafe { GetStockObject(BLACK_BRUSH) },
        lpszMenuName: std::ptr::null(),
        lpszClassName: name.as_ptr(),
        hIconSm: std::ptr::null_mut(),
    };
    // SAFETY: the class description and name are valid for the call.
    let atom = unsafe { RegisterClassExW(&raw const class) };
    if atom == 0 {
        return Err(WindowError::RegisterWindowClass {
            source: std::io::Error::last_os_error(),
        });
    }
    Ok(class_pointer(*ATOM.get_or_init(|| atom)))
}

/// Returns a class atom in the form that stands for a class name.
fn class_pointer(atom: u16) -> *const u16 {
    std::ptr::without_provenance(usize::from(atom))
}

/// Handles native messages, upgrading the host callbacks only for the duration of a message.
unsafe extern "system" fn window_procedure(hwnd: HWND, message: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    // SAFETY: user data is null or the boxed weak callback stored by `NativeWindow::create`.
    let state: *mut Weak<dyn EditorCallbacks> =
        std::ptr::with_exposed_provenance_mut(unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) }.cast_unsigned());
    if message == WM_NCDESTROY && !state.is_null() {
        // SAFETY: the state comes from `Box::into_raw`, and clearing user data first frees it only once.
        unsafe {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            drop(Box::from_raw(state));
        }
    } else if !state.is_null()
        // SAFETY: the state lives until the window is destroyed.
        && let Some(callbacks) = unsafe { &*state }.upgrade()
        // SAFETY: the window and message parameters come from the system.
        && let Some(result) = unsafe { handle_message(hwnd, &*callbacks, message, wparam, lparam) }
    {
        return result;
    }
    // SAFETY: the parameters come from the system unchanged.
    unsafe { DefWindowProcW(hwnd, message, wparam, lparam) }
}

/// Handles editor messages, returning none for default handling.
///
/// # Safety
///
/// Parameters must be those the system passed to the window procedure.
unsafe fn handle_message(
    hwnd: HWND,
    callbacks: &dyn EditorCallbacks,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> Option<LRESULT> {
    match message {
        WM_CLOSE => {
            // SAFETY: the window is valid.
            unsafe { ShowWindow(hwnd, SW_HIDE) };
            callbacks.close_requested();
            Some(0)
        }
        WM_SIZE => {
            if u32::try_from(wparam).is_ok_and(|kind| kind != SIZE_MINIMIZED) {
                callbacks.resized(WindowSize {
                    width: low_word(lparam.cast_unsigned()),
                    height: low_word(lparam.cast_unsigned() >> 16),
                });
            }
            Some(0)
        }
        WM_SIZING => {
            // SAFETY: this message carries a valid proposed window rectangle.
            let rect = unsafe { &mut *std::ptr::with_exposed_provenance_mut::<RECT>(lparam.cast_unsigned()) };
            constrain_drag(hwnd, callbacks, u32::try_from(wparam).unwrap_or(0), rect);
            Some(1)
        }
        WM_DPICHANGED => {
            callbacks.scale_changed(dpi_scale(u32::try_from(wparam & 0xFFFF).unwrap_or(0)));
            // SAFETY: this message carries a valid suggested window rectangle.
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

/// Constrains a dragged rectangle's content size while keeping the opposite edges fixed.
fn constrain_drag(hwnd: HWND, callbacks: &dyn EditorCallbacks, edge: u32, rect: &mut RECT) {
    let (extra_width, extra_height) = frame_extent(hwnd);
    let proposed = WindowSize {
        width: (rect.right - rect.left - extra_width).max(1),
        height: (rect.bottom - rect.top - extra_height).max(1),
    };
    let accepted = callbacks.constrain(proposed);
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

/// Returns the frame rectangle on the virtual screen, or none when the system cannot tell.
fn window_rect(hwnd: HWND) -> Option<RECT> {
    let mut rect = RECT {
        left: 0,
        top: 0,
        right: 0,
        bottom: 0,
    };
    // SAFETY: the window is valid, and the rectangle lives for the call.
    (unsafe { GetWindowRect(hwnd, &raw mut rect) } != 0).then_some(rect)
}

/// Moves a window rectangle minimally into a work area, keeping oversized windows' title bars reachable.
const fn keep_inside(window: &RECT, area: &RECT) -> WindowPosition {
    WindowPosition {
        x: keep_span_inside(window.left, window.right - window.left, area.left, area.right),
        y: keep_span_inside(window.top, window.bottom - window.top, area.top, area.bottom),
    }
}

/// Fits a span between two edges, returning the lower edge if the span is longer than the area.
const fn keep_span_inside(start: i32, length: i32, lower: i32, upper: i32) -> i32 {
    let highest = upper - length;
    if start > highest {
        if highest > lower { highest } else { lower }
    } else if start < lower {
        lower
    } else {
        start
    }
}

/// Returns the frame's additional width and height at the window's display scale.
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

/// Returns the display scale for a DPI value, or one when it is unknown.
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

#[cfg(test)]
mod test_window_placement {
    use super::*;

    struct ObservationCallbacks;

    impl EditorCallbacks for ObservationCallbacks {
        fn constrain(&self, size: WindowSize) -> WindowSize {
            size
        }
        fn resized(&self, _size: WindowSize) {}
        fn scale_changed(&self, _factor: f32) {}
        fn close_requested(&self) {}
    }

    #[test]
    fn native_observation_tracks_client_resize_and_visibility() {
        let size = WindowSize {
            width: 640,
            height: 480,
        };
        let resized_size = WindowSize {
            width: 800,
            height: 600,
        };
        let observations = crate::test_helpers::dispatcher()
            .call(
                move |ui| {
                    std::panic::catch_unwind(|| {
                        let callbacks: Rc<dyn EditorCallbacks> = Rc::new(ObservationCallbacks);
                        let window =
                            NativeWindow::create(ui, &callbacks, "Native observation contract", size, true, None)
                                .expect("create native parent");
                        let hidden = window.observation().expect("observe hidden window");
                        window.show();
                        let shown = window.observation().expect("observe visible window");
                        window.set_content_size(resized_size);
                        let resized = window.observation().expect("observe resized window");
                        window.hide();
                        let hidden_again = window.observation().expect("observe hidden window again");
                        [hidden, shown, resized, hidden_again]
                    })
                },
                &|| false,
            )
            .expect("dispatch native observations")
            .unwrap_or_else(|panic| std::panic::resume_unwind(panic));
        let [hidden, shown, resized, hidden_again] = observations;
        assert_eq!(hidden.physical_size, size);
        assert!(!hidden.visible);
        assert!(!hidden.minimized);
        assert!(hidden.content_scale.is_finite() && hidden.content_scale > 0.0);
        assert!(shown.visible);
        assert_eq!(resized.physical_size, resized_size);
        assert!(!hidden_again.visible);
    }

    /// Returns a rectangle from its top-left corner and size.
    const fn rect(left: i32, top: i32, width: i32, height: i32) -> RECT {
        RECT {
            left,
            top,
            right: left + width,
            bottom: top + height,
        }
    }

    #[test]
    fn a_window_inside_the_work_area_stays_where_it_is() {
        let area = rect(0, 0, 1920, 1040);
        assert_eq!(
            keep_inside(&rect(200, 100, 800, 600), &area),
            WindowPosition { x: 200, y: 100 }
        );
    }

    #[test]
    fn a_window_past_an_edge_moves_back_just_inside() {
        let area = rect(0, 0, 1920, 1040);
        assert_eq!(
            keep_inside(&rect(1500, 900, 800, 600), &area),
            WindowPosition { x: 1120, y: 440 }
        );
        assert_eq!(
            keep_inside(&rect(-300, -50, 800, 600), &area),
            WindowPosition { x: 0, y: 0 }
        );
    }

    #[test]
    fn a_window_on_a_second_monitor_keeps_its_offset_coordinates() {
        let area = rect(-2560, 0, 2560, 1400);
        assert_eq!(
            keep_inside(&rect(-2000, 300, 800, 600), &area),
            WindowPosition { x: -2000, y: 300 }
        );
    }

    #[test]
    fn a_window_larger_than_the_work_area_keeps_its_title_bar_reachable() {
        let area = rect(0, 0, 1280, 680);
        assert_eq!(
            keep_inside(&rect(400, 300, 1600, 900), &area),
            WindowPosition { x: 0, y: 0 }
        );
    }
}
