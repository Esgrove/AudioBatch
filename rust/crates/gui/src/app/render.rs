//! Layout of the main window.
//!
//! The results table, and below a draggable divider the preview with the controls, the information panel,
//! and the waveform.
//! In a wide window the controls and the information panel switch to their wide layouts.
//! Their independently resizable widths follow responsive baselines, leaving the waveform the remaining space.
//! A status bar along the bottom edge shows the listed folders, the run status with its progress,
//! and the output device with its sample rate and buffer size.
//! The window title shows the progress of analysis, normalization, or plugin processing,
//! and files dropped anywhere on the window are added.

use std::rc::Rc;
use std::sync::Arc;

#[cfg(test)]
use gpui_kit::test::TestSupportExt as _;

use crate::settings::PreviewViewMode;
use audiobatch_core::format::format_kilohertz;
use audiobatch_playback::Player;
use gpui_kit::base::{ResizableState, ResizeHandleContext, ResizeHandleRenderer, ResizeHandleState};
use gpui_kit::component::menu::ContextMenuExt as _;
use gpui_kit::component::resizable::{resizable_panel, v_resizable};
use gpui_kit::component::table::DataTable;
use gpui_kit::component::{Sizable as _, Size};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, AppContext as _, Axis, Context, Div, ExternalPaths, FocusHandle, Focusable, InteractiveElement as _,
    IntoElement, ParentElement as _, Pixels, Render, Role, SharedString, Stateful, StatefulInteractiveElement as _,
    Styled as _, Window, div, px, relative, transparent_black,
};

use super::panel_widths::{PreviewPanel, baseline_widths, compact_controls_width};
use super::{
    APP_TITLE, AppView, CONTENT_PADDING, PREVIEW_MINIMUM_HEIGHT, SECTION_SPACING, STATUS_BAR_HEIGHT,
    TABLE_FRAME_BORDER, content_size,
};
use crate::control_ids;
use crate::controls::{
    ControlsEvent, ControlsState, EventCallback, NormalizeControls, PlaybackControls, SECTION_TOGGLE_SIZE,
    fit_height as controls_fit_height, render_controls, render_playback_row, render_section_toggle,
};
#[cfg(windows)]
use crate::file_drag::DraggedFiles;
use crate::info_panel::{COMPACT_METRIC_SPACING, render_compact_info_panel, render_info_panel};
use crate::menus::{
    About, AddFiles, AddFolder, AddPluginToChain, CancelRun, ChooseFolder, ClearCustomGain, ClearList,
    ClearPluginChain, JumpToHotCue, KEY_CONTEXT, LoadChainPreset, NormalizeAll, NormalizeSelected, OpenAudioSettings,
    OpenParentFolder, OpenPluginChain, OpenPluginManager, ProcessAll, ProcessSelected, ReanalyzeSelected,
    RemoveSelected, Rescan, RescanPlugins, RevealSelected, SaveChainPreset, SelectAll, SelectOutputDevice,
    SetPreviewViewMode, ShowPresetsFolder, StopPlayback, ToggleAlbumArtwork, ToggleColumn, ToggleContinuePlayback,
    ToggleGainAdjustedMetrics, ToggleLoudnessGraph, ToggleOverloadLines, TogglePlayFromFirstCuePoint, TogglePlayback,
    TogglePluginPreview, TogglePreviewLatencyCompensation, TogglePreviewSection, ToggleSampleOverloads,
    ToggleSeratoColours, ToggleTruePeakOverloads, TrashSelected, TrashSelectedNow,
};
use crate::normalization::target_value_text;
use crate::playback_status::format_playback_time;
use crate::results::{HEADER_HEIGHT, ROW_HEIGHT, ResultRow, RowMenuState, fit_results_columns};
use crate::theme::{TEXT_FONT, color, palette, tooltip};
use crate::waveform::MINIMUM_ZOOM;

/// Height of the overall progress line along the top edge of the status bar.
const PROGRESS_LINE_HEIGHT: f32 = 3.0;

/// Widest output device text in the status bar before it is truncated.
const OUTPUT_MAXIMUM_WIDTH: f32 = 360.0;

/// Font size of the status bar text.
const STATUS_FONT_SIZE: f32 = 12.0;

/// Horizontal padding at both ends of the status bar.
const STATUS_PADDING: f32 = 8.0;

/// Space between the sections of the status bar, with the separator in the middle.
const STATUS_SECTION_SPACING: f32 = 16.0;

/// Gap above and below the separators between the status bar sections.
const STATUS_SEPARATOR_INSET: f32 = 5.0;

