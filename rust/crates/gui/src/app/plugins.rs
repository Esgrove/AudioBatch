//! The plugin chain, the plugin windows, and plugin scanning of the main view.
//!
//! Every change to the chain, from the Plugin Chain window, the Plugin Manager, or the Plugins menu,
//! captures the settings of the instances, saves the chain, and refreshes the menus and the open windows.
//! Saving a preset captures the settings first, and loading one replaces every slot with the preset's slots.
//! Changes to the scan folders and the blacklist take the scan lock and reload `plugins.json` first,
//! so they do not undo what a scan in the CLI wrote in the meantime.
//! A housekeeping tick prepares instances again after a change of the output rate
//! or a plugin's restart request, keeps the live preview in step, and shows the progress of a scan.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Context as _;
use audiobatch_core::format::format_kilohertz;
use audiobatch_playback::Player;
use audiobatch_plugins::{
    KnownPlugins, PluginDescription, SCAN_BUSY_MESSAGE, ScanLock, ScanOptions, ScanReport, scan_plugins,
};
use gpui_kit::{
    AnyWindowHandle, App, AppContext as _, Bounds, Context, Entity, PathPromptOptions, Subscription, Task,
    TitlebarOptions, WeakEntity, WindowBounds, WindowOptions, px, size,
};
use vst3_host::UiThread;

use super::AppView;
use crate::menus::{AddPluginToChain, set_menus};
use crate::plugins::chain_window::{
    CHAIN_HEIGHT, CHAIN_TITLE, CHAIN_WIDTH, ChainEvent, ChainRow, ChainSnapshot, ChainView,
};
use crate::plugins::labels::{chain_summary, group_by_vendor, latency_text, plugin_label, slot_label};
use crate::plugins::live_chain::{LiveSlot, SlotId};
use crate::plugins::manager_window::{
    MANAGER_HEIGHT, MANAGER_TITLE, MANAGER_WIDTH, ManagerEvent, ManagerSnapshot, ManagerView,
};
use crate::plugins::{FALLBACK_SAMPLE_RATE, RunningScan};

/// How often the plugin housekeeping runs.
const PLUGIN_TICK_INTERVAL: Duration = Duration::from_millis(100);

/// Smallest size of the plugin windows.
const PLUGIN_WINDOW_MINIMUM_SIZE: (f32, f32) = (480.0, 300.0);

/// Status text when the scan folders or the blacklist are changed during a scan.
const WAIT_FOR_SCAN: &str = "Wait for the plugin scan to finish";

/// An open plugin window and the subscription to its events.
pub(super) struct PluginWindow<V: 'static> {
    handle: AnyWindowHandle,
    view: WeakEntity<V>,
    _subscription: Subscription,
}

impl<V: 'static> PluginWindow<V> {
    /// Returns true when the window was closed.
    fn is_closed(&self) -> bool {
        self.view.upgrade().is_none()
    }
}

impl AppView {
    /// Returns the rate the live instances run at: the output rate of the player.
    pub(super) fn preview_sample_rate(&self) -> u32 {
        self.player.as_ref().map_or(FALLBACK_SAMPLE_RATE, Player::output_rate)
    }

    /// Returns the summary of the chain, for example "2 plugins".
    pub(super) fn chain_summary(&self) -> String {
        let chain = &self.plugins.chain;
        chain_summary(chain.enabled_count(), chain.len())
    }

    /// Opens the Plugin Chain window, or brings it to the front when it is already open.
    pub(super) fn open_chain_window(&mut self, cx: &mut Context<Self>) {
        if activate(self.chain_window.as_ref(), cx) {
            return;
        }
        let snapshot = self.chain_snapshot();
        let options = plugin_window_options(CHAIN_TITLE, CHAIN_WIDTH, CHAIN_HEIGHT, cx);
        match gpui_kit::open_window(options, cx, move |window, cx| {
            cx.new(|cx| ChainView::new(snapshot, window, cx))
        }) {
            Ok((handle, view)) => {
                let subscription = cx.subscribe(&view, Self::on_chain_event);
                self.chain_window = Some(PluginWindow {
                    handle,
                    view: view.downgrade(),
                    _subscription: subscription,
                });
            }
            Err(error) => self.report_status(format!("Could not open the plugin chain: {error:#}"), cx),
        }
    }

