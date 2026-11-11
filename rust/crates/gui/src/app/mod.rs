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
//! The tags of the selected file are read in `tags` and its album artwork in `artwork`,
//! the preferences are in `preferences`, and the layout is in `render`.

mod actions;
mod analysis;
mod artwork;
mod cue_start;
mod enrichment;
mod gain;
mod gain_loudness;
pub mod gain_writer;
#[cfg(test)]
mod integration_tests;
mod normalize;
mod panel_widths;
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
use std::sync::atomic::{AtomicBool, Ordering};

use audiobatch_core::output_file::FileDisposer;
#[cfg(not(test))]
use audiobatch_core::output_file::trash_disposer;
use audiobatch_core::storage::StoragePaths;
use audiobatch_core::{AnalysisCoordinator, FileTags, NormalizeCoordinator};
use audiobatch_playback::Player;
use audiobatch_plugins::ProcessingCoordinator;
use gpui_kit::base::ResizableState;
#[cfg(target_os = "windows")]
use gpui_kit::component::menu::AppMenuBar;
use gpui_kit::component::slider::SliderState;
use gpui_kit::component::table::TableState;
use gpui_kit::{
    AppContext as _, Context, Entity, FocusHandle, Pixels, RenderImage, SharedString, Size, Subscription, Task, Window,
};
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
use artwork::{ArtworkIdentity, ArtworkRequests};
use gain::DEFAULT_GAIN_DB;
use plugins::PluginWindow;
use preferences::SettingsWindow;
use render::PreviewStartupFit;
use selection::column_choices;
use startup::{Startup, new_results_table, new_sliders, open_startup};

/// Window title, followed by the analysis progress while a run is in progress.
pub const APP_TITLE: &str = "AudioBatchRs";

/// Padding around the window content.
const CONTENT_PADDING: f32 = 4.0;

/// Height of the status bar at the bottom of the window.
const STATUS_BAR_HEIGHT: f32 = 24.0;

/// Height reserved for the in-window menu bar, including its bottom border.
const MENU_BAR_HEIGHT: f32 = if cfg!(target_os = "windows") { 28.0 } else { 0.0 };

/// Width of the outline around the results table.
const TABLE_FRAME_BORDER: f32 = 1.0;

/// Space between the preview columns.
const SECTION_SPACING: f32 = 6.0;