/// Share of the status bar width given to the listed folders, leaving the status text the larger part.
const PATH_PROPORTION: f32 = 0.4;

/// Tooltip of the status text.
const STATUS_TOOLTIP: &str = "Background analysis, normalization, and plugin processing status.";

/// Smallest height of the results table.
const TABLE_MINIMUM_HEIGHT: f32 = 240.0;

/// Largest share of the workspace height the preview can be dragged to.
const PREVIEW_MAXIMUM_PROPORTION: f32 = 0.7;

/// Space on each side of the divider between the table and the preview.
const DIVIDER_SPACING: f32 = 4.0;

/// Height of the collapsed preview, including the space below the table.
const COMPACT_PREVIEW_HEIGHT: f32 = 52.0;

/// Space between the chevron and the playback buttons of the compact row.
const COMPACT_SPACING: f32 = 6.0;

/// Share of the width right of the playback buttons given to the compact metrics.
const COMPACT_METRICS_PROPORTION: f32 = 0.35;

/// Narrowest width of the compact metrics: four cards 100 pixels wide with the spacing between them.
const COMPACT_METRICS_MINIMUM_WIDTH: f32 = 400.0 + COMPACT_METRIC_SPACING * 3.0;

/// Narrowest width the compact row leaves the waveform before the metrics stop shrinking it.
const COMPACT_WAVEFORM_MINIMUM_WIDTH: f32 = 140.0;

/// Maximum startup frames to wait when a platform does not report its requested maximized state.
const STARTUP_GEOMETRY_FRAME_LIMIT: u16 = 60;

/// Consecutive startup geometry observations, independent of the rendered split state.
#[derive(Default)]
pub(super) struct PreviewStartupFit {
    previous: Option<(f32, f32)>,
    observations: u16,
    complete: bool,
}

impl PreviewStartupFit {
    /// Commits once after stable valid geometry and platform readiness, with a bounded platform fallback.
    fn observe(&mut self, geometry: (f32, f32), platform_ready: bool) -> bool {
        if self.complete || geometry.0 <= 0.0 || geometry.1 <= 0.0 {
            return false;
        }
        self.observations = self.observations.saturating_add(1);
        let stable = self
            .previous
            .is_some_and(|previous| (previous.0 - geometry.0).abs() < 0.5 && (previous.1 - geometry.1).abs() < 0.5);
        self.previous = Some(geometry);
        if stable && (platform_ready || self.observations >= STARTUP_GEOMETRY_FRAME_LIMIT) {
            self.complete = true;
            return true;
        }
        false
    }
}

impl AppView {
    /// Applies a queued zoom reset through the retained slider state before composing the frame.
    fn apply_pending_zoom_reset(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if std::mem::take(&mut self.zoom_reset_pending) {
            self.zoom_slider
                .update(cx, |slider, cx| slider.set_value(MINIMUM_ZOOM as f32, window, cx));
        }
    }

    /// Returns the status bar text for the audio output:
    /// the device name, its sample rate, and its buffer size, for example "Speakers, 48 kHz, 512 samples".
    /// The buffer size is left out until the first audio callback reports it.
    pub(super) fn output_label(&self) -> SharedString {
        self.player.as_ref().map_or_else(
            || SharedString::from("No output device"),
            |player| {
                let mut parts = vec![
                    player.device_name().to_string(),
                    format!("{} kHz", format_kilohertz(player.output_rate())),
                ];
                if let Some(frames) = player.buffer_frames() {
                    parts.push(format!("{frames} samples"));
                }
                SharedString::from(parts.join(", "))
            },
        )
    }

    /// Waits for matching startup geometry on consecutive frames, including deferred platform maximization.
    /// Commits the controls-fit split once, and never refits it on later resize, selection, or artwork arrival.
    pub fn schedule_preview_startup_fit(&self, initially_maximized: bool, window: &Window, cx: &Context<Self>) {
        if self.preview_startup_fit.complete {
            return;
        }
        let this = cx.weak_entity();
        window.on_next_frame(move |window, cx| {
            let _ = this.update(cx, |view, cx| {
                let geometry = content_size(window.viewport_size());
                let platform_ready = !initially_maximized || window.is_maximized();
                if view.preview_startup_fit.observe(geometry, platform_ready) {
                    view.preview_height = preview_fit_height(geometry.0, geometry.1);
                    view.workspace_split = cx.new(|_| ResizableState::default());
                    cx.notify();
                }
                if !view.preview_startup_fit.complete {
                    view.schedule_preview_startup_fit(initially_maximized, window, cx);
                }
            });
        });
    }

