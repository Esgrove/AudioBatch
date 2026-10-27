//! Layout of the main window.
//!
//! The results table, and below a draggable divider the preview with the controls, the information panel,
//! and the waveform.
//! A status bar along the bottom edge shows the listed folders, the run status with its progress,
//! and the output device with its sample rate and buffer size.
//! The window title shows the progress of analysis, normalization, or plugin processing,
//! and files dropped anywhere on the window are added.

use std::rc::Rc;
use std::sync::Arc;

use audiobatch_core::format::format_kilohertz;
use audiobatch_playback::Player;
use gpui_kit::base::{ResizeHandleContext, ResizeHandleRenderer, ResizeHandleState};
use gpui_kit::component::resizable::{resizable_panel, v_resizable};
use gpui_kit::component::table::DataTable;
use gpui_kit::component::{Sizable as _, Size};
use gpui_kit::{
    App, Axis, Context, Div, ExternalPaths, FocusHandle, Focusable, InteractiveElement as _, IntoElement,
    ParentElement as _, Pixels, Render, SharedString, StatefulInteractiveElement as _, Styled as _, Window, div, px,
    relative, transparent_black,
};

use super::{
    APP_TITLE, AppView, CONTENT_PADDING, PREVIEW_MINIMUM_HEIGHT, SECTION_SPACING, STATUS_BAR_HEIGHT,
    TABLE_FRAME_BORDER, content_size,
};
use crate::controls::{ControlsEvent, ControlsState, NormalizeControls, PluginControls, render_controls};
use crate::info_panel::render_info_panel;
use crate::menus::{
    About, AddFiles, AddFolder, AddPluginToChain, CancelProcessing, ChooseFolder, ClearCustomGain, ClearList,
    ClearPluginChain, KEY_CONTEXT, NormalizeAll, NormalizeSelected, OpenAudioSettings, OpenPluginChain,
    OpenPluginManager, ProcessAll, ProcessSelected, ReanalyzeSelected, RemoveSelected, Rescan, RevealSelected,
    SelectOutputDevice, StopPlayback, ToggleLoudnessGraph, TogglePlayback, TrashSelected,
};
use crate::normalization::target_value_text;
use crate::results::{HEADER_HEIGHT, ROW_HEIGHT, fill_status_column};
use crate::theme::{TEXT_FONT, color, palette, tooltip};

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

    /// Renders the preview: the controls, the information panel, and the waveform side by side.
    fn render_preview(&self, content_width: f32, cx: &Context<Self>) -> Div {
        let controls_width =
            (content_width * CONTROLS_WIDTH_PROPORTION).clamp(CONTROLS_MINIMUM_WIDTH, CONTROLS_MAXIMUM_WIDTH);
        let information_width =
            (content_width * INFORMATION_WIDTH_PROPORTION).clamp(INFORMATION_MINIMUM_WIDTH, INFORMATION_MAXIMUM_WIDTH);
        let selected_row = self
            .selected
            .as_ref()
            .and_then(|path| self.table.read(cx).delegate().row_for(path).cloned());
        let selected_tags = self
            .selected_tags
            .as_ref()
            .filter(|(path, _)| self.selected.as_ref() == Some(path))
            .map(|(_, tags)| Arc::clone(tags));
        let controls = ControlsState {
            zoom: &self.zoom_slider,
            gain: &self.gain_slider,
            playing: self.player.as_ref().is_some_and(Player::is_playing),
            can_play: self.player.is_some() && (self.selected.is_some() || self.is_demo()),
            can_adjust_gain: self.can_adjust_gain(cx),
            normalize: NormalizeControls {
                mode: self.settings.normalize_mode,
                target: self.normalize_target_slider(self.settings.normalize_mode),
                target_text: target_value_text(&self.settings),
                overwrite: self.overwrite_existing,
                can_normalize: self.can_normalize_selected(cx),
            },
            plugins: PluginControls {
                summary: self.chain_summary(),
                normalize_first: self.normalize_first,
                can_process: self.can_process_selected(cx),
                processing: self.is_processing(),
            },
        };
        let on_event = cx.listener(|view, event: &ControlsEvent, window, cx| {
            view.handle_controls_event(*event, window, cx);
        });

        div()
            .size_full()
            .pt(px(DIVIDER_SPACING))
            .flex()
            .flex_row()
            .gap(px(SECTION_SPACING))
            .child(
                div()
                    .w(px(controls_width))
                    .h_full()
                    .flex_none()
                    .child(render_controls(&controls, on_event, cx)),
            )
            .child(
                div()
                    .w(px(information_width))
                    .h_full()
                    .flex_none()
                    .child(render_info_panel(selected_row.as_ref(), selected_tags.as_deref())),
            )
            .child(div().flex_1().min_w_0().h_full().child(self.waveform.clone()))
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
        let preview_maximum = (workspace_height * PREVIEW_MAXIMUM_PROPORTION).max(PREVIEW_MINIMUM_HEIGHT + 1.0);
        let table_width = TABLE_FRAME_BORDER.mul_add(-2.0, content_width);
        self.table.update(cx, |state, cx| {
            fill_status_column(state, table_width, window, cx);
        });
        self.sync_gain_slider(window, cx);

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
            .on_action(cx.listener(|view, _: &TrashSelected, _, cx| view.trash_selected(cx)))
            .on_action(cx.listener(|view, _: &ClearList, _, cx| view.clear_list(cx)))
            .on_action(cx.listener(|view, _: &ToggleLoudnessGraph, _, cx| view.toggle_loudness_graph(cx)))
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
            .on_action(cx.listener(|view, _: &ProcessSelected, window, cx| view.process_selected(window, cx)))
            .on_action(cx.listener(|view, _: &ProcessAll, window, cx| view.process_all(window, cx)))
            .on_action(cx.listener(|view, _: &CancelProcessing, _, cx| view.cancel_processing(cx)))
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
                    div().flex_1().min_h_0().child(
                        v_resizable("workspace")
                            .with_handle_appearance(divider_appearance())
                            .child(
                                resizable_panel()
                                    .size_range(px(TABLE_MINIMUM_HEIGHT)..Pixels::MAX)
                                    .child(self.render_table()),
                            )
                            .child(
                                resizable_panel()
                                    .size(px(self.preview_height))
                                    .size_range(px(PREVIEW_MINIMUM_HEIGHT)..px(preview_maximum))
                                    .child(self.render_preview(content_width, cx)),
                            ),
                    ),
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
