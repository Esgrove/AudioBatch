//! Running the platform event loop for a plugin host without a GUI framework.
//!
//! Plugins can post main-loop work while loading, so a headless host must keep that loop running.
//! A GUI application runs the loop already and does not need this.

#![cfg_attr(
    any(target_os = "macos", windows),
    expect(unsafe_code, reason = "calls the platform event loop")
)]

use std::time::Duration;
#[cfg(windows)]
use std::time::Instant;

#[cfg(target_os = "macos")]
use objc2_core_foundation::{CFRunLoop, kCFRunLoopDefaultMode};
#[cfg(windows)]
use windows_sys::Win32::UI::WindowsAndMessaging::{
    DispatchMessageW, MSG, MsgWaitForMultipleObjects, PM_REMOVE, PeekMessageW, QS_ALLINPUT, TranslateMessage,
};

use crate::thread::UiThread;

/// Handles platform events within `timeout`, returning early after the first one.
#[cfg(target_os = "macos")]
pub fn pump_platform_events(_ui: &UiThread, timeout: Duration) {
    // SAFETY: the run loop mode is a constant initialized by Core Foundation before any code runs.
    let mode = unsafe { kCFRunLoopDefaultMode };
    CFRunLoop::run_in_mode(mode, timeout.as_secs_f64(), true);
}

/// Handles queued window messages within `timeout`, including the wait for new work.
#[cfg(windows)]
pub fn pump_platform_events(_ui: &UiThread, timeout: Duration) {
    let deadline = Instant::now() + timeout;
    // SAFETY: `MSG` is a plain struct for which all zero bits are valid.
    let mut message: MSG = unsafe { std::mem::zeroed() };
    let milliseconds = u32::try_from(timeout.as_millis()).unwrap_or(u32::MAX);
    // SAFETY: waiting with no handles only watches this thread's message queue.
    unsafe { MsgWaitForMultipleObjects(0, std::ptr::null(), 0, milliseconds, QS_ALLINPUT) };
    // SAFETY: the message lives for every call, and a null window takes all messages of this thread.
    unsafe {
        while PeekMessageW(&raw mut message, std::ptr::null_mut(), 0, 0, PM_REMOVE) != 0 {
            TranslateMessage(&raw const message);
            DispatchMessageW(&raw const message);
            if Instant::now() >= deadline {
                break;
            }
        }
    }
}

/// Waits for `timeout` when no native event loop implementation is available.
#[cfg(not(any(target_os = "macos", windows)))]
pub fn pump_platform_events(_ui: &UiThread, timeout: Duration) {
    std::thread::sleep(timeout);
}

#[cfg(all(test, windows))]
mod tests {
    use windows_sys::Win32::UI::WindowsAndMessaging::{PostMessageW, WM_APP};

    use super::*;

    #[test]
    fn expired_budget_leaves_queued_work_for_the_next_turn() {
        const TEST_MESSAGE: u32 = WM_APP + 173;
        const MESSAGE_COUNT: usize = 128;

        let remaining = crate::test_helpers::dispatcher()
            .call(
                |ui| -> std::io::Result<usize> {
                    // SAFETY: MSG has a valid all-zero representation.
                    let mut message: MSG = unsafe { std::mem::zeroed() };
                    // SAFETY: the live local message also ensures this thread has a message queue.
                    unsafe {
                        PeekMessageW(&raw mut message, std::ptr::null_mut(), TEST_MESSAGE, TEST_MESSAGE, 0);
                    }
                    for _ in 0..MESSAGE_COUNT {
                        // SAFETY: a null HWND queues this private, pointer-free message on the current thread.
                        if unsafe { PostMessageW(std::ptr::null_mut(), TEST_MESSAGE, 0, 0) } == 0 {
                            return Err(std::io::Error::last_os_error());
                        }
                    }
                    pump_platform_events(ui, Duration::ZERO);
                    let mut remaining = 0;
                    // SAFETY: only this test's pointer-free messages are removed into a live local MSG.
                    unsafe {
                        while PeekMessageW(
                            &raw mut message,
                            std::ptr::null_mut(),
                            TEST_MESSAGE,
                            TEST_MESSAGE,
                            PM_REMOVE,
                        ) != 0
                        {
                            remaining += 1;
                        }
                    }
                    Ok(remaining)
                },
                &|| false,
            )
            .expect("dispatch bounded message pump check")
            .expect("queue native test messages");
        assert!(remaining > 0, "an expired budget must not drain the queue");
    }
}
