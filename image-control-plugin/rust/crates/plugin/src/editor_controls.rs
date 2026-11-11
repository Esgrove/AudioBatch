//! Retained parameter widgets, settings callout, and value presentation for the Iced editor.

use std::time::{Duration, Instant};

use iced::advanced::text::Paragraph as _;

use iced::widget::{Column, Row, Space, button, checkbox, container, pick_list, pin, scrollable, slider, stack, text};
use iced::{Alignment, Element, Length, Rectangle, Size, Theme};
use iced_baseview as iced;
use image_control_dsp::{AnalysisStatus, ColourMode, CorrelationSnapshot, DisplayPreferences};

use super::presets::Action;
use super::{Message, layout, theme};
use crate::params::{ParameterKind, ParameterSpec, parameter_spec};

#[path = "editor_interaction.rs"]
pub(super) mod interaction;
use interaction::FocusSafeSlider;

/// Tracks the last observed publication on the UI thread, without audio-thread clock calls.
pub(super) struct CorrelationObservation {
    readings: [f32; 4],
    generation: u64,
    revision: u64,
    updated: Option<Instant>,
}

impl Default for CorrelationObservation {
    /// Starts without an audio publication so inactive meters cannot fabricate a correlation reading.
    fn default() -> Self {
        Self {
            readings: [f32::NAN; 4],
            generation: u64::MAX,
            revision: 0,
            updated: None,
        }
    }
}

impl CorrelationObservation {
    /// Only a fresh audio publication restarts expiry. GUI refreshes never fabricate meter activity.
    pub(super) const fn observe(&mut self, snapshot: CorrelationSnapshot, now: Instant) {
        if snapshot.generation != self.generation {
            self.readings = [f32::NAN; 4];
            self.updated = None;
            self.generation = snapshot.generation;
            self.revision = snapshot.revision;
            return;
        }
        if snapshot.revision != self.revision {
            self.readings = snapshot.readings;
            self.revision = snapshot.revision;
            self.updated = Some(now);
        }
    }

    /// Returns unavailable values after two maximum host blocks, with a 250 ms minimum.
    pub(super) fn readings(&self, now: Instant, expiry: Duration) -> [Option<f32>; 4] {
        if self
            .updated
            .is_none_or(|updated| now.saturating_duration_since(updated) >= expiry.max(Duration::from_millis(250)))
        {
            return [None; 4];
        }
        self.readings
            .map(|value| value.is_finite().then(|| value.clamp(-1.0, 1.0)))
    }
}

/// Explains analysis-only limitations while all ordinary processing controls remain available.
pub(super) const fn analysis_status_text(status: AnalysisStatus) -> Option<&'static str> {
    match status {
        AnalysisStatus::Waiting => Some("Waiting for audio"),
        AnalysisStatus::UnsupportedRate => Some("Spatial analysis unavailable at this sample rate"),
        AnalysisStatus::Failed => Some("Spatial analysis failed"),
        AnalysisStatus::Disabled => Some("Spatial analysis disabled"),
        AnalysisStatus::Running => None,
    }
}

