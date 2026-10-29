//! Entry point of the `AudioBatch` RS desktop app.
//!
//! Usage: `AudioBatchRs [paths...]` analyzes the given files and folders at startup,
//! `--version` prints the version, build time, branch, and commit and quits,
//! and `--autoplay` then selects and plays the first file that analyzed successfully.
//! `AudioBatchRs --demo-rows <count>` runs the table and waveform stress test with synthetic data instead.
//! Messages go to the `AudioBatchRs.log` log file and to stderr.
//! Setting `AUDIOBATCH_GUI_DEBUG` adds diagnostics to them.
//!
//! Only one app runs: a second launch hands its paths to the running one and quits.
//!
//! The executable also serves as the plugin scan worker,
//! so a plugin that crashes while it is scanned takes down only the worker process.
//! The worker flag is checked before anything else starts.
//! The main thread is the VST3 UI thread, and a foreground task runs the plugin jobs other threads send to it.

#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

// The Action derive generates `gpui::` paths, which resolve through the kit under this name.
extern crate gpui_kit as gpui;

mod app;
mod build_info;
mod controls;
mod demo;
mod files;
mod info_panel;
mod menus;
mod normalization;
mod operation_run;
mod plugins;
mod results;
mod settings;
mod settings_window;
mod single_instance;
#[cfg(feature = "snapshot")]
mod snapshot;
#[cfg(test)]
mod test_helpers;
mod theme;
mod waveform;

use std::path::PathBuf;

use audiobatch_core::log_file;
use audiobatch_plugins::{is_scan_worker_invocation, run_scan_worker};
use gpui_kit::{App, AppContext as _, Bounds, TitlebarOptions, WindowBounds, WindowOptions, px, size};
use tracing::level_filters::LevelFilter;
use vst3_host::{UiDispatcher, UiThread, ui_channel};

use crate::app::{APP_TITLE, AppView, LaunchOptions};

/// Default window width.
const DEFAULT_WINDOW_WIDTH: f32 = 1920.0;

/// Default window height.
const DEFAULT_WINDOW_HEIGHT: f32 = 1080.0;

/// Room for the title bar, which the platform adds above the window content.
const TITLE_BAR_ALLOWANCE: f32 = 40.0;

/// Space left around the restored window on displays smaller than the default size.
const DISPLAY_MARGIN: f32 = 40.0;

/// Smallest window size.
const MINIMUM_WINDOW_SIZE: (f32, f32) = (800.0, 600.0);

/// Returns true when `AUDIOBATCH_GUI_DEBUG` is set.
/// The app then adds the waveform frame rate, analysis summaries, and player events to the log.
#[must_use]
pub fn debug_enabled() -> bool {
    std::env::var_os("AUDIOBATCH_GUI_DEBUG").is_some()
}

/// Runs the plugin scan worker when started as one.
/// Otherwise parses the launch options and opens the main window, quitting when it closes.
fn main() {
    if is_scan_worker_invocation(std::env::args_os()) {
        std::process::exit(run_scan_worker());
    }
    // Checked before logging and the single instance handoff, so it never reaches a running app.
    if is_version_request(std::env::args().skip(1)) {
        println!("{}", build_info::VERSION_STRING);
        return;
    }
    let options = match parse_arguments(std::env::args().skip(1)) {
        Ok(options) => options,
        Err(message) => {
            eprintln!("{message}");
            eprintln!("Usage: AudioBatchRs [--version] [--autoplay] [--demo-rows <count>] [paths...]");
            return;
        }
    };
    // Logging also writes each message to stderr when it is available.
    if let Err(error) = log_file::install(env!("CARGO_BIN_NAME"), LevelFilter::TRACE) {
        let error = anyhow::Error::from(error);
        eprintln!("{error:#}");
    }
    tracing::info!("{}", build_info::VERSION_STRING);
    // A snapshot run always opens its own window instead of handing its paths to a running app.
    let launches = if cfg!(feature = "snapshot") {
        None
    } else {
        match single_instance::claim(&options.paths) {
            single_instance::Claim::Forwarded => return,
            single_instance::Claim::Primary(launches) => Some(launches),
        }
    };

    gpui_kit::application()
        .with_assets(gpui_kit::assets::Assets)
        .run(move |cx: &mut App| {
            gpui_kit::init(cx);
            theme::install(cx);
            menus::install(cx);
            let dispatcher = match start_plugin_jobs(cx) {
                Ok(dispatcher) => dispatcher,
                Err(error) => {
                    tracing::error!("Failed to start the plugin host: {error:#}");
                    cx.quit();
                    return;
                }
            };
            let window_options = WindowOptions {
                window_bounds: Some(initial_window_bounds(cx)),
                window_min_size: Some(size(px(MINIMUM_WINDOW_SIZE.0), px(MINIMUM_WINDOW_SIZE.1))),
                titlebar: Some(TitlebarOptions {
                    title: Some(APP_TITLE.into()),
                    ..Default::default()
                }),
                focus: true,
                ..Default::default()
            };
            let window = match gpui_kit::open_window(window_options, cx, |window, cx| {
                cx.new(|cx| {
                    let mut view = AppView::new(&options, dispatcher, window, cx);
                    if let Some(launches) = launches.clone() {
                        view.listen_for_launches(launches, window, cx);
                    }
                    view
                })
            }) {
                Ok((window, _)) => window,
                Err(error) => {
                    tracing::error!("Failed to open the main window: {error:#}");
                    cx.quit();
                    return;
                }
            };
            #[cfg(feature = "snapshot")]
            snapshot::schedule(window, cx);
            // Closing the main window quits, also while the Audio Settings window is open.
            let main_window = window.window_id();
            cx.on_window_closed(move |cx, closed_window| {
                if closed_window == main_window || cx.windows().is_empty() {
                    cx.quit();
                }
            })
            .detach();
            cx.activate(true);
        });
}