    /// Opens the Plugin Manager window, or brings it to the front when it is already open.
    pub(super) fn open_manager_window(&mut self, cx: &mut Context<Self>) {
        if activate(self.manager_window.as_ref(), cx) {
            return;
        }
        let snapshot = self.manager_snapshot();
        let options = plugin_window_options(MANAGER_TITLE, MANAGER_WIDTH, MANAGER_HEIGHT, cx);
        match gpui_kit::open_window(options, cx, move |window, cx| {
            cx.new(|cx| ManagerView::new(snapshot, window, cx))
        }) {
            Ok((handle, view)) => {
                let subscription = cx.subscribe(&view, Self::on_manager_event);
                self.manager_window = Some(PluginWindow {
                    handle,
                    view: view.downgrade(),
                    _subscription: subscription,
                });
            }
            Err(error) => self.report_status(format!("Could not open the plugin manager: {error:#}"), cx),
        }
    }

    /// Adds the effect a Plugins menu item names.
    pub(super) fn add_plugin_from_menu(&mut self, action: &AddPluginToChain, cx: &mut Context<Self>) {
        let bundle = Path::new(action.bundle.as_ref());
        let plugin = self
            .plugins
            .known
            .plugins
            .iter()
            .find(|plugin| plugin.bundle == bundle && plugin.class_id.to_string() == action.class_id.as_ref())
            .cloned();
        match plugin {
            Some(plugin) => self.add_plugin(plugin, cx),
            None => self.report_status(
                String::from("The plugin is no longer known. Scan for plugins again."),
                cx,
            ),
        }
    }

    /// Removes every plugin from the chain.
    pub(super) fn clear_plugin_chain(&mut self, cx: &mut Context<Self>) {
        self.plugins.chain.clear();
        self.status = "Cleared the plugin chain".into();
        self.chain_changed(cx);
    }

    /// Replaces the chain with a saved preset, with the settings of each plugin,
    /// and reports the plugins of the preset that could not be loaded.
    /// The preview chain is taken out of the player first, because the old instances are destroyed.
    pub(super) fn load_chain_preset(&mut self, name: &str, cx: &mut Context<Self>) {
        let Some(ui) = UiThread::current() else {
            return;
        };
        let preset = match self.plugins.presets.load(name) {
            Ok(preset) => preset,
            Err(error) => return self.report_status(format!("Could not load the preset: {error:#}"), cx),
        };
        self.detach_plugin_preview();
        let preset_name = preset.name.clone();
        self.plugins.chain.replace(&ui, preset.chain(), Some(preset.name));
        let failed: Vec<String> = self
            .plugins
            .chain
            .slots()
            .iter()
            .filter(|slot| slot.error().is_some())
            .map(|slot| plugin_label(slot.plugin()))
            .collect();
        self.status = if failed.is_empty() {
            format!("Loaded the preset {preset_name}")
        } else {
            format!(
                "Loaded the preset {preset_name}, but these plugins could not be loaded: {}",
                failed.join(", ")
            )
        }
        .into();
        self.chain_changed(cx);
    }

    /// Captures the settings of the instances and saves the chain, reporting a failure on stderr.
    /// Used when the app quits, where the status bar is no longer seen.
    /// The preview chain is taken out of the player first, so it delivers no more edits.
    pub(super) fn save_plugin_chain(&mut self) {
        self.detach_plugin_preview();
        if let Some(ui) = UiThread::current() {
            self.plugins.chain.capture_states(&ui);
        }
        if let Err(error) = self.plugins.chain.save() {
            tracing::error!("{error:#}");
        }
    }