/// Displays real processed-band readings, with no marker or invented number while unavailable.
pub(super) fn correlation_meter<'a>(
    snapshot: &crate::correlation_display::MeterReading,
    smoothed: bool,
) -> Element<'a, Message> {
    let reading = snapshot.live;
    let number = snapshot.value(smoothed);
    let label = Row::new()
        .push(text("Correlation").size(11))
        .push(Space::new().width(Length::Fill))
        .push(
            text(number.map_or_else(String::new, |value| format!("{value:+.2}")))
                .font(theme::MONO)
                .size(11),
        )
        .height(17.0);
    let mut track = stack![
        container(Space::new().height(5.0))
            .width(Length::Fill)
            .style(|_| container::Style::default().background(theme::GREY_MEDIUM)),
    ];
    let mut field = Row::new();
    for intensity in snapshot.range_intensity {
        field = field.push(
            container(Space::new().height(5.0))
                .width(Length::FillPortion(1))
                .style(move |_| container::Style::default().background(theme::BLUE.scale_alpha(intensity * 0.35))),
        );
    }
    track = track.push(field);
    if let Some(value) = snapshot.trend {
        let position = ((value + 1.0) * 500.0).round() as u16;
        track = track.push(
            Row::new()
                .push(Space::new().width(Length::FillPortion(position.max(1))))
                .push(
                    container(Space::new().width(9.0).height(5.0))
                        .style(|_| container::Style::default().background(theme::YELLOW.scale_alpha(0.6))),
                )
                .push(Space::new().width(Length::FillPortion((1000 - position).max(1)))),
        );
    }
    if let Some(value) = reading {
        let position = ((value + 1.0) * 500.0).round() as u16;
        track = track.push(
            Row::new()
                .push(Space::new().width(Length::FillPortion(position.max(1))))
                .push(container(Space::new().width(3.0).height(5.0)).style(move |_| {
                    container::Style::default().background(if value < 0.0 { theme::ORANGE } else { theme::BLUE })
                }))
                .push(Space::new().width(Length::FillPortion((1000 - position).max(1)))),
        );
    }
    let scale = Row::new()
        .push(text("-1").size(11))
        .push(Space::new().width(Length::Fill))
        .push(text("0").size(11))
        .push(Space::new().width(Length::Fill))
        .push(text("+1").size(11));
    container(
        Column::new()
            .push(label)
            .push(track)
            .push(Space::new().height(2.0))
            .push(scale),
    )
    .padding([2.0, 10.0])
    .width(Length::FillPortion(1))
    .height(layout::CORRELATION_HEIGHT)
    .style(|_| container::Style::default().color(theme::GREY_MIDDLE_LIGHT))
    .into()
}

/// Builds the same retained parameter widget for production views and hardware-free event tests.
pub(super) fn parameter_input(id: u32, value: f64) -> FocusSafeSlider<'static, f64, Message> {
    let spec = parameter_spec(id).expect("visible control has registry metadata");
    FocusSafeSlider::new(
        slider(0.0..=1.0, value, move |value| Message::Change(id, value))
            .step(0.001_f64)
            .shift_step(0.0001_f64)
            .default(spec.default_normalized)
            .height(layout::SLIDER_HEIGHT)
            .style(theme::parameter_slider),
    )
    .id(format!("parameter-slider-{id}"))
    .on_release(Message::EndGesture)
    .on_numeric(id)
}

/// Gives every button a compact, fixed target and disables unavailable features rather than simulating them.
pub(super) fn control(
    label: &str,
    width: f32,
    message: Option<Message>,
    active: bool,
    accent: iced::Color,
) -> Element<'_, Message> {
    control_button(label, width, message, active, accent).into()
}

/// Shares the production button with event-only tests rather than simulating its messages.
pub(super) fn control_button(
    label: &str,
    width: f32,
    message: Option<Message>,
    active: bool,
    accent: iced::Color,
) -> button::Button<'_, Message> {
    control_input(
        text(label)
            .size(theme::LABEL_SIZE)
            .line_height(theme::LINE_HEIGHT)
            .width(Length::Fill)
            .height(Length::Fill)
            .align_x(iced::alignment::Horizontal::Center)
            .align_y(iced::alignment::Vertical::Center),
        width,
        message,
        active,
        accent,
    )
}

/// Retains production click and selected styling behavior independently of glyph rasterization.
pub(super) fn control_input<'a, Renderer: iced::advanced::Renderer>(
    content: impl Into<iced::core::Element<'a, Message, Theme, Renderer>>,
    width: f32,
    message: Option<Message>,
    active: bool,
    accent: iced::Color,
) -> button::Button<'a, Message, Theme, Renderer> {
    button(content)
        .width(width)
        .height(24.0)
        .padding(0)
        .on_press_maybe(message)
        .style(move |_, status| theme::control(status, active, accent))
}