/// Registers the main thread as the VST3 UI thread,
/// and runs the plugin jobs that other threads send to it on the foreground executor for the life of the app.
/// Returns the dispatcher that sends those jobs.
///
/// # Errors
///
/// Returns an error when another thread is already registered as the UI thread.
fn start_plugin_jobs(cx: &App) -> anyhow::Result<UiDispatcher> {
    let ui = UiThread::register_current()?;
    let (dispatcher, jobs) = ui_channel();
    cx.spawn(async move |_| {
        while let Some(job) = jobs.recv().await {
            job.run(&ui);
        }
    })
    .detach();
    Ok(dispatcher)
}

/// Returns where the main window opens on the main display.
///
/// The default size opens centered in the visible area, the part not covered by the menu bar, Dock, or taskbar.
/// When it does not fit there, as on a laptop screen, the window opens maximized,
/// which fills the visible area exactly on both macOS and Windows,
/// and restores to the default size shrunk to fit.
fn initial_window_bounds(cx: &App) -> WindowBounds {
    let default_size = size(px(DEFAULT_WINDOW_WIDTH), px(DEFAULT_WINDOW_HEIGHT));
    let Some(visible) = cx.primary_display().map(|display| display.visible_bounds()) else {
        return WindowBounds::Windowed(Bounds::centered(None, default_size, cx));
    };
    let available = size(visible.size.width, visible.size.height - px(TITLE_BAR_ALLOWANCE));
    if default_size.width <= available.width && default_size.height <= available.height {
        return WindowBounds::Windowed(Bounds::centered(None, default_size, cx));
    }
    let shrunk = size(
        available.width - px(DISPLAY_MARGIN),
        available.height - px(DISPLAY_MARGIN),
    );
    WindowBounds::Maximized(Bounds::centered(None, default_size.min(&shrunk), cx))
}

/// Returns true when the arguments after the program name ask for the version with "--version" or "-V".
fn is_version_request(mut arguments: impl Iterator<Item = String>) -> bool {
    arguments.any(|argument| argument == "--version" || argument == "-V")
}

/// Parses the command line into launch options.
///
/// The process serial number argument that macOS can pass to an app launched from the Finder is skipped.
fn parse_arguments(arguments: impl Iterator<Item = String>) -> Result<LaunchOptions, String> {
    let mut options = LaunchOptions::default();
    let mut arguments = arguments;
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            serial_number if serial_number.starts_with("-psn_") => {}
            "--demo-rows" => {
                let value = arguments.next().ok_or("--demo-rows needs a row count")?;
                let rows = value
                    .parse::<usize>()
                    .map_err(|_| format!("Invalid row count: {value}"))?;
                options.demo_rows = Some(rows);
            }
            "--autoplay" => options.autoplay = true,
            "-h" | "--help" => return Err(String::from("AudioBatchRs")),
            other if other.starts_with('-') => return Err(format!("Unknown option: {other}")),
            path => options.paths.push(PathBuf::from(path)),
        }
    }
    Ok(options)
}

#[cfg(test)]
mod test_parse_arguments {
    use super::*;

    /// Parses the launch arguments after the program name.
    fn parse(arguments: &[&str]) -> Result<LaunchOptions, String> {
        parse_arguments(arguments.iter().map(ToString::to_string))
    }

    #[test]
    fn collects_paths_and_flags() {
        let options = parse(&["--autoplay", "first.wav", "folder"]).expect("valid arguments");
        assert!(options.autoplay);
        assert_eq!(options.paths, [PathBuf::from("first.wav"), PathBuf::from("folder")]);
        assert_eq!(options.demo_rows, None);
    }

    #[test]
    fn reads_demo_row_count() {
        let options = parse(&["--demo-rows", "250"]).expect("valid arguments");
        assert_eq!(options.demo_rows, Some(250));
        assert!(parse(&["--demo-rows"]).is_err());
        assert!(parse(&["--demo-rows", "many"]).is_err());
    }

    #[test]
    fn skips_finder_process_serial_number() {
        let options = parse(&["-psn_0_123456", "track.aif"]).expect("valid arguments");
        assert_eq!(options.paths, [PathBuf::from("track.aif")]);
    }

    #[test]
    fn recognizes_version_request() {
        let arguments = |list: &[&str]| list.iter().map(ToString::to_string).collect::<Vec<_>>().into_iter();
        assert!(is_version_request(arguments(&["--version"])));
        assert!(is_version_request(arguments(&["track.aif", "-V"])));
        assert!(!is_version_request(arguments(&["--autoplay", "track.aif"])));
    }

    #[test]
    fn rejects_unknown_options() {
        assert_eq!(
            parse(&["--verbose"]).err().as_deref(),
            Some("Unknown option: --verbose")
        );
    }
}
