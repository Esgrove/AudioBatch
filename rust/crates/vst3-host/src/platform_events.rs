//! Running the platform's event loop on the UI thread of a process without a GUI framework.
//!
//! Some plugins post work to the main run loop while they load and wait for it,
//! copy-protected ones in particular, so a headless host such as the scan worker must keep that loop running.
//! A GUI application runs the loop already and does not need this.

#![cfg_attr(
    any(target_os = "macos", windows),
    expect(unsafe_code, reason = "calls the platform event loop")
)]

use std::time::Duration;

#[cfg(target_os = "macos")]
use objc2_core_foundation::{CFRunLoop, kCFRunLoopDefaultMode};
#[cfg(windows)]
use windows_sys::Win32::UI::WindowsAndMessaging::{
    DispatchMessageW, MSG, MsgWaitForMultipleObjects, PM_REMOVE, PeekMessageW, QS_ALLINPUT, TranslateMessage,
};

use crate::thread::UiThread;

/// Handles the platform events that arrive within `timeout`, returning early after the first one.
#[cfg(target_os = "macos")]
pub fn pump_platform_events(_ui: &UiThread, timeout: Duration) {
    // SAFETY: the default run loop mode is a constant that Core Foundation initializes before any code runs.
    let mode = unsafe { kCFRunLoopDefaultMode };
    CFRunLoop::run_in_mode(mode, timeout.as_secs_f64(), true);
}

/// Handles the window messages that are queued, then waits up to `timeout` for another one.
#[cfg(windows)]
pub fn pump_platform_events(_ui: &UiThread, timeout: Duration) {
    // SAFETY: `MSG` is a plain struct for which all zero bits are valid.
    let mut message: MSG = unsafe { std::mem::zeroed() };
    let milliseconds = u32::try_from(timeout.as_millis()).unwrap_or(u32::MAX);
    // SAFETY: waiting with no handles only watches the message queue of this thread.
    unsafe { MsgWaitForMultipleObjects(0, std::ptr::null(), 0, milliseconds, QS_ALLINPUT) };
    // SAFETY: the message lives for every call, and a null window takes the messages of all windows of this thread.
    unsafe {
        while PeekMessageW(&raw mut message, std::ptr::null_mut(), 0, 0, PM_REMOVE) != 0 {
            TranslateMessage(&raw const message);
            DispatchMessageW(&raw const message);
        }
    }
}

/// Waits for `timeout`, because there is no platform event loop to run.
#[cfg(not(any(target_os = "macos", windows)))]
pub fn pump_platform_events(_ui: &UiThread, timeout: Duration) {
    std::thread::sleep(timeout);
}
