//! The Audio Settings window: the output device and the preview preferences.
//!
//! It is a separate window with two groups.
//! "Audio interface and playback" chooses the output device, its sample rate, and its buffer size,
//! and "Preview preferences" holds the plugin chain preview, the playback options,
//! and the loudness graph with its axis range.
//! Colouring the waveform from the Serato Overview tag is turned on and off in the Options menu, not in this window.
//! The main view stays the owner of the settings.
//! This view sends every change as a complete `GuiSettings`,
//! and the main view applies it and syncs the window back with what it actually opened.

use audiobatch_core::format::format_kilohertz;
use audiobatch_playback::DeviceCapabilities;
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use gpui_kit::component::{IndexPath, Sizable as _};
use gpui_kit::{
    AnyElement, AppContext as _, Context, Div, ElementId, Entity, EventEmitter, FocusHandle, Focusable,
    InteractiveElement as _, IntoElement, ParentElement as _, Render, SharedString, StatefulInteractiveElement as _,
    Styled as _, Subscription, Window, div, px,
};

use crate::info_panel::{CORNER_RADIUS, OUTLINE_ALPHA};
use crate::menus::{CloseAudioSettings, SETTINGS_KEY_CONTEXT};
use crate::settings::{GuiSettings, is_valid_loudness_range, parse_loudness_limit};
use crate::theme::{TEXT_FONT, color, palette, tooltip, white};

/// Title of the window.
pub const SETTINGS_TITLE: &str = "Audio Settings";

/// Initial width of the window.
pub const SETTINGS_WIDTH: f32 = 540.0;

/// Initial height of the window.
pub const SETTINGS_HEIGHT: f32 = 410.0;

/// Padding around the content and inside the groups.
const PADDING: f32 = 12.0;

/// Space between the groups.
const SECTION_SPACING: f32 = 16.0;

/// Space between the rows of a group.
const ROW_SPACING: f32 = 8.0;

/// Width of the row captions.
const CAPTION_WIDTH: f32 = 110.0;

/// Width of a loudness limit field.
const LIMIT_INPUT_WIDTH: f32 = 44.0;

/// Font size of the window text.
const FONT_SIZE: f32 = 13.0;

/// Font size of the group headings.
const HEADING_FONT_SIZE: f32 = 12.0;

/// Label of the buffer size that leaves the choice to the device.
const DEFAULT_BUFFER_LABEL: &str = "Device default";

/// Shown when the typed loudness range is not valid.
const RANGE_ERROR: &str = "Use whole LUFS (-60 to +6), with minimum < maximum.";

/// Tooltip of the output device choice.
const DEVICE_TOOLTIP: &str = "Choose the audio interface or speakers used for preview playback.";

/// Tooltip of the sample rate choice.
const SAMPLE_RATE_TOOLTIP: &str = "Choose the audio device's playback sample rate. This does not change source files.";

/// Tooltip of the buffer size choice.
const BUFFER_SIZE_TOOLTIP: &str = "Smaller buffers reduce preview latency. Larger buffers can prevent dropouts.";

/// Tooltip of the plugin chain preview option.
const PLUGIN_PREVIEW_TOOLTIP: &str =
    "Listen through the enabled plugins of the chain. Does not normalize or write files.";

/// Tooltip of the latency compensation option.
const COMPENSATION_TOOLTIP: &str = "Align processed audio with the file position. \
     High-latency chains need a preparation pause after starts or seeks.";

/// Tooltip of the playback continuation option.
const CONTINUE_TOOLTIP: &str = "Start a newly selected file only if the previous preview was playing.";

/// Tooltip of the first cue point option.
const FIRST_CUE_TOOLTIP: &str = "When loading a file, start at its first cue point only if it is at or before \
     halfway through the file. Otherwise, or if there is no cue point, start at zero.";

/// Tooltip of the loudness graph option.
const LOUDNESS_GRAPH_TOOLTIP: &str = "Overlay source-file short-term loudness (the trailing three seconds). \
     Uses cached analysis, not preview gain.";

/// Tooltip of the loudness limit fields.
const LOUDNESS_LIMIT_TOOLTIP: &str = "Whole-number LUFS from -60 to +6. Maximum must be at least 1 LU above minimum.";

