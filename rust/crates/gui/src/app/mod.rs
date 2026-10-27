//! The main window of `AudioBatch` RS.
//!
//! `AppView` owns the results table, the preview section, the analysis, normalization,
//! and plugin processing coordinators, the plugin chain, and the preview player.
//! This module holds the view state, the analysis runs, and the selection.
//! The menu and transport actions are in `actions`, normalization is in `normalize`,
//! plugin processing is in `process`, the plugin chain and the plugin windows are in `plugins`,
//! and the live preview through the chain is in `preview`.
//! The tags of the selected file are read in `tags`, and the layout is in `render`.
//! Coordinator events are drained from their channel by a foreground task in batches,
//! so a large run updates the table at a steady rate instead of once per event.

mod actions;
mod normalize;
mod plugins;
mod preferences;
mod preview;
mod process;
mod render;
mod tags;

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use audiobatch_core::discovery::{has_supported_extension, normalize_path};
use audiobatch_core::record::{custom_gain_from_decibels, decibels_to_gain};
use audiobatch_core::{
    AnalysisCache, AnalysisCoordinator, AnalysisEvent, AnalysisOptions, AnalysisRecord, FileTags, NormalizeCoordinator,
};
use audiobatch_playback::Player;
use audiobatch_plugins::{ProcessingCoordinator, default_worker_count};
use crossbeam_channel::{Receiver, TryRecvError};
use gpui_kit::component::slider::{SliderEvent, SliderScale, SliderState};
use gpui_kit::component::table::{TableEvent, TableState};
use gpui_kit::{
    App, AppContext as _, Context, Entity, FocusHandle, Pixels, SharedString, Size, Subscription, Task, Window,
};
use raw_window_handle::{HasWindowHandle as _, RawWindowHandle};
use vst3_host::UiDispatcher;

use crate::menus::{MenuState, set_menus};
use crate::normalization::{describe_target, target_range};
use crate::operation_run::OperationRun;
use crate::plugins::chain_window::ChainView;
use crate::plugins::labels::{chain_summary, group_by_vendor};
use crate::plugins::manager_window::ManagerView;
use crate::plugins::{FALLBACK_SAMPLE_RATE, PluginHost};
use crate::results::{ResultRow, ResultsDelegate, RowState};
use crate::settings::{GuiSettings, NormalizeMode};
use crate::waveform::{MAXIMUM_ZOOM, MINIMUM_ZOOM, NO_FILE_TEXT, PlayheadSource, WaveformEvent, WaveformView};
use plugins::PluginWindow;
use preferences::{SettingsWindow, open_player};

/// Window title, followed by the analysis progress while a run is in progress.
pub const APP_TITLE: &str = "AudioBatchRs";

/// How often the analysis and normalization events are applied to the table.
const ANALYSIS_POLL_INTERVAL: Duration = Duration::from_millis(50);

/// Padding around the window content.
const CONTENT_PADDING: f32 = 4.0;

/// Height of the status bar at the bottom of the window.
const STATUS_BAR_HEIGHT: f32 = 24.0;

/// Width of the outline around the results table.
const TABLE_FRAME_BORDER: f32 = 1.0;

/// Space between the preview columns.
const SECTION_SPACING: f32 = 10.0;

/// Smallest height of the preview.
const PREVIEW_MINIMUM_HEIGHT: f32 = 230.0;

/// Initial share of the workspace height given to the preview.
const PREVIEW_PROPORTION: f32 = 0.28;

/// Lowest input gain in decibels.
const MINIMUM_GAIN_DB: f32 = -12.0;

/// Highest input gain in decibels.
const MAXIMUM_GAIN_DB: f32 = 12.0;

/// Input gain at startup and after a reset, in decibels.
const DEFAULT_GAIN_DB: f32 = 0.0;

/// Difference between the gain slider and the selected file's gain that moves the slider, in decibels.
const GAIN_SLIDER_TOLERANCE_DB: f32 = 1e-3;

/// How long the mouse wheel has to rest before a gain it changed is written to the cache.
const GAIN_STORE_DELAY: Duration = Duration::from_millis(500);

/// What an analysis run covers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RunKind {
    /// Every listed folder and file, replacing the rows with what is found.
    Scan,
    /// Some listed files again, keeping the other rows.
    Reanalyze,
}

/// Startup options from the command line.
#[derive(Debug, Default, Clone)]
pub struct LaunchOptions {
    /// Files and folders to analyze at startup.
    pub paths: Vec<PathBuf>,
    /// Number of synthetic rows for the stress test, when set.
    pub demo_rows: Option<usize>,
    /// Selects and plays the first analyzed file once the startup analysis completes.
    pub autoplay: bool,
}

