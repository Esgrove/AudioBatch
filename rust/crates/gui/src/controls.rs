//! The controls column at the left of the preview:
//! zoom, input gain, the Normalize and Overwrite options, the normalization target,
//! the Plugin Chain and Audio Settings buttons, and Play/Stop and Process side by side at the bottom.
//!
//! A wide window keeps the zoom and input gain sliders across the full width,
//! and splits the rest into two columns of three rows, so the controls take less height.
//! The left column has a Preview chain option that plays the preview through the plugin chain,
//! and the Plugin Chain and Audio Settings buttons.
//! The right column has the Normalize and Overwrite options and the normalization target.
//!
//! The chevron in the top left corner, before the Zoom caption, collapses the preview into its compact row,
//! which starts with the same chevron, followed by the same Play/Stop and Process buttons.
//! Play/Stop is green while stopped and red while playing,
//! and Process turns into Cancel during an analysis, normalization, or processing run.
//! The Normalize option normalizes files before the plugins, and the Overwrite option applies to both.
//! The target has the Peak and Loudness modes, with the chosen one in the primary color,
//! the target slider of the chosen mode, a bar showing the target inside it,
//! and the button that normalizes the selected files.
//! The modes and the target slider are disabled while the option that normalizes before the plugins is off.
//! Everything above the playback row scrolls when the preview is short, so the buttons always stay visible.
//! Double-clicking a slider resets it to its default value.

use std::rc::Rc;