/// Events the settings window sends to the main view.
#[derive(Debug, Clone)]
pub enum SettingsEvent {
    /// The user changed a setting, carrying the complete settings to apply.
    Changed(GuiSettings),
}

/// What the settings window shows, collected by the main view from its player and settings.
#[derive(Debug, Clone, Default)]
pub struct SettingsSnapshot {
    /// Output devices of the audio host.
    pub devices: Vec<String>,
    /// Device the player has open, if any.
    pub device: Option<String>,
    /// Sample rates and buffer sizes of that device.
    pub capabilities: Option<DeviceCapabilities>,
    /// Sample rate the player outputs at.
    pub sample_rate: Option<u32>,
    /// Buffer size the player opened with, or `None` for the device default.
    pub buffer_size: Option<u32>,
    /// The saved settings.
    pub settings: GuiSettings,
}

/// The content of the Audio Settings window.
pub struct SettingsView {
    focus_handle: FocusHandle,
    device_select: Entity<SelectState<Vec<SharedString>>>,
    rate_select: Entity<SelectState<Vec<SharedString>>>,
    buffer_select: Entity<SelectState<Vec<SharedString>>>,
    minimum_input: Entity<InputState>,
    maximum_input: Entity<InputState>,
    /// Sample rates in the order of the rate choice.
    sample_rates: Vec<u32>,
    /// Buffer sizes in the order of the buffer choice, with `None` for the device default.
    buffer_sizes: Vec<Option<u32>>,
    /// Sample rate used to show the buffer sizes as durations.
    output_rate: Option<u32>,
    settings: GuiSettings,
    range_error: Option<SharedString>,
    _subscriptions: Vec<Subscription>,
}

impl SettingsView {
    /// Creates the window content showing a snapshot.
    pub fn new(snapshot: &SettingsSnapshot, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let device_select = cx.new(|cx| SelectState::new(Vec::new(), None, window, cx));
        let rate_select = cx.new(|cx| SelectState::new(Vec::new(), None, window, cx));
        let buffer_select = cx.new(|cx| SelectState::new(Vec::new(), None, window, cx));
        let minimum_input = cx.new(|cx| InputState::new(window, cx));
        let maximum_input = cx.new(|cx| InputState::new(window, cx));
        let subscriptions = vec![
            cx.subscribe_in(&device_select, window, Self::on_device_selected),
            cx.subscribe_in(&rate_select, window, Self::on_rate_selected),
            cx.subscribe_in(&buffer_select, window, Self::on_buffer_selected),
            cx.subscribe_in(&minimum_input, window, Self::on_limit_input),
            cx.subscribe_in(&maximum_input, window, Self::on_limit_input),
        ];
        let focus_handle = cx.focus_handle();
        window.focus(&focus_handle, cx);
        let mut view = Self {
            focus_handle,
            device_select,
            rate_select,
            buffer_select,
            minimum_input,
            maximum_input,
            sample_rates: Vec::new(),
            buffer_sizes: Vec::new(),
            output_rate: None,
            settings: snapshot.settings.clone(),
            range_error: None,
            _subscriptions: subscriptions,
        };
        view.sync(snapshot, window, cx);
        view
    }

    /// Shows a snapshot: the devices and the choices of the open device, and the saved preferences.
    pub fn sync(&mut self, snapshot: &SettingsSnapshot, window: &mut Window, cx: &mut Context<Self>) {
        self.settings = snapshot.settings.clone();
        self.output_rate = snapshot.sample_rate;

        let device_labels: Vec<SharedString> = snapshot.devices.iter().map(SharedString::from).collect();
        let device_index = snapshot
            .device
            .as_ref()
            .and_then(|device| snapshot.devices.iter().position(|name| name == device));
        set_choices(&self.device_select, device_labels, device_index, window, cx);

        self.sample_rates = snapshot
            .capabilities
            .as_ref()
            .map(|capabilities| capabilities.sample_rates.clone())
            .unwrap_or_default();
        if let Some(rate) = snapshot.sample_rate
            && !self.sample_rates.contains(&rate)
        {
            self.sample_rates.push(rate);
            self.sample_rates.sort_unstable();
        }
        let rate_labels = self.sample_rates.iter().map(|&rate| rate_label(rate)).collect();
        let rate_index = snapshot
            .sample_rate
            .and_then(|rate| self.sample_rates.iter().position(|&candidate| candidate == rate));
        set_choices(&self.rate_select, rate_labels, rate_index, window, cx);

        self.buffer_sizes = std::iter::once(None)
            .chain(
                snapshot
                    .capabilities
                    .iter()
                    .flat_map(|capabilities| capabilities.buffer_sizes.iter().copied().map(Some)),
            )
            .collect();
        if snapshot.buffer_size.is_some() && !self.buffer_sizes.contains(&snapshot.buffer_size) {
            self.buffer_sizes.push(snapshot.buffer_size);
        }
        let buffer_labels = self
            .buffer_sizes
            .iter()
            .map(|&frames| buffer_label(frames, self.output_rate))
            .collect();
        let buffer_index = self
            .buffer_sizes
            .iter()
            .position(|&frames| frames == snapshot.buffer_size);
        set_choices(&self.buffer_select, buffer_labels, buffer_index, window, cx);

        set_text(
            &self.minimum_input,
            self.settings.loudness_minimum.to_string(),
            window,
            cx,
        );
        set_text(
            &self.maximum_input,
            self.settings.loudness_maximum.to_string(),
            window,
            cx,
        );
        cx.notify();
    }