/// Routes comparison commands independently from audition controls and display preferences.
pub(super) fn processing_toolbar<'a>(active: usize, preset_name: String) -> Element<'a, Message> {
    container(
        Row::new()
            .push(control("A", 32.0, Some(Message::Compare(0)), active == 0, theme::BLUE))
            .push(control("B", 32.0, Some(Message::Compare(1)), active == 1, theme::BLUE))
            .push(Space::new().width(4.0))
            .push(control(
                if active == 0 { "A → B" } else { "B → A" },
                72.0,
                Some(Message::CopyComparison),
                false,
                theme::BLUE,
            ))
            .push(Space::new().width(12.0))
            .push(
                container(
                    text(preset_name)
                        .size(theme::LABEL_SIZE)
                        .line_height(theme::LINE_HEIGHT)
                        .color(theme::GREY_MIDDLE),
                )
                .padding([3.0, 8.0])
                .width(Length::Fill)
                .height(24.0)
                .style(theme::value),
            )
            .push(Space::new().width(160.0))
            .push(control("Presets", 72.0, Some(Message::Presets), false, theme::BLUE))
            .height(24.0)
            .align_y(Alignment::Center),
    )
    .padding([4.0, 0.0])
    .height(layout::TOOLBAR_HEIGHT)
    .into()
}

/// Pins a real retained widget into an independently testable logical target.
pub(super) fn at<'a>(content: impl Into<Element<'a, Message>>, bounds: Rectangle) -> Element<'a, Message> {
    pin(container(content).width(bounds.width).height(bounds.height))
        .x(bounds.x)
        .y(bounds.y)
        .into()
}

/// Matches the C++ callout's preference order. Unsupported renderer settings retain disabled native controls.
pub(super) fn settings(
    display: DisplayPreferences,
    correlation_shown: bool,
    hidden: bool,
    stopped: bool,
) -> Element<'static, Message> {
    let preferences = Column::new()
        .push(appearance_settings(display))
        .push(Space::new().height(12.0))
        .push(stereo_position_settings(display))
        .push(Space::new().height(12.0))
        .push(settings_heading("Correlation"))
        .push(settings_check(
            "Show band correlation",
            correlation_shown,
            Some(Message::Correlation),
        ))
        .push(settings_check(
            "Smooth correlation numbers",
            display.smooth_correlation,
            Some(Message::SmoothCorrelation),
        ))
        .push(Space::new().height(12.0))
        .push(settings_heading("Display behavior"))
        .push(
            text("Persistence")
                .size(theme::LABEL_SIZE)
                .line_height(theme::LINE_HEIGHT)
                .height(18.0),
        )
        .push(setting_slider(
            display.persistence_seconds,
            0.05..=3.0,
            0.01,
            Message::Persistence,
            format!("{:.2} s", display.persistence_seconds),
        ))
        .push(settings_check(
            "Hide spectrogram",
            hidden,
            Some(Message::HideSpectrogram),
        ))
        .push(settings_check(
            "Stop spectrogram analysis",
            stopped,
            Some(Message::StopAnalysis),
        ))
        .push(settings_check(
            "Show frame rate",
            display.show_fps,
            Some(Message::ShowFps),
        ))
        .push(Space::new().height(8.0))
        .push(control(
            "Reset display appearance",
            366.0,
            Some(Message::ResetAppearance),
            false,
            theme::BLUE,
        ))
        .push(Space::new().height(8.0))
        .push(control(
            "Keyboard shortcuts",
            366.0,
            Some(Message::Shortcuts),
            false,
            theme::BLUE,
        ));
    container(scrollable(container(preferences).padding(iced::Padding {
        top: 4.0,
        right: 12.0,
        bottom: 12.0,
        left: 12.0,
    })))
    .height(Length::Fill)
    .width(390.0)
    .style(theme::callout)
    .into()
}

/// Groups appearance choices without changing their order or retained widget geometry.
fn appearance_settings(display: DisplayPreferences) -> Element<'static, Message> {
    let colour = if display.color_mode == ColourMode::Frequency {
        "Frequency"
    } else {
        "Phase coherence"
    };
    Column::new()
        .push(settings_heading("Appearance"))
        .push(colour_choice(colour))
        .push(preference_choice(
            "Frequency scale",
            ["Logarithmic", "Mel"],
            display.mel_scale,
            Message::MelScale,
        ))
        .push(preference_choice(
            "Rendering",
            ["Particles", "Solid layers"],
            display.solid_rendering,
            Message::SolidRendering,
        ))
        .push(
            text("Contrast")
                .size(theme::LABEL_SIZE)
                .line_height(theme::LINE_HEIGHT)
                .height(18.0),
        )
        .push(setting_slider(
            display.contrast,
            0.0..=1.0,
            0.01,
            Message::Contrast,
            format!("{:.0}%", display.contrast * 100.0),
        ))
        .into()
}