/// The root view of the main window.
pub struct AppView {
    focus_handle: FocusHandle,
    table: Entity<TableState<ResultsDelegate>>,
    waveform: Entity<WaveformView>,
    zoom_slider: Entity<SliderState>,
    gain_slider: Entity<SliderState>,
    /// Target slider of peak normalization in dBFS.
    peak_target_slider: Entity<SliderState>,
    /// Target slider of loudness normalization in LUFS.
    loudness_target_slider: Entity<SliderState>,
    coordinator: Option<Arc<AnalysisCoordinator>>,
    /// Normalizes files in the background, sharing the cache of the analysis coordinator.
    normalize_coordinator: Option<Arc<NormalizeCoordinator>>,
    /// Progress of the current or last normalization run.
    normalize_run: Option<OperationRun>,
    /// Applies the events of the normalization run. Dropping it stops listening to the run.
    normalize_task: Option<Task<()>>,
    /// Processes files with the plugin chain in the background, sharing the cache of the analysis coordinator.
    process_coordinator: Option<Arc<ProcessingCoordinator>>,
    /// Progress of the current or last plugin processing run.
    process_run: Option<OperationRun>,
    /// Applies the events of the processing run. Dropping it stops listening to the run.
    process_task: Option<Task<()>>,
    /// Normalizes files before the plugin chain, for this session only.
    normalize_first: bool,
    /// Replaces an existing output file when normalizing or processing, for this session only.
    overwrite_existing: bool,
    /// The known plugins, the plugin chain, and the plugin scan.
    plugins: PluginHost,
    /// The Plugin Chain window while it is open.
    chain_window: Option<PluginWindow<ChainView>>,
    /// The Plugin Manager window while it is open.
    manager_window: Option<PluginWindow<ManagerView>>,
    player: Option<Player>,
    /// Folders and files the list is built from.
    roots: Vec<PathBuf>,
    /// Files removed from the list, which later scans of the same folders leave out.
    excluded: HashSet<PathBuf>,
    run: Option<RunProgress>,
    selected: Option<PathBuf>,
    /// Tags of the selected file, read in the background after selection.
    selected_tags: Option<(PathBuf, Arc<FileTags>)>,
    /// Reads the tags of the selected file. Replacing it drops a read that is no longer wanted.
    tags_task: Option<Task<()>>,
    status: SharedString,
    /// Output text of the status bar at the last check, so a change in the buffer size renders again.
    shown_output: SharedString,
    menu_state: MenuState,
    /// Preferences saved between launches.
    settings: GuiSettings,
    /// The Audio Settings window while it is open.
    settings_window: Option<SettingsWindow>,
    window_title: String,
    /// Initial height of the preview, from the window size at startup.
    preview_height: f32,
    /// True until the first completed run has selected and started playing its first file.
    autoplay_pending: bool,
    /// Applies the events of the current run. Dropping it stops listening to the run.
    analysis_task: Option<Task<()>>,
    /// Drives the stress test, present only in demo mode.
    demo_task: Option<Task<()>>,
    /// File whose custom gain the mouse wheel changed, written to the cache once the wheel rests.
    pending_gain_path: Option<PathBuf>,
    /// Writes the pending gain after the delay. Replacing it restarts the delay.
    gain_store_task: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
    _player_poll_task: Task<()>,
    _plugin_tick_task: Task<()>,
}

/// Progress of the current analysis run.
#[derive(Debug, Clone)]
struct RunProgress {
    run_id: u64,
    kind: RunKind,
    planned: bool,
    total: usize,
    finished: usize,
    completed: bool,
}

/// The background coordinators, sharing one analysis cache, or none when the cache cannot be opened.
#[derive(Default)]
struct Coordinators {
    analysis: Option<Arc<AnalysisCoordinator>>,
    normalize: Option<Arc<NormalizeCoordinator>>,
    process: Option<Arc<ProcessingCoordinator>>,
}

/// What the main view opens at startup, with the problem to show in the status line.
struct Startup {
    coordinators: Coordinators,
    player: Option<Player>,
    plugins: PluginHost,
    status: SharedString,
}

/// The sliders of the controls column.
struct Sliders {
    zoom: Entity<SliderState>,
    gain: Entity<SliderState>,
    peak_target: Entity<SliderState>,
    loudness_target: Entity<SliderState>,
}