    /// Sends the changed settings to the main view.
    fn send(&mut self, settings: GuiSettings, cx: &mut Context<Self>) {
        self.settings = settings.clone();
        cx.emit(SettingsEvent::Changed(settings));
        cx.notify();
    }

    /// Switches to another output device with its default sample rate and buffer size.
    fn on_device_selected(
        &mut self,
        _select: &Entity<SelectState<Vec<SharedString>>>,
        event: &SelectEvent<Vec<SharedString>>,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let SelectEvent::Confirm(Some(name)) = event else {
            return;
        };
        let mut settings = self.settings.clone();
        settings.output_device = Some(name.to_string());
        settings.sample_rate = None;
        settings.buffer_size = None;
        self.send(settings, cx);
    }

    /// Switches the output device to another sample rate.
    fn on_rate_selected(
        &mut self,
        _select: &Entity<SelectState<Vec<SharedString>>>,
        event: &SelectEvent<Vec<SharedString>>,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let SelectEvent::Confirm(Some(label)) = event else {
            return;
        };
        let Some(&rate) = self.sample_rates.iter().find(|&&rate| rate_label(rate) == *label) else {
            return;
        };
        let mut settings = self.settings.clone();
        settings.sample_rate = Some(rate);
        self.send(settings, cx);
    }

    /// Switches the output device to another buffer size.
    fn on_buffer_selected(
        &mut self,
        _select: &Entity<SelectState<Vec<SharedString>>>,
        event: &SelectEvent<Vec<SharedString>>,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let SelectEvent::Confirm(Some(label)) = event else {
            return;
        };
        let output_rate = self.output_rate;
        let Some(&frames) = self
            .buffer_sizes
            .iter()
            .find(|&&frames| buffer_label(frames, output_rate) == *label)
        else {
            return;
        };
        let mut settings = self.settings.clone();
        settings.buffer_size = frames;
        self.send(settings, cx);
    }

    /// Applies the typed loudness range when Enter is pressed or a field loses focus.
    fn on_limit_input(
        &mut self,
        _input: &Entity<InputState>,
        event: &InputEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if matches!(event, InputEvent::PressEnter { .. } | InputEvent::Blur) {
            self.commit_loudness_range(window, cx);
        }
    }

    /// Validates the typed loudness range and sends it, or restores the saved range and shows the error.
    fn commit_loudness_range(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let minimum = parse_loudness_limit(&self.minimum_input.read(cx).value());
        let maximum = parse_loudness_limit(&self.maximum_input.read(cx).value());
        match (minimum, maximum) {
            (Some(minimum), Some(maximum)) if is_valid_loudness_range(minimum, maximum) => {
                self.range_error = None;
                if minimum != self.settings.loudness_minimum || maximum != self.settings.loudness_maximum {
                    let mut settings = self.settings.clone();
                    settings.loudness_minimum = minimum;
                    settings.loudness_maximum = maximum;
                    self.send(settings, cx);
                }
            }
            _ => {
                self.range_error = Some(SharedString::from(RANGE_ERROR));
                set_text(
                    &self.minimum_input,
                    self.settings.loudness_minimum.to_string(),
                    window,
                    cx,
                );
                set_text(
                    &self.maximum_input,
                    self.settings.loudness_maximum.to_string(),
                    window,
                    cx,
                );
            }
        }
        cx.notify();
    }