use gpui_kit::base::Selectable as _;
use gpui_kit::base::slider::{Slider as BaseSlider, SliderIndicator, SliderTrack};
use gpui_kit::component::button::{Button, ButtonGroup, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::slider::{Slider, SliderState};
use gpui_kit::component::{Disableable as _, IconName, Sizable as _};
use gpui_kit::{
    AnyElement, App, CursorStyle, Div, ElementId, Entity, InteractiveElement as _, IntoElement as _, MouseButton,
    MouseUpEvent, ParentElement as _, Stateful, StatefulInteractiveElement as _, Styled as _, Window, div, px,
    relative,
};

use crate::info_panel::{CORNER_RADIUS, OUTLINE_ALPHA};
use crate::settings::NormalizeMode;
use crate::theme::{MONO_FONT, color, palette, tooltip, white};

/// Height of the Play/Stop and Process buttons.
pub const PLAYBACK_BUTTON_HEIGHT: f32 = 38.0;

/// Width and height of the chevron that collapses and expands the preview.
pub const SECTION_TOGGLE_SIZE: f32 = 18.0;

/// Height of the Plugin Chain, Audio Settings, and Normalize buttons.
const BUTTON_HEIGHT: f32 = 28.0;

/// Height of the caption row above a slider, tall enough for the chevron and the caption text.
const CAPTION_ROW_HEIGHT: f32 = 20.0;

/// Height of the zoom and input gain slider rows, the slider track with room for its thumb.
const SLIDER_ROW_HEIGHT: f32 = 24.0;

/// Height of the row with the Normalize and Overwrite options in the one-column layout.
const OPTION_ROW_HEIGHT: f32 = 18.0;

/// Height of the row with the normalization mode buttons in the one-column layout.
const MODE_ROW_HEIGHT: f32 = 24.0;

/// Width of the outline around the controls column.
const BORDER_WIDTH: f32 = 1.0;

/// Space between the control groups.
const GROUP_SPACING: f32 = 10.0;

/// Space inside a control group, and between the two buttons of the playback row.
const ITEM_SPACING: f32 = 6.0;

/// Horizontal padding of the controls column.
const HORIZONTAL_PADDING: f32 = 10.0;

/// Vertical padding of the controls column.
const VERTICAL_PADDING: f32 = 8.0;

/// Font size of the captions and their values.
const CAPTION_FONT_SIZE: f32 = 12.0;

/// Horizontal inset of a slider track: the thumb radius plus the width of its hover ring.
/// The thumb is centred on the track ends, so a smaller inset lets the scroll area clip it at either end.
const SLIDER_INSET: f32 = 11.0;

/// Tooltip of the Play/Stop button.
const PLAY_TOOLTIP: &str = "Start or stop preview playback for the selected file.";

/// Tooltip of the chevron while the preview is expanded.
const COLLAPSE_TOOLTIP: &str = if cfg!(target_os = "macos") {
    "Collapse bottom section (Cmd+Shift+Space)."
} else {
    "Collapse bottom section (Ctrl+Shift+Space)."
};

/// Tooltip of the chevron while the preview is collapsed.
const EXPAND_TOOLTIP: &str = if cfg!(target_os = "macos") {
    "Expand bottom section (Cmd+Shift+Space)."
} else {
    "Expand bottom section (Ctrl+Shift+Space)."
};

/// Tooltip of the zoom slider.
const ZOOM_TOOLTIP: &str =
    "Adjust waveform zoom. You can also use the mouse wheel over the waveform. Double-click to reset.";

/// Tooltip of the input gain slider.
const GAIN_TOOLTIP: &str = if cfg!(target_os = "macos") {
    "Per-file gain in dB, saved for the selected file and applied to the preview and the waveform. \
     You can also use Cmd with the mouse wheel over the waveform. Double-click to reset."
} else {
    "Per-file gain in dB, saved for the selected file and applied to the preview and the waveform. \
     You can also use Ctrl with the mouse wheel over the waveform. Double-click to reset."
};

/// Tooltip of the normalization mode buttons.
const MODE_TOOLTIP: &str =
    "Normalize to a sample peak level, or to an integrated loudness limited to a -0.3 dBTP true peak.";

/// Tooltip of the normalization target slider.
const TARGET_TOOLTIP: &str = "Target level for normalization. Drag to adjust, double-click to reset.";

/// Opacity of the grey filled part of the target slider.
/// The fill is neutral, so it does not compete with the blue of the chosen mode right above it.
const TARGET_FILL_ALPHA: f32 = 0.7;

/// Opacity of the mode buttons while they are disabled, which keeps the chosen mode recognizable.
const DISABLED_MODES_OPACITY: f32 = 0.4;

/// Tooltip of the overwrite option.
const OVERWRITE_TOOLTIP: &str = "When enabled, normalizing or processing replaces an existing output file \
     with the same name. The replaced file is moved to the system trash.";

/// Tooltip of the Normalize button.
const NORMALIZE_TOOLTIP: &str = "Normalize the selected files to the target. \
     The output is written as AIFF next to the source, and a converted source is moved to the system trash.";

/// Tooltip of the option that normalizes before the plugins.
const NORMALIZE_FIRST_TOOLTIP: &str = "When enabled, files without a custom gain are normalized \
     to the selected peak or loudness target before the plugin chain.";

/// Tooltip of the Process button.
const PROCESS_TOOLTIP: &str = "Process the selected files with the plugin chain. \
     The output is written as 16-bit AIFF next to the source, and a converted source is moved to the system trash.";

/// Tooltip of the Cancel button while files are processed.
const CANCEL_TOOLTIP: &str =
    "Cancel the running analysis, normalization, or processing. Files in progress keep their originals.";

/// Tooltip of the Plugin Chain button, followed by the chain summary.
const CHAIN_TOOLTIP: &str = "Open the plugin chain, where plugins are added, ordered, bypassed, and edited.";

/// Tooltip of the Audio Settings button.
const SETTINGS_TOOLTIP: &str = "Open audio device and preview settings.";

/// Tooltip of the Preview chain option.
const PREVIEW_CHAIN_TOOLTIP: &str = "When enabled, the preview plays through the plugin chain.";

/// A slider of the controls column.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SliderControl {
    /// The waveform zoom.
    Zoom,
    /// The input gain.
    Gain,
    /// The normalization target of the current mode.
    NormalizeTarget,
}

/// Something the user did in the controls column or the compact row.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ControlsEvent {
    /// Play/Stop was clicked.
    TogglePlayback,
    /// The chevron was clicked to collapse or expand the preview.
    TogglePreviewSection,
    /// A slider was double-clicked to reset it.
    ResetSlider(SliderControl),
    /// The Audio Settings button was clicked.
    OpenSettings,
    /// A normalization mode was chosen.
    SetNormalizeMode(NormalizeMode),
    /// The overwrite option was switched on or off.
    SetOverwrite(bool),
    /// The Normalize button was clicked.
    NormalizeSelected,
    /// The option that normalizes before the plugins was switched on or off.
    SetNormalizeFirst(bool),
    /// The Process button was clicked.
    ProcessSelected,
    /// The Cancel button was clicked during an analysis, normalization, or processing run.
    CancelRun,
    /// The Plugin Chain button was clicked.
    OpenPluginChain,
    /// The Preview chain option was switched on or off.
    SetPreviewChain(bool),
}