    /// Sets the window title, with the activity and its progress while a run is in progress.
    fn update_window_title(&mut self, progress: Option<(&str, f32)>, window: &mut Window) {
        let title = progress.map_or_else(
            || String::from(APP_TITLE),
            |(activity, fraction)| format!("{APP_TITLE} - {activity} {:.0}%", fraction * 100.0),
        );
        if title != self.window_title {
            window.set_window_title(&title);
            self.window_title = title;
        }
    }

    /// Returns the status bar text for the listed folders: the first one, with the number of further roots.
    fn root_label(&self) -> String {
        match self.roots.as_slice() {
            [] => String::new(),
            [single] => single.display().to_string(),
            [first, rest @ ..] => format!("{} (+{} more)", first.display(), rest.len()),
        }
    }

    /// Renders the status bar along the bottom edge of the window:
    /// the listed folders, the run status, and the output device, with the run progress along its top edge.
    fn render_status_bar(&self, progress: Option<f32>) -> Div {
        let roots = self
            .roots
            .iter()
            .map(|root| root.display().to_string())
            .collect::<Vec<_>>()
            .join("\n");
        let path = div()
            .id("root-path")
            .w(relative(PATH_PROPORTION))
            .flex_none()
            .min_w_0()
            .truncate()
            .child(self.root_label());
        let path = if roots.is_empty() {
            path
        } else {
            path.tooltip(tooltip(roots))
        };
        let output = self.output_label();
        let playback_time = self.player.as_ref().and_then(|player| {
            let duration = player.duration_seconds()?;
            (duration > 0.0).then(|| format_playback_time(player.position_seconds(), duration))
        });
        let status = if self.is_running() {
            self.status.clone()
        } else {
            self.enrichment.progress().map_or_else(
                || self.status.clone(),
                |(completed, total)| SharedString::from(format!("Background previews: {completed}/{total} files")),
            )
        };
        div()
            .relative()
            .h(px(STATUS_BAR_HEIGHT))
            .flex_none()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(STATUS_SECTION_SPACING * 0.5))
            .px(px(STATUS_PADDING))
            .bg(color(palette::GREY_SEMI_DARK))
            .border_t_1()
            .border_color(color(palette::GREY_MEDIUM))
            .text_size(px(STATUS_FONT_SIZE))
            .text_color(color(palette::GREY_MIDDLE_LIGHT))
            .child(path)
            .child(status_separator())
            .child(status_text(status))
            .children(playback_time.map(|time| {
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .child(status_separator())
                    .child(div().id("playback-time").flex_none().child(time))
            }))
            .child(status_separator())
            .child(
                div()
                    .id("output-device")
                    .flex_none()
                    .max_w(px(OUTPUT_MAXIMUM_WIDTH))
                    .truncate()
                    .tooltip(tooltip(format!("Preview audio output: {output}")))
                    .child(output),
            )
            .children(progress.map(render_progress_line))
    }

    /// Renders the results table in its outlined frame, highlighted while files are dragged over it.
    fn render_table(&self) -> impl IntoElement {
        let frame = div().id("results-frame");
        // TODO: delete this trigger and `file_drag::windows` once gpui implements `start_external_drag` on Windows,
        // since the rows' `external_drag_payload` then covers Windows as well.
        #[cfg(windows)]
        let frame = frame.on_drag_move::<DraggedFiles>(start_windows_file_drag);
        // The table's own observed element is "table", so tests read the shared "results" control within this scope.
        div()
            .id(control_ids::RESULTS)
            .size_full()
            .pb(px(DIVIDER_SPACING))
            .child(
                frame
                    .size_full()
                    .rounded(px(4.0))
                    .border(px(TABLE_FRAME_BORDER))
                    .border_color(color(palette::GREY_MEDIUM))
                    .overflow_hidden()
                    .drag_over::<ExternalPaths>(|style, _, _, _| {
                        style
                            .border_color(color(palette::BLUE))
                            .bg(color(palette::BLUE).opacity(0.08))
                    })
                    .child(
                        DataTable::new(&self.table)
                            .accessibility_label(control_ids::RESULTS_LABEL)
                            .stripe(true)
                            .bordered(false)
                            .with_size(Size::Size(px(ROW_HEIGHT)))
                            .header_row_height(px(HEADER_HEIGHT)),
                    ),
            )
    }

    /// Returns the row of the selected file, for the information panel.
    fn selected_row(&self, cx: &App) -> Option<ResultRow> {
        self.selected
            .as_ref()
            .and_then(|path| self.table.read(cx).delegate().row_for(path).cloned())
    }

    /// Returns the app state the right-click row menu of the results table shows.
    fn row_menu_state(&self) -> RowMenuState {
        RowMenuState {
            action_context: Some(self.focus_handle.clone()),
            idle: !self.is_running(),
            can_normalize: self.can_start_normalization(),
            can_process: self.can_start_processing(),
            normalize_target: self.menu_state.normalize_target.clone(),
        }
    }

    /// Returns what the Play/Stop and Process buttons show.
    fn playback_controls(&self, cx: &Context<Self>) -> PlaybackControls {
        PlaybackControls {
            playing: self.player.as_ref().is_some_and(Player::is_playing),
            can_play: self.player.is_some() && (self.selected.is_some() || self.is_demo()),
            can_process: self.can_process_selected(cx),
            running: self.is_running(),
        }
    }

    /// Renders the preview: the controls, the information panel, and the waveform side by side.
    /// A wide window gives the controls and the information panel their wide layouts, see `baseline_widths`.
    fn render_preview(&self, content_width: f32, cx: &Context<Self>) -> Div {
        let widths = self.panel_widths(content_width);
        let (controls_width, information_width, wide) = (widths.controls, widths.information, widths.wide);
        let selected_tags = self
            .selected_tags
            .as_ref()
            .filter(|(path, _)| self.selected.as_ref() == Some(path))
            .map(|(_, tags)| Arc::clone(tags));
        let controls = ControlsState {
            zoom: &self.zoom_slider,
            gain: &self.gain_slider,
            can_adjust_gain: self.can_adjust_gain(cx),
            normalize: NormalizeControls {
                mode: self.settings.normalize_mode,
                target: self.normalize_target_slider(self.settings.normalize_mode),
                target_text: target_value_text(&self.settings),
                overwrite: self.overwrite_existing,
                normalize_first: self.normalize_first,
                can_normalize: self.can_normalize_selected(cx),
            },
            chain_summary: self.chain_summary(),
            preview_chain: self.settings.preview_plugin_chain,
            playback: self.playback_controls(cx),
        };

        div()
            .size_full()
            .pt(px(DIVIDER_SPACING))
            .flex()
            .flex_row()
            .on_drag_move(cx.listener(move |view, event, _, cx| view.drag_panel_width(event, content_width, cx)))
            .child(div().w(px(controls_width)).h_full().flex_none().child(render_controls(
                &controls,
                wide,
                &Self::controls_listener(cx),
                cx,
            )))
            .child(self.render_panel_width_divider(PreviewPanel::Controls, content_width, cx))
            .child(
                div()
                    .w(px(information_width))
                    .h_full()
                    .flex_none()
                    .child(render_info_panel(
                        self.selected_row(cx).as_ref(),
                        selected_tags.as_deref(),
                        self.selected_artwork(),
                        wide,
                        self.settings.show_gain_adjusted_metrics,
                        self.gained_loudness(cx),
                    )),
            )
            .child(self.render_panel_width_divider(PreviewPanel::Information, content_width, cx))
            .child(
                div()
                    .flex_1()
                    .min_w(px(widths.waveform))
                    .h_full()
                    .child(self.render_preview_plot("preview-plot")),
            )
    }

    /// Composes the retained plot with a standard context menu targeting the canonical preference owner.
    fn render_preview_plot(&self, id: &'static str) -> impl IntoElement {
        let focus = self.focus_handle.clone();
        let mode = self.settings.preview_view_mode;
        let plot = div().id(id).size_full().child(self.waveform.clone());
        #[cfg(test)]
        let plot = plot.test_support();
        plot.context_menu(move |mut menu, _, _| {
            menu = menu.action_context(focus.clone());
            for choice in [
                PreviewViewMode::Waveform,
                PreviewViewMode::Spectrogram,
                PreviewViewMode::SideBySide,
                PreviewViewMode::Stacked,
            ] {
                menu = menu.menu_with_check(
                    choice.label(),
                    choice == mode,
                    Box::new(SetPreviewViewMode { mode: choice }),
                );
            }
            menu
        })
    }

    /// Renders the collapsed preview in one row:
    /// the chevron, Play/Stop and Process at their original responsive widths, the four metrics, and the waveform.
    fn render_compact_preview(&self, content_width: f32, cx: &Context<Self>) -> Div {
        let on_event = Self::controls_listener(cx);
        let playback_width = compact_controls_width(content_width) - SECTION_TOGGLE_SIZE - COMPACT_SPACING;
        let remaining = SECTION_SPACING.mul_add(-2.0, content_width - compact_controls_width(content_width));
        let metrics_width = (remaining * COMPACT_METRICS_PROPORTION).clamp(
            COMPACT_METRICS_MINIMUM_WIDTH,
            (remaining - COMPACT_WAVEFORM_MINIMUM_WIDTH).max(COMPACT_METRICS_MINIMUM_WIDTH),
        );
        div()
            .h(px(COMPACT_PREVIEW_HEIGHT))
            .flex_none()
            .pt(px(DIVIDER_SPACING))
            .flex()
            .flex_row()
            .items_center()
            .child(render_section_toggle(true, &on_event))
            .child(
                render_playback_row(self.playback_controls(cx), &on_event)
                    .ml(px(COMPACT_SPACING))
                    .w(px(playback_width))
                    .h_full(),
            )
            .child(
                div()
                    .ml(px(SECTION_SPACING))
                    .w(px(metrics_width))
                    .h_full()
                    .flex_none()
                    .child(render_compact_info_panel(
                        self.selected_row(cx).as_ref(),
                        self.settings.show_gain_adjusted_metrics,
                        self.gained_loudness(cx),
                    )),
            )
            .child(
                div()
                    .ml(px(SECTION_SPACING))
                    .flex_1()
                    .min_w_0()
                    .h_full()
                    .child(self.render_preview_plot("compact-preview-plot")),
            )
    }

    /// Renders the results table above the preview:
    /// split by a draggable divider while expanded, and above the compact row while collapsed.
    fn render_workspace(&self, content_width: f32, workspace_height: f32, cx: &Context<Self>) -> Div {
        if self.preview_compact {
            return div()
                .size_full()
                .flex()
                .flex_col()
                .child(div().flex_1().min_h_0().child(self.render_table()))
                .child(self.render_compact_preview(content_width, cx));
        }
        let preview_maximum = preview_maximum_height(workspace_height);
        let preview = resizable_panel()
            .size(px(self.preview_height))
            .size_range(px(PREVIEW_MINIMUM_HEIGHT)..px(preview_maximum))
            .child(self.render_preview(content_width, cx));
        let preview = if self.preview_fixed_size {
            preview.fixed()
        } else {
            preview
        };
        div().size_full().child(
            v_resizable("workspace")
                .with_state(&self.workspace_split)
                .with_handle_appearance(divider_appearance())
                .on_resize(cx.listener(|view, _, _, _| {
                    view.preview_fixed_size = false;
                    view.preview_startup_fit.complete = true;
                }))
                .child(
                    resizable_panel()
                        .size_range(px(TABLE_MINIMUM_HEIGHT)..Pixels::MAX)
                        .child(self.render_table()),
                )
                .child(preview),
        )
    }

    /// Returns the callback that hands the events of the controls to the view.
    fn controls_listener(cx: &Context<Self>) -> EventCallback {
        Rc::new(cx.listener(|view, event: &ControlsEvent, window, cx| {
            view.handle_controls_event(*event, window, cx);
        }))
    }
}

