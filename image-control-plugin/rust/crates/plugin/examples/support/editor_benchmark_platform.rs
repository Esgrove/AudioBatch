//! Native event pumping and best-effort geometry, visibility, and display-refresh capability observations.

use anyhow::Context as _;
use anyhow::Result;
#[cfg(target_os = "macos")]
use objc2::MainThreadMarker;
#[cfg(target_os = "macos")]
use objc2_app_kit::{NSApplication, NSApplicationActivationPolicy, NSEventMask, NSWindowOcclusionState};
#[cfg(target_os = "macos")]
use objc2_foundation::{NSDate, NSDefaultRunLoopMode};
use serde_json::json;
#[cfg(target_os = "windows")]
use std::time::Duration;
use vst3_host::{EditorWindow, UiThread};

/// Starts only this dedicated executable's `AppKit` application, leaving other hosts untouched.
#[cfg(target_os = "macos")]
pub fn start_application() -> Result<()> {
    let marker = MainThreadMarker::new().context("editor benchmark requires the main thread")?;
    let application = NSApplication::sharedApplication(marker);
    application.setActivationPolicy(NSApplicationActivationPolicy::Regular);
    application.finishLaunching();
    application.activate();
    Ok(())
}

/// The shared Win32 native window backend initializes its own resources.
#[cfg(target_os = "windows")]
#[expect(clippy::unnecessary_wraps, reason = "shared fallible native startup contract")]
pub const fn start_application() -> Result<()> {
    Ok(())
}

/// Dispatches real application events without synthetic desktop input or pixel inspection.
#[cfg(target_os = "macos")]
#[expect(
    unsafe_code,
    reason = "private main-thread AppKit event loop for this dedicated host"
)]
pub fn handle_events(_ui: &UiThread) {
    let Some(marker) = MainThreadMarker::new() else { return };
    let application = NSApplication::sharedApplication(marker);
    let until = NSDate::dateWithTimeIntervalSinceNow(0.01);
    // SAFETY: the native application and run-loop inputs remain live on their owner main thread.
    if let Some(event) = unsafe {
        application.nextEventMatchingMask_untilDate_inMode_dequeue(
            NSEventMask::Any,
            Some(&until),
            NSDefaultRunLoopMode,
            true,
        )
    } {
        application.sendEvent(&event);
    }
    application.updateWindows();
}

/// Pumps this host's Win32 event queue on its registered owner thread.
#[cfg(target_os = "windows")]
pub fn handle_events(ui: &UiThread) {
    vst3_host::pump_platform_events(ui, Duration::from_millis(10));
}

/// Reads only this host's own window, reporting maximum refresh capability separately from achieved encoding.
#[cfg(target_os = "macos")]
pub fn native_observation(_ui: &UiThread, _editor: &EditorWindow) -> Result<serde_json::Value> {
    let marker = MainThreadMarker::new().context("native observation requires main thread")?;
    let application = NSApplication::sharedApplication(marker);
    let windows = application.windows();
    let window = windows
        .iter()
        .find(|window| window.title().to_string() == "ImageControl visible benchmark")
        .context("benchmark native window unavailable")?;
    let content = window.contentView().context("native content view")?;
    let bounds = content.bounds();
    let screen = window.screen();
    let maximum_refresh = screen
        .as_ref()
        .map(|screen| screen.maximumFramesPerSecond())
        .filter(|refresh| *refresh > 0);
    let visible = window.isVisible() && !window.isMiniaturized();
    let occlusion_visible = window.occlusionState().contains(NSWindowOcclusionState::Visible);
    Ok(json!({
        "observed_unix_seconds": super::unix_seconds()?,
        "window_visible": visible,
        "occlusion_state_visible": occlusion_visible,
        "logical_width": bounds.size.width,
        "logical_height": bounds.size.height,
        "backing_scale": window.backingScaleFactor(),
        "physical_width": bounds.size.width * window.backingScaleFactor(),
        "physical_height": bounds.size.height * window.backingScaleFactor(),
        "expected_display_refresh_hz": maximum_refresh,
        "refresh_source": "NSScreen.maximumFramesPerSecond capability, not current variable refresh",
        "physical_scanout_observed": false,
        "display_name": screen.map(|screen| screen.localizedName().to_string())
    }))
}

/// Queries only the benchmark's owned Win32 window, without discovering other desktop windows.
#[cfg(target_os = "windows")]
pub fn native_observation(ui: &UiThread, editor: &EditorWindow) -> Result<serde_json::Value> {
    let observation = editor
        .native_observation(ui)
        .context("observe benchmark native window")?;
    let physical_width = f64::from(observation.physical_size.width);
    let physical_height = f64::from(observation.physical_size.height);
    Ok(json!({
        "observed_unix_seconds": super::unix_seconds()?,
        "window_visible": observation.visible && !observation.minimized,
        "window_minimized": observation.minimized,
        "occlusion_state_visible": null,
        "logical_width": physical_width / observation.content_scale,
        "logical_height": physical_height / observation.content_scale,
        "backing_scale": observation.content_scale,
        "physical_width": observation.physical_size.width,
        "physical_height": observation.physical_size.height,
        "expected_display_refresh_hz": null,
        "refresh_source": "unavailable through the current host window API",
        "physical_scanout_observed": false
    }))
}