impl AppView {
    /// Creates the main view, opens the cache, the audio device, and the plugin chain,
    /// and starts any startup analysis.
    /// The dispatcher runs plugin calls from the processing workers on this thread.
    pub fn new(options: &LaunchOptions, dispatcher: UiDispatcher, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let (content_width, workspace_height) = content_size(window.viewport_size());
        let table = cx.new(|cx| {
            TableState::new(
                ResultsDelegate::new(TABLE_FRAME_BORDER.mul_add(-2.0, content_width)),
                window,
                cx,
            )
            .sortable(true)
            .col_resizable(true)
            .row_selectable(true)
        });
        let waveform = cx.new(|_| WaveformView::new());
        let settings = GuiSettings::load();
        let (sliders, slider_subscriptions) = new_sliders(&settings, window, cx);
        let mut subscriptions = vec![
            cx.subscribe_in(&table, window, Self::on_table_event),
            cx.subscribe_in(&waveform, window, Self::on_waveform_event),
        ];
        subscriptions.extend(slider_subscriptions);
        subscriptions.push(cx.on_app_quit(|view, cx| {
            view.flush_gain_store(cx);
            view.save_plugin_chain();
            async {}
        }));

        let mut menu_state = MenuState {
            output_device: None,
            show_loudness_graph: settings.show_loudness_graph,
            preview_plugin_chain: settings.preview_plugin_chain,
            compensate_preview_latency: settings.compensate_preview_latency,
            normalize_target: describe_target(&settings),
            ..MenuState::default()
        };
        let editor_owner = window.window_handle().ok().map(|handle| handle.as_raw());
        let Startup {
            coordinators,
            player,
            plugins,
            status,
        } = open_startup(&settings, dispatcher, editor_owner, &mut menu_state);
        set_menus(cx, &menu_state);
        waveform.update(cx, |waveform, cx| {
            waveform.set_show_loudness(settings.show_loudness_graph, cx);
            waveform.set_loudness_range(settings.loudness_minimum, settings.loudness_maximum, cx);
        });

        let focus_handle = cx.focus_handle();
        window.focus(&focus_handle, cx);
        let player_poll_task = Self::spawn_player_poll(cx);
        let plugin_tick_task = Self::spawn_plugin_tick(cx);

        let mut view = Self {
            focus_handle,
            table,
            waveform,
            zoom_slider: sliders.zoom,
            gain_slider: sliders.gain,
            peak_target_slider: sliders.peak_target,
            loudness_target_slider: sliders.loudness_target,
            coordinator: coordinators.analysis,
            normalize_coordinator: coordinators.normalize,
            normalize_run: None,
            normalize_task: None,
            process_coordinator: coordinators.process,
            process_run: None,
            process_task: None,
            normalize_first: false,
            overwrite_existing: false,
            plugins,
            chain_window: None,
            manager_window: None,
            player,
            roots: Vec::new(),
            excluded: HashSet::new(),
            run: None,
            selected: None,
            selected_tags: None,
            tags_task: None,
            status,
            shown_output: SharedString::default(),
            menu_state,
            settings,
            settings_window: None,
            window_title: String::from(APP_TITLE),
            preview_height: (workspace_height * PREVIEW_PROPORTION).max(PREVIEW_MINIMUM_HEIGHT),
            autoplay_pending: options.autoplay,
            analysis_task: None,
            demo_task: None,
            pending_gain_path: None,
            gain_store_task: None,
            _subscriptions: subscriptions,
            _player_poll_task: player_poll_task,
            _plugin_tick_task: plugin_tick_task,
        };
        view.connect_playhead(cx);
        view.refresh_plugin_preview(cx);
        if let Some(rows) = options.demo_rows {
            view.start_demo(rows, cx);
        } else if !options.paths.is_empty() {
            view.add_roots(&options.paths, cx);
        }
        view
    }

    /// Clears the custom gain of the selected file, in the table and in the cache.
    pub(super) fn clear_selected_gain(&self, cx: &mut Context<Self>) {
        self.change_selected_gain(DEFAULT_GAIN_DB, cx);
        self.store_selected_gain(cx);
        cx.notify();
    }

    /// Points the waveform playhead at the current player.
    fn connect_playhead(&self, cx: &mut Context<Self>) {
        let playhead = self
            .player
            .as_ref()
            .map(|player| PlayheadSource::Player(player.playhead()));
        self.waveform.update(cx, |view, cx| view.set_playhead(playhead, cx));
    }

    /// Adds folders and files to the list and scans every listed root again.
    /// Cached files come back immediately, so only new or changed files are decoded.
    fn add_roots(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        if self.blocked_by_file_operation(cx) {
            return;
        }
        let mut added = false;
        for path in paths.iter().map(|path| normalize_path(path)) {
            self.excluded.remove(&path);
            if !self.roots.contains(&path) {
                self.roots.push(path);
                added = true;
            }
        }
        if added {
            self.start_run(RunKind::Scan, self.roots.clone(), false, cx);
        } else {
            self.status = SharedString::from("Folder is already included");
            cx.notify();
        }
    }

    /// Replaces the listed folders and files and scans them.
    fn replace_roots(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        if self.blocked_by_file_operation(cx) {
            return;
        }
        self.roots = paths.iter().map(|path| normalize_path(path)).collect();
        self.excluded.clear();
        self.start_run(RunKind::Scan, self.roots.clone(), false, cx);
    }

    /// Adds dropped paths, or replaces the listed folders when folders are dropped without Shift held.
    fn handle_drop(&mut self, paths: &[PathBuf], append: bool, cx: &mut Context<Self>) {
        let supported: Vec<PathBuf> = paths
            .iter()
            .filter(|path| path.is_dir() || has_supported_extension(path))
            .cloned()
            .collect();
        if supported.is_empty() {
            self.status = SharedString::from("No supported dropped files");
            cx.notify();
            return;
        }
        if !append && supported.iter().any(|path| path.is_dir()) {
            self.replace_roots(&supported, cx);
        } else {
            self.add_roots(&supported, cx);
        }
    }