/// Keeps the two dependent stereo position controls together within the settings callout.
fn stereo_position_settings(display: DisplayPreferences) -> Element<'static, Message> {
    Column::new()
        .push(settings_heading("Stereo position"))
        .push(
            text("Midpoint (50%)")
                .size(theme::LABEL_SIZE)
                .line_height(theme::LINE_HEIGHT)
                .height(18.0),
        )
        .push(setting_slider(
            display.pan_midpoint_db,
            3.0..=24.0,
            1.0,
            Message::PanMidpoint,
            format!("{:.0} dB", display.pan_midpoint_db),
        ))
        .push(
            text("Edge (99%)")
                .size(theme::LABEL_SIZE)
                .line_height(theme::LINE_HEIGHT)
                .height(18.0),
        )
        .push(setting_slider(
            display.pan_edge_db,
            12.0..=60.0,
            1.0,
            Message::PanEdge,
            format!("{:.0} dB", display.pan_edge_db),
        ))
        .into()
}

/// Exposes the independent preset library, explicit destructive confirmation, and portable file paths.
pub(super) fn preset_panel(presets: &super::presets::PresetEditor) -> Element<'_, Message> {
    let available = !presets.busy();
    let mut content = Column::new()
        .spacing(8.0)
        .push(settings_heading("User presets"))
        .push(
            iced::widget::text_input("Preset name", &presets.name)
                .on_input(Message::PresetName)
                .size(theme::LABEL_SIZE),
        )
        .push(text("Portable JSON file path for import/export").size(theme::LABEL_SIZE))
        .push(
            iced::widget::text_input("Absolute file path", &presets.path)
                .on_input(Message::PresetPath)
                .size(theme::LABEL_SIZE),
        );
    for actions in [
        [
            (Action::Save, "Save"),
            (Action::SaveAs, "Save As"),
            (Action::Rename, "Rename"),
        ],
        [
            (Action::Delete, "Delete"),
            (Action::Import, "Import"),
            (Action::Export, "Export"),
        ],
    ] {
        let mut row = Row::new().spacing(8.0);
        for (action, label) in actions {
            row = row.push(control(
                label,
                122.0,
                available.then_some(Message::PresetAction(action)),
                false,
                theme::BLUE,
            ));
        }
        content = content.push(row);
    }
    content = content.push(
        Row::new()
            .spacing(8.0)
            .push(control(
                "Reconnect",
                122.0,
                available.then_some(Message::PresetAction(Action::Reconnect)),
                false,
                theme::BLUE,
            ))
            .push(control(
                "Refresh",
                122.0,
                available.then_some(Message::PresetAction(Action::Refresh)),
                false,
                theme::BLUE,
            )),
    );
    if presets.confirmation.is_some() {
        content = content
            .push(text("Confirm changing or deleting the selected library revision?").size(theme::LABEL_SIZE))
            .push(control(
                "Confirm",
                122.0,
                Some(Message::ConfirmPreset),
                false,
                theme::ORANGE,
            ));
    }
    if let Some(error) = &presets.error {
        content = content.push(text(error).size(theme::LABEL_SIZE).color(theme::ORANGE));
    }
    if presets.busy() {
        content = content.push(text("Working…").size(theme::LABEL_SIZE));
    }
    for (index, preset) in presets.entries.iter().enumerate() {
        content = content.push(control(
            &preset.name,
            398.0,
            available.then_some(Message::PresetLoad(index)),
            false,
            theme::BLUE,
        ));
    }
    content = content.push(control("Close", 122.0, Some(Message::CancelPreset), false, theme::BLUE));
    container(scrollable(content))
        .padding(12.0)
        .width(430.0)
        .height(Length::Fill)
        .style(theme::callout)
        .into()
}

