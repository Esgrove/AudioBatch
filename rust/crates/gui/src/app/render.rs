//! Layout of the main window.
//!
//! The results table, and below a draggable divider the preview with the controls, the information panel,
//! and the waveform.
//! In a wide window the controls and the information panel switch to their wide layouts and share half the width,
//! so they take less height and the waveform keeps at least the other half.
//! A status bar along the bottom edge shows the listed folders, the run status with its progress,
//! and the output device with its sample rate and buffer size.
//! The window title shows the progress of analysis, normalization, or plugin processing,
//! and files dropped anywhere on the window are added.

use std::rc::Rc;
use std::sync::Arc;

use audiobatch_core::format::format_kilohertz;
use audiobatch_playback::Player;
use gpui_kit::base::{ResizableState, ResizeHandleContext, ResizeHandleRenderer, ResizeHandleState};
use gpui_kit::component::resizable::{resizable_panel, v_resizable};
use gpui_kit::component::table::DataTable;
use gpui_kit::component::{Sizable as _, Size};
use gpui_kit::{
    App, AppContext as _, Axis, Context, Div, ExternalPaths, FocusHandle, Focusable, InteractiveElement as _,
    IntoElement, ParentElement as _, Pixels, Render, SharedString, StatefulInteractiveElement as _, Styled as _,
    Window, div, px, relative, transparent_black,
};

use super::{
    APP_TITLE, AppView, CONTENT_PADDING, PREVIEW_MINIMUM_HEIGHT, SECTION_SPACING, STATUS_BAR_HEIGHT,
    TABLE_FRAME_BORDER, content_size,
};
use crate::controls::{
    ControlsEvent, ControlsState, EventCallback, NormalizeControls, PlaybackControls, SECTION_TOGGLE_SIZE,
    fit_height as controls_fit_height, render_controls, render_playback_row, render_section_toggle,
};
use crate::info_panel::{COMPACT_METRIC_SPACING, render_compact_info_panel, render_info_panel};
use crate::menus::{
    About, AddFiles, AddFolder, AddPluginToChain, CancelRun, ChooseFolder, ClearCustomGain, ClearList,
    ClearPluginChain, JumpToHotCue, KEY_CONTEXT, LoadChainPreset, NormalizeAll, NormalizeSelected, OpenAudioSettings,
    OpenPluginChain, OpenPluginManager, ProcessAll, ProcessSelected, ReanalyzeSelected, RemoveSelected, Rescan,
    RevealSelected, SaveChainPreset, SelectAll, SelectOutputDevice, StopPlayback, ToggleColumn, ToggleLoudnessGraph,
    TogglePlayback, TogglePluginPreview, TogglePreviewLatencyCompensation, TogglePreviewSection, ToggleSeratoColours,
    TrashSelected, TrashSelectedNow,
};
use crate::normalization::target_value_text;
use crate::results::{HEADER_HEIGHT, ROW_HEIGHT, ResultRow, fill_status_column};
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

/// Narrowest width of the controls column.
const CONTROLS_MINIMUM_WIDTH: f32 = 209.0;

/// Widest width of the controls column.
const CONTROLS_MAXIMUM_WIDTH: f32 = 228.0;

/// Share of the window width given to the controls column.
const CONTROLS_WIDTH_PROPORTION: f32 = 0.14;

/// Narrowest width of the information panel.
const INFORMATION_MINIMUM_WIDTH: f32 = 234.0;

/// Widest width of the information panel.
const INFORMATION_MAXIMUM_WIDTH: f32 = 270.0;

/// Share of the window width given to the information panel.
const INFORMATION_WIDTH_PROPORTION: f32 = 0.22;

/// Narrowest half of the content width that turns on the wide preview,
/// in which the controls and the information panel share half the width and the waveform gets the other half.
const WIDE_MINIMUM_HALF_WIDTH: f32 = 790.0;

/// Share of the wide half given to the controls, the rest going to the information panel.
const WIDE_CONTROLS_PROPORTION: f32 = 0.48;

/// Widest width of the controls in the wide preview, so on very wide windows the waveform grows instead.
const WIDE_CONTROLS_MAXIMUM_WIDTH: f32 = 500.0;

/// Widest width of the information panel in the wide preview.
const WIDE_INFORMATION_MAXIMUM_WIDTH: f32 = 540.0;

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