    /// Starts an analysis run, replacing any run in progress.
    /// Rows the replaced run left waiting or analyzing keep their previous result,
    /// because the events of the replaced run are ignored from now on.
    fn start_run(&mut self, kind: RunKind, inputs: Vec<PathBuf>, refresh: bool, cx: &mut Context<Self>) {
        let Some(coordinator) = self.coordinator.clone() else {
            return;
        };
        if inputs.is_empty() || self.is_demo() || self.blocked_by_file_operation(cx) {
            return;
        }
        if self.is_analyzing() {
            self.table.update(cx, |state, cx| {
                state.delegate_mut().finish_interrupted_rows();
                cx.notify();
            });
        }
        let (run_id, receiver) = coordinator.start(AnalysisOptions {
            input_paths: inputs,
            recursive: true,
            refresh,
        });
        self.run = Some(RunProgress {
            run_id,
            kind,
            planned: false,
            total: 0,
            finished: 0,
            completed: false,
        });
        self.status = SharedString::from(match kind {
            RunKind::Scan => "Scanning for audio files...",
            RunKind::Reanalyze => "Analyzing...",
        });
        self.analysis_task = Some(spawn_event_drain(receiver, cx));
        cx.notify();
    }

    /// Returns true while a run is in progress.
    fn is_analyzing(&self) -> bool {
        self.analysis_task.is_some() && self.run.as_ref().is_some_and(|run| !run.completed)
    }

    /// Returns true when the table shows the synthetic stress test.
    const fn is_demo(&self) -> bool {
        self.demo_task.is_some()
    }

    /// Returns true when the selected file has a usable result, so its custom gain can be changed,
    /// or while the stress test runs, where the gain only scales the drawn waveform.
    fn can_adjust_gain(&self, cx: &App) -> bool {
        self.is_demo() || self.selected_record(cx).is_some()
    }

