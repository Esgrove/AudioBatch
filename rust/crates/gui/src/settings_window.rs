//! The Audio Settings window: the output device and the preview preferences.
//!
//! It is a separate window with two groups.
//! "Audio interface and playback" chooses the output device, its sample rate, and its buffer size,
//! and "Preview preferences" holds the plugin chain preview, the playback options,
//! the source visualization, the waveform colours, independent overload indicators, and the loudness graph range.
//! The main view stays the owner of the settings.
//! This view sends every change with the previously displayed settings,
//! so the main view applies only edited fields before syncing back what it actually opened.

use audiobatch_core::format::format_kilohertz;
use audiobatch_playback::DeviceCapabilities;
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use gpui_kit::component::{IndexPath, Sizable as _};
use gpui_kit::{
    AnyElement, AppContext as _, Context, Div, ElementId, Entity, EventEmitter, FocusHandle, Focusable,
    InteractiveElement as _, IntoElement, ParentElement as _, Render, SharedString, StatefulInteractiveElement as _,
    Styled as _, Subscription, Window, div, prelude::FluentBuilder as _, relative, rems,
};

use crate::info_panel::{CORNER_RADIUS, OUTLINE_ALPHA};
use crate::menus::{CloseAudioSettings, SETTINGS_KEY_CONTEXT};
use crate::settings::{GuiSettings, PreviewViewMode, is_valid_loudness_range, parse_loudness_limit};
use crate::theme::{TEXT_FONT, color, palette, tooltip, white};

/// Title of the window.
pub const SETTINGS_TITLE: &str = "Audio Settings";

/// Initial width of the window.
pub const SETTINGS_WIDTH: f32 = 540.0;

/// Initial height of the window.
pub const SETTINGS_HEIGHT: f32 = 390.0;

/// Padding inside the groups, in rem.
const PADDING: f32 = 0.75;

/// Space between the groups, in rem.
const SECTION_SPACING: f32 = 1.0;

/// Space between the rows of a group, in rem.
const ROW_SPACING: f32 = 0.5;

/// Width of the row captions, in rem.
const CAPTION_WIDTH: f32 = 6.875;

/// Width of a loudness limit field, in rem.
const LIMIT_INPUT_WIDTH: f32 = 2.75;

/// Font size of the window text, in rem.
const FONT_SIZE: f32 = 0.8125;

/// Font size of the group headings, in rem.
const HEADING_FONT_SIZE: f32 = 0.75;

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

/// Tooltip of the preview visualization choice.
const PREVIEW_VIEW_TOOLTIP: &str = "Choose the source visualization and its arrangement in the preview.";

/// Tooltip of the Serato waveform colours option.
const SERATO_COLOURS_TOOLTIP: &str =
    "Use Serato spectral waveform colours where available, without changing cues or playback.";

/// Tooltip of the sample-peak overload option.
const SAMPLE_OVERLOADS_TOOLTIP: &str = "Show source sample-peak overload indicators independently of true peaks.";

/// Tooltip of the true-peak overload option.
const TRUE_PEAK_OVERLOADS_TOOLTIP: &str = "Show source true-peak overload indicators independently of sample peaks.";

/// Tooltip of the overload line option.
const OVERLOAD_LINES_TOOLTIP: &str = "Extend enabled overload indicators as vertical lines across the preview.";

/// Proportion of the available control lane occupied by the preview mode selector.
const PREVIEW_SELECTOR_WIDTH_PROPORTION: f32 = 0.7;

/// Tooltip of the plugin chain preview option.
const PLUGIN_PREVIEW_TOOLTIP: &str =
    "Listen through enabled plugins and the file's custom input gain. Does not normalize or write files.";

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
     Uses cached analysis, not preview gain or live plugins.";

/// Tooltip of the loudness limit fields.
const LOUDNESS_LIMIT_TOOLTIP: &str = "Whole-number LUFS from -60 to +6. Maximum must be at least 1 LU above minimum.";

/// Events the settings window sends to the main view.
#[derive(Debug, Clone)]
pub enum SettingsEvent {
    /// The requested settings and the snapshot they were edited from, allowing stale fields to be ignored.
    Changed {
        /// Settings the window displayed before this edit.
        previous: GuiSettings,
        /// Settings containing the user's requested change.
        settings: GuiSettings,
    },
}