impl Focusable for AppView {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl AppView {
    /// Synchronizes table sizing and pending controls before composing the main frame.
    fn prepare_frame(&mut self, window: &mut Window, cx: &mut Context<Self>) -> (Option<f32>, f32, f32) {
        let progress = self.overall_progress(cx);
        self.update_window_title(progress, window);
        let (content_width, workspace_height) = content_size(window.viewport_size());
        let table_width = TABLE_FRAME_BORDER.mul_add(-2.0, content_width);
        let row_menu = self.row_menu_state();
        self.table.update(cx, |state, cx| {
            fit_results_columns(state, table_width, window, cx);
            state.delegate_mut().set_row_menu_state(row_menu);
        });
        self.sync_gain_slider(window, cx);
        self.apply_pending_zoom_reset(window, cx);
        (progress.map(|(_, fraction)| fraction), content_width, workspace_height)
    }
}

impl AppView {
    /// Handles the Options menu actions that change the preview and the preferences.
    fn on_option_actions(element: Stateful<Div>, cx: &Context<Self>) -> Stateful<Div> {
        element
            .on_action(cx.listener(|view, _: &ToggleLoudnessGraph, _, cx| view.toggle_loudness_graph(cx)))
            .on_action(
                cx.listener(|view, action: &SetPreviewViewMode, _, cx| view.set_preview_view_mode(action.mode, cx)),
            )
            .on_action(cx.listener(|view, _: &ToggleSampleOverloads, _, cx| view.toggle_sample_overloads(cx)))
            .on_action(cx.listener(|view, _: &ToggleTruePeakOverloads, _, cx| view.toggle_true_peak_overloads(cx)))
            .on_action(cx.listener(|view, _: &ToggleOverloadLines, _, cx| view.toggle_overload_lines(cx)))
            .on_action(cx.listener(|view, _: &ToggleSeratoColours, _, cx| view.toggle_serato_colours(cx)))
            .on_action(cx.listener(|view, _: &ToggleAlbumArtwork, _, cx| view.toggle_album_artwork(cx)))
            .on_action(cx.listener(|view, _: &ToggleGainAdjustedMetrics, _, cx| view.toggle_gain_adjusted_metrics(cx)))
            .on_action(cx.listener(|view, _: &TogglePreviewSection, _, cx| view.toggle_preview_section(cx)))
            .on_action(cx.listener(|view, _: &TogglePluginPreview, _, cx| view.toggle_plugin_preview(cx)))
            .on_action(cx.listener(|view, _: &TogglePreviewLatencyCompensation, _, cx| {
                view.toggle_preview_latency_compensation(cx);
            }))
            .on_action(cx.listener(|view, _: &ToggleContinuePlayback, _, cx| view.toggle_continue_playback(cx)))
            .on_action(cx.listener(|view, _: &TogglePlayFromFirstCuePoint, _, cx| {
                view.toggle_play_from_first_cue_point(cx);
            }))
            .on_action(cx.listener(|view, _: &OpenAudioSettings, window, cx| view.open_settings_window(window, cx)))
    }
}

impl Render for AppView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let (progress, content_width, workspace_height) = self.prepare_frame(window, cx);
        Self::on_option_actions(div().id("app"), cx)
            .key_context(KEY_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_mouse_up(
                gpui_kit::MouseButton::Left,
                cx.listener(|view, _, _, cx| view.finish_panel_width_drag(cx)),
            )
            .on_mouse_up_out(
                gpui_kit::MouseButton::Left,
                cx.listener(|view, _, _, cx| view.finish_panel_width_drag(cx)),
            )
            .on_action(cx.listener(|_, _: &ChooseFolder, _, cx| Self::prompt_for_paths(true, true, cx)))
            .on_action(cx.listener(|_, _: &AddFolder, _, cx| Self::prompt_for_paths(true, false, cx)))
            .on_action(cx.listener(|_, _: &AddFiles, _, cx| Self::prompt_for_paths(false, false, cx)))
            .on_action(cx.listener(|view, _: &Rescan, _, cx| view.rescan(cx)))
            .on_action(cx.listener(|view, _: &RevealSelected, _, cx| view.reveal_selected(cx)))
            .on_action(cx.listener(|view, _: &OpenParentFolder, _, cx| view.open_parent_folder(cx)))
            .on_action(cx.listener(|view, _: &ReanalyzeSelected, _, cx| view.reanalyze_selected(cx)))
            .on_action(cx.listener(|view, _: &NormalizeSelected, window, cx| view.normalize_selected(window, cx)))
            .on_action(cx.listener(|view, _: &NormalizeAll, window, cx| view.normalize_all(window, cx)))
            .on_action(cx.listener(|view, _: &ClearCustomGain, _, cx| view.clear_selected_gain(cx)))
            .on_action(cx.listener(|view, _: &RemoveSelected, _, cx| view.remove_selected(cx)))
            .on_action(cx.listener(|view, _: &SelectAll, _, cx| view.select_all(cx)))
            .on_action(cx.listener(|view, action: &ToggleColumn, _, cx| view.toggle_column(&action.id, cx)))
            .on_action(cx.listener(|view, _: &TrashSelected, window, cx| view.trash_selected(true, window, cx)))
            .on_action(cx.listener(|view, _: &TrashSelectedNow, window, cx| view.trash_selected(false, window, cx)))
            .on_action(cx.listener(|view, _: &ClearList, _, cx| view.clear_list(cx)))
            .on_action(cx.listener(|view, action: &JumpToHotCue, _, cx| view.jump_to_hot_cue(action.slot, cx)))
            .on_action(cx.listener(|view, _: &TogglePlayback, _, cx| view.toggle_playback(cx)))
            .on_action(cx.listener(|view, _: &StopPlayback, _, cx| view.stop_playback(cx)))
            .on_action(cx.listener(|view, action: &SelectOutputDevice, _, cx| {
                view.select_output_device(&action.name, cx);
            }))
            .on_action(cx.listener(|view, _: &OpenPluginChain, _, cx| view.open_chain_window(cx)))
            .on_action(cx.listener(|view, _: &OpenPluginManager, _, cx| view.open_manager_window(cx)))
            .on_action(cx.listener(|view, _: &RescanPlugins, _, cx| view.start_plugin_scan(cx)))
            .on_action(cx.listener(|view, _: &ClearPluginChain, _, cx| view.clear_plugin_chain(cx)))
            .on_action(cx.listener(|view, action: &AddPluginToChain, _, cx| view.add_plugin_from_menu(action, cx)))
            .on_action(cx.listener(|view, _: &SaveChainPreset, _, cx| view.open_chain_window(cx)))
            .on_action(cx.listener(|view, _: &ShowPresetsFolder, _, cx| view.show_presets_folder(cx)))
            .on_action(cx.listener(|view, action: &LoadChainPreset, _, cx| {
                view.load_chain_preset(&action.name, cx);
            }))
            .on_action(cx.listener(|view, _: &ProcessSelected, window, cx| view.process_selected(window, cx)))
            .on_action(cx.listener(|view, _: &ProcessAll, window, cx| view.process_all(window, cx)))
            .on_action(cx.listener(|view, _: &CancelRun, _, cx| view.cancel_run(cx)))
            .on_action(cx.listener(|_, _: &About, window, cx| Self::show_about(window, cx)))
            .on_drop(cx.listener(|view, paths: &ExternalPaths, window, cx| {
                view.handle_drop(paths.paths(), window.modifiers().shift, cx);
            }))
            .size_full()
            .flex()
            .flex_col()
            .bg(color(palette::GREY_DARK))
            .text_color(color(palette::GREY_LIGHT))
            .font_family(TEXT_FONT)
            .text_size(px(15.0))
            .when(cfg!(target_os = "windows"), |element| {
                #[cfg(target_os = "windows")]
                let element = element.child(
                    div()
                        .w_full()
                        .h(px(super::MENU_BAR_HEIGHT))
                        .flex_none()
                        .border_b_1()
                        .border_color(color(palette::GREY_MEDIUM))
                        .child(self.menu_bar.clone()),
                );
                element
            })
            .child(
                div().flex_1().min_h_0().flex().flex_col().p(px(CONTENT_PADDING)).child(
                    div()
                        .flex_1()
                        .min_h_0()
                        .child(self.render_workspace(content_width, workspace_height, cx)),
                ),
            )
            .child(self.render_status_bar(progress))
    }
}

