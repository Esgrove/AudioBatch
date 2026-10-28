//! The controls column at the left of the preview:
//! zoom, input gain, the Normalize and Overwrite options, the normalization target,
//! the Plugin Chain and Audio Settings buttons, and Play/Stop and Process side by side at the bottom.
//!
//! The chevron at the right end of the Zoom row collapses the preview into its compact row,
//! which shows the same chevron and the same Play/Stop and Process buttons.
//! Play/Stop is green while stopped and red while playing,
//! and Process turns into Cancel during an analysis, normalization, or processing run.
//! The Normalize option normalizes files before the plugins, and the Overwrite option applies to both.
//! The target has the Peak and Loudness modes, with the chosen one in the primary color,
//! the target slider of the chosen mode, a bar showing the target inside it,
//! and the button that normalizes the selected files.
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
    AnyElement, App, Div, ElementId, Entity, InteractiveElement as _, IntoElement as _, MouseButton, MouseUpEvent,
    ParentElement as _, Stateful, StatefulInteractiveElement as _, Styled as _, Window, div, px, relative,
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

/// Share of the target row given to the Normalize button, wide enough for its label in the narrowest column.
const NORMALIZE_BUTTON_PROPORTION: f32 = 0.5;

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

/// Opacity of the filled part of the target slider.
const TARGET_FILL_ALPHA: f32 = 0.7;

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
    /// The Play/Stop and Process buttons.
    pub playback: PlaybackControls,
}

/// Callback that receives the events of the controls column.
pub type EventCallback = Rc<dyn Fn(&ControlsEvent, &mut Window, &mut App)>;

/// Renders the controls column, sending what the user does to `on_event`.
pub fn render_controls(state: &ControlsState<'_>, on_event: &EventCallback, cx: &App) -> Div {
    let zoom = f64::from(state.zoom.read(cx).value().start());
    let gain = state.gain.read(cx).value().start();

    let settings = div()
        .id("controls-settings")
        .flex_1()
        .min_h_0()
        .overflow_y_scroll()
        .flex()
        .flex_col()
        .gap(px(GROUP_SPACING))
        .child(
            slider_group(
                "Zoom",
                format!("{:.0}%", zoom * 100.0),
                ZOOM_TOOLTIP,
                true,
                Some(render_section_toggle(false, on_event).into_any_element()),
            )
            .child(slider_track(
                Slider::new(state.zoom),
                true,
                send_on_double_click(on_event, ControlsEvent::ResetSlider(SliderControl::Zoom)),
            )),
        )
        .child(
            slider_group(
                "Input gain",
                format!("{gain:+.1} dB"),
                GAIN_TOOLTIP,
                state.can_adjust_gain,
                None,
            )
            .child(slider_track(
                Slider::new(state.gain),
                state.can_adjust_gain,
                send_on_double_click(on_event, ControlsEvent::ResetSlider(SliderControl::Gain)),
            )),
        )
        .child(option_row(&state.normalize, on_event))
        .child(target_group(&state.normalize, on_event, cx))
        .child(column_button(
            "plugin-chain",
            "Plugin Chain",
            format!("{CHAIN_TOOLTIP}\nChain: {}", state.chain_summary),
            ControlsEvent::OpenPluginChain,
            on_event,
        ))
        .child(column_button(
            "audio-settings",
            "Audio Settings",
            String::from(SETTINGS_TOOLTIP),
            ControlsEvent::OpenSettings,
            on_event,
        ));

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
/// and the target slider with the Normalize button on its right.
fn target_group(state: &NormalizeControls<'_>, on_event: &EventCallback, cx: &App) -> Div {
    let mode_button = |id: &'static str, label: &'static str, mode: NormalizeMode| {
        let selected = state.mode == mode;
        let button = Button::new(id)
            .label(label)
            .tooltip(MODE_TOOLTIP)
            .flex_1()
            .selected(selected);
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
            .w(relative(NORMALIZE_BUTTON_PROPORTION))
            .h(px(BUTTON_HEIGHT))
            .flex_none()
            .on_click(move |_, window, cx| on_event(&ControlsEvent::NormalizeSelected, window, cx))
    };
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
fn target_slider(state: &NormalizeControls<'_>, on_event: &EventCallback, cx: &App) -> Stateful<Div> {
    let filled = state.target.read(cx).percentage().end;
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
                .bg(color(palette::BLUE).opacity(TARGET_FILL_ALPHA)),
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
                .text_color(color(palette::GREY_SUPER_LIGHT))
                .child(state.target_text.clone()),
        );
    div()
        .id("normalize-target")
        .flex_1()
        .min_w_0()
        .h(px(BUTTON_HEIGHT))
        .tooltip(tooltip(TARGET_TOOLTIP))
        .capture_any_mouse_up(send_on_double_click(
            on_event,
            ControlsEvent::ResetSlider(SliderControl::NormalizeTarget),
        ))
        .child(
            BaseSlider::new(state.target)
                .accessibility_label("Normalization target")
                .size_full()
                .child(SliderTrack::new(state.target).size_full().cursor_pointer().child(bar)),
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

/// Renders a slider group: a caption with its value and an optional accessory on the right.
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
                .flex()
                .flex_row()
                .items_center()
                .gap(px(ITEM_SPACING))
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
                )
                .children(accessory),
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
        .px(px(SLIDER_INSET))
        .capture_any_mouse_up(move |mouse_event, window, cx| {
            if enabled {
                on_mouse_up(mouse_event, window, cx);
            }
        })
        .child(slider.disabled(!enabled))
}