/// Smallest height of the preview.
const PREVIEW_MINIMUM_HEIGHT: f32 = 230.0;

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
    #[cfg(target_os = "windows")]
    menu_bar: Entity<AppMenuBar>,
    table: Entity<TableState<ResultsDelegate>>,
    waveform: Entity<WaveformView>,
    zoom_slider: Entity<SliderState>,
    gain_slider: Entity<SliderState>,
    /// Stable semantic focus for zoom, gain, and the active normalization target.
    slider_focus: [FocusHandle; 3],
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
    /// Captured source version and generation shared by playback, presentation, and metadata reads.
    preview_source: preview::PreviewSource,
    /// Tags of the selected file, read in the background after selection.
    selected_tags: Option<(PathBuf, Arc<FileTags>)>,
    /// Admits one expensive metadata or waveform job after measurement loading completes.
    enrichment: enrichment::EnrichmentQueue,
    /// Drains the active reader even after cooperative cancellation.
    enrichment_task: Option<Task<()>>,
    /// Analysis receiver drains still owning workers, including cancelled or replaced runs.
    analysis_workers: usize,
    /// Ordered, coalescing persistence for new explicit gain edits only.
    gain_writer: Option<gain_writer::GainWriter>,
    /// Album artwork thumbnail of the selected file, read in the background after selection.
    selected_artwork: Option<(ArtworkIdentity, Arc<RenderImage>)>,
    /// Coalesces selected-file identities and rejects obsolete artwork generations.
    artwork_requests: ArtworkRequests,
    /// Sources released for output replacement, excluded from artwork reads until their operation drains.
    artwork_rewrites: HashSet<PathBuf>,
    /// Drains the active artwork read before starting its latest pending replacement.
    artwork_task: Option<Task<()>>,
    status: SharedString,
    /// Output text of the status bar at the last check, so a change in the buffer size renders again.
    shown_output: SharedString,
    /// Last formatted source time, avoiding whole-window redraws at every player poll.
    shown_playback_time: Option<String>,
    menu_state: MenuState,
    /// Preferences saved between launches.
    settings: GuiSettings,
    /// Captured preference destination, never resolved again during a view's lifetime.
    settings_path: PathBuf,
    /// Disposes of replaced source files for normalization and plugin processing.
    dispose: FileDisposer,
    /// The Audio Settings window while it is open.
    settings_window: Option<SettingsWindow>,
    /// User-resized Audio Settings dimensions, retained only for this app session.
    settings_window_size: Option<Size<Pixels>>,
    window_title: String,
    /// Height the expanded preview starts at, the height that fits its controls in the opening window.
    preview_height: f32,
    /// Samples startup geometry until platform maximization and layout have settled, then commits the split once.
    preview_startup_fit: PreviewStartupFit,
    /// Keeps the initial preview height on container resize until the user chooses a proportional split by dragging.
    preview_fixed_size: bool,
    /// Sizes of the table and the expanded preview, kept while the preview is collapsed.
    workspace_split: Entity<ResizableState>,
    /// True while the preview is collapsed into its compact row.
    preview_compact: bool,
    /// Stable keyboard targets for the controls and information width dividers.
    panel_width_focus: [FocusHandle; 2],
    /// Live width feedback, committed to settings only on pointer release.
    panel_width_drag: Option<panel_widths::PanelWidthDrag>,
    /// True until the first completed run has selected and started playing its first file.
    autoplay_pending: bool,
    /// Applies the events of the current run. Dropping it stops listening to the run.
    analysis_task: Option<Task<()>>,
    /// Drives the stress test, present only in demo mode.
    demo_task: Option<Task<()>>,
    /// Files with pending explicit slider or wheel gain edits, retained until submitted to the writer.
    pending_gain_paths: Vec<PathBuf>,
    /// Deferred play or seek intent, resolved after matching cue metadata and source duration are ready.
    start_after_cues: Option<cue_start::CueStartIntent>,
    /// True when a new file reset the waveform zoom, which the next render shows on the zoom slider.
    zoom_reset_pending: bool,
    /// Writes the pending gain after the delay. Replacing it restarts the delay.
    gain_store_task: Option<Task<()>>,
    /// Saves the latest normalization targets after edits rest, independent of pointer release.
    normalize_target_store_task: Option<Task<()>>,
    /// Debounced transient loudness after custom gain and hard clipping.
    gain_prediction: gain_loudness::GainPrediction,
    /// Drains the sole prediction worker even after cooperative cancellation.
    gain_prediction_task: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
    _player_poll_task: Task<()>,
    /// Opens the paths that later launches of the app hand over, when this launch is the running app.
    launch_task: Option<Task<()>>,
    _plugin_tick_task: Task<()>,
    /// Removes cache rows of deleted files once after startup, stopping at quit through its flag.
    cache_prune: Option<(Arc<AtomicBool>, Task<()>)>,
    /// Keeps scratch storage alive until every view-owned worker and persistence service has been dropped.
    #[cfg(test)]
    test_directory: Arc<tempfile::TempDir>,
}

/// Startup services captured once before constructing the view and starting its workers.
struct AppOptions {
    paths: StoragePaths,
    playback: bool,
    workers: usize,
    dispose: FileDisposer,
    #[cfg(test)]
    directory: Arc<tempfile::TempDir>,
}

/// Current foreground activity and its bounded completion fraction for the title and status bar.
#[derive(Clone, Copy)]
struct ActivityProgress {
    activity: &'static str,
    fraction: f32,
}