/// What the settings window shows, collected by the main view from its player and settings.
#[non_exhaustive]
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
    preview_view_select: Entity<SelectState<Vec<SharedString>>>,
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
        let preview_view_select = cx.new(|cx| {
            SelectState::new(
                PreviewViewMode::ALL
                    .map(|mode| SharedString::from(mode.label()))
                    .to_vec(),
                None,
                window,
                cx,
            )
        });
        let minimum_input = cx.new(|cx| InputState::new(window, cx));
        let maximum_input = cx.new(|cx| InputState::new(window, cx));
        let subscriptions = vec![
            cx.subscribe_in(&device_select, window, Self::on_device_selected),
            cx.subscribe_in(&rate_select, window, Self::on_rate_selected),
            cx.subscribe_in(&buffer_select, window, Self::on_buffer_selected),
            cx.subscribe_in(&preview_view_select, window, Self::on_preview_view_selected),
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
            preview_view_select,
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

        let mode_index = PreviewViewMode::ALL
            .iter()
            .position(|&mode| mode == self.settings.preview_view_mode);
        self.preview_view_select.update(cx, |state, cx| {
            state.set_selected_index(mode_index.map(IndexPath::new), window, cx);
        });

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
        let previous = std::mem::replace(&mut self.settings, settings.clone());
        cx.emit(SettingsEvent::Changed { previous, settings });
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

    /// Requests a source visualization while leaving the other preview preferences unchanged.
    fn on_preview_view_selected(
        &mut self,
        _select: &Entity<SelectState<Vec<SharedString>>>,
        event: &SelectEvent<Vec<SharedString>>,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let SelectEvent::Confirm(Some(label)) = event else {
            return;
        };
        let Some(mode) = PreviewViewMode::ALL
            .into_iter()
            .find(|mode| mode.label() == label.as_ref())
        else {
            return;
        };
        if mode != self.settings.preview_view_mode {
            let mut settings = self.settings.clone();
            settings.preview_view_mode = mode;
            self.send(settings, cx);
        }
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

    /// Renders the preview group: the playback options, source visualization, and source indicators.
    fn render_preview_group(&self, cx: &Context<Self>) -> Div {
        let settings = &self.settings;
        group("Preview preferences")
            .child(choice_row(
                "Preview view",
                PREVIEW_VIEW_TOOLTIP,
                div().w(relative(PREVIEW_SELECTOR_WIDTH_PROPORTION)).child(
                    Select::new(&self.preview_view_select)
                        .id("preview-view-mode")
                        .small()
                        .accessibility_label("Preview view"),
                ),
            ))
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
            .child(Self::option(
                "serato-waveform-colours",
                "Serato waveform colours",
                SERATO_COLOURS_TOOLTIP,
                settings.serato_waveform_colours,
                |settings, checked| settings.serato_waveform_colours = checked,
                cx,
            ))
            .child(self.render_overload_types(cx))
            .child(Self::option(
                "show-overload-lines",
                "Overload vertical lines",
                OVERLOAD_LINES_TOOLTIP,
                settings.show_overload_lines,
                |settings, checked| settings.show_overload_lines = checked,
                cx,
            ))
            .child(self.render_loudness_range(cx))
            .when_some(self.range_error.clone(), |group, error| {
                group.child(
                    div()
                        .text_size(rems(HEADING_FONT_SIZE))
                        .text_color(color(palette::ORANGE_RED))
                        .child(error),
                )
            })
    }

    /// Renders the two independently enabled overload types on a shared row.
    fn render_overload_types(&self, cx: &Context<Self>) -> Div {
        div()
            .flex()
            .flex_row()
            .flex_wrap()
            .gap_3()
            .child(div().flex_1().child(Self::option(
                "show-sample-overloads",
                "Peak overloads",
                SAMPLE_OVERLOADS_TOOLTIP,
                self.settings.show_sample_overloads,
                |settings, checked| settings.show_sample_overloads = checked,
                cx,
            )))
            .child(div().flex_1().child(Self::option(
                "show-true-peak-overloads",
                "True-peak overloads",
                TRUE_PEAK_OVERLOADS_TOOLTIP,
                self.settings.show_true_peak_overloads,
                |settings, checked| settings.show_true_peak_overloads = checked,
                cx,
            )))
    }

    /// Renders the loudness graph toggle and its two associated axis limits.
    fn render_loudness_range(&self, cx: &Context<Self>) -> Div {
        let enabled = self.settings.show_loudness_graph;
        div()
            .flex()
            .flex_row()
            .items_center()
            .gap(rems(PADDING))
            .child(div().flex_1().child(Self::option(
                "show-loudness-graph",
                "Show loudness graph",
                LOUDNESS_GRAPH_TOOLTIP,
                enabled,
                |settings, checked| settings.show_loudness_graph = checked,
                cx,
            )))
            .child(limit_field(
                "loudness-minimum",
                "Min (LUFS)",
                &self.minimum_input,
                enabled,
            ))
            .child(limit_field(
                "loudness-maximum",
                "Max (LUFS)",
                &self.maximum_input,
                enabled,
            ))
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
            .text_size(rems(FONT_SIZE))
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
            .min_h_0()
            .overflow_y_scroll()
            .bg(color(palette::GREY_DARK))
            .text_color(color(palette::GREY_LIGHT))
            .font_family(TEXT_FONT)
            .text_size(rems(FONT_SIZE))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(rems(SECTION_SPACING))
                    .p_4()
                    .child(self.render_device_group())
                    .child(self.render_preview_group(cx)),
            )
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
        .gap(rems(ROW_SPACING))
        .p(rems(PADDING))
        .rounded(rems(CORNER_RADIUS * 0.0625))
        .bg(color(palette::GREY_SEMI_DARK))
        .border_1()
        .border_color(white(OUTLINE_ALPHA))
        .child(
            div()
                .pb(rems(0.125))
                .text_size(rems(HEADING_FONT_SIZE))
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
        .gap(rems(PADDING))
        .child(
            div()
                .w(rems(CAPTION_WIDTH))
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
        .gap(rems(0.375))
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
                .w(rems(LIMIT_INPUT_WIDTH))
                .child(Input::new(input).small().disabled(!enabled)),
        )
        .into_any_element()
}

#[cfg(test)]
mod integration_tests {
    use gpui_kit::test::TestWindowExt as _;
    use gpui_kit::{AnyWindowHandle, Bounds, Point, TestAppContext, WindowBounds, WindowOptions, px, size};

    use super::*;

    /// Opens the production settings view with enough space to exercise every preference without audio hardware.
    fn open_settings(cx: &TestAppContext) -> (AnyWindowHandle, Entity<SettingsView>) {
        cx.update(|cx| {
            gpui_kit::init(cx);
            crate::theme::install(cx);
        });
        let (window, view) = cx.update(|cx| {
            gpui_kit::open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Windowed(Bounds::new(
                        Point::default(),
                        size(px(800.0), px(900.0)),
                    ))),
                    ..WindowOptions::default()
                },
                cx,
                |window, cx| cx.new(|cx| SettingsView::new(&SettingsSnapshot::default(), window, cx)),
            )
            .expect("opens the settings view")
        });
        (window, view)
    }

    #[gpui_kit::test]
    fn overload_controls_accept_all_eight_independent_combinations(cx: &mut TestAppContext) {
        let (handle, view) = open_settings(cx);
        for sample in [false, true] {
            for true_peak in [false, true] {
                for lines in [false, true] {
                    for (id, checked) in [
                        ("show-sample-overloads", sample),
                        ("show-true-peak-overloads", true_peak),
                        ("show-overload-lines", lines),
                    ] {
                        cx.update_window(handle, |_, window, cx| {
                            window.render_frame(cx);
                            if window.find(id).checked() != Some(checked) {
                                window.click(id, cx);
                            }
                            assert_eq!(window.find(id).checked(), Some(checked));
                        })
                        .expect("operates an overload checkbox");
                        cx.run_until_parked();
                    }
                    cx.read(|cx| {
                        let settings = &view.read(cx).settings;
                        assert_eq!(settings.show_sample_overloads, sample);
                        assert_eq!(settings.show_true_peak_overloads, true_peak);
                        assert_eq!(settings.show_overload_lines, lines);
                    });
                }
            }
        }
    }

    #[gpui_kit::test]
    fn preview_selector_uses_keyboard_and_retains_its_identity_when_synced(cx: &mut TestAppContext) {
        let (handle, view) = open_settings(cx);
        let selector = cx.read(|cx| view.read(cx).preview_view_select.entity_id());
        for mode in PreviewViewMode::ALL {
            if mode != PreviewViewMode::Waveform {
                cx.update_window(handle, |_, window, cx| {
                    window.render_frame(cx);
                    window.within("preview-view-mode").click("input", cx);
                    window.press("down", cx);
                    window.press("enter", cx);
                })
                .expect("selects the next preview mode");
                cx.run_until_parked();
            }
            cx.read(|cx| assert_eq!(view.read(cx).settings.preview_view_mode, mode));
        }
        let snapshot = SettingsSnapshot {
            settings: GuiSettings {
                preview_view_mode: PreviewViewMode::Spectrogram,
                ..GuiSettings::default()
            },
            ..SettingsSnapshot::default()
        };
        cx.update_window(handle, |_, window, cx| {
            view.update(cx, |view, cx| view.sync(&snapshot, window, cx));
            window.render_frame(cx);
            assert_eq!(window.find("preview-view-mode").value(), Some("Spectrogram"));
            assert_eq!(view.read(cx).preview_view_select.entity_id(), selector);
        })
        .expect("syncs the canonical preview mode");
    }
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
