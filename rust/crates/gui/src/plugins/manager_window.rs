//! The Plugin Manager window: scanning, the scan folders, the known effects, and the files a scan skipped.
//!
//! The scan group starts and cancels a scan and shows its progress and the summary of the last one.
//! The folder group lists the folders a scan searches.
//! Without custom folders those are the platform's default VST3 folders,
//! and adding or removing a folder switches to a custom list that starts from the folders shown.
//! The plugin list shows the known effects by vendor, each with a button that adds it to the chain.
//! The last group lists the bundles that crashed a scan, which later scans skip until they are removed
//! from the blacklist, and the bundles the last scan skipped because they took too long or had no effects.
//! The main view owns the known plugins: this view sends every change as a `ManagerEvent`,
//! and the main view applies it and shows the window a new snapshot.

use std::path::{Path, PathBuf};

use audiobatch_plugins::PluginDescription;
use gpui_kit::component::Sizable as _;
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::{
    Context, Div, EventEmitter, FocusHandle, Focusable, InteractiveElement as _, IntoElement, ParentElement as _,
    Render, StatefulInteractiveElement as _, Styled as _, Window, div, px,
};

use super::labels::VendorGroup;
use super::{ROW_SPACING, SMALL_FONT_SIZE, WINDOW_FONT_SIZE, WINDOW_PADDING, hint, panel};
use crate::menus::{ClosePluginWindow, PLUGIN_WINDOW_KEY_CONTEXT};
use crate::theme::{MONO_FONT, TEXT_FONT, color, palette, tooltip};

/// Title of the window.
pub const MANAGER_TITLE: &str = "Plugin Manager";

/// Initial width of the window.
pub const MANAGER_WIDTH: f32 = 720.0;

/// Initial height of the window.
pub const MANAGER_HEIGHT: f32 = 640.0;

/// Tooltip of the Scan button.
const SCAN_TOOLTIP: &str = "Search the folders for new and changed VST3 plugins. \
     Each plugin is loaded in a separate process, so a plugin that crashes cannot take down AudioBatchRs.";

/// Something the user did in the plugin manager.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ManagerEvent {
    /// Scan was clicked.
    Scan,
    /// Cancel was clicked during a scan.
    CancelScan,
    /// Add Folder was clicked.
    AddFolder,
    /// A folder was removed from the scan folders.
    RemoveFolder(PathBuf),
    /// The default folders were chosen again.
    UseDefaultFolders,
    /// A bundle was removed from the blacklist.
    RemoveFromBlacklist(PathBuf),
    /// An effect was added to the chain.
    AddToChain(PluginDescription),
}

/// What the window shows, collected by the main view.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ManagerSnapshot {
    /// The known effects by vendor.
    pub groups: Vec<VendorGroup>,
    /// Number of known instruments, which the chain cannot use.
    pub instrument_count: usize,
    /// The folders a scan searches.
    pub folders: Vec<PathBuf>,
    /// True when the folders are a custom list rather than the defaults.
    pub custom_folders: bool,
    /// Bundles that crashed a scan.
    pub blacklist: Vec<PathBuf>,
    /// Bundles the last scan skipped, with the reason.
    pub skipped: Vec<(PathBuf, &'static str)>,
    /// Share of the bundles handled and the bundle being scanned, while a scan runs.
    pub scanning: Option<(f32, String)>,
    /// Summary of the last scan of this session.
    pub last_scan: Option<String>,
}

/// The content of the Plugin Manager window.
pub struct ManagerView {
    focus_handle: FocusHandle,
    snapshot: ManagerSnapshot,
}

impl ManagerView {
    /// Creates the window content showing a snapshot.
    pub fn new(snapshot: ManagerSnapshot, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let focus_handle = cx.focus_handle();
        window.focus(&focus_handle, cx);
        Self { focus_handle, snapshot }
    }