/// File-run counts before active per-file progress is folded into the displayed fraction.
struct FileRunProgress {
    activity: &'static str,
    finished: usize,
    total: usize,
}

impl AppView {
    /// Creates the main view, opens the cache, the audio device, and the plugin chain,
    /// and starts any startup analysis.
    /// The dispatcher runs plugin calls from the processing workers on this thread.
    pub fn new(options: &LaunchOptions, dispatcher: UiDispatcher, window: &mut Window, cx: &mut Context<Self>) -> Self {
        Self::with_options(options, dispatcher, AppOptions::default(), window, cx)
    }

    /// Constructs the real view with explicit storage and capabilities, without changing the environment.
    #[expect(
        clippy::too_many_lines,
        reason = "Root construction keeps entity owners and startup services together"
    )]
    fn with_options(
        options: &LaunchOptions,
        dispatcher: UiDispatcher,
        services: AppOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let (content_width, workspace_height) = content_size(window.viewport_size());
        let settings_path = services.paths.data().join(crate::settings::SETTINGS_FILE_NAME);
        let settings = GuiSettings::load_from(&settings_path);
        let table = new_results_table(content_width, &settings, window, cx);
        let waveform = cx.new(|_| WaveformView::new());
        let startup::SubscribedSliders {
            sliders,
            subscriptions: slider_subscriptions,
        } = new_sliders(&settings, window, cx);
        let mut subscriptions = vec![
            cx.subscribe_in(&table, window, Self::on_table_event),
            cx.subscribe_in(&waveform, window, Self::on_waveform_event),
        ];
        subscriptions.extend(slider_subscriptions);
        subscriptions.push(cx.on_app_quit(Self::prepare_quit));

        let mut menu_state = initial_menu_state(&settings, &table, cx);
        let editor_owner = window.window_handle().ok().map(|handle| handle.as_raw());
        let Startup {
            coordinators,
            player,
            plugins,
            status,
        } = open_startup(
            &settings,
            dispatcher,
            editor_owner,
            &mut menu_state,
            &services.paths,
            services.playback,
            services.workers,
        );
        set_menus(cx, &menu_state);
        configure_waveform(&waveform, &settings, cx);

        let focus_handle = cx.focus_handle();
        window.focus(&focus_handle, cx);
        let (player_poll_task, plugin_tick_task) = (Self::spawn_player_poll(cx), Self::spawn_plugin_tick(cx));
        let gain_writer = coordinators
            .analysis
            .as_ref()
            .map(|coordinator| gain_writer::GainWriter::new(Arc::clone(coordinator.cache())));

        let panel_width_focus = Self::new_panel_width_focus(cx);
        let mut view = Self {
            focus_handle,
            #[cfg(target_os = "windows")]
            menu_bar: AppMenuBar::new(cx),
            table,
            waveform,
            zoom_slider: sliders.zoom,
            gain_slider: sliders.gain,
            slider_focus: [cx.focus_handle().tab_stop(true), cx.focus_handle(), cx.focus_handle()],
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
            preview_source: preview::PreviewSource::default(),
            selected_tags: None,
            enrichment: enrichment::EnrichmentQueue::default(),
            enrichment_task: None,
            analysis_workers: 0,
            gain_writer,
            selected_artwork: None,
            artwork_requests: ArtworkRequests::default(),
            artwork_rewrites: HashSet::new(),
            artwork_task: None,
            status,
            shown_output: SharedString::default(),
            shown_playback_time: None,
            menu_state,
            settings,
            settings_path,
            dispose: services.dispose,
            settings_window: None,
            settings_window_size: None,
            window_title: String::from(APP_TITLE),
            preview_height: render::preview_fit_height(content_width, workspace_height),
            preview_startup_fit: PreviewStartupFit::default(),
            preview_fixed_size: true,
            workspace_split: cx.new(|_| ResizableState::default()),
            preview_compact: false,
            panel_width_focus,
            panel_width_drag: None,
            autoplay_pending: options.autoplay,
            analysis_task: None,
            demo_task: None,
            pending_gain_paths: Vec::new(),
            start_after_cues: None,
            zoom_reset_pending: false,
            gain_store_task: None,
            normalize_target_store_task: None,
            gain_prediction: gain_loudness::GainPrediction::default(),
            gain_prediction_task: None,
            _subscriptions: subscriptions,
            _player_poll_task: player_poll_task,
            launch_task: None,
            _plugin_tick_task: plugin_tick_task,
            cache_prune: None,
            #[cfg(test)]
            test_directory: services.directory,
        };
        view.start_cache_prune(&*cx);
        view.connect_playhead(cx);
        view.refresh_plugin_preview(cx);
        if let Some(rows) = options.demo_rows {
            view.start_demo(rows, cx);
        } else if !options.paths.is_empty() {
            view.add_roots(&options.paths, cx);
        }
        view
    }

    /// Saves preferences to the destination captured by the constructor.
    fn save_settings(&self) {
        if let Err(error) = self.settings.save_to(&self.settings_path) {
            tracing::error!("Failed to save settings: {error:#}");
        }
    }

    /// Removes cache rows of deleted files on a background task, keeping filesystem checks off the GUI thread.
    /// Rows of files in missing folders stay, since an unmounted drive must not lose its custom gains.
    fn start_cache_prune(&mut self, cx: &Context<Self>) {
        let Some(cache) = self
            .coordinator
            .as_ref()
            .map(|coordinator| Arc::clone(coordinator.cache()))
        else {
            return;
        };
        let stop = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop);
        let task = cx.background_executor().spawn(async move {
            if let Err(error) = cache.prune_missing_files(&|| worker_stop.load(Ordering::Acquire)) {
                tracing::warn!("Could not prune cache rows of missing files: {error}");
            }
        });
        self.cache_prune = Some((stop, task));
    }

    /// Invalidates readers and drains ordered explicit gains off the message thread before application quit.
    fn prepare_quit(&mut self, cx: &mut Context<Self>) -> Task<()> {
        if let Some((stop, _)) = &self.cache_prune {
            stop.store(true, Ordering::Release);
        }
        self.flush_gain_store(cx);
        if self.normalize_target_store_task.is_some() {
            self.store_normalize_target(cx);
        }
        self.save_plugin_chain();
        self.invalidate_artwork(cx);
        self.enrichment.invalidate();
        self.gain_prediction.stopped = true;
        self.gain_prediction.cancel();
        let writer = self.gain_writer.as_mut().and_then(gain_writer::GainWriter::finish);
        cx.background_executor().spawn(async move {
            if let Some(writer) = writer {
                let _ = writer.join();
            }
        })
    }

    /// Updates both the system menus and the Windows in-window menu bar.
    fn refresh_menus(&self, cx: &mut Context<Self>) {
        set_menus(cx, &self.menu_state);
        #[cfg(target_os = "windows")]
        self.menu_bar.update(cx, AppMenuBar::reload);
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
    /// or none when no analysis, normalization, plugin processing run, or plugin scan is in progress.
    /// Files being worked on count with their fraction, so the line moves smoothly on long files.
    /// A plugin scan shows only while no file run is in progress, since that run is what the table shows.
    fn overall_progress(&self, cx: &Context<Self>) -> Option<ActivityProgress> {
        let file_run = self
            .normalize_run
            .as_ref()
            .filter(|_| self.is_normalizing())
            .or_else(|| self.process_run.as_ref().filter(|_| self.is_processing()));
        let FileRunProgress {
            activity,
            finished,
            total,
        } = if let Some(run) = file_run {
            FileRunProgress {
                activity: run.kind.activity(),
                finished: run.finished,
                total: run.total,
            }
        } else if let Some(run) = self.run.as_ref().filter(|_| self.is_analyzing()) {
            FileRunProgress {
                activity: "Analyzing",
                finished: run.finished,
                total: run.total,
            }
        } else {
            let scan = self.plugins.scan_progress()?;
            return Some(ActivityProgress {
                activity: "Scanning plugins",
                fraction: scan.fraction,
            });
        };
        if total == 0 {
            return Some(ActivityProgress {
                activity,
                fraction: 0.0,
            });
        }
        let active = self.table.read(cx).delegate().active_progress();
        Some(ActivityProgress {
            activity,
            fraction: ((finished as f32 + active) / total as f32).clamp(0.0, 1.0),
        })
    }
}