/// Draws the divider between the table and the preview.
/// It is invisible at rest, so the gap between the two stays clean,
/// grey while the pointer is over it, and blue while it is pressed or dragged.
/// The resize cursor and the drag come from the panel group, whatever this draws.
fn divider_appearance() -> ResizeHandleRenderer {
    Rc::new(|handle: &ResizeHandleContext, _: &mut Window, _: &mut App| {
        let line_color = match handle.state() {
            ResizeHandleState::Idle => transparent_black(),
            ResizeHandleState::Hovered => color(palette::GREY_MIDDLE),
            ResizeHandleState::Pressed | ResizeHandleState::Dragging => color(palette::BLUE),
        };
        let line = div().flex_none().bg(line_color);
        let line = match handle.axis() {
            Axis::Horizontal => line.h_full().w(px(1.0)),
            Axis::Vertical => line.w_full().h(px(1.0)),
        };
        Some(line.into_any_element())
    })
}

/// Returns the height the expanded preview starts at in a content area of the given size:
/// the height that shows all controls of the layout the width picks without scrolling,
/// with the space below the divider.
/// It stays within the range the divider allows, so a short window keeps the table usable
/// and the controls scroll instead.
pub(super) fn preview_fit_height(content_width: f32, workspace_height: f32) -> f32 {
    (DIVIDER_SPACING + controls_fit_height(baseline_widths(content_width).wide))
        .clamp(PREVIEW_MINIMUM_HEIGHT, preview_maximum_height(workspace_height))
}