impl AppView {
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
        div()
            .relative()
            .h(px(STATUS_BAR_HEIGHT))
            .flex_none()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(STATUS_SECTION_SPACING / 2.0))
            .px(px(STATUS_PADDING))
            .bg(color(palette::GREY_SEMI_DARK))
            .border_t_1()
            .border_color(color(palette::GREY_MEDIUM))
            .text_size(px(STATUS_FONT_SIZE))
            .text_color(color(palette::GREY_MIDDLE_LIGHT))
            .child(path)
            .child(status_separator())
            .child(
                div()
                    .id("status")
                    .flex_1()
                    .min_w_0()
                    .truncate()
                    .tooltip(tooltip(STATUS_TOOLTIP))
                    .child(self.status.clone()),
            )
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
        div().id("results").size_full().pb(px(DIVIDER_SPACING)).child(
            div()
                .id("results-frame")
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

    /// Returns what the Play/Stop and Process buttons show.
    fn playback_controls(&self, cx: &Context<Self>) -> PlaybackControls {
        PlaybackControls {
            playing: self.player.as_ref().is_some_and(Player::is_playing),
            can_play: self.player.is_some() && (self.selected.is_some() || self.is_demo()),
            can_process: self.can_process_selected(cx),
            running: self.is_running(),
        }
    }

    /// Returns the callback that hands the events of the controls to the view.
    fn controls_listener(cx: &Context<Self>) -> EventCallback {
        Rc::new(cx.listener(|view, event: &ControlsEvent, window, cx| {
            view.handle_controls_event(*event, window, cx);
        }))
    }

    /// Renders the preview: the controls, the information panel, and the waveform side by side.
    /// A wide window gives the controls and the information panel their wide layouts, see `preview_widths`.
    fn render_preview(&self, content_width: f32, cx: &Context<Self>) -> Div {
        let (controls_width, information_width, wide) = preview_widths(content_width);
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
            .gap(px(SECTION_SPACING))
            .child(div().w(px(controls_width)).h_full().flex_none().child(render_controls(
                &controls,
                wide,
                &Self::controls_listener(cx),
                cx,
            )))
            .child(
                div()
                    .w(px(information_width))
                    .h_full()
                    .flex_none()
                    .child(render_info_panel(
                        self.selected_row(cx).as_ref(),
                        selected_tags.as_deref(),
                        wide,
                    )),
            )
            .child(div().flex_1().min_w_0().h_full().child(self.waveform.clone()))
    }

    /// Renders the collapsed preview in one row:
    /// the chevron, Play/Stop and Process as wide as in the expanded controls, the four metrics, and the waveform.
    fn render_compact_preview(&self, content_width: f32, cx: &Context<Self>) -> Div {
        let on_event = Self::controls_listener(cx);
        let playback_width = controls_width(content_width) - SECTION_TOGGLE_SIZE - COMPACT_SPACING;
        let remaining = SECTION_SPACING.mul_add(-2.0, content_width - controls_width(content_width));
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
                    .child(render_compact_info_panel(self.selected_row(cx).as_ref())),
            )
            .child(
                div()
                    .ml(px(SECTION_SPACING))
                    .flex_1()
                    .min_w_0()
                    .h_full()
                    .child(self.waveform.clone()),
            )
    }

    /// Fits the expanded preview to its controls again when the content area changed size,
    /// until the user drags the divider.
    /// The fit height only changes when the width crosses the wide layout breakpoint,
    /// or when a short window clamps it, since the preview panel keeps its size while it fits.
    /// This also covers the platform maximizing the window after its first frame.
    /// A new split state lays out with the initial panel size, which the existing one only uses once.
    fn fit_preview_to_content(&mut self, content_width: f32, workspace_height: f32, cx: &mut Context<Self>) {
        if !self.preview_auto_fit || self.preview_compact {
            return;
        }
        let height = preview_fit_height(content_width, workspace_height);
        if (height - self.preview_height).abs() < f32::EPSILON {
            return;
        }
        self.preview_height = height;
        self.workspace_split = cx.new(|_| ResizableState::default());
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
        let preview = if self.preview_auto_fit {
            preview.fixed()
        } else {
            preview
        };
        div().size_full().child(
            v_resizable("workspace")
                .with_state(&self.workspace_split)
                .with_handle_appearance(divider_appearance())
                .on_resize(cx.listener(|view, _, _, _| view.preview_auto_fit = false))
                .child(
                    resizable_panel()
                        .size_range(px(TABLE_MINIMUM_HEIGHT)..Pixels::MAX)
                        .child(self.render_table()),
                )
                .child(preview),
        )
    }
}

impl Focusable for AppView {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for AppView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let progress = self.overall_progress(cx);
        self.update_window_title(progress, window);
        let progress = progress.map(|(_, fraction)| fraction);
        let (content_width, workspace_height) = content_size(window.viewport_size());
        let table_width = TABLE_FRAME_BORDER.mul_add(-2.0, content_width);
        self.table.update(cx, |state, cx| {
            fill_status_column(state, table_width, window, cx);
        });
        self.sync_gain_slider(window, cx);
        self.fit_preview_to_content(content_width, workspace_height, cx);
        if std::mem::take(&mut self.zoom_reset_pending) {
            self.zoom_slider
                .update(cx, |slider, cx| slider.set_value(MINIMUM_ZOOM as f32, window, cx));
        }

