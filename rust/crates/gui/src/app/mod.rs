//! The main window of `AudioBatch` RS.
//!
//! `AppView` owns the results table, the preview section, the analysis, normalization,
//! and plugin processing coordinators, the plugin chain, and the preview player.
//! This module holds the view state and its construction.
//! What it opens at startup is in `startup`, the analysis runs are in `analysis`,
//! the selection is in `selection`, and the per-file gain is in `gain`.
//! The menu and transport actions are in `actions`, normalization is in `normalize`,
//! plugin processing is in `process`, the plugin chain and the plugin windows are in `plugins`,
//! and the live preview through the chain is in `preview`.
//! The tags of the selected file are read in `tags`, the preferences in `preferences`, and the layout is in `render`.

mod actions;
mod analysis;
mod gain;
mod normalize;
mod plugins;
mod preferences;
mod preview;
mod process;
mod render;
mod selection;
mod startup;
mod tags;

use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::Arc;

use audiobatch_core::{AnalysisCoordinator, FileTags, NormalizeCoordinator};
use audiobatch_playback::Player;
use audiobatch_plugins::ProcessingCoordinator;
use gpui_kit::base::ResizableState;
use gpui_kit::component::slider::SliderState;
use gpui_kit::component::table::TableState;
use gpui_kit::{AppContext as _, Context, Entity, FocusHandle, Pixels, SharedString, Size, Subscription, Task, Window};
use raw_window_handle::HasWindowHandle as _;
use vst3_host::UiDispatcher;

use crate::menus::{MenuState, set_menus};
use crate::normalization::describe_target;
use crate::operation_run::OperationRun;
use crate::plugins::PluginHost;
use crate::plugins::chain_window::ChainView;
use crate::plugins::manager_window::ManagerView;
use crate::results::ResultsDelegate;
use crate::settings::GuiSettings;
use crate::waveform::{PlayheadSource, WaveformView};
use analysis::{RunKind, RunProgress, drain_events};
use gain::DEFAULT_GAIN_DB;
use plugins::PluginWindow;
use preferences::SettingsWindow;
use selection::column_choices;
use startup::{Startup, new_results_table, new_sliders, open_startup};

/// Window title, followed by the analysis progress while a run is in progress.
pub const APP_TITLE: &str = "AudioBatchRs";

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
    /// True after a normalization run was cancelled, until its workers are done,
    /// since a file past its last cancellation check may still commit.
    normalize_winding_down: bool,
    /// Processes files with the plugin chain in the background, sharing the cache of the analysis coordinator.
    process_coordinator: Option<Arc<ProcessingCoordinator>>,
    /// Progress of the current or last plugin processing run.
    process_run: Option<OperationRun>,
    /// Applies the events of the processing run. Dropping it stops listening to the run.
    process_task: Option<Task<()>>,
    /// True after a processing run was cancelled, until its workers are done.
    process_winding_down: bool,
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
    /// Sizes of the table and the expanded preview, kept while the preview is collapsed.
    workspace_split: Entity<ResizableState>,
    /// True while the preview is collapsed into its compact row.
    preview_compact: bool,
    /// True until the first completed run has selected and started playing its first file.
    autoplay_pending: bool,
    /// Applies the events of the current run. Dropping it stops listening to the run.
    analysis_task: Option<Task<()>>,
    /// Drives the stress test, present only in demo mode.
    demo_task: Option<Task<()>>,
    /// Files whose custom gain the mouse wheel changed, written to the cache once the wheel rests.
    pending_gain_paths: Vec<PathBuf>,
    /// File to start playing once its cue points are read, when playing from the first cue is on.
    start_after_cues: Option<PathBuf>,
    /// True when a new file reset the waveform zoom, which the next render shows on the zoom slider.
    zoom_reset_pending: bool,
    /// Writes the pending gain after the delay. Replacing it restarts the delay.
    gain_store_task: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
    _player_poll_task: Task<()>,
    /// Opens the paths that later launches of the app hand over, when this launch is the running app.
    launch_task: Option<Task<()>>,
    _plugin_tick_task: Task<()>,
}

impl AppView {
    /// Creates the main view, opens the cache, the audio device, and the plugin chain,
    /// and starts any startup analysis.
    /// The dispatcher runs plugin calls from the processing workers on this thread.
    pub fn new(options: &LaunchOptions, dispatcher: UiDispatcher, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let (content_width, workspace_height) = content_size(window.viewport_size());
        let settings = GuiSettings::load();
        let table = new_results_table(content_width, &settings, window, cx);
        let waveform = cx.new(|_| WaveformView::new());
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
            serato_colours: settings.serato_waveform_colours,
            preview_plugin_chain: settings.preview_plugin_chain,
            compensate_preview_latency: settings.compensate_preview_latency,
            normalize_target: describe_target(&settings),
            columns: column_choices(table.read(cx).delegate()),
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
            waveform.set_serato_colours(settings.serato_waveform_colours, cx);
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
            normalize_winding_down: false,
            process_coordinator: coordinators.process,
            process_run: None,
            process_task: None,
            process_winding_down: false,
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
            workspace_split: cx.new(|_| ResizableState::default()),
            preview_compact: false,
            autoplay_pending: options.autoplay,
            analysis_task: None,
            demo_task: None,
            pending_gain_paths: Vec::new(),
            start_after_cues: None,
            zoom_reset_pending: false,
            gain_store_task: None,
            _subscriptions: subscriptions,
            _player_poll_task: player_poll_task,
            launch_task: None,
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

    /// Points the waveform playhead at the current player.
    fn connect_playhead(&self, cx: &mut Context<Self>) {
        let playhead = self
            .player
            .as_ref()
            .map(|player| PlayheadSource::Player(player.playhead()));
        self.waveform.update(cx, |view, cx| view.set_playhead(playhead, cx));
    }

    /// Returns true when the table shows the synthetic stress test.
    const fn is_demo(&self) -> bool {
        self.demo_task.is_some()
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
}

/// Returns the content width and the height left for the table and the preview in a window of the given size.
fn content_size(viewport: Size<Pixels>) -> (f32, f32) {
    let width = CONTENT_PADDING.mul_add(-2.0, f32::from(viewport.width));
    let height = CONTENT_PADDING.mul_add(-2.0, f32::from(viewport.height)) - STATUS_BAR_HEIGHT;
    (width, height)
}