/// Values the Play/Stop and Process buttons show, in the controls column and in the compact row.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PlaybackControls {
    /// True while audio plays.
    pub playing: bool,
    /// True when a file is selected or loaded, so Play has something to play.
    pub can_play: bool,
    /// True when the selected files can be processed now.
    pub can_process: bool,
    /// True during an analysis, normalization, or processing run, which turns Process into Cancel.
    pub running: bool,
}

/// Values the normalization controls show.
pub struct NormalizeControls<'a> {
    /// The chosen mode.
    pub mode: NormalizeMode,
    /// Target slider of the chosen mode.
    pub target: &'a Entity<SliderState>,
    /// Target text inside the slider, for example "-14.0 LUFS".
    pub target_text: String,
    /// True when an existing output is replaced.
    pub overwrite: bool,
    /// True when files are normalized before the plugins.
    pub normalize_first: bool,
    /// True when the selected files can be normalized now.
    pub can_normalize: bool,
}

/// Values the controls column shows, collected by the app view for one render.
pub struct ControlsState<'a> {
    /// Zoom slider, from 1 to 20 times.
    pub zoom: &'a Entity<SliderState>,
    /// Custom gain slider of the selected file in decibels.
    pub gain: &'a Entity<SliderState>,
    /// True when the selected file has a usable result, so the gain has something to apply to.
    pub can_adjust_gain: bool,
    /// The normalization controls.
    pub normalize: NormalizeControls<'a>,
    /// Summary of the plugin chain, for example "Pro-Q 4 > (Limiter)", shown in the Plugin Chain tooltip.
    pub chain_summary: String,
    /// True when the preview plays through the plugin chain, shown by the Preview chain option of the wide layout.
    pub preview_chain: bool,
    /// The Play/Stop and Process buttons.
    pub playback: PlaybackControls,
}

/// Callback that receives the events of the controls column.
pub type EventCallback = Rc<dyn Fn(&ControlsEvent, &mut Window, &mut App)>;

/// Returns the height of the controls column that shows all of its controls without scrolling,
/// in the wide or the one-column layout.
/// It adds the scrolling controls, the playback row pinned below them, the padding, and the outline,
/// from the same row heights the column is drawn with.
pub const fn fit_height(wide: bool) -> f32 {
    let settings = if wide {
        wide_settings_height()
    } else {
        narrow_settings_height()
    };
    settings + GROUP_SPACING + PLAYBACK_BUTTON_HEIGHT + VERTICAL_PADDING * 2.0 + BORDER_WIDTH * 2.0
}

/// Returns the height of the zoom and input gain groups with the spacing between them.
const fn slider_groups_height() -> f32 {
    let group = CAPTION_ROW_HEIGHT + ITEM_SPACING + SLIDER_ROW_HEIGHT;
    group * 2.0 + GROUP_SPACING
}

/// Returns the height of the scrolling controls of the one-column layout:
/// the sliders, the options, the normalization target, and the two buttons, one group below the other.
const fn narrow_settings_height() -> f32 {
    let target = MODE_ROW_HEIGHT + ITEM_SPACING + BUTTON_HEIGHT;
    slider_groups_height() + OPTION_ROW_HEIGHT + target + BUTTON_HEIGHT * 2.0 + GROUP_SPACING * 4.0
}

/// Returns the height of the scrolling controls of the wide layout:
/// the sliders above two columns of three button-high rows.
const fn wide_settings_height() -> f32 {
    slider_groups_height() + GROUP_SPACING + BUTTON_HEIGHT * 3.0 + ITEM_SPACING * 2.0
}