    /// Returns the usable result of the selected file.
    fn selected_record<'app>(&self, cx: &'app App) -> Option<&'app AnalysisRecord> {
        let selected = self.selected.as_deref()?;
        self.table.read(cx).delegate().row_for(selected)?.ready_record()
    }

    /// Returns the activity and the overall progress of the current run from 0 to 1,
    /// or none when no analysis, normalization, or plugin processing run is in progress.
    /// Files being worked on count with their fraction, so the line moves smoothly on long files.
    fn overall_progress(&self, cx: &Context<Self>) -> Option<(&'static str, f32)> {
        let file_run = self
            .normalize_run
            .as_ref()
            .filter(|_| self.is_normalizing())
            .or_else(|| self.process_run.as_ref().filter(|_| self.is_processing()));
        let (activity, finished, total) = if let Some(run) = file_run {
            (run.kind.activity(), run.finished, run.total)
        } else {
            let run = self.run.as_ref().filter(|_| self.is_analyzing())?;
            ("Analyzing", run.finished, run.total)
        };
        if total == 0 {
            return Some((activity, 0.0));
        }
        let active = self.table.read(cx).delegate().active_progress();
        Some((activity, ((finished as f32 + active) / total as f32).clamp(0.0, 1.0)))
    }

    /// Returns the target slider of a normalization mode.
    const fn normalize_target_slider(&self, mode: NormalizeMode) -> &Entity<SliderState> {
        match mode {
            NormalizeMode::Peak => &self.peak_target_slider,
            NormalizeMode::Loudness => &self.loudness_target_slider,
        }
    }

    /// Stores the target while a target slider moves, and saves it when the slider is released.
    fn on_target_slider_event(&mut self, mode: NormalizeMode, event: &SliderEvent, cx: &mut Context<Self>) {
        match event {
            SliderEvent::Change(value) => self.change_normalize_target(mode, value.start(), cx),
            SliderEvent::Release(_) => self.store_normalize_target(cx),
        }
    }

    /// Applies a batch of analysis events to the table and the selection.
    fn apply_analysis_events(&mut self, events: Vec<AnalysisEvent>, cx: &mut Context<Self>) {
        let Some((run_id, kind)) = self.run.as_ref().map(|run| (run.run_id, run.kind)) else {
            return;
        };
        let mut planned_total = None;
        let mut finished = 0;
        let mut selected_finished = false;
        let mut completed = false;
        let mut carried_gains = Vec::new();
        let excluded = &self.excluded;
        let selected = self.selected.as_deref();
        self.table.update(cx, |state, cx| {
            let delegate = state.delegate_mut();
            for event in events.into_iter().filter(|event| event.run_id() == run_id) {
                match event {
                    AnalysisEvent::Planned { files, .. } => {
                        let files: Vec<PathBuf> = files.into_iter().filter(|path| !excluded.contains(path)).collect();
                        planned_total = Some(files.len());
                        match kind {
                            RunKind::Scan => delegate.set_files(&files),
                            RunKind::Reanalyze => files.iter().for_each(|path| delegate.mark_pending(path)),
                        }
                    }
                    AnalysisEvent::Started { path, .. } => delegate.mark_started(&path),
                    AnalysisEvent::Progress { path, fraction, .. } => delegate.mark_progress(&path, fraction),
                    AnalysisEvent::Finished { record, .. } => {
                        if excluded.contains(&record.path) {
                            continue;
                        }
                        finished += 1;
                        selected_finished |= selected == Some(record.path.as_path());
                        let path = record.path.clone();
                        if let Some(gain) = delegate.set_record(*record) {
                            carried_gains.push((path, gain));
                        }
                    }
                    AnalysisEvent::Completed { .. } => {
                        delegate.sort_rows();
                        completed = true;
                    }
                }
            }
            cx.notify();
        });
        // The coordinator stored each row before publishing it, so a carried over gain can be stored now.
        for (path, gain) in carried_gains {
            self.store_custom_gain(&path, Some(gain));
        }

        if let Some(run) = &mut self.run {
            if let Some(total) = planned_total {
                run.planned = true;
                run.total = total;
            }
            run.finished += finished;
            run.completed |= completed;
        }
        self.update_run_status(cx);
        if planned_total.is_some() || completed {
            self.restore_selection(cx);
        }
        if completed {
            if crate::debug_enabled() {
                tracing::info!("analysis: {}", self.status);
            }
            if std::mem::take(&mut self.autoplay_pending) {
                let first_playable = {
                    let delegate = self.table.read(cx).delegate();
                    (0..delegate.len()).find_map(|index| {
                        delegate
                            .row(index)
                            .filter(|row| row.ready_record().is_some())
                            .map(|row| (index, row.path.clone()))
                    })
                };
                if let Some((index, path)) = first_playable {
                    self.table.update(cx, |state, cx| state.set_selected_row(index, cx));
                    self.select_path(&path, true, cx);
                }
            }
        }
        if selected_finished {
            self.refresh_selected_waveform(cx);
            self.apply_preview_gain(cx);
        }
        cx.notify();
    }

    /// Sets the status text from the progress of the current run.
    fn update_run_status(&mut self, cx: &Context<Self>) {
        let Some(run) = &self.run else {
            return;
        };
        let (listed, failed) = {
            let delegate = self.table.read(cx).delegate();
            (delegate.len(), delegate.failed_count())
        };
        if run.completed {
            let failures = if failed > 0 {
                format!(", {failed} failed")
            } else {
                String::new()
            };
            self.status = match run.kind {
                RunKind::Scan if listed == 0 => String::from("No supported audio files found"),
                RunKind::Scan => format!("Loaded {listed} files{failures}"),
                RunKind::Reanalyze => format!("Analyzed {} files{failures}", run.total),
            }
            .into();
        } else if run.planned {
            self.status = format!("Analyzing {}/{}", run.finished, run.total).into();
        }
    }

    /// Selects the row of the selected file again after the rows were sorted or replaced,
    /// because the table keeps its selection by index.
    /// Clears the selection when the file is no longer listed.
    fn restore_selection(&mut self, cx: &mut Context<Self>) {
        let Some(selected) = self.selected.clone() else {
            return;
        };
        let index = self.table.read(cx).delegate().index_of(&selected);
        match index {
            Some(index) => self.table.update(cx, |state, cx| {
                if state.selected_row() != Some(index) {
                    state.set_selected_row(index, cx);
                }
            }),
            None if !self.is_demo() => self.clear_selection(cx),
            None => {}
        }
    }

    /// Handles row selection and double clicks in the table,
    /// and renders again after a column resize so the Status column fills the width again.
    /// With playback continuation on, selecting another file while one plays starts the new one.
    fn on_table_event(
        &mut self,
        table: &Entity<TableState<ResultsDelegate>>,
        event: &TableEvent,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let continue_playback = self.settings.continue_playback && self.player.as_ref().is_some_and(Player::is_playing);
        let (index, play) = match event {
            // Events arrive after the update that sent them.
            // A selection the rows have since moved away from would pick another file.
            TableEvent::SelectRow(index) if table.read(cx).selected_row() != Some(*index) => return,
            TableEvent::SelectRow(index) => (*index, continue_playback),
            TableEvent::DoubleClickedRow(index) => (*index, true),
            TableEvent::ColumnWidthsChanged(_) => {
                cx.notify();
                return;
            }
            _ => return,
        };
        let path = table.read(cx).delegate().row(index).map(|row| row.path.clone());
        if let Some(path) = path {
            self.select_path(&path, play, cx);
        }
    }

    /// Handles seeks, wheel zoom, and wheel gain from the waveform.
    fn on_waveform_event(
        &mut self,
        _waveform: &Entity<WaveformView>,
        event: &WaveformEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match event {
            WaveformEvent::SeekRequested(seconds) => {
                if let Some(player) = &mut self.player {
                    player.seek(*seconds);
                }
            }
            WaveformEvent::ZoomBy(factor) => {
                let current = f64::from(self.zoom_slider.read(cx).value().start());
                let zoom = (current * factor).clamp(MINIMUM_ZOOM, MAXIMUM_ZOOM);
                self.zoom_slider
                    .update(cx, |slider, cx| slider.set_value(zoom as f32, window, cx));
                self.waveform.update(cx, |waveform, cx| waveform.set_zoom(zoom, cx));
            }
            WaveformEvent::GainBy(decibels) if self.can_adjust_gain(cx) => {
                let current = self.gain_slider.read(cx).value().start();
                let gain = ((current + decibels) * 10.0).round() / 10.0;
                let gain = gain.clamp(MINIMUM_GAIN_DB, MAXIMUM_GAIN_DB);
                self.gain_slider
                    .update(cx, |slider, cx| slider.set_value(gain, window, cx));
                self.change_selected_gain(gain, cx);
                self.schedule_gain_store(cx);
            }
            WaveformEvent::GainBy(_) => {}
        }
        cx.notify();
    }

    /// Sets the custom gain of the selected file while the slider or the mouse wheel moves,
    /// and applies it to the preview and the drawn waveform.
    /// The cache is written by `store_selected_gain` once the change is finished.
    /// In the stress test the gain only scales the drawn waveform.
    fn change_selected_gain(&self, decibels: f32, cx: &mut Context<Self>) {
        if self.is_demo() {
            let linear = decibels_to_gain(f64::from(decibels)) as f32;
            self.waveform
                .update(cx, |waveform, cx| waveform.set_display_gain(linear, cx));
            return;
        }
        let Some(selected) = self.selected.clone() else {
            return;
        };
        let gain = custom_gain_from_decibels(decibels);
        let changed = self.table.update(cx, |state, cx| {
            let changed = state.delegate_mut().set_custom_gain(&selected, gain);
            if changed {
                cx.notify();
            }
            changed
        });
        if changed {
            self.apply_preview_gain(cx);
        }
    }

    /// Stores the custom gain of the selected file in the cache.
    fn store_selected_gain(&self, cx: &App) {
        if let (Some(selected), Some(record)) = (&self.selected, self.selected_record(cx)) {
            self.store_custom_gain(selected, record.custom_gain_db);
        }
    }

    /// Writes the custom gain of the selected file to the cache once the mouse wheel rests,
    /// so a wheel gesture does not write to the database on the UI thread for every step.
    /// A pending gain of another file is written right away.
    fn schedule_gain_store(&mut self, cx: &Context<Self>) {
        let Some(selected) = self.selected.clone() else {
            return;
        };
        if self.pending_gain_path.as_ref().is_some_and(|path| *path != selected) {
            self.flush_gain_store(cx);
        }
        self.pending_gain_path = Some(selected);
        self.gain_store_task = Some(cx.spawn(async move |this, cx| {
            cx.background_executor().timer(GAIN_STORE_DELAY).await;
            let _ = this.update(cx, |view, cx| view.flush_gain_store(cx));
        }));
    }

    /// Writes the current custom gain of the file the mouse wheel changed, if any, to the cache.
    fn flush_gain_store(&mut self, cx: &App) {
        let Some(path) = self.pending_gain_path.take() else {
            return;
        };
        let gain = self
            .table
            .read(cx)
            .delegate()
            .row_for(&path)
            .and_then(|row| row.ready_record())
            .map(|record| record.custom_gain_db);
        if let Some(gain) = gain {
            self.store_custom_gain(&path, gain);
        }
    }

    /// Writes the custom gain of a file to the cache.
    /// A failure is only reported, because the table keeps the gain for this session either way.
    fn store_custom_gain(&self, path: &Path, custom_gain_db: Option<f32>) {
        if let Some(coordinator) = &self.coordinator
            && let Err(error) = coordinator.cache().store_custom_gain(path, custom_gain_db)
        {
            tracing::warn!("Failed to store the custom gain of {}: {error:#}", path.display());
        }
    }

    /// Applies the custom gain of the selected file to the preview and to the drawn waveform,
    /// or unity gain when it has none.
    fn apply_preview_gain(&self, cx: &mut Context<Self>) {
        let linear = self.selected_record(cx).map_or(1.0, AnalysisRecord::custom_gain_linear);
        if let Some(player) = &self.player {
            player.set_gain(linear);
        }
        self.waveform
            .update(cx, |waveform, cx| waveform.set_display_gain(linear, cx));
    }

    /// Moves the gain slider to the custom gain of the selected file, 0 dB without one.
    /// The record is the source of truth.
    /// A new selection, a cleared gain, and a gain loaded from the cache all show on the slider this way,
    /// without each of them having to update it.
    fn sync_gain_slider(&self, window: &mut Window, cx: &mut Context<Self>) {
        if self.is_demo() {
            return;
        }
        let target = self
            .selected_record(cx)
            .and_then(|record| record.custom_gain_db)
            .unwrap_or(DEFAULT_GAIN_DB);
        let current = self.gain_slider.read(cx).value().start();
        if (current - target).abs() >= GAIN_SLIDER_TOLERANCE_DB {
            self.gain_slider
                .update(cx, |slider, cx| slider.set_value(target, window, cx));
        }
    }

    /// Selects a file: shows its waveform and loads it into the player, playing it when asked.
    fn select_path(&mut self, path: &Path, play: bool, cx: &mut Context<Self>) {
        let changed = self.selected.as_deref() != Some(path);
        self.selected = Some(path.to_path_buf());
        self.table.update(cx, |state, _| {
            state.delegate_mut().set_selected_path(Some(path.to_path_buf()));
        });
        if self.is_demo() {
            cx.notify();
            return;
        }
        self.refresh_selected_waveform(cx);
        self.apply_preview_gain(cx);
        if changed {
            self.selected_tags = None;
            self.clear_tag_marks(cx);
            self.tags_task = Some(self.spawn_tag_read(path.to_path_buf(), cx));
        }
        if let Some(player) = &mut self.player {
            if changed || player.loaded_path() != Some(path) {
                player.load(path, 0.0, play);
            } else if play {
                player.play();
            }
        }
        cx.notify();
    }

    /// Clears the selection and the preview.
    fn clear_selection(&mut self, cx: &mut Context<Self>) {
        self.selected = None;
        self.selected_tags = None;
        self.tags_task = None;
        self.table
            .update(cx, |state, _| state.delegate_mut().set_selected_path(None));
        if let Some(player) = &mut self.player {
            player.unload();
        }
        self.table.update(cx, TableState::clear_selection);
        self.clear_tag_marks(cx);
        self.waveform
            .update(cx, |waveform, cx| waveform.show_placeholder(NO_FILE_TEXT, cx));
    }

    /// Shows the waveform of the selected file, or a placeholder while it has no result.
    fn refresh_selected_waveform(&self, cx: &mut Context<Self>) {
        let Some(selected) = self.selected.clone() else {
            return;
        };
        let row = self.table.read(cx).delegate().row_for(&selected).cloned();
        self.waveform.update(cx, |view, cx| match row {
            None => view.show_placeholder(NO_FILE_TEXT, cx),
            Some(row) => {
                if let Some(record) = row.ready_record() {
                    view.show(record.waveform.clone(), record.short_term_loudness.clone(), cx);
                } else {
                    view.show_placeholder(placeholder_text(&row), cx);
                }
            }
        });
    }
}