    /// Renders the device group: the output device, the sample rate, and the buffer size.
    fn render_device_group(&self) -> Div {
        group("Audio interface and playback")
            .child(choice_row(
                "Output",
                DEVICE_TOOLTIP,
                Select::new(&self.device_select).small(),
            ))
            .child(choice_row(
                "Sample rate",
                SAMPLE_RATE_TOOLTIP,
                Select::new(&self.rate_select).small(),
            ))
            .child(choice_row(
                "Buffer size",
                BUFFER_SIZE_TOOLTIP,
                Select::new(&self.buffer_select).small(),
            ))
    }

    /// Renders the preview group: the playback options and the loudness graph with its axis range.
    fn render_preview_group(&self, cx: &Context<Self>) -> Div {
        let settings = &self.settings;
        let limits_enabled = settings.show_loudness_graph;
        let loudness_row = div()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(PADDING))
            .child(div().flex_1().child(Self::option(
                "show-loudness-graph",
                "Show loudness graph",
                LOUDNESS_GRAPH_TOOLTIP,
                settings.show_loudness_graph,
                |settings, checked| settings.show_loudness_graph = checked,
                cx,
            )))
            .child(limit_field(
                "loudness-minimum",
                "Min (LUFS)",
                &self.minimum_input,
                limits_enabled,
            ))
            .child(limit_field(
                "loudness-maximum",
                "Max (LUFS)",
                &self.maximum_input,
                limits_enabled,
            ));
        let error = div()
            .min_h(px(18.0))
            .text_size(px(HEADING_FONT_SIZE))
            .text_color(color(palette::ORANGE_RED))
            .children(self.range_error.clone());
        group("Preview preferences")
            .child(Self::option(
                "preview-plugin-chain",
                "Preview plugin chain",
                PLUGIN_PREVIEW_TOOLTIP,
                settings.preview_plugin_chain,
                |settings, checked| settings.preview_plugin_chain = checked,
                cx,
            ))
            .child(Self::option(
                "compensate-preview-latency",
                "Compensate plugin latency",
                COMPENSATION_TOOLTIP,
                settings.compensate_preview_latency,
                |settings, checked| settings.compensate_preview_latency = checked,
                cx,
            ))
            .child(Self::option(
                "continue-playback",
                "Continue playback when changing files",
                CONTINUE_TOOLTIP,
                settings.continue_playback,
                |settings, checked| settings.continue_playback = checked,
                cx,
            ))
            .child(Self::option(
                "play-from-first-cue-point",
                "Play from first cue point",
                FIRST_CUE_TOOLTIP,
                settings.play_from_first_cue_point,
                |settings, checked| settings.play_from_first_cue_point = checked,
                cx,
            ))
            .child(loudness_row)
            .child(error)
    }

    /// Renders an option checkbox that changes one setting.
    fn option(
        id: &'static str,
        label: &'static str,
        tooltip_text: &'static str,
        checked: bool,
        update: fn(&mut GuiSettings, bool),
        cx: &Context<Self>,
    ) -> Checkbox {
        Checkbox::new(id)
            .label(label)
            .small()
            .checked(checked)
            .tooltip(tooltip_text)
            .text_size(px(FONT_SIZE))
            .on_change(cx.listener(move |view, checked: &bool, _, cx| {
                let mut settings = view.settings.clone();
                update(&mut settings, *checked);
                view.send(settings, cx);
            }))
    }
}

impl EventEmitter<SettingsEvent> for SettingsView {}

impl Focusable for SettingsView {
    fn focus_handle(&self, _cx: &gpui_kit::App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for SettingsView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .id("audio-settings")
            .key_context(SETTINGS_KEY_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(|_, _: &CloseAudioSettings, window, _| window.remove_window()))
            .size_full()
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap(px(SECTION_SPACING))
            .p(px(PADDING + 4.0))
            .bg(color(palette::GREY_DARK))
            .text_color(color(palette::GREY_LIGHT))
            .font_family(TEXT_FONT)
            .text_size(px(FONT_SIZE))
            .child(self.render_device_group())
            .child(self.render_preview_group(cx))
    }
}