/// Renders the controls column, sending what the user does to `on_event`.
/// The wide layout puts the controls below the sliders in two columns, see the module documentation.
pub fn render_controls(state: &ControlsState<'_>, wide: bool, on_event: &EventCallback, cx: &App) -> Div {
    let settings = div()
        .id("controls-settings")
        .flex_1()
        .min_h_0()
        .overflow_y_scroll()
        .flex()
        .flex_col()
        .gap(px(GROUP_SPACING))
        .child(zoom_group(state, on_event, cx))
        .child(gain_group(state, on_event, cx));
    let settings = if wide {
        settings.child(
            div()
                .flex()
                .flex_row()
                .gap(px(GROUP_SPACING))
                .child(wide_left_column(state, on_event))
                .child(wide_right_column(state, on_event, cx)),
        )
    } else {
        settings
            .child(option_row(&state.normalize, on_event).h(px(OPTION_ROW_HEIGHT)))
            .child(target_group(&state.normalize, false, on_event, cx))
            .child(plugin_chain_button(state, on_event))
            .child(audio_settings_button(on_event))
    };

    div()
        .size_full()
        .flex()
        .flex_col()
        .gap(px(GROUP_SPACING))
        .px(px(HORIZONTAL_PADDING))
        .py(px(VERTICAL_PADDING))
        .rounded(px(CORNER_RADIUS))
        .bg(color(palette::GREY_SEMI_DARK))
        .border_1()
        .border_color(white(OUTLINE_ALPHA))
        .child(settings)
        .child(render_playback_row(state.playback, on_event).h(px(PLAYBACK_BUTTON_HEIGHT)))
}

/// Renders Play/Stop and Process side by side, filling the height and width they are given.
/// Play/Stop is green while stopped, red while playing, and grey with nothing to play.
/// The solid colors come from the theme's success and danger buttons,
/// because a custom button variant only tints its background with the color.
pub fn render_playback_row(state: PlaybackControls, on_event: &EventCallback) -> Div {
    let play = {
        let on_event = Rc::clone(on_event);
        let button = Button::new("play-stop")
            .label(if state.playing { "Stop" } else { "Play" })
            .tooltip(PLAY_TOOLTIP)
            .disabled(!state.can_play)
            .flex_1()
            .h_full()
            .on_click(move |_, window, cx| on_event(&ControlsEvent::TogglePlayback, window, cx));
        match (state.can_play, state.playing) {
            (false, _) => button,
            (true, false) => button.success(),
            (true, true) => button.danger(),
        }
    };
    let process = {
        let on_event = Rc::clone(on_event);
        let (label, tooltip_text, event, enabled) = if state.running {
            ("Cancel", CANCEL_TOOLTIP, ControlsEvent::CancelRun, true)
        } else {
            (
                "Process",
                PROCESS_TOOLTIP,
                ControlsEvent::ProcessSelected,
                state.can_process,
            )
        };
        let button = Button::new("process-selected")
            .label(label)
            .tooltip(tooltip_text)
            .disabled(!enabled)
            .flex_1()
            .h_full()
            .on_click(move |_, window, cx| on_event(&event, window, cx));
        if enabled { button.primary() } else { button }
    };
    div()
        .flex()
        .flex_row()
        .flex_none()
        .gap(px(ITEM_SPACING))
        .child(play)
        .child(process)
}

/// Renders the chevron that collapses the preview, pointing down while expanded and up while collapsed.
pub fn render_section_toggle(compact: bool, on_event: &EventCallback) -> Button {
    let on_event = Rc::clone(on_event);
    Button::new("preview-section")
        .icon(if compact {
            IconName::ChevronUp
        } else {
            IconName::ChevronDown
        })
        .ghost()
        .xsmall()
        .tooltip(if compact { EXPAND_TOOLTIP } else { COLLAPSE_TOOLTIP })
        .w(px(SECTION_TOGGLE_SIZE))
        .h(px(SECTION_TOGGLE_SIZE))
        .flex_none()
        .on_click(move |_, window, cx| on_event(&ControlsEvent::TogglePreviewSection, window, cx))
}

/// Renders the zoom slider below its caption, with the chevron that collapses the preview before the caption.
fn zoom_group(state: &ControlsState<'_>, on_event: &EventCallback, cx: &App) -> Stateful<Div> {
    slider_group(
        "Zoom",
        zoom_text(state, cx),
        ZOOM_TOOLTIP,
        true,
        Some(render_section_toggle(false, on_event).into_any_element()),
    )
    .child(zoom_track(state, on_event))
}

/// Renders the input gain slider below its caption.
fn gain_group(state: &ControlsState<'_>, on_event: &EventCallback, cx: &App) -> Stateful<Div> {
    slider_group(
        "Input gain",
        gain_text(state, cx),
        GAIN_TOOLTIP,
        state.can_adjust_gain,
        None,
    )
    .child(gain_track(state, on_event))
}

