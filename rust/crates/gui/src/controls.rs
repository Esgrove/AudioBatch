//! The controls column at the left of the preview:
//! zoom, input gain, the normalization target, plugin processing, the Plugin Chain and Audio Settings buttons,
//! and Play/Stop.
//!
//! The zoom and gain sliders and their value labels are on top,
//! and the large Play/Stop button at the bottom, green while stopped and red while playing.
//! The normalization group has the Peak and Loudness modes, the target slider of the chosen mode,
//! the overwrite option, and the button that normalizes the selected file.
//! The overwrite option applies to plugin processing too.
//! The plugin group shows the chain summary, the option that normalizes files before the plugins,
//! and the button that processes the selected file, which cancels the run while files are processed.
//! The playback position is shown by the playhead on the waveform,
//! and the output device in the status bar of the main window.
//! Everything above the button scrolls when the preview is short, so the button always stays visible.
//! Double-clicking a slider resets it to its default value.

use std::rc::Rc;

use gpui_kit::base::Selectable as _;
use gpui_kit::component::button::{Button, ButtonGroup, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::slider::{Slider, SliderState};
use gpui_kit::component::{Disableable as _, Sizable as _};
use gpui_kit::{
    App, Div, ElementId, Entity, InteractiveElement as _, MouseButton, MouseUpEvent, ParentElement as _, Stateful,
    StatefulInteractiveElement as _, Styled as _, Window, div, px,
};

use crate::info_panel::{CORNER_RADIUS, OUTLINE_ALPHA};
use crate::settings::NormalizeMode;
use crate::theme::{MONO_FONT, color, palette, tooltip, white};

/// Height of the Play/Stop button.
const PLAY_BUTTON_HEIGHT: f32 = 38.0;

/// Height of the Audio Settings button.
const SETTINGS_BUTTON_HEIGHT: f32 = 28.0;

/// Tooltip of the Audio Settings button.
const SETTINGS_TOOLTIP: &str = "Open audio device and preview settings.";

/// Space between the control groups.
const GROUP_SPACING: f32 = 10.0;

/// Font size of the captions and their values.
const CAPTION_FONT_SIZE: f32 = 12.0;

/// Horizontal inset of a slider track: the thumb radius plus the width of its hover ring.
/// The thumb is centred on the track ends, so a smaller inset lets the scroll area clip it at either end.
const SLIDER_INSET: f32 = 11.0;

/// Tooltip of the Play/Stop button.
const PLAY_TOOLTIP: &str = "Start or stop preview playback for the selected file.";

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
const TARGET_TOOLTIP: &str = "Target level for normalization. Double-click to reset.";

/// Tooltip of the overwrite option.
const OVERWRITE_TOOLTIP: &str = "When enabled, normalizing or processing replaces an existing output file \
     with the same name. The replaced file is moved to the system trash.";

/// Tooltip of the Normalize button.
const NORMALIZE_TOOLTIP: &str = "Normalize the selected file to the target. \
     The output is written as AIFF next to the source, and a converted source is moved to the system trash.";

/// Tooltip of the option that normalizes before the plugins.
const NORMALIZE_FIRST_TOOLTIP: &str = "When enabled, files without a custom gain are normalized \
     to the selected peak or loudness target before the plugin chain.";

/// Tooltip of the Process button.
const PROCESS_TOOLTIP: &str = "Process the selected file with the plugin chain. \
     The output is written as 16-bit AIFF next to the source, and a converted source is moved to the system trash.";

/// Tooltip of the Cancel button while files are processed.
const CANCEL_TOOLTIP: &str =
    "Cancel the running analysis, normalization, or processing. Files in progress keep their originals.";

/// Tooltip of the Plugin Chain button.
const CHAIN_TOOLTIP: &str = "Open the plugin chain, where plugins are added, ordered, bypassed, and edited.";

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

/// Something the user did in the controls column.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ControlsEvent {
    /// Play/Stop was clicked.
    TogglePlayback,
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

/// Values the plugin group shows.
pub struct PluginControls {
    /// Summary of the chain, for example "2 plugins".
    pub summary: String,
    /// True when files are normalized before the plugins.
    pub normalize_first: bool,
    /// True when the selected file can be processed now.
    pub can_process: bool,
    /// True during an analysis, normalization, or processing run, which turns the button into Cancel.
    pub running: bool,
}

/// Values the normalization group shows.
pub struct NormalizeControls<'a> {
    /// The chosen mode.
    pub mode: NormalizeMode,
    /// Target slider of the chosen mode.
    pub target: &'a Entity<SliderState>,
    /// Target text next to the caption, for example "-14.0 LUFS".
    pub target_text: String,
    /// True when an existing output is replaced.
    pub overwrite: bool,
    /// True when the selected file can be normalized now.
    pub can_normalize: bool,
}