/// Replaces the choices of a select and selects one of them.
fn set_choices(
    select: &Entity<SelectState<Vec<SharedString>>>,
    labels: Vec<SharedString>,
    selected: Option<usize>,
    window: &mut Window,
    cx: &mut Context<SettingsView>,
) {
    select.update(cx, |state, cx| {
        state.set_items(labels, window, cx);
        state.set_selected_index(selected.map(IndexPath::new), window, cx);
    });
}

/// Replaces the text of a field unless it already shows it, so the caret of a field being edited stays.
fn set_text(input: &Entity<InputState>, text: String, window: &mut Window, cx: &mut Context<SettingsView>) {
    input.update(cx, |state, cx| {
        if state.value() != text {
            state.set_value(text, window, cx);
        }
    });
}

/// Returns the label of a sample rate, for example "44.1 kHz".
fn rate_label(rate: u32) -> SharedString {
    SharedString::from(format!("{} kHz", format_kilohertz(rate)))
}

/// Returns the label of a buffer size with its duration at the output rate, for example "256 samples (5.3 ms)".
fn buffer_label(frames: Option<u32>, output_rate: Option<u32>) -> SharedString {
    frames.map_or_else(
        || SharedString::from(DEFAULT_BUFFER_LABEL),
        |frames| {
            let text = output_rate.filter(|&rate| rate > 0).map_or_else(
                || format!("{frames} samples"),
                |rate| {
                    format!(
                        "{frames} samples ({:.1} ms)",
                        f64::from(frames) * 1000.0 / f64::from(rate)
                    )
                },
            );
            SharedString::from(text)
        },
    )
}

/// Renders a titled group of rows on a rounded panel.
fn group(title: &'static str) -> Div {
    div()
        .flex()
        .flex_col()
        .gap(px(ROW_SPACING))
        .p(px(PADDING))
        .rounded(px(CORNER_RADIUS))
        .bg(color(palette::GREY_SEMI_DARK))
        .border_1()
        .border_color(white(OUTLINE_ALPHA))
        .child(
            div()
                .pb(px(2.0))
                .text_size(px(HEADING_FONT_SIZE))
                .text_color(color(palette::GREY_MIDDLE_LIGHT))
                .child(title),
        )
}

/// Renders a captioned row holding a choice, with a tooltip over the whole row.
fn choice_row(caption: &'static str, tooltip_text: &'static str, control: impl IntoElement) -> impl IntoElement {
    div()
        .id(ElementId::Name(caption.into()))
        .tooltip(tooltip(tooltip_text))
        .flex()
        .flex_row()
        .items_center()
        .gap(px(PADDING))
        .child(
            div()
                .w(px(CAPTION_WIDTH))
                .flex_none()
                .text_color(color(palette::GREY_MIDDLE_LIGHT))
                .child(caption),
        )
        .child(div().flex_1().min_w_0().child(control))
}

/// Renders a captioned loudness limit field.
fn limit_field(id: &'static str, caption: &'static str, input: &Entity<InputState>, enabled: bool) -> AnyElement {
    div()
        .id(id)
        .tooltip(tooltip(LOUDNESS_LIMIT_TOOLTIP))
        .flex()
        .flex_row()
        .items_center()
        .gap(px(6.0))
        .child(
            div()
                .text_color(color(if enabled {
                    palette::GREY_MIDDLE_LIGHT
                } else {
                    palette::GREY_MIDDLE
                }))
                .child(caption),
        )
        .child(
            div()
                .w(px(LIMIT_INPUT_WIDTH))
                .child(Input::new(input).small().disabled(!enabled)),
        )
        .into_any_element()
}

#[cfg(test)]
mod test_choice_labels {
    use super::*;

    #[test]
    fn buffer_sizes_show_their_duration() {
        assert_eq!(buffer_label(Some(256), Some(48_000)), "256 samples (5.3 ms)");
        assert_eq!(buffer_label(Some(256), None), "256 samples");
        assert_eq!(buffer_label(None, Some(48_000)), DEFAULT_BUFFER_LABEL);
    }

    #[test]
    fn rates_show_kilohertz() {
        assert_eq!(rate_label(44_100), "44.1 kHz");
        assert_eq!(rate_label(48_000), "48 kHz");
    }
}