        div()
            .id("app")
            .key_context(KEY_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(|_, _: &ChooseFolder, _, cx| Self::prompt_for_paths(true, true, cx)))
            .on_action(cx.listener(|_, _: &AddFolder, _, cx| Self::prompt_for_paths(true, false, cx)))
            .on_action(cx.listener(|_, _: &AddFiles, _, cx| Self::prompt_for_paths(false, false, cx)))
            .on_action(cx.listener(|view, _: &Rescan, _, cx| view.rescan(cx)))
            .on_action(cx.listener(|view, _: &RevealSelected, _, cx| view.reveal_selected(cx)))
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
            .on_action(cx.listener(|view, _: &ToggleLoudnessGraph, _, cx| view.toggle_loudness_graph(cx)))
            .on_action(cx.listener(|view, _: &ToggleSeratoColours, _, cx| view.toggle_serato_colours(cx)))
            .on_action(cx.listener(|view, _: &TogglePreviewSection, _, cx| view.toggle_preview_section(cx)))
            .on_action(cx.listener(|view, action: &JumpToHotCue, _, cx| view.jump_to_hot_cue(action.slot, cx)))
            .on_action(cx.listener(|view, _: &TogglePluginPreview, _, cx| view.toggle_plugin_preview(cx)))
            .on_action(cx.listener(|view, _: &TogglePreviewLatencyCompensation, _, cx| {
                view.toggle_preview_latency_compensation(cx);
            }))
            .on_action(cx.listener(|view, _: &OpenAudioSettings, _, cx| view.open_settings_window(cx)))
            .on_action(cx.listener(|view, _: &TogglePlayback, _, cx| view.toggle_playback(cx)))
            .on_action(cx.listener(|view, _: &StopPlayback, _, cx| view.stop_playback(cx)))
            .on_action(cx.listener(|view, action: &SelectOutputDevice, _, cx| {
                view.select_output_device(&action.name, cx);
            }))
            .on_action(cx.listener(|view, _: &OpenPluginChain, _, cx| view.open_chain_window(cx)))
            .on_action(cx.listener(|view, _: &OpenPluginManager, _, cx| view.open_manager_window(cx)))
            .on_action(cx.listener(|view, _: &ClearPluginChain, _, cx| view.clear_plugin_chain(cx)))
            .on_action(cx.listener(|view, action: &AddPluginToChain, _, cx| view.add_plugin_from_menu(action, cx)))
            .on_action(cx.listener(|view, _: &SaveChainPreset, _, cx| view.open_chain_window(cx)))
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
    let (_, _, wide) = preview_widths(content_width);
    (DIVIDER_SPACING + controls_fit_height(wide))
        .clamp(PREVIEW_MINIMUM_HEIGHT, preview_maximum_height(workspace_height))
}

/// Returns the tallest height the divider allows the expanded preview in a workspace of the given height.
fn preview_maximum_height(workspace_height: f32) -> f32 {
    (workspace_height * PREVIEW_MAXIMUM_PROPORTION).max(PREVIEW_MINIMUM_HEIGHT + 1.0)
}

/// Returns the width of the controls column, which the compact row also uses for its chevron and playback buttons.
fn controls_width(content_width: f32) -> f32 {
    (content_width * CONTROLS_WIDTH_PROPORTION).clamp(CONTROLS_MINIMUM_WIDTH, CONTROLS_MAXIMUM_WIDTH)
}

/// Returns the widths of the controls and the information panel of the expanded preview,
/// and whether they use their wide layouts.
/// The wide layouts apply once half the content width is at least `WIDE_MINIMUM_HALF_WIDTH`.
/// The controls and the panel then share that half, each up to its maximum,
/// so the waveform gets at least the other half.
fn preview_widths(content_width: f32) -> (f32, f32, bool) {
    let half = content_width / 2.0;
    if half < WIDE_MINIMUM_HALF_WIDTH {
        let information_width =
            (content_width * INFORMATION_WIDTH_PROPORTION).clamp(INFORMATION_MINIMUM_WIDTH, INFORMATION_MAXIMUM_WIDTH);
        return (controls_width(content_width), information_width, false);
    }
    let shared = half - SECTION_SPACING;
    (
        (shared * WIDE_CONTROLS_PROPORTION).min(WIDE_CONTROLS_MAXIMUM_WIDTH),
        (shared * (1.0 - WIDE_CONTROLS_PROPORTION)).min(WIDE_INFORMATION_MAXIMUM_WIDTH),
        true,
    )
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

/// Renders the thin vertical line between two sections of the status bar.
fn status_separator() -> Div {
    div()
        .flex_none()
        .w(px(1.0))
        .h(px(STATUS_SEPARATOR_INSET.mul_add(-2.0, STATUS_BAR_HEIGHT)))
        .bg(color(palette::GREY_MEDIUM))
}