/// Values the controls column shows, collected by the app view for one render.
pub struct ControlsState<'a> {
    /// Zoom slider, from 1 to 20 times.
    pub zoom: &'a Entity<SliderState>,
    /// Custom gain slider of the selected file in decibels.
    pub gain: &'a Entity<SliderState>,
    /// True while audio plays.
    pub playing: bool,
    /// True when a file is selected or loaded, so Play has something to play.
    pub can_play: bool,
    /// True when the selected file has a usable result, so the gain has something to apply to.
    pub can_adjust_gain: bool,
    /// The normalization group.
    pub normalize: NormalizeControls<'a>,
    /// The plugin group.
    pub plugins: PluginControls,
}

/// Callback that receives the events of the controls column.
type EventCallback = Rc<dyn Fn(&ControlsEvent, &mut Window, &mut App)>;

/// Renders the controls column, sending what the user does to `on_event`.
pub fn render_controls(
    state: &ControlsState<'_>,
    on_event: impl Fn(&ControlsEvent, &mut Window, &mut App) + 'static,
    cx: &App,
) -> Div {
    let on_event: EventCallback = Rc::new(on_event);
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
        .child(slider_group(
            "Zoom",
            format!("{:.0}%", zoom * 100.0),
            Slider::new(state.zoom),
            ZOOM_TOOLTIP,
            true,
            send_on_double_click(&on_event, ControlsEvent::ResetSlider(SliderControl::Zoom)),
        ))
        .child(slider_group(
            "Input gain",
            format!("{gain:+.1} dB"),
            Slider::new(state.gain),
            GAIN_TOOLTIP,
            state.can_adjust_gain,
            send_on_double_click(&on_event, ControlsEvent::ResetSlider(SliderControl::Gain)),
        ))
        .child(normalize_group(&state.normalize, &on_event))
        .child(plugin_group(&state.plugins, &on_event))
        .child({
            let on_event = Rc::clone(&on_event);
            Button::new("plugin-chain")
                .label("Plugin Chain")
                .tooltip(CHAIN_TOOLTIP)
                .w_full()
                .h(px(SETTINGS_BUTTON_HEIGHT))
                .flex_none()
                .on_click(move |_, window, cx| on_event(&ControlsEvent::OpenPluginChain, window, cx))
        })
        .child({
            let on_event = Rc::clone(&on_event);
            Button::new("audio-settings")
                .label("Audio Settings")
                .tooltip(SETTINGS_TOOLTIP)
                .w_full()
                .h(px(SETTINGS_BUTTON_HEIGHT))
                .flex_none()
                .on_click(move |_, window, cx| on_event(&ControlsEvent::OpenSettings, window, cx))
        });

    div()
        .size_full()
        .flex()
        .flex_col()
        .gap(px(8.0))
        .px(px(10.0))
        .py(px(8.0))
        .rounded(px(CORNER_RADIUS))
        .bg(color(palette::GREY_SEMI_DARK))
        .border_1()
        .border_color(white(OUTLINE_ALPHA))
        .child(settings)
        .child(play_button(state, on_event))
}

/// Renders the Play/Stop button: green while stopped, red while playing, and grey with nothing to play.
/// The solid colors come from the theme's success and danger buttons,
/// because a custom button variant only tints its background with the color.
fn play_button(state: &ControlsState<'_>, on_event: EventCallback) -> Button {
    let button = Button::new("play-stop")
        .label(if state.playing { "Stop" } else { "Play" })
        .tooltip(PLAY_TOOLTIP)
        .disabled(!state.can_play)
        .w_full()
        .h(px(PLAY_BUTTON_HEIGHT))
        .flex_none()
        .on_click(move |_, window, cx| on_event(&ControlsEvent::TogglePlayback, window, cx));
    match (state.can_play, state.playing) {
        (false, _) => button,
        (true, false) => button.success(),
        (true, true) => button.danger(),
    }
}