/// Keeps shortcut help inside the same modal focus barrier as ordinary display settings.
pub(super) fn shortcuts() -> Element<'static, Message> {
    let modifier = if cfg!(target_os = "macos") { "Cmd" } else { "Ctrl" };
    let descriptions = [
        format!("{modifier}+Z: Undo     {modifier}+Shift+Z / {modifier}+Y: Redo"),
        format!("{modifier}+B: Bypass     {modifier}+D: Delta"),
        format!("{modifier}+, : Display settings     F1: Shortcuts"),
        "S: Solo the band under the pointer".into(),
        "Tab / Shift+Tab: Next / previous control".into(),
        "Arrows: Adjust focused control     Shift: Fine adjustment".into(),
        "Enter: Commit exact value     Escape: Cancel / back".into(),
        "Double-click: Reset slider or crossover".into(),
        "Drag plot: Move crossover or whole band".into(),
        format!("{modifier}+vertical drag / scroll: Band Mid"),
        "Alt/Option+vertical drag or scroll: Band Side".into(),
        "Focused divider Up/Down: 8 px     Shift: 1 px".into(),
    ];
    let mut content = Column::new()
        .spacing(12.0)
        .push(settings_heading("Keyboard and pointer shortcuts"));
    for description in descriptions {
        content = content.push(text(description).size(theme::LABEL_SIZE));
    }
    container(scrollable(content.push(control(
        "Back to Display settings",
        366.0,
        Some(Message::Shortcuts),
        false,
        theme::BLUE,
    ))))
    .padding(12.0)
    .width(390.0)
    .height(Length::Fill)
    .style(theme::callout)
    .into()
}

/// Builds a display preference toggle without creating a sound-parameter gesture.
fn settings_check(label: &str, checked: bool, message: Option<Message>) -> Element<'_, Message> {
    container(
        checkbox(checked)
            .label(label)
            .text_size(theme::LABEL_SIZE)
            .size(theme::CHECKBOX_SIZE)
            .style(theme::check)
            .on_toggle_maybe(message.map(|message| move |_| message.clone())),
    )
    .center_y(28.0)
    .into()
}

/// Separates settings groups with a shared heading baseline and following gap.
fn settings_heading(label: &str) -> Element<'_, Message> {
    Column::new()
        .push(
            container(
                text(label)
                    .font(theme::SEMIBOLD)
                    .size(theme::SECTION_TITLE_SIZE)
                    .line_height(theme::LINE_HEIGHT)
                    .color(theme::GREY_LIGHT),
            )
            .height(24.0),
        )
        .push(Space::new().height(8.0))
        .into()
}

/// Builds the one active settings choice with the same label and control baselines as disabled choices.
fn colour_choice(colour: &str) -> Element<'_, Message> {
    Row::new()
        .push(
            text("Colour")
                .size(theme::LABEL_SIZE)
                .line_height(theme::LINE_HEIGHT)
                .width(112.0),
        )
        .push(
            pick_list(["Frequency", "Phase coherence"], Some(colour), |value| {
                Message::Colour(if value == "Frequency" {
                    ColourMode::Frequency
                } else {
                    ColourMode::Phase
                })
            })
            .text_size(theme::LABEL_SIZE)
            .padding([3.0, 8.0])
            .width(Length::Fill)
            .style(theme::choice),
        )
        .height(28.0)
        .align_y(Alignment::Center)
        .into()
}

/// Uses one ordinary retained choice for every binary display preference.
fn preference_choice<'a>(
    label: &'a str,
    choices: [&'static str; 2],
    second: bool,
    message: fn(bool) -> Message,
) -> Element<'a, Message> {
    let selected = *choices.get(usize::from(second)).expect("binary preference");
    Row::new()
        .push(
            text(label)
                .size(theme::LABEL_SIZE)
                .line_height(theme::LINE_HEIGHT)
                .width(112.0),
        )
        .push(
            pick_list(choices, Some(selected), move |value| {
                message(value == *choices.get(1).expect("second preference"))
            })
            .text_size(theme::LABEL_SIZE)
            .padding([3.0, 8.0])
            .width(Length::Fill)
            .style(theme::choice),
        )
        .height(28.0)
        .align_y(Alignment::Center)
        .into()
}