/// Returns the tallest height the divider allows the expanded preview in a workspace of the given height.
fn preview_maximum_height(workspace_height: f32) -> f32 {
    (workspace_height * PREVIEW_MAXIMUM_PROPORTION)
        .min(workspace_height - TABLE_MINIMUM_HEIGHT)
        .max(PREVIEW_MINIMUM_HEIGHT + 1.0)
}

/// Hands a drag of result rows to the OLE drag of `file_drag::windows` once the pointer leaves the window.
///
/// This trigger exists only because gpui-pre 0.3.10 returns false from `can_start_external_drag` on Windows,
/// so the rows' `external_drag_payload` never starts a platform drag there.
/// Stopping the gpui drag ends the gesture inside the window, so this runs once per gesture.
/// The modal OLE loop starts from a foreground task, after this mouse event has been handled,
/// so its message pump does not re-enter gpui while the app is being updated.
#[cfg(windows)]
fn start_windows_file_drag(event: &gpui_kit::DragMoveEvent<DraggedFiles>, window: &mut Window, cx: &mut App) {
    let position = event.event.position;
    let viewport = window.viewport_size();
    let inside =
        position.x >= px(0.0) && position.y >= px(0.0) && position.x < viewport.width && position.y < viewport.height;
    if inside {
        return;
    }
    let paths = event.drag(cx).existing_paths(cx);
    cx.stop_active_drag(window);
    cx.refresh_windows();
    if paths.is_empty() {
        return;
    }
    cx.spawn(async move |cx| {
        let copied = crate::file_drag::start_file_drag(&paths);
        tracing::debug!(files = paths.len(), copied, "file drag out of the window ended");
        cx.update(App::refresh_windows);
    })
    .detach();
}

