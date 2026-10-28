//! What the main window opens at startup: the results table, the sliders of the controls column,
//! the analysis cache with the coordinators sharing it, the audio device, and the plugin chain.

use std::sync::Arc;

use audiobatch_core::{AnalysisCache, AnalysisCoordinator, NormalizeCoordinator};
use audiobatch_playback::Player;
use audiobatch_plugins::{ProcessingCoordinator, default_worker_count};
use gpui_kit::component::slider::{SliderEvent, SliderScale, SliderState};
use gpui_kit::component::table::TableState;
use gpui_kit::{AppContext as _, Context, Entity, SharedString, Subscription, Window};
use raw_window_handle::RawWindowHandle;
use vst3_host::UiDispatcher;

use super::gain::{DEFAULT_GAIN_DB, MAXIMUM_GAIN_DB, MINIMUM_GAIN_DB};
use super::preferences::open_player;
use super::{AppView, TABLE_FRAME_BORDER};
use crate::menus::MenuState;
use crate::normalization::target_range;
use crate::plugins::labels::group_by_vendor;
use crate::plugins::{FALLBACK_SAMPLE_RATE, PluginHost};
use crate::results::ResultsDelegate;
use crate::settings::{GuiSettings, NormalizeMode};
use crate::waveform::{MAXIMUM_ZOOM, MINIMUM_ZOOM};

/// The background coordinators, sharing one analysis cache, or none when the cache cannot be opened.
#[derive(Default)]
pub(super) struct Coordinators {
    /// Analyzes files in the background.
    pub(super) analysis: Option<Arc<AnalysisCoordinator>>,
    /// Normalizes files in the background.
    pub(super) normalize: Option<Arc<NormalizeCoordinator>>,
    /// Processes files with the plugin chain in the background.
    pub(super) process: Option<Arc<ProcessingCoordinator>>,
}

/// What the main view opens at startup, with the problem to show in the status line.
pub(super) struct Startup {
    /// The background coordinators, or none when the cache cannot be opened.
    pub(super) coordinators: Coordinators,
    /// The preview player, or none when no output device opens.
    pub(super) player: Option<Player>,
    /// The known plugins and the plugin chain.
    pub(super) plugins: PluginHost,
    /// The first problem to show in the status line, or empty.
    pub(super) status: SharedString,
}

/// The sliders of the controls column.
pub(super) struct Sliders {
    /// The waveform zoom.
    pub(super) zoom: Entity<SliderState>,
    /// The input gain of the selected files in decibels.
    pub(super) gain: Entity<SliderState>,
    /// The peak normalization target in dBFS.
    pub(super) peak_target: Entity<SliderState>,
    /// The loudness normalization target in LUFS.
    pub(super) loudness_target: Entity<SliderState>,
}

/// Creates the results table for the content width, with multi-row selection kept by the delegate.
/// It hides the columns the settings name, or the columns hidden by default when no choice is saved.
pub(super) fn new_results_table(
    content_width: f32,
    settings: &GuiSettings,
    window: &mut Window,
    cx: &mut Context<AppView>,
) -> Entity<TableState<ResultsDelegate>> {
    let mut delegate = ResultsDelegate::new(TABLE_FRAME_BORDER.mul_add(-2.0, content_width));
    if let Some(hidden) = &settings.hidden_columns {
        delegate.set_hidden_columns(hidden.iter().filter_map(|id| ResultsDelegate::column_from_id(id)));
    }
    cx.new(|cx| {
        TableState::new(delegate, window, cx)
            .sortable(true)
            .col_resizable(true)
            .row_selectable(true)
            .select_on_modifier_click(false)
    })
}

/// Creates the sliders of the controls column, and subscribes the view to them.
pub(super) fn new_sliders(
    settings: &GuiSettings,
    window: &Window,
    cx: &mut Context<AppView>,
) -> (Sliders, [Subscription; 4]) {
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

/// Opens the coordinators, the audio device, and the plugin chain, and fills in their part of the menus.
/// Plugin editors open as windows owned by `editor_owner`.
/// A problem with playback replaces one with analysis in the status line,
/// and plugin problems are shown only when neither failed.
pub(super) fn open_startup(
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
    menu_state.chain_summary = plugins.chain.summary();
    menu_state.plugin_groups = group_by_vendor(&plugins.known.plugins);
    menu_state.presets = plugins.preset_names();
    Startup {
        coordinators,
        player,
        plugins,
        status,
    }
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