#[cfg(test)]
impl AppOptions {
    /// Reuses one owned scratch directory across view recreation, without consulting the environment.
    fn in_test_directory(directory: Arc<tempfile::TempDir>) -> Self {
        Self {
            paths: StoragePaths::in_root(directory.path()).expect("absolute test storage"),
            playback: false,
            workers: 1,
            dispose: Arc::new(|path| std::fs::remove_file(path).is_ok()),
            directory,
        }
    }
}

#[cfg(not(test))]
impl Default for AppOptions {
    fn default() -> Self {
        Self {
            paths: StoragePaths::from_environment().expect("valid AUDIOBATCH_DATA_ROOT"),
            playback: true,
            workers: std::thread::available_parallelism().map_or(1, std::num::NonZeroUsize::get),
            dispose: trash_disposer(),
        }
    }
}

#[cfg(test)]
impl Default for AppOptions {
    fn default() -> Self {
        Self::in_test_directory(Arc::new(tempfile::tempdir().expect("isolated view storage")))
    }
}

/// Applies saved waveform presentation before any source is selected.
fn configure_waveform(waveform: &Entity<WaveformView>, settings: &GuiSettings, cx: &mut Context<AppView>) {
    waveform.update(cx, |waveform, cx| {
        waveform.set_view_mode(settings.preview_view_mode, cx);
        waveform.set_overload_settings(
            settings.show_sample_overloads,
            settings.show_true_peak_overloads,
            settings.show_overload_lines,
            cx,
        );
        waveform.set_show_loudness(settings.show_loudness_graph, cx);
        waveform.set_serato_colours(settings.serato_waveform_colours, cx);
        waveform.set_loudness_range(settings.loudness_minimum, settings.loudness_maximum, cx);
    });
}