/// Renders the normalization group: the mode buttons, the target slider,
/// and a row with the overwrite option and the Normalize button.
fn normalize_group(state: &NormalizeControls<'_>, on_event: &EventCallback) -> Div {
    let modes = {
        let on_event = Rc::clone(on_event);
        ButtonGroup::new("normalize-mode")
            .small()
            .w_full()
            .child(
                Button::new("normalize-peak")
                    .label("Peak")
                    .tooltip(MODE_TOOLTIP)
                    .flex_1()
                    .selected(state.mode == NormalizeMode::Peak),
            )
            .child(
                Button::new("normalize-loudness")
                    .label("Loudness")
                    .tooltip(MODE_TOOLTIP)
                    .flex_1()
                    .selected(state.mode == NormalizeMode::Loudness),
            )
            .on_click(move |clicked: &Vec<usize>, window, cx| {
                let mode = if clicked.contains(&1) {
                    NormalizeMode::Loudness
                } else {
                    NormalizeMode::Peak
                };
                on_event(&ControlsEvent::SetNormalizeMode(mode), window, cx);
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
    let normalize = {
        let on_event = Rc::clone(on_event);
        Button::new("normalize-selected")
            .label("Normalize")
            .tooltip(NORMALIZE_TOOLTIP)
            .disabled(!state.can_normalize)
            .flex_1()
            .h(px(SETTINGS_BUTTON_HEIGHT))
            .on_click(move |_, window, cx| on_event(&ControlsEvent::NormalizeSelected, window, cx))
    };
    div()
        .flex()
        .flex_col()
        .gap(px(6.0))
        .child(modes)
        .child(slider_group(
            "Normalize target",
            state.target_text.clone(),
            Slider::new(state.target),
            TARGET_TOOLTIP,
            true,
            send_on_double_click(on_event, ControlsEvent::ResetSlider(SliderControl::NormalizeTarget)),
        ))
        .child(
            div()
                .flex()
                .flex_row()
                .items_center()
                .gap(px(8.0))
                .child(overwrite)
                .child(normalize),
        )
}

/// Renders the plugin group: the caption with the chain summary,
/// and a row with the option that normalizes first and the Process button, which is Cancel during any run.
fn plugin_group(state: &PluginControls, on_event: &EventCallback) -> Div {
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
    let button = {
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
        Button::new("process-selected")
            .label(label)
            .tooltip(tooltip_text)
            .disabled(!enabled)
            .flex_1()
            .h(px(SETTINGS_BUTTON_HEIGHT))
            .on_click(move |_, window, cx| on_event(&event, window, cx))
    };
    div()
        .flex()
        .flex_col()
        .gap(px(6.0))
        .child(
            div()
                .flex()
                .flex_row()
                .justify_between()
                .text_size(px(CAPTION_FONT_SIZE))
                .child(
                    div()
                        .text_color(color(palette::GREY_MIDDLE_LIGHT))
                        .child("Plugin chain"),
                )
                .child(
                    div()
                        .font_family(MONO_FONT)
                        .text_color(color(palette::GREY_LIGHT))
                        .child(state.summary.clone()),
                ),
        )
        .child(
            div()
                .flex()
                .flex_row()
                .items_center()
                .gap(px(8.0))
                .child(normalize_first)
                .child(button),
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

/// Renders a caption with its value on the right, above a slider that `on_mouse_up` listens to.
/// A disabled group dims its value, and its slider ignores dragging and double-clicks.
fn slider_group(
    title: &'static str,
    value: String,
    slider: Slider,
    tooltip_text: &'static str,
    enabled: bool,
    on_mouse_up: impl Fn(&MouseUpEvent, &mut Window, &mut App) + 'static,
) -> Stateful<Div> {
    div()
        .id(ElementId::Name(title.into()))
        .flex()
        .flex_col()
        .gap(px(6.0))
        .tooltip(tooltip(tooltip_text))
        .child(
            div()
                .flex()
                .flex_row()
                .justify_between()
                .child(
                    div()
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
        .child(
            div()
                .px(px(SLIDER_INSET))
                .capture_any_mouse_up(move |mouse_event, window, cx| {
                    if enabled {
                        on_mouse_up(mouse_event, window, cx);
                    }
                })
                .child(slider.disabled(!enabled)),
        )
}