/// Returns the content width and the height left for the table and the preview in a window of the given size.
fn content_size(viewport: Size<Pixels>) -> (f32, f32) {
    let width = CONTENT_PADDING.mul_add(-2.0, f32::from(viewport.width));
    let height = CONTENT_PADDING.mul_add(-2.0, f32::from(viewport.height)) - STATUS_BAR_HEIGHT;
    (width, height)
}

/// Creates the sliders of the controls column, and subscribes the view to them.
fn new_sliders(settings: &GuiSettings, window: &Window, cx: &mut Context<AppView>) -> (Sliders, [Subscription; 4]) {
    let (zoom, gain) = new_view_sliders(cx);
    let peak_target = new_target_slider(NormalizeMode::Peak, settings.normalize_target_peak_dbfs, cx);
    let loudness_target = new_target_slider(NormalizeMode::Loudness, settings.normalize_target_lufs, cx);
    let subscriptions = [
        cx.subscribe_in(&zoom, window, |view, _, event: &SliderEvent, _, cx| {
            if let SliderEvent::Change(value) = event {
                let zoom = f64::from(value.start());
                view.waveform.update(cx, |waveform, cx| waveform.set_zoom(zoom, cx));
                cx.notify();
            }
        }),
        cx.subscribe_in(&gain, window, |view, _, event: &SliderEvent, _, cx| {
            match event {
                SliderEvent::Change(value) => view.change_selected_gain(value.start(), cx),
                SliderEvent::Release(_) => view.store_selected_gain(cx),
            }
            cx.notify();
        }),
        cx.subscribe_in(&peak_target, window, |view, _, event: &SliderEvent, _, cx| {
            view.on_target_slider_event(NormalizeMode::Peak, event, cx);
        }),
        cx.subscribe_in(&loudness_target, window, |view, _, event: &SliderEvent, _, cx| {
            view.on_target_slider_event(NormalizeMode::Loudness, event, cx);
        }),
    ];
    let sliders = Sliders {
        zoom,
        gain,
        peak_target,
        loudness_target,
    };
    (sliders, subscriptions)
}

