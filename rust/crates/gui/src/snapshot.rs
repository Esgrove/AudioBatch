//! Saves a PNG image of the main window, for checking the layout without screen recording permission.
//!
//! Built only with the `snapshot` feature, which enables the GPUI test support that renders a frame to an image.
//! `AUDIOBATCH_GUI_SNAPSHOT=<path>` saves the window after `AUDIOBATCH_GUI_SNAPSHOT_DELAY` seconds, five by default,
//! and then quits.
//! `AUDIOBATCH_GUI_SNAPSHOT_ACTIONS` takes a comma separated list of action names,
//! such as `audiobatch::ToggleLoudnessGraph`, dispatched one second before the image is taken.
//! Setting `AUDIOBATCH_GUI_SNAPSHOT_ACTIVE_WINDOW` saves the active window instead of the main window,
//! for example the Audio Settings window that `audiobatch::OpenAudioSettings` opens.

use std::path::PathBuf;
use std::time::Duration;

use gpui_kit::{AnyWindowHandle, App};

/// Delay before the image is taken when `AUDIOBATCH_GUI_SNAPSHOT_DELAY` is not set.
const DEFAULT_DELAY: Duration = Duration::from_secs(5);

/// Time between dispatching the actions and taking the image, so their effects are drawn.
const ACTION_SETTLE_TIME: Duration = Duration::from_secs(1);

/// Schedules the image when `AUDIOBATCH_GUI_SNAPSHOT` is set.
pub fn schedule(window: AnyWindowHandle, cx: &App) {
    let Some(path) = std::env::var_os("AUDIOBATCH_GUI_SNAPSHOT").map(PathBuf::from) else {
        return;
    };
    let delay = std::env::var("AUDIOBATCH_GUI_SNAPSHOT_DELAY")
        .ok()
        .and_then(|seconds| seconds.parse::<f64>().ok())
        .and_then(|seconds| Duration::try_from_secs_f64(seconds).ok())
        .unwrap_or(DEFAULT_DELAY);
    let actions: Vec<String> = std::env::var("AUDIOBATCH_GUI_SNAPSHOT_ACTIONS")
        .map(|names| names.split(',').map(|name| name.trim().to_string()).collect())
        .unwrap_or_default();
    let capture_active_window = std::env::var_os("AUDIOBATCH_GUI_SNAPSHOT_ACTIVE_WINDOW").is_some();
    cx.spawn(async move |cx| {
        cx.background_executor()
            .timer(delay.saturating_sub(ACTION_SETTLE_TIME))
            .await;
        let _ = window.update(cx, |_, window, cx| {
            for name in actions.iter().filter(|name| !name.is_empty()) {
                match cx.build_action(name, None) {
                    Ok(action) => window.dispatch_action(action, cx),
                    Err(error) => tracing::error!("Unknown action {name}: {error}"),
                }
            }
            window.refresh();
        });
        cx.background_executor().timer(ACTION_SETTLE_TIME).await;
        let target = if capture_active_window {
            cx.update(|cx| cx.active_window()).unwrap_or(window)
        } else {
            window
        };
        let _ = target.update(cx, |_, window, cx| {
            match window.render_to_image() {
                Ok(image) => match image.save(&path) {
                    Ok(()) => tracing::info!("Saved a snapshot to {}", path.display()),
                    Err(error) => tracing::error!("Failed to save the snapshot: {error}"),
                },
                Err(error) => tracing::error!("Failed to render the snapshot: {error:#}"),
            }
            cx.quit();
        });
    })
    .detach();
}