/// Renders the left column of the wide layout: the Preview chain option,
/// in line with the Normalize and Overwrite options of the right column,
/// and the Plugin Chain and Audio Settings buttons.
fn wide_left_column(state: &ControlsState<'_>, on_event: &EventCallback) -> Div {
    let preview_chain = {
        let on_event = Rc::clone(on_event);
        Checkbox::new("preview-chain")
            .label("Preview chain")
            .small()
            .tooltip(PREVIEW_CHAIN_TOOLTIP)
            .checked(state.preview_chain)
            .on_change(move |checked: &bool, window, cx| {
                on_event(&ControlsEvent::SetPreviewChain(*checked), window, cx);
            })
    };
    div()
        .flex_1()
        .min_w_0()
        .flex()
        .flex_col()
        .gap(px(ITEM_SPACING))
        .child(div().h(px(BUTTON_HEIGHT)).flex().items_center().child(preview_chain))
        .child(plugin_chain_button(state, on_event))
        .child(audio_settings_button(on_event))
}

/// Renders the right column of the wide layout: the Normalize and Overwrite options and the normalization target,
/// each row as tall as a row of the left column.
fn wide_right_column(state: &ControlsState<'_>, on_event: &EventCallback, cx: &App) -> Div {
    div()
        .flex_1()
        .min_w_0()
        .flex()
        .flex_col()
        .gap(px(ITEM_SPACING))
        .child(option_row(&state.normalize, on_event).h(px(BUTTON_HEIGHT)))
        .child(target_group(&state.normalize, true, on_event, cx))
}

/// Returns the zoom as the slider value shows it, for example "100%".
fn zoom_text(state: &ControlsState<'_>, cx: &App) -> String {
    format!("{:.0}%", f64::from(state.zoom.read(cx).value().start()) * 100.0)
}

/// Returns the input gain as the slider value shows it, for example "+0.0 dB".
fn gain_text(state: &ControlsState<'_>, cx: &App) -> String {
    format!("{:+.1} dB", state.gain.read(cx).value().start())
}

/// Renders the zoom slider, which resets on a double click.
fn zoom_track(state: &ControlsState<'_>, on_event: &EventCallback) -> Div {
    slider_track(
        Slider::new(state.zoom),
        true,
        send_on_double_click(on_event, ControlsEvent::ResetSlider(SliderControl::Zoom)),
    )
}

/// Renders the input gain slider, which resets on a double click while the gain can be adjusted.
fn gain_track(state: &ControlsState<'_>, on_event: &EventCallback) -> Div {
    slider_track(
        Slider::new(state.gain),
        state.can_adjust_gain,
        send_on_double_click(on_event, ControlsEvent::ResetSlider(SliderControl::Gain)),
    )
}

/// Renders the Plugin Chain button, with the chain summary in its tooltip.
fn plugin_chain_button(state: &ControlsState<'_>, on_event: &EventCallback) -> Button {
    column_button(
        "plugin-chain",
        "Plugin Chain",
        format!("{CHAIN_TOOLTIP}\nChain: {}", state.chain_summary),
        ControlsEvent::OpenPluginChain,
        on_event,
    )
}

/// Renders the Audio Settings button.
fn audio_settings_button(on_event: &EventCallback) -> Button {
    column_button(
        "audio-settings",
        "Audio Settings",
        String::from(SETTINGS_TOOLTIP),
        ControlsEvent::OpenSettings,
        on_event,
    )
}

/// Renders a full-width button of the controls column that sends `event` when clicked.
fn column_button(
    id: &'static str,
    label: &'static str,
    tooltip_text: String,
    event: ControlsEvent,
    on_event: &EventCallback,
) -> Button {
    let on_event = Rc::clone(on_event);
    Button::new(id)
        .label(label)
        .tooltip(tooltip_text)
        .w_full()
        .h(px(BUTTON_HEIGHT))
        .flex_none()
        .on_click(move |_, window, cx| on_event(&event, window, cx))
}

