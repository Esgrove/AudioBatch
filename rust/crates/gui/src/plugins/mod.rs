//! Plugin hosting in the desktop app: the known plugins, the live chain, scanning, the live preview,
//! and the plugin windows.
//!
//! `PluginHost` holds the plugin state the main view owns.
//! The known plugins come from `plugins.json`, which scans and the CLI also write,
//! the live chain from `plugin-chain.json`, and the chain presets from the `chain-presets` folder.
//! A scan runs on a background thread and starts the executable again as its worker process,
//! so a plugin that crashes while it is scanned takes down only the worker.
//! The chain editor and the plugin manager are separate windows that show snapshots of this state
//! and send what the user does back to the main view as events.

pub mod chain_window;
pub mod labels;
pub mod live_chain;
pub mod manager_window;
pub mod preview;

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use audiobatch_plugins::{KnownPlugins, PluginDataPaths, PresetStore, ScanReport};
use gpui_kit::{Div, ParentElement as _, SharedString, Styled as _, Task, div, px};
use raw_window_handle::RawWindowHandle;
use vst3_host::{UiDispatcher, UiThread};

use crate::info_panel::{CORNER_RADIUS, OUTLINE_ALPHA};
use crate::theme::{color, palette, white};
use live_chain::LiveChain;
use preview::PluginPreview;

/// Padding inside a window and its panels.
pub const WINDOW_PADDING: f32 = 12.0;

/// Space between the rows of a panel.
pub const ROW_SPACING: f32 = 6.0;

/// Font size of the window text.
pub const WINDOW_FONT_SIZE: f32 = 13.0;

/// Font size of the panel headings and the hints.
pub const SMALL_FONT_SIZE: f32 = 12.0;

/// Sample rate the live instances are prepared at while no output device is open.
pub const FALLBACK_SAMPLE_RATE: u32 = 48_000;

/// The plugin state of the main view.
pub struct PluginHost {
    /// Runs plugin calls from other threads on the UI thread.
    pub dispatcher: UiDispatcher,
    /// Where the plugin data files are.
    pub paths: PluginDataPaths,
    /// The plugins found by scans, the blacklist, and the scan folders.
    pub known: KnownPlugins,
    /// The chain the user edits.
    pub chain: LiveChain,
    /// The saved chain presets.
    pub presets: PresetStore,
    /// What the player holds of the chain for the live preview.
    pub preview: PluginPreview,
    /// The main window, which owns the plugin editor windows on Windows.
    pub editor_owner: Option<RawWindowHandle>,
    /// The scan in progress.
    pub scan: Option<RunningScan>,
    /// What the last scan of this session found.
    pub last_report: Option<ScanReport>,
}

/// A scan running on a background thread.
pub struct RunningScan {
    /// Set to stop the scan at the next bundle.
    pub cancel: Arc<AtomicBool>,
    /// Share of the bundles handled so far, and the bundle being scanned.
    pub progress: Arc<Mutex<(f32, PathBuf)>>,
    /// Waits for the scan and applies its result. Dropping it stops listening to the scan.
    pub task: Task<()>,
}

impl PluginHost {
    /// Loads the known plugins and the saved chain, preparing the chain's instances at `sample_rate`.
    /// Plugin editors open as windows owned by `editor_owner`.
    /// Returns the host and the messages of the files that could not be read.
    #[must_use]
    pub fn open(
        dispatcher: UiDispatcher,
        sample_rate: u32,
        editor_owner: Option<RawWindowHandle>,
    ) -> (Self, Vec<String>) {
        let paths = PluginDataPaths::default();
        let mut problems = Vec::new();
        let known = match KnownPlugins::load_recovering(&paths.known_plugins) {
            Ok((known, message)) => {
                problems.extend(message);
                known
            }
            Err(error) => {
                problems.push(format!("{error:#}"));
                KnownPlugins::default()
            }
        };
        let chain = match UiThread::current() {
            Some(ui) => {
                let (chain, error) = LiveChain::load(&ui, paths.chain.clone(), dispatcher.clone(), sample_rate);
                problems.extend(error);
                chain
            }
            None => LiveChain::new(paths.chain.clone(), dispatcher.clone(), sample_rate),
        };
        let presets = PresetStore::new(paths.presets.clone());
        let host = Self {
            dispatcher,
            paths,
            known,
            chain,
            presets,
            preview: PluginPreview::default(),
            editor_owner,
            scan: None,
            last_report: None,
        };
        (host, problems)
    }

    /// Returns true while a scan runs.
    #[must_use]
    pub const fn is_scanning(&self) -> bool {
        self.scan.is_some()
    }

    /// Returns the progress of the scan in progress: the share of the bundles handled and the bundle's name.
    #[must_use]
    pub fn scan_progress(&self) -> Option<(f32, String)> {
        let scan = self.scan.as_ref()?;
        let (fraction, bundle) = scan.progress.lock().map(|progress| progress.clone()).ok()?;
        let name = bundle
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_default();
        Some((fraction, name))
    }

    /// Returns the names of the saved presets in the order they are listed,
    /// or none when the preset folder cannot be read.
    #[must_use]
    pub fn preset_names(&self) -> Vec<String> {
        self.presets
            .list()
            .map(|presets| presets.into_iter().map(|preset| preset.name).collect())
            .unwrap_or_default()
    }

    /// Asks the scan in progress to stop at the next bundle.
    pub fn cancel_scan(&self) {
        if let Some(scan) = &self.scan {
            scan.cancel.store(true, Ordering::Relaxed);
        }
    }
}

/// Renders a titled panel of rows, the way the settings window groups its options.
pub fn panel(title: impl Into<SharedString>) -> Div {
    div()
        .flex()
        .flex_col()
        .gap(px(ROW_SPACING))
        .p(px(WINDOW_PADDING))
        .rounded(px(CORNER_RADIUS))
        .bg(color(palette::GREY_SEMI_DARK))
        .border_1()
        .border_color(white(OUTLINE_ALPHA))
        .child(
            div()
                .pb(px(2.0))
                .text_size(px(SMALL_FONT_SIZE))
                .text_color(color(palette::GREY_MIDDLE_LIGHT))
                .child(title.into()),
        )
}

/// Renders a dimmed hint line.
pub fn hint(text: impl Into<SharedString>) -> Div {
    div()
        .text_size(px(SMALL_FONT_SIZE))
        .text_color(color(palette::GREY_MIDDLE))
        .child(text.into())
}