/// Renders the overall progress of a run as a thin line along the top edge of the status bar.
fn render_progress_line(fraction: f32) -> Div {
    div()
        .absolute()
        .top_0()
        .left_0()
        .right_0()
        .h(px(PROGRESS_LINE_HEIGHT))
        .bg(color(palette::GREY_MIDDLE).opacity(0.25))
        .child(div().h_full().w(relative(fraction)).bg(color(palette::BLUE)))
}

/// Renders the status text as a live status region, so assistive technology and UI tests can read it.
fn status_text(status: SharedString) -> impl IntoElement {
    let text = div()
        .id(control_ids::STATUS)
        .role(Role::Status)
        .aria_label(status.clone())
        .flex_1()
        .min_w_0()
        .truncate()
        .tooltip(tooltip(STATUS_TOOLTIP))
        .child(status);
    #[cfg(test)]
    let text = text.test_support();
    text
}

/// Renders the thin vertical line between two sections of the status bar.
fn status_separator() -> Div {
    div()
        .flex_none()
        .w(px(1.0))
        .h(px(STATUS_SEPARATOR_INSET.mul_add(-2.0, STATUS_BAR_HEIGHT)))
        .bg(color(palette::GREY_MEDIUM))
}

#[cfg(test)]
mod test_preview_startup_fit {
    use super::*;