/// Renders the option that normalizes before the plugins and the overwrite option in two equal halves.
fn option_row(state: &NormalizeControls<'_>, on_event: &EventCallback) -> Div {
    let normalize_first = {
        let on_event = Rc::clone(on_event);
        Checkbox::new("process-normalize-first")
            .label("Normalize")
            .small()
            .tooltip(NORMALIZE_FIRST_TOOLTIP)
            .checked(state.normalize_first)
            .on_change(move |checked: &bool, window, cx| {
                on_event(&ControlsEvent::SetNormalizeFirst(*checked), window, cx);
            })
    };
    let overwrite = {
        let on_event = Rc::clone(on_event);
        Checkbox::new("normalize-overwrite")
            .label("Overwrite")
            .small()
            .tooltip(OVERWRITE_TOOLTIP)
            .checked(state.overwrite)
            .on_change(move |checked: &bool, window, cx| {
                on_event(&ControlsEvent::SetOverwrite(*checked), window, cx);
            })
    };
    div()
        .flex()
        .flex_row()
        .items_center()
        .gap(px(ITEM_SPACING))
        .child(div().flex_1().min_w_0().child(normalize_first))
        .child(div().flex_1().min_w_0().child(overwrite))
}

/// Renders the normalization target: the mode buttons, with the chosen mode in the primary color,
/// and the target slider and the Normalize button in two equal halves below them.
/// The mode buttons and the slider are disabled while the option that normalizes before the plugins is off,
/// and the Normalize button only depends on the selection.
/// The wide layout centres the mode buttons in a row as tall as the other rows, so the two columns line up.
fn target_group(state: &NormalizeControls<'_>, wide: bool, on_event: &EventCallback, cx: &App) -> Div {
    let mode_button = |id: &'static str, label: &'static str, mode: NormalizeMode| {
        let selected = state.mode == mode;
        let button = Button::new(id)
            .label(label)
            .tooltip(MODE_TOOLTIP)
            .flex_1()
            .selected(selected)
            .disabled(!state.normalize_first);
        if selected { button.primary() } else { button }
    };
    let modes = {
        let on_event = Rc::clone(on_event);
        ButtonGroup::new("normalize-mode")
            .small()
            .w_full()
            .child(mode_button("normalize-peak", "Peak", NormalizeMode::Peak))
            .child(mode_button("normalize-loudness", "Loudness", NormalizeMode::Loudness))
            .on_click(move |clicked: &Vec<usize>, window, cx| {
                let mode = if clicked.contains(&1) {
                    NormalizeMode::Loudness
                } else {
                    NormalizeMode::Peak
                };
                on_event(&ControlsEvent::SetNormalizeMode(mode), window, cx);
            })
    };
    let normalize = {
        let on_event = Rc::clone(on_event);
        Button::new("normalize-selected")
            .label("Normalize")
            .tooltip(NORMALIZE_TOOLTIP)
            .disabled(!state.can_normalize)
            .w_full()
            .h(px(BUTTON_HEIGHT))
            .on_click(move |_, window, cx| on_event(&ControlsEvent::NormalizeSelected, window, cx))
    };
    // The button's padding would be added to its share of the row, so a plain cell takes the share instead.
    let normalize = div().flex_1().min_w_0().child(normalize);
    let modes_row = div()
        .h(px(if wide { BUTTON_HEIGHT } else { MODE_ROW_HEIGHT }))
        .flex()
        .items_center();
    let modes = modes_row
        .w_full()
        .opacity(if state.normalize_first {
            1.0
        } else {
            DISABLED_MODES_OPACITY
        })
        .child(modes);
    div().flex().flex_col().gap(px(ITEM_SPACING)).child(modes).child(
        div()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(ITEM_SPACING))
            .child(target_slider(state, on_event, cx))
            .child(normalize),
    )
}