    /// Shows a new snapshot.
    pub fn sync(&mut self, snapshot: ManagerSnapshot, cx: &mut Context<Self>) {
        if snapshot != self.snapshot {
            self.snapshot = snapshot;
            cx.notify();
        }
    }

    /// Renders the scan group: the Scan or Cancel button, the progress, and the last summary.
    fn render_scan(&self, cx: &Context<Self>) -> Div {
        let button = if self.snapshot.scanning.is_some() {
            Button::new("scan-cancel")
                .label("Cancel Scan")
                .small()
                .on_click(cx.listener(|_, _, _, cx| cx.emit(ManagerEvent::CancelScan)))
        } else {
            Button::new("scan-start")
                .label("Scan for Plugins")
                .small()
                .primary()
                .tooltip(SCAN_TOOLTIP)
                .on_click(cx.listener(|_, _, _, cx| cx.emit(ManagerEvent::Scan)))
        };
        let status = match (&self.snapshot.scanning, &self.snapshot.last_scan) {
            (Some((fraction, bundle)), _) => format!("Scanning {:.0}%: {bundle}", fraction * 100.0),
            (None, Some(summary)) => summary.clone(),
            (None, None) => String::from("Scan to find plugins that were installed or updated."),
        };
        panel("Scan").child(
            div()
                .flex()
                .flex_row()
                .items_center()
                .gap(px(WINDOW_PADDING))
                .child(button)
                .child(div().flex_1().min_w_0().truncate().child(status)),
        )
    }

    /// Renders the folder group.
    fn render_folders(&self, cx: &Context<Self>) -> Div {
        let title = if self.snapshot.custom_folders {
            "Scan folders (custom)"
        } else {
            "Scan folders (defaults)"
        };
        let mut group = panel(title);
        for (index, folder) in self.snapshot.folders.iter().enumerate() {
            let removed = folder.clone();
            group = group.child(path_row(
                ("folder", index),
                folder,
                Button::new(("folder-remove", index))
                    .label("Remove")
                    .small()
                    .on_click(cx.listener(move |_, _, _, cx| cx.emit(ManagerEvent::RemoveFolder(removed.clone())))),
            ));
        }
        let mut buttons = div().flex().flex_row().gap(px(ROW_SPACING)).child(
            Button::new("folder-add")
                .label("Add Folder…")
                .small()
                .on_click(cx.listener(|_, _, _, cx| cx.emit(ManagerEvent::AddFolder))),
        );
        if self.snapshot.custom_folders {
            buttons = buttons.child(
                Button::new("folder-defaults")
                    .label("Use Default Folders")
                    .small()
                    .on_click(cx.listener(|_, _, _, cx| cx.emit(ManagerEvent::UseDefaultFolders))),
            );
        }
        group.child(buttons)
    }

    /// Renders the known effects by vendor.
    fn render_plugins(&self, cx: &Context<Self>) -> Div {
        let count: usize = self.snapshot.groups.iter().map(|group| group.plugins.len()).sum();
        let mut list = panel(format!("Effects ({count})"));
        if count == 0 {
            list = list.child(hint("No effects are known yet."));
        }
        let mut index = 0_usize;
        for group in &self.snapshot.groups {
            list = list.child(
                div()
                    .pt(px(4.0))
                    .text_size(px(SMALL_FONT_SIZE))
                    .text_color(color(palette::GREY_MIDDLE_LIGHT))
                    .child(group.vendor.clone()),
            );
            for plugin in &group.plugins {
                let added = plugin.clone();
                let detail = [plugin.version.as_str(), plugin.category.as_str()]
                    .iter()
                    .filter(|part| !part.is_empty())
                    .copied()
                    .collect::<Vec<_>>()
                    .join(", ");
                list = list.child(
                    div()
                        .id(("plugin", index))
                        .flex()
                        .flex_row()
                        .items_center()
                        .gap(px(ROW_SPACING))
                        .tooltip(tooltip(plugin.bundle.display().to_string()))
                        .child(div().flex_1().min_w_0().truncate().child(plugin.name.clone()))
                        .child(
                            div()
                                .flex_none()
                                .font_family(MONO_FONT)
                                .text_size(px(SMALL_FONT_SIZE))
                                .text_color(color(palette::GREY_MIDDLE_LIGHT))
                                .child(detail),
                        )
                        .child(
                            Button::new(("plugin-add", index))
                                .label("Add to Chain")
                                .small()
                                .on_click(
                                    cx.listener(move |_, _, _, cx| cx.emit(ManagerEvent::AddToChain(added.clone()))),
                                ),
                        ),
                );
                index += 1;
            }
        }
        if self.snapshot.instrument_count > 0 {
            list = list.child(hint(format!(
                "{} instruments are not listed, because the chain only processes audio.",
                self.snapshot.instrument_count
            )));
        }
        list
    }