/// Pairs a display preference slider with its formatted value without host sound automation.
fn setting_slider(
    value: f32,
    range: std::ops::RangeInclusive<f32>,
    step: f32,
    change: fn(f32) -> Message,
    label: String,
) -> Element<'static, Message> {
    Row::new()
        .push(
            container(FocusSafeSlider::new(
                slider(range, value, change).step(step).style(theme::parameter_slider),
            ))
            .padding([0.0, 6.0])
            .width(Length::Fill),
        )
        .push(
            container(
                text(label)
                    .font(theme::MONO)
                    .size(theme::VALUE_SIZE)
                    .line_height(theme::LINE_HEIGHT)
                    .width(Length::Fill)
                    .align_x(iced::alignment::Horizontal::Center),
            )
            .width(76.0)
            .center_y(22.0)
            .style(theme::value),
        )
        .height(28.0)
        .align_y(Alignment::Center)
        .into()
}

/// Measures worst-case numeric glyphs once per view so compact full names never depend on the current value.
pub(super) fn compact_text_width(content: &str) -> f32 {
    let paragraph =
        <iced::Renderer as iced::advanced::text::Renderer>::Paragraph::with_text(iced::advanced::text::Text {
            content,
            bounds: Size::new(f32::INFINITY, 11.0),
            size: 11.0.into(),
            line_height: iced::advanced::text::LineHeight::Relative(1.0),
            font: theme::MONO,
            align_x: iced::advanced::text::Alignment::Center,
            align_y: iced::alignment::Vertical::Center,
            shaping: iced::advanced::text::Shaping::Basic,
            wrapping: iced::advanced::text::Wrapping::None,
        });
    paragraph.min_width().ceil() + 4.0
}

/// Keeps compact values in their parameter domains while appending the chosen field identity.
pub(super) fn format_compact_value(spec: ParameterSpec, normalized: f64, suffix: &str) -> String {
    let value = spec.normalized_to_plain(normalized);
    match spec.kind {
        ParameterKind::BandGain if value <= -60.0 => format!("-inf{suffix}"),
        ParameterKind::BandGain => format!("{value:+.1}{suffix}"),
        ParameterKind::Width => format!("{value:.2}{suffix}"),
        ParameterKind::Pan if value.round().abs() < 0.5 => format!("C{suffix}"),
        ParameterKind::Pan => format!("{:.0}{}{suffix}", value.abs(), if value < 0.0 { "L" } else { "R" }),
        _ => format_value(spec, normalized),
    }
}

/// Formats band and header values using the same domains as their host metadata.
pub(super) fn format_value(spec: ParameterSpec, normalized: f64) -> String {
    let value = spec.normalized_to_plain(normalized);
    match spec.kind {
        ParameterKind::BandGain if value <= -60.0 => "-inf dB".into(),
        ParameterKind::BandGain | ParameterKind::OutputGain => {
            let value = if value.abs() < 0.05 { 0.0 } else { value };
            format!("{value:.1} dB")
        }
        ParameterKind::Width => format!("{value:.2}"),
        ParameterKind::Recovery => format!("{value:.0}%"),
        ParameterKind::Pan if value.round().abs() < 0.5 => "C".into(),
        ParameterKind::Pan => format!("{:.0}{}", value.abs(), if value < 0.0 { "L" } else { "R" }),
        ParameterKind::Crossover => format_frequency(value),
        ParameterKind::Switch => {
            if value >= 0.5 {
                "On".into()
            } else {
                "Off".into()
            }
        }
    }
}

/// Formats the visible frequency ranges and marker labels without redundant trailing precision.
pub(super) fn format_frequency(frequency: f64) -> String {
    if frequency.round() < 1000.0 {
        format!("{frequency:.0} Hz")
    } else if frequency < 10_000.0 {
        format!("{:.2} kHz", frequency * 0.001)
    } else {
        format!("{:.1} kHz", frequency * 0.001)
    }
}