/// Renders the target slider as a bar that fills up to the target and shows the target text inside it,
/// so the value needs no caption row of its own.
/// Clicking or dragging along the bar sets the target, and double-clicking resets it.
/// A disabled bar dims its fill and text and ignores the mouse.
fn target_slider(state: &NormalizeControls<'_>, on_event: &EventCallback, cx: &App) -> Stateful<Div> {
    let enabled = state.normalize_first;
    let filled = state.target.read(cx).percentage().end;
    let reset = send_on_double_click(on_event, ControlsEvent::ResetSlider(SliderControl::NormalizeTarget));
    let bar = SliderIndicator::new(state.target)
        .relative()
        .size_full()
        .overflow_hidden()
        .rounded(px(CORNER_RADIUS))
        .bg(color(palette::GREY_MEDIUM_DARK))
        .border_1()
        .border_color(color(palette::GREY_MEDIUM))
        .child(
            div()
                .absolute()
                .top_0()
                .bottom_0()
                .left_0()
                .w(relative(filled))
                .bg(if enabled {
                    color(palette::GREY_MIDDLE).opacity(TARGET_FILL_ALPHA)
                } else {
                    color(palette::GREY_MEDIUM)
                }),
        )
        .child(
            div()
                .absolute()
                .size_full()
                .flex()
                .items_center()
                .justify_center()
                .font_family(MONO_FONT)
                .text_size(px(CAPTION_FONT_SIZE))
                .text_color(color(if enabled {
                    palette::GREY_SUPER_LIGHT
                } else {
                    palette::GREY_MIDDLE
                }))
                .child(state.target_text.clone()),
        );
    div()
        .id("normalize-target")
        .flex_1()
        .min_w_0()
        .h(px(BUTTON_HEIGHT))
        .tooltip(tooltip(TARGET_TOOLTIP))
        .capture_any_mouse_up(move |mouse_event, window, cx| {
            if enabled {
                reset(mouse_event, window, cx);
            }
        })
        .child(
            BaseSlider::new(state.target)
                .accessibility_label("Normalization target")
                .disabled(!enabled)
                .size_full()
                .child(
                    SliderTrack::new(state.target)
                        .disabled(!enabled)
                        .size_full()
                        .cursor(if enabled {
                            CursorStyle::PointingHand
                        } else {
                            CursorStyle::Arrow
                        })
                        .child(bar),
                ),
        )
}

/// Returns a mouse up listener that sends an event on the second click of a double click.
/// It listens in the capture phase, because the slider thumb stops the mouse events from bubbling,
/// and on mouse up, so the event comes after the slider has handled the press itself.
fn send_on_double_click(
    on_event: &EventCallback,
    event: ControlsEvent,
) -> impl Fn(&MouseUpEvent, &mut Window, &mut App) + 'static {
    let on_event = Rc::clone(on_event);
    move |mouse_event, window, cx| {
        if mouse_event.button == MouseButton::Left && mouse_event.click_count >= 2 {
            on_event(&event, window, cx);
        }
    }
}

/// Renders a slider group: an optional accessory on the left, and a caption with its value.
/// The caller adds the slider below.
/// A disabled group dims its value.
fn slider_group(
    title: &'static str,
    value: String,
    tooltip_text: &'static str,
    enabled: bool,
    accessory: Option<AnyElement>,
) -> Stateful<Div> {
    div()
        .id(ElementId::Name(title.into()))
        .flex()
        .flex_col()
        .gap(px(ITEM_SPACING))
        .tooltip(tooltip(tooltip_text))
        .child(
            div()
                .h(px(CAPTION_ROW_HEIGHT))
                .flex()
                .flex_row()
                .items_center()
                .gap(px(ITEM_SPACING))
                .children(accessory)
                .child(
                    div()
                        .flex_1()
                        .text_size(px(CAPTION_FONT_SIZE))
                        .text_color(color(palette::GREY_MIDDLE_LIGHT))
                        .child(title),
                )
                .child(
                    div()
                        .font_family(MONO_FONT)
                        .text_size(px(CAPTION_FONT_SIZE))
                        .text_color(color(if enabled {
                            palette::GREY_LIGHT
                        } else {
                            palette::GREY_MIDDLE
                        }))
                        .child(value),
                ),
        )
}

/// Renders a slider inset so its thumb is not clipped, with `on_mouse_up` listening to it.
/// A disabled slider ignores dragging and double-clicks.
fn slider_track(
    slider: Slider,
    enabled: bool,
    on_mouse_up: impl Fn(&MouseUpEvent, &mut Window, &mut App) + 'static,
) -> Div {
    div()
        .h(px(SLIDER_ROW_HEIGHT))
        .flex()
        .items_center()
        .px(px(SLIDER_INSET))
        .capture_any_mouse_up(move |mouse_event, window, cx| {
            if enabled {
                on_mouse_up(mouse_event, window, cx);
            }
        })
        .child(slider.disabled(!enabled))
}