/// Creates the zoom slider, logarithmic from 1 to 20 times, and the input gain slider in decibels.
fn new_view_sliders(cx: &mut Context<AppView>) -> (Entity<SliderState>, Entity<SliderState>) {
    let zoom = cx.new(|_| {
        SliderState::new()
            .min(MINIMUM_ZOOM as f32)
            .max(MAXIMUM_ZOOM as f32)
            .scale(SliderScale::Logarithmic)
            .step(0.01)
            .default_value(MINIMUM_ZOOM as f32)
    });
    let gain = cx.new(|_| {
        SliderState::new()
            .min(MINIMUM_GAIN_DB)
            .max(MAXIMUM_GAIN_DB)
            .step(0.1)
            .default_value(DEFAULT_GAIN_DB)
    });
    (zoom, gain)
}

/// Creates the target slider of a normalization mode, starting at the saved target.
fn new_target_slider(mode: NormalizeMode, target: f64, cx: &mut Context<AppView>) -> Entity<SliderState> {
    let range = target_range(mode);
    cx.new(|_| {
        SliderState::new()
            .min(range.minimum)
            .max(range.maximum)
            .step(range.step)
            .default_value(target as f32)
    })
}

/// Returns the waveform placeholder for a file without a usable result.
fn placeholder_text(row: &ResultRow) -> String {
    match row.state {
        RowState::Pending => String::from("Waiting for analysis"),
        RowState::Analyzing(fraction) => format!("Analyzing {:.0}%", fraction * 100.0),
        RowState::Normalizing(fraction) => format!("Normalizing {:.0}%", fraction * 100.0),
        RowState::Processing(fraction) => format!("Processing {:.0}%", fraction * 100.0),
        RowState::Done if row.has_error() => String::from("Could not read this file"),
        RowState::Done => String::from("No waveform for this file"),
    }
}