/// Returns the menu state of the saved settings and the table columns, before the output device is opened.
fn initial_menu_state(
    settings: &GuiSettings,
    table: &Entity<TableState<ResultsDelegate>>,
    cx: &Context<AppView>,
) -> MenuState {
    MenuState {
        output_device: None,
        show_loudness_graph: settings.show_loudness_graph,
        preview_view_mode: settings.preview_view_mode,
        show_sample_overloads: settings.show_sample_overloads,
        show_true_peak_overloads: settings.show_true_peak_overloads,
        show_overload_lines: settings.show_overload_lines,
        serato_colours: settings.serato_waveform_colours,
        show_album_artwork: settings.show_album_artwork,
        show_gain_adjusted_metrics: settings.show_gain_adjusted_metrics,
        preview_plugin_chain: settings.preview_plugin_chain,
        compensate_preview_latency: settings.compensate_preview_latency,
        continue_playback: settings.continue_playback,
        play_from_first_cue_point: settings.play_from_first_cue_point,
        normalize_target: describe_target(settings),
        columns: column_choices(table.read(cx).delegate()),
        ..MenuState::default()
    }
}

/// Returns the content width and the height left for the table and the preview in a window of the given size.
fn content_size(viewport: Size<Pixels>) -> (f32, f32) {
    let width = CONTENT_PADDING.mul_add(-2.0, f32::from(viewport.width));
    let height = CONTENT_PADDING.mul_add(-2.0, f32::from(viewport.height)) - STATUS_BAR_HEIGHT - MENU_BAR_HEIGHT;
    (width, height)
}