    /// Runs the plugin housekeeping every `PLUGIN_TICK_INTERVAL` for the life of the view.
    pub(super) fn spawn_plugin_tick(cx: &Context<Self>) -> Task<()> {
        cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(PLUGIN_TICK_INTERVAL).await;
                if this.update(cx, Self::plugin_tick).is_err() {
                    break;
                }
            }
        })
    }

    /// Shows the menus and the open plugin windows with the current chain and known plugins.
    pub(super) fn refresh_plugin_views(&mut self, cx: &mut Context<Self>) {
        let summary = self.chain_summary();
        let groups = group_by_vendor(&self.plugins.known.plugins);
        let presets = self.plugins.preset_names();
        let current_preset = self.plugins.chain.preset_name().map(str::to_owned);
        if self.menu_state.chain_summary != summary
            || self.menu_state.plugin_groups != groups
            || self.menu_state.presets != presets
            || self.menu_state.current_preset != current_preset
        {
            self.menu_state.chain_summary = summary;
            self.menu_state.plugin_groups = groups;
            self.menu_state.presets = presets;
            self.menu_state.current_preset = current_preset;
            set_menus(cx, &self.menu_state);
        }
        self.sync_chain_window(cx);
        self.sync_manager_window(cx);
        cx.notify();
    }

    /// Prepares the live instances again after a change of the output rate or a restart request,
    /// keeps the preview in step, forgets closed windows, saves the settings of closed editors,
    /// and shows the progress of a scan.
    fn plugin_tick(&mut self, cx: &mut Context<Self>) {
        if self.chain_window.as_ref().is_some_and(PluginWindow::is_closed) {
            self.chain_window = None;
        }
        if self.manager_window.as_ref().is_some_and(PluginWindow::is_closed) {
            self.manager_window = None;
        }
        let Some(ui) = UiThread::current() else {
            return;
        };
        let prepared = self.sync_plugin_preview(&ui, cx);
        if self.plugins.chain.reap_closed_editors(&ui) {
            self.save_chain_settings(cx);
        } else if prepared {
            self.sync_chain_window(cx);
        }
        if self.plugins.is_scanning() {
            self.sync_manager_window(cx);
        }
    }

    /// Applies what the user did in the Plugin Chain window.
    fn on_chain_event(&mut self, _view: Entity<ChainView>, event: &ChainEvent, cx: &mut Context<Self>) {
        let chain = &mut self.plugins.chain;
        let changed = match event {
            ChainEvent::SetEnabled(id, enabled) => chain.set_enabled(*id, *enabled),
            ChainEvent::Edit(id) => {
                if let Err(error) = self.open_editor(*id) {
                    let label = self
                        .plugins
                        .chain
                        .slot(*id)
                        .map(|slot| plugin_label(slot.plugin()))
                        .unwrap_or_default();
                    self.report_status(format!("{label}: {error:#}"), cx);
                }
                false
            }
            ChainEvent::Move(id, offset) => chain.move_slot(*id, *offset),
            ChainEvent::Remove(id) => chain.remove(*id),
            ChainEvent::Add(plugin) => {
                self.add_plugin(plugin.clone(), cx);
                false
            }
            ChainEvent::Clear => {
                self.clear_plugin_chain(cx);
                false
            }
            ChainEvent::SavePreset(name) => {
                self.save_chain_preset(name, cx);
                false
            }
            ChainEvent::LoadPreset(name) => {
                self.load_chain_preset(name, cx);
                false
            }
            ChainEvent::DeletePreset(name) => {
                self.delete_chain_preset(name, cx);
                false
            }
        };
        if changed {
            self.chain_changed(cx);
        }
    }

    /// Applies what the user did in the Plugin Manager window.
    fn on_manager_event(&mut self, _view: Entity<ManagerView>, event: &ManagerEvent, cx: &mut Context<Self>) {
        match event {
            ManagerEvent::Scan => self.start_plugin_scan(cx),
            ManagerEvent::CancelScan => {
                self.plugins.cancel_scan();
                self.report_status(String::from("Cancelling the plugin scan..."), cx);
            }
            ManagerEvent::AddFolder => Self::prompt_for_plugin_folders(cx),
            ManagerEvent::RemoveFolder(folder) => self.edit_known_plugins(cx, |known| {
                let mut folders = known.search_folders();
                folders.retain(|candidate| candidate != folder);
                known.custom_folders = folders;
            }),
            ManagerEvent::UseDefaultFolders => self.edit_known_plugins(cx, |known| known.custom_folders.clear()),
            ManagerEvent::RemoveFromBlacklist(bundle) => self.edit_known_plugins(cx, |known| {
                known.remove_from_blacklist(bundle);
            }),
            ManagerEvent::AddToChain(plugin) => self.add_plugin(plugin.clone(), cx),
        }
    }

    /// Adds a plugin at the end of the chain and opens its editor, reporting a plugin that could not be loaded.
    /// A plugin without an editor is added without a message about it,
    /// because the user only asked for the plugin.
    fn add_plugin(&mut self, plugin: PluginDescription, cx: &mut Context<Self>) {
        let Some(ui) = UiThread::current() else {
            return;
        };
        let label = plugin_label(&plugin);
        let id = self.plugins.chain.add(&ui, plugin);
        self.status = self
            .plugins
            .chain
            .slot(id)
            .and_then(LiveSlot::error)
            .map_or_else(
                || format!("Added {label} to the plugin chain"),
                |error| format!("{label}: {error}"),
            )
            .into();
        self.chain_changed(cx);
        if self.plugins.chain.slot(id).is_some_and(LiveSlot::is_ready) {
            self.open_editor(id).ok();
        }
    }

    /// Shows the editor of a slot, opening its window when it has none.
    fn open_editor(&mut self, id: SlotId) -> anyhow::Result<()> {
        let ui = UiThread::current().context("Plugin editors can only be opened on the UI thread")?;
        let owner = self.plugins.editor_owner;
        self.plugins.chain.open_editor(&ui, id, owner)
    }

    /// Saves the chain after a change to its structure, shows it in the menus and the windows,
    /// and gives the preview the new chain.
    fn chain_changed(&mut self, cx: &mut Context<Self>) {
        self.save_chain_settings(cx);
        self.refresh_plugin_preview(cx);
    }

    /// Captures the settings of the instances, saves the chain, and shows it in the menus and the windows.
    fn save_chain_settings(&mut self, cx: &mut Context<Self>) {
        self.capture_plugin_states(cx);
        if let Err(error) = self.plugins.chain.save() {
            self.status = format!("{error:#}").into();
        }
        self.refresh_plugin_views(cx);
    }

    /// Saves the chain, with the current settings of each plugin, as the named preset.
    fn save_chain_preset(&mut self, name: &str, cx: &mut Context<Self>) {
        if self.plugins.chain.len() == 0 {
            return self.report_status(String::from("Add a plugin before saving the chain as a preset"), cx);
        }
        self.capture_plugin_states(cx);
        match self.plugins.presets.save(name, &self.plugins.chain.chain()) {
            Ok(path) => {
                let saved = path
                    .file_stem()
                    .map_or_else(|| name.to_owned(), |stem| stem.to_string_lossy().into_owned());
                self.status = format!("Saved the plugin chain as the preset {saved}").into();
                self.plugins.chain.set_preset_name(Some(saved));
            }
            Err(error) => self.status = format!("{error:#}").into(),
        }
        self.save_chain_settings(cx);
    }

    /// Deletes the named preset. The chain itself is not changed.
    fn delete_chain_preset(&mut self, name: &str, cx: &mut Context<Self>) {
        match self.plugins.presets.delete(name) {
            Ok(_) => {
                if self.plugins.chain.preset_name() == Some(name) {
                    self.plugins.chain.set_preset_name(None);
                }
                self.status = format!("Deleted the preset {name}").into();
            }
            Err(error) => self.status = format!("{error:#}").into(),
        }
        self.refresh_plugin_views(cx);
    }

    /// Collects what the Plugin Chain window shows.
    fn chain_snapshot(&self) -> ChainSnapshot {
        let chain = &self.plugins.chain;
        let sample_rate = chain.sample_rate();
        let rows = chain
            .slots()
            .iter()
            .enumerate()
            .map(|(index, slot)| ChainRow {
                id: slot.id(),
                label: slot_label(index + 1, slot.plugin()),
                enabled: slot.is_enabled(),
                detail: slot.error().map_or_else(
                    || latency_text(u64::from(slot.latency_samples()), sample_rate),
                    ToString::to_string,
                ),
                failed: slot.error().is_some(),
                can_edit: slot.has_instance(),
            })
            .collect();
        let available = group_by_vendor(&self.plugins.known.plugins)
            .into_iter()
            .flat_map(|group| group.plugins)
            .map(|plugin| (plugin_label(&plugin), plugin))
            .collect();
        ChainSnapshot {
            rows,
            total_latency: latency_text(chain.total_latency_samples(), sample_rate),
            sample_rate: format!("{} kHz", format_kilohertz(sample_rate)),
            available,
            presets: self.plugins.preset_names(),
            current_preset: chain.preset_name().map(str::to_owned),
        }
    }

    /// Collects what the Plugin Manager window shows.
    fn manager_snapshot(&self) -> ManagerSnapshot {
        let known = &self.plugins.known;
        let skipped = self
            .plugins
            .last_report
            .iter()
            .flat_map(|report| {
                let timed_out = report
                    .timed_out
                    .iter()
                    .map(|bundle| (bundle.clone(), "Timed out".to_string()));
                let failed = report
                    .failed
                    .iter()
                    .map(|(bundle, error)| (bundle.clone(), format!("Could not be loaded: {error}")));
                let no_plugins = report
                    .no_plugins
                    .iter()
                    .map(|bundle| (bundle.clone(), "No audio effects".to_string()));
                timed_out.chain(failed).chain(no_plugins)
            })
            .collect();
        ManagerSnapshot {
            groups: group_by_vendor(&known.plugins),
            instrument_count: known.plugins.iter().filter(|plugin| plugin.is_instrument).count(),
            folders: known.search_folders(),
            custom_folders: !known.custom_folders.is_empty(),
            blacklist: known.blacklist.clone(),
            skipped,
            scanning: self.plugins.scan_progress(),
            last_scan: self.plugins.last_report.as_ref().map(ScanReport::summary),
        }
    }

    /// Shows the current chain in an open Plugin Chain window.
    fn sync_chain_window(&self, cx: &mut Context<Self>) {
        let Some(plugin_window) = &self.chain_window else {
            return;
        };
        let Some(view) = plugin_window.view.upgrade() else {
            return;
        };
        let snapshot = self.chain_snapshot();
        let _ = plugin_window.handle.update(cx, |_, window, cx| {
            view.update(cx, |view, cx| view.sync(snapshot, window, cx));
        });
    }

    /// Shows the current known plugins and scan state in an open Plugin Manager window.
    fn sync_manager_window(&self, cx: &mut Context<Self>) {
        let Some(plugin_window) = &self.manager_window else {
            return;
        };
        let Some(view) = plugin_window.view.upgrade() else {
            return;
        };
        let snapshot = self.manager_snapshot();
        view.update(cx, |view, cx| view.sync(snapshot, cx));
    }

    /// Starts a scan of the scan folders on a background thread.
    /// The scan lock is held until the result is saved, so a scan in the CLI cannot run at the same time.
    fn start_plugin_scan(&mut self, cx: &mut Context<Self>) {
        if self.plugins.is_scanning() {
            return;
        }
        let paths = self.plugins.paths.clone();
        let prepared = ScanLock::try_acquire(&paths.scan_lock).and_then(|lock| {
            let Some(lock) = lock else {
                return Ok(None);
            };
            let (known, recovered) = KnownPlugins::load_recovering(&paths.known_plugins)?;
            let options = ScanOptions::new(known.search_folders(), Some(paths.worker_log.clone()))?;
            Ok(Some((lock, known, options, recovered)))
        });
        let (lock, mut known, options, recovered) = match prepared {
            Ok(Some(prepared)) => prepared,
            Ok(None) => return self.report_status(String::from(SCAN_BUSY_MESSAGE), cx),
            Err(error) => return self.report_status(format!("Could not start the plugin scan: {error:#}"), cx),
        };
        if let Some(message) = recovered {
            self.report_status(message, cx);
        }
        let cancel = Arc::new(AtomicBool::new(false));
        let progress = Arc::new(Mutex::new((0.0, PathBuf::new())));
        let scan = {
            let cancel = Arc::clone(&cancel);
            let progress = Arc::clone(&progress);
            cx.background_executor().spawn(async move {
                let report = scan_plugins(
                    &lock,
                    &mut known,
                    &options,
                    &|| cancel.load(Ordering::Relaxed),
                    &mut |update| {
                        if let Ok(mut shared) = progress.lock() {
                            *shared = (update.fraction, update.bundle.to_path_buf());
                        }
                    },
                )?;
                known
                    .save_to(&paths.known_plugins)
                    .context("Could not save the plugin list")?;
                drop(lock);
                anyhow::Ok((known, report))
            })
        };
        let task = cx.spawn(async move |this, cx| {
            let result = scan.await;
            let _ = this.update(cx, |view, cx| view.finish_plugin_scan(result, cx));
        });
        self.plugins.scan = Some(RunningScan { cancel, progress, task });
        self.status = "Scanning for plugins...".into();
        self.refresh_plugin_views(cx);
    }

    /// Applies the result of a scan to the known plugins and shows its summary.
    fn finish_plugin_scan(&mut self, result: anyhow::Result<(KnownPlugins, ScanReport)>, cx: &mut Context<Self>) {
        if let Some(scan) = self.plugins.scan.take() {
            scan.task.detach();
        }
        match result {
            Ok((known, report)) => {
                self.plugins.known = known;
                self.status = report.summary().into();
                self.plugins.last_report = Some(report);
            }
            Err(error) => self.status = format!("Plugin scan failed: {error:#}").into(),
        }
        self.refresh_plugin_views(cx);
    }

    /// Changes the scan folders or the blacklist under the scan lock,
    /// starting from the saved list so the change does not undo what another scan wrote.
    fn edit_known_plugins(&mut self, cx: &mut Context<Self>, edit: impl FnOnce(&mut KnownPlugins)) {
        if self.plugins.is_scanning() {
            return self.report_status(String::from(WAIT_FOR_SCAN), cx);
        }
        let path = self.plugins.paths.known_plugins.clone();
        let result = ScanLock::try_acquire(&self.plugins.paths.scan_lock).and_then(|lock| {
            let Some(lock) = lock else {
                return Ok(None);
            };
            // A file that cannot be parsed is moved aside, so the edit starts a new list instead of failing every time.
            let (mut known, _) = KnownPlugins::load_recovering(&path)?;
            edit(&mut known);
            known.sort();
            known.save_to(&path)?;
            drop(lock);
            Ok(Some(known))
        });
        match result {
            Ok(Some(known)) => {
                self.plugins.known = known;
                self.refresh_plugin_views(cx);
            }
            Ok(None) => self.report_status(String::from(SCAN_BUSY_MESSAGE), cx),
            Err(error) => self.report_status(format!("Could not change the plugin settings: {error:#}"), cx),
        }
    }

    /// Asks for folders and adds them to the scan folders.
    fn prompt_for_plugin_folders(cx: &Context<Self>) {
        let receiver = cx.prompt_for_paths(PathPromptOptions {
            files: false,
            directories: true,
            multiple: true,
            prompt: None,
        });
        cx.spawn(async move |this, cx| {
            if let Ok(Ok(Some(paths))) = receiver.await {
                let _ = this.update(cx, |view, cx| {
                    view.edit_known_plugins(cx, |known| {
                        let mut folders = known.search_folders();
                        for path in paths {
                            if !folders.contains(&path) {
                                folders.push(path);
                            }
                        }
                        known.custom_folders = folders;
                    });
                });
            }
        })
        .detach();
    }

    /// Shows a message in the status bar.
    pub(super) fn report_status(&mut self, message: String, cx: &mut Context<Self>) {
        self.status = message.into();
        cx.notify();
    }
}

/// Brings an open plugin window to the front. Returns false when it is not open.
fn activate<V: 'static>(plugin_window: Option<&PluginWindow<V>>, cx: &mut App) -> bool {
    plugin_window.is_some_and(|plugin_window| {
        !plugin_window.is_closed()
            && plugin_window
                .handle
                .update(cx, |_, window, _| window.activate_window())
                .is_ok()
    })
}

/// Returns the options of a plugin window centered on the main display.
fn plugin_window_options(title: &'static str, width: f32, height: f32, cx: &App) -> WindowOptions {
    let bounds = Bounds::centered(None, size(px(width), px(height)), cx);
    WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(bounds)),
        window_min_size: Some(size(px(PLUGIN_WINDOW_MINIMUM_SIZE.0), px(PLUGIN_WINDOW_MINIMUM_SIZE.1))),
        titlebar: Some(TitlebarOptions {
            title: Some(title.into()),
            ..Default::default()
        }),
        focus: true,
        ..Default::default()
    }
}