/// Opens the default analysis cache, and the coordinators sharing it.
/// Analysis and normalization get one worker per CPU.
/// Plugin processing leaves one physical core free, because plugins are much heavier per file
/// and the preview and the editors keep running on the UI thread.
/// Falls back to a cache in the temporary directory when the default location cannot be opened.
fn open_coordinators() -> anyhow::Result<Coordinators> {
    let cache = AnalysisCache::open_default().or_else(|_| {
        let cache = AnalysisCache::with_path(std::env::temp_dir().join("AudioBatch-rs").join("analysis.db"));
        cache.open()?;
        anyhow::Ok(Arc::new(cache))
    })?;
    let workers = std::thread::available_parallelism().map_or(1, std::num::NonZeroUsize::get);
    Ok(Coordinators {
        normalize: Some(Arc::new(NormalizeCoordinator::new(Arc::clone(&cache), workers)?)),
        process: Some(Arc::new(ProcessingCoordinator::new(
            Arc::clone(&cache),
            default_worker_count(),
        )?)),
        analysis: Some(Arc::new(AnalysisCoordinator::new(cache, workers)?)),
    })
}

/// Opens the coordinators, the audio device, and the plugin chain, and fills in their part of the menus.
/// Plugin editors open as windows owned by `editor_owner`.
/// A problem with playback replaces one with analysis in the status line,
/// and plugin problems are shown only when neither failed.
fn open_startup(
    settings: &GuiSettings,
    dispatcher: UiDispatcher,
    editor_owner: Option<RawWindowHandle>,
    menu_state: &mut MenuState,
) -> Startup {
    let mut status = SharedString::default();
    let coordinators = open_coordinators().unwrap_or_else(|error| {
        status = format!("Analysis unavailable: {error:#}").into();
        Coordinators::default()
    });
    let player = match open_player(&settings.output_config()) {
        Ok(player) => {
            menu_state.output_device = Some(player.device_name().to_string());
            Some(player)
        }
        Err(error) => {
            status = format!("Playback unavailable: {error:#}").into();
            None
        }
    };
    let sample_rate = player.as_ref().map_or(FALLBACK_SAMPLE_RATE, Player::output_rate);
    let (plugins, plugin_problems) = PluginHost::open(dispatcher, sample_rate, editor_owner);
    for problem in plugin_problems {
        tracing::warn!("{problem}");
        if status.is_empty() {
            status = problem.into();
        }
    }
    menu_state.chain_summary = chain_summary(plugins.chain.enabled_count(), plugins.chain.len());
    menu_state.plugin_groups = group_by_vendor(&plugins.known.plugins);
    menu_state.presets = plugins.preset_names();
    Startup {
        coordinators,
        player,
        plugins,
        status,
    }
}

/// Takes every event waiting in a coordinator channel.
/// Returns the events and whether the channel is disconnected, which means the run has no more events.
fn drain_events<T>(receiver: &Receiver<T>) -> (Vec<T>, bool) {
    let mut batch = Vec::new();
    loop {
        match receiver.try_recv() {
            Ok(event) => batch.push(event),
            Err(TryRecvError::Empty) => return (batch, false),
            Err(TryRecvError::Disconnected) => return (batch, true),
        }
    }
}

/// Drains the analysis events on a timer and applies them to the view in batches.
fn spawn_event_drain(receiver: Receiver<AnalysisEvent>, cx: &Context<AppView>) -> Task<()> {
    cx.spawn(async move |this, cx| {
        loop {
            let (batch, disconnected) = drain_events(&receiver);
            if !batch.is_empty()
                && this
                    .update(cx, |view, cx| view.apply_analysis_events(batch, cx))
                    .is_err()
            {
                break;
            }
            if disconnected {
                break;
            }
            cx.background_executor().timer(ANALYSIS_POLL_INTERVAL).await;
        }
    })
}