    #[test]
    fn workspace_reserves_menu_padding_and_status_bar() {
        let (width, height) = content_size(gpui_kit::size(px(1000.0), px(700.0)));
        assert!((width - 992.0).abs() < f32::EPSILON);
        let expected_height = if cfg!(target_os = "windows") { 640.0 } else { 668.0 };
        assert!((height - expected_height).abs() < f32::EPSILON);
    }

    #[test]
    fn width_breakpoint_matches_the_controls_layout() {
        let breakpoint = 1580.0;
        let wide = preview_fit_height(breakpoint, 1000.0);
        let narrow = preview_fit_height(breakpoint - 1.0, 1000.0);
        assert!((wide - (DIVIDER_SPACING + controls_fit_height(true))).abs() < f32::EPSILON);
        assert!((narrow - (DIVIDER_SPACING + controls_fit_height(false))).abs() < f32::EPSILON);
        assert!(narrow > wide);
        assert!((preview_fit_height(breakpoint + 1.0, 1000.0) - wide).abs() < f32::EPSILON);
    }

    #[test]
    fn short_workspaces_reserve_the_table_minimum() {
        for workspace in [480.0, 520.0, 600.0] {
            let height = preview_fit_height(1000.0, workspace);
            assert!(height >= PREVIEW_MINIMUM_HEIGHT);
            assert!(height <= workspace - TABLE_MINIMUM_HEIGHT);
            assert!(height <= workspace * PREVIEW_MAXIMUM_PROPORTION);
        }
    }

    #[test]
    fn provisional_maximized_geometry_is_not_committed() {
        let mut startup = PreviewStartupFit::default();
        assert!(!startup.observe((1400.0, 700.0), false));
        assert!(!startup.observe((1400.0, 700.0), false));
        assert!(!startup.observe((1900.0, 1000.0), true));
        assert!(startup.observe((1900.0, 1000.0), true));
        assert!(!startup.observe((1200.0, 700.0), true));
    }

    #[test]
    fn manual_split_blocks_startup_refit_and_normal_startup_commits_once() {
        let mut startup = PreviewStartupFit::default();
        assert!(!startup.observe((1900.0, 1000.0), true));
        startup.complete = true;
        assert!(!startup.observe((1900.0, 1000.0), true));
        let mut normal = PreviewStartupFit::default();
        assert!(!normal.observe((1200.0, 700.0), true));
        assert!(normal.observe((1200.0, 700.0), true));
        assert!(!normal.observe((1900.0, 1000.0), true));
    }
}