    /// Renders the blacklisted bundles and the bundles the last scan skipped.
    fn render_problems(&self, cx: &Context<Self>) -> Div {
        let mut group = panel("Skipped files");
        if self.snapshot.blacklist.is_empty() && self.snapshot.skipped.is_empty() {
            return group.child(hint("No plugin files were skipped."));
        }
        for (index, bundle) in self.snapshot.blacklist.iter().enumerate() {
            let removed = bundle.clone();
            group = group.child(path_row(
                ("blacklisted", index),
                bundle,
                Button::new(("blacklist-remove", index))
                    .label("Scan Again")
                    .small()
                    .tooltip("Remove the file from the blacklist, so the next scan tries it again.")
                    .on_click(
                        cx.listener(move |_, _, _, cx| cx.emit(ManagerEvent::RemoveFromBlacklist(removed.clone()))),
                    ),
            ));
        }
        if !self.snapshot.blacklist.is_empty() {
            group = group.child(hint("Blacklisted after crashing a scan. Later scans skip them."));
        }
        for (index, (bundle, reason)) in self.snapshot.skipped.iter().enumerate() {
            group = group.child(
                div()
                    .id(("skipped", index))
                    .flex()
                    .flex_row()
                    .gap(px(ROW_SPACING))
                    .tooltip(tooltip(bundle.display().to_string()))
                    .child(div().flex_1().min_w_0().truncate().child(file_name(bundle)))
                    .child(
                        div()
                            .flex_none()
                            .text_size(px(SMALL_FONT_SIZE))
                            .text_color(color(palette::GREY_MIDDLE_LIGHT))
                            .child(*reason),
                    ),
            );
        }
        group
    }
}

impl EventEmitter<ManagerEvent> for ManagerView {}

impl Focusable for ManagerView {
    fn focus_handle(&self, _cx: &gpui_kit::App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for ManagerView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .id("plugin-manager")
            .key_context(PLUGIN_WINDOW_KEY_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(|_, _: &ClosePluginWindow, window, _| window.remove_window()))
            .size_full()
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap(px(WINDOW_PADDING))
            .p(px(WINDOW_PADDING + 4.0))
            .bg(color(palette::GREY_DARK))
            .text_color(color(palette::GREY_LIGHT))
            .font_family(TEXT_FONT)
            .text_size(px(WINDOW_FONT_SIZE))
            .child(self.render_scan(cx))
            .child(self.render_folders(cx))
            .child(self.render_plugins(cx))
            .child(self.render_problems(cx))
    }
}

/// Renders a row with a path, its full text as the tooltip, and a button.
fn path_row(id: (&'static str, usize), path: &Path, button: Button) -> impl IntoElement {
    div()
        .id(id)
        .flex()
        .flex_row()
        .items_center()
        .gap(px(ROW_SPACING))
        .tooltip(tooltip(path.display().to_string()))
        .child(div().flex_1().min_w_0().truncate().child(path.display().to_string()))
        .child(button)
}

/// Returns the file name of a path for the lists.
fn file_name(path: &Path) -> String {
    path.file_name().map_or_else(
        || path.display().to_string(),
        |name| name.to_string_lossy().into_owned(),
    )
}
