//! Retained editor composition, contextual disclosures, and parameter presentation.

use iced::widget::{Column, Row, Space, Stack, checkbox, container, mouse_area, stack, text, tooltip};
use iced::{Alignment, Element, Length, Size};
use iced_baseview as iced;
use image_control_dsp::AnalysisTap;

use super::chrome::{Chrome, Symbol};
use super::controls::{
    analysis_status_text, at, compact_text_width, control, control_button, correlation_meter, format_compact_value,
    format_frequency, format_value, parameter_input, processing_toolbar, settings,
};
use super::{EditorState, Message, layout, numeric_input, plot_message, plot_overlay, theme};
use crate::params::{
    BYPASS_PARAMETER, DELTA_PARAMETER, GAIN_PARAMETER, MONO_PARAMETER, band_parameter_id, parameter_spec,
};
use crate::spectrogram;

impl EditorState {
    /// Composes the shader and plot guides without creating a second analysis image.
    /// Opening settings leaves the spectrum geometry unchanged.
    fn plot(&self) -> Element<'_, Message> {
        let plot_content = self.spectrogram.as_ref().map_or_else(
            |_| {
                container(
                    text("Spatial analysis unavailable")
                        .size(theme::LABEL_SIZE)
                        .line_height(theme::LINE_HEIGHT)
                        .color(theme::GREY_MIDDLE_LIGHT),
                )
                .center(Length::Fill)
                .into()
            },
            spectrogram::Spectrogram::view::<Message>,
        );
        let mut plot = stack![
            container(plot_content)
                .padding(iced::Padding {
                    top: layout::PLOT_INSETS[0],
                    right: layout::PLOT_INSETS[1],
                    bottom: layout::PLOT_INSETS[2],
                    left: layout::PLOT_INSETS[3],
                })
                .width(Length::Fill)
                .height(Length::Fill),
            plot_overlay::PlotOverlay::new(self.crossovers(), plot_message)
                .display(self.shared.display_preferences().mel_scale, self.parameters.snapshot())
                .interaction_epoch(self.parameters.restore_generation())
                .enabled(!self.settings_shown && !self.presets.shown)
                .position_curve(
                    f64::from(self.shared.display_preferences().pan_midpoint_db),
                    f64::from(self.shared.display_preferences().pan_edge_db)
                ),
        ];
        if let Ok(spectrogram) = &self.spectrogram
            && let Some(status) = analysis_status_text(spectrogram.status())
        {
            plot = plot.push(container(text(status).size(13).color(theme::GREY_MIDDLE_LIGHT)).center(Length::Fill));
        }
        if self.shared.display_preferences().show_fps {
            plot = plot.push(
                container(
                    text(format!("{:.0} fps", self.refresh_rate))
                        .size(11)
                        .color(theme::GREY_MIDDLE_LIGHT),
                )
                .padding(16.0),
            );
        }
        container(plot)
            .width(Length::Fill)
            .height(Length::Fill)
            .style(theme::panel)
            .into()
    }

    /// Positions retained controls with the reference section bounds.
    /// The settings callout is modal and never reallocates the plot.
    pub fn view(&self) -> Element<'_, Message> {
        let size = self.size();
        let sections = self.sections(size);
        let mut content = stack![
            at(self.header(size.width), layout::rect(0.0, 0.0, size.width, 36.0)),
            at(self.plot_section(sections.plot.width), sections.plot),
        ]
        .width(Length::Fill)
        .height(Length::Fill);
        if self.toolbar_shown {
            content = content.push(at(
                processing_toolbar(
                    self.parameters
                        .with_editing(|editing, snapshot| editing.comparison(snapshot))
                        .active,
                    self.preset_label(),
                ),
                layout::rect(8.0, 44.0, size.width - 16.0, 32.0),
            ));
        }
        if !self.compact_bands && !self.spectrogram_hidden {
            content = content.push(at(
                Chrome::new(Symbol::Divider, sections.divider.size(), Some(Message::Ignore), false),
                sections.divider,
            ));
        }
        for band in 0..4 {
            let x = (band as f32).mul_add(sections.strip_width + layout::GAP, sections.bands.x);
            if self.correlation_shown {
                content = content.push(at(
                    correlation_meter(
                        &self.correlation_history.get(band).expect("fixed band").snapshot(),
                        self.shared.display_preferences().smooth_correlation,
                    ),
                    layout::rect(
                        x,
                        sections.correlation.y,
                        sections.strip_width,
                        sections.correlation.height,
                    ),
                ));
            }
            content = content.push(at(
                self.band_strip(band, sections),
                layout::rect(x, sections.bands.y, sections.strip_width, sections.band),
            ));
        }
        self.with_settings(self.with_disclosures(content, size, sections).into(), size)
    }

    /// Retains the spectrogram's available, hidden, and stopped presentations in one layout slot.
    fn plot_section(&self, width: f32) -> Element<'_, Message> {
        if self.spectrogram_hidden {
            control("Spectrogram", width, Some(Message::HideSpectrogram), false, theme::BLUE)
        } else if self.analysis_stopped {
            container(
                text("Spectrogram analysis stopped")
                    .size(13)
                    .color(theme::GREY_MIDDLE_LIGHT),
            )
            .center(Length::Fill)
            .into()
        } else {
            self.plot()
        }
    }

    /// Gives each repeated section disclosure a stable focus identity and named action.
    fn with_disclosures<'a>(
        &self,
        mut content: Stack<'a, Message>,
        size: Size,
        sections: layout::Sections,
    ) -> Stack<'a, Message> {
        if !self.correlation_shown {
            content = content.push(at(
                text("Correlation").size(11).color(theme::GREY_MIDDLE_LIGHT),
                sections.correlation,
            ));
        }
        let correlation_action = layout::rect((size.width - 72.0) * 0.5, sections.correlation.y - 6.0, 72.0, 20.0);
        content = content.push(at(
            chrome_action(
                Chrome::new(
                    Symbol::Disclosure(self.correlation_shown),
                    correlation_action.size(),
                    Some(Message::Correlation),
                    false,
                )
                .id("correlation-disclosure"),
                "Show or hide correlation",
            ),
            correlation_action,
        ));
        let band_action = layout::rect(
            (size.width - 48.0) * 0.5,
            sections.bands.y - if self.compact_bands { 12.0 } else { 6.0 },
            48.0,
            12.0,
        );
        content = content.push(at(
            chrome_action(
                Chrome::new(
                    Symbol::Disclosure(!self.compact_bands),
                    band_action.size(),
                    Some(Message::CompactBands),
                    false,
                )
                .id("band-controls-disclosure"),
                "Expand or compact band controls",
            ),
            band_action,
        ));
        let grip = layout::rect(size.width - 18.0, size.height - 18.0, 18.0, 18.0);
        content.push(at(
            Chrome::new(Symbol::Resize, grip.size(), Some(Message::Ignore), false),
            grip,
        ))
    }

    /// Blocks underlying controls while retaining the release-driven Settings opener as the one exception.
    fn with_settings<'a>(&'a self, content: Element<'a, Message>, size: Size) -> Element<'a, Message> {
        let mut content = stack![super::controls::interaction::modal_background(
            content,
            self.settings_shown || self.presets.shown
        )]
        .width(Length::Fill)
        .height(Length::Fill);
        if self.settings_shown {
            let header = layout::Header::new(size.width);
            let callout = layout::rect(
                (header.settings.center_x() - 195.0).max(8.0),
                44.0,
                390.0,
                (size.height - 52.0).min(734.0),
            );
            content = content.push(
                mouse_area(container(Space::new()).width(Length::Fill).height(Length::Fill))
                    .on_press(Message::CloseSettings),
            );
            content = content.push(at(
                mouse_area(if self.shortcuts_shown {
                    super::controls::shortcuts()
                } else {
                    settings(
                        self.shared.display_preferences(),
                        self.correlation_shown,
                        self.spectrogram_hidden,
                        self.analysis_stopped,
                    )
                })
                .on_press(Message::Ignore),
                callout,
            ));
            content = content.push(at(
                chrome_action(
                    Chrome::new(Symbol::Settings, header.settings.size(), Some(Message::Settings), true)
                        .id("header-settings"),
                    "Display settings",
                ),
                header.settings,
            ));
        }
        if self.presets.shown {
            content = content.push(
                mouse_area(container(Space::new()).width(Length::Fill).height(Length::Fill))
                    .on_press(Message::CancelPreset),
            );
            content = content.push(at(
                super::controls::preset_panel(&self.presets),
                layout::rect(
                    (size.width - 430.0).max(0.0) * 0.5,
                    84.0,
                    430.0,
                    (size.height - 100.0).max(100.0),
                ),
            ));
        }
        container(content)
            .width(Length::Fill)
            .height(Length::Fill)
            .style(theme::background)
            .into()
    }

    /// Preserves the header's exact left tools and right-anchored Mono/Delta/Bypass/Output ordering.
    fn header(&self, width: f32) -> Element<'_, Message> {
        let bounds = layout::Header::new(width);
        let mut header = stack![at(
            container(
                text("ImageControl")
                    .font(theme::BOLD)
                    .size(theme::TITLE_SIZE)
                    .line_height(theme::LINE_HEIGHT)
                    .color(theme::GREY_SUPER_LIGHT)
            )
            .center_y(Length::Fill),
            bounds.title
        )]
        .width(Length::Fill)
        .height(36.0);
        for (symbol, target, message, id, action) in [
            (
                Symbol::Settings,
                bounds.settings,
                Some(Message::Settings),
                "settings",
                "Display settings",
            ),
            (
                Symbol::Undo,
                bounds.undo,
                self.parameters
                    .with_editing(|editing, _| editing.can_undo())
                    .then_some(Message::Undo),
                "undo",
                "Undo",
            ),
            (
                Symbol::Redo,
                bounds.redo,
                self.parameters
                    .with_editing(|editing, _| editing.can_redo())
                    .then_some(Message::Redo),
                "redo",
                "Redo",
            ),
            (
                Symbol::Chevron(self.toolbar_shown),
                bounds.toolbar,
                Some(Message::Toolbar),
                "processing-toolbar-disclosure",
                "Processing toolbar",
            ),
        ] {
            header = header.push(at(
                chrome_action(
                    Chrome::new(symbol, target.size(), message, false).id(format!("header-{id}")),
                    action,
                ),
                target,
            ));
        }
        for (label, target, tap) in [
            ("Pre", bounds.pre, AnalysisTap::Pre),
            ("Post", bounds.post, AnalysisTap::Post),
        ] {
            header = header.push(at(
                control_button(
                    label,
                    target.width,
                    Some(Message::Tap(tap)),
                    self.shared.analysis_tap() == tap,
                    theme::BLUE,
                )
                .style(move |_, status| {
                    let mut style = theme::control(status, self.shared.analysis_tap() == tap, theme::BLUE);
                    style.border.radius = if tap == AnalysisTap::Pre {
                        iced::border::left(theme::CONTROL_RADIUS)
                    } else {
                        iced::border::right(theme::CONTROL_RADIUS)
                    };
                    style
                }),
                layout::rect(target.x, target.y + 2.0, target.width, 24.0),
            ));
        }
        for (label, id, target, accent) in [
            ("Mono", MONO_PARAMETER, bounds.mono, theme::BLUE),
            ("Delta", DELTA_PARAMETER, bounds.delta, theme::YELLOW),
            ("Bypass", BYPASS_PARAMETER, bounds.bypass, theme::ORANGE),
        ] {
            header = header.push(at(
                self.toggle(label, id, target.width, accent),
                layout::rect(target.x, target.y + 2.0, target.width, 24.0),
            ));
        }
        header = header.push(at(
            container(text("Output").size(theme::LABEL_SIZE).line_height(theme::LINE_HEIGHT)).center_y(Length::Fill),
            bounds.output_label,
        ));
        header
            .push(at(self.parameter_slider(GAIN_PARAMETER, 70.0), bounds.output))
            .into()
    }

    /// Builds one quiet band surface with Solo/Link in its heading, followed by five stacked labelled rows.
    fn band_strip(&self, band: usize, sections: layout::Sections) -> Element<'_, Message> {
        if self.compact_bands {
            return self.compact_band(band, sections.strip_width);
        }
        let solo = band_parameter_id(band, 5).expect("fixed band Solo id");
        let heading = Row::new()
            .push(
                text(*layout::BAND_NAMES.get(band).expect("fixed band name"))
                    .font(theme::SEMIBOLD)
                    .size(theme::BAND_TITLE_SIZE)
                    .line_height(theme::LINE_HEIGHT)
                    .color(theme::GREY_SUPER_LIGHT),
            )
            .push(Space::new().width(Length::Fill))
            .push(chrome_action(
                Chrome::new(
                    Symbol::Headphones,
                    Size::new(20.0, 20.0),
                    Some(Message::Toggle(solo)),
                    self.parameters.get(solo).is_some_and(|value| value >= 0.5),
                )
                .id(format!("band-{band}-solo")),
                "Solo band",
            ))
            .push(Space::new().width(6.0))
            .push(
                container(
                    checkbox(self.linked.get(band).copied().unwrap_or(false))
                        .label("Link")
                        .text_size(theme::LABEL_SIZE)
                        .size(theme::CHECKBOX_SIZE)
                        .spacing(6.0)
                        .style(theme::check)
                        .on_toggle(move |value| Message::Link(band, value)),
                )
                .width(60.0),
            )
            .height(layout::BAND_HEADER_HEIGHT)
            .align_y(Alignment::Center);
        let crossovers = self.crossovers();
        let range = match band {
            0 => format!("Below {}", format_frequency(crossovers[0])),
            3 => format!("Above {}", format_frequency(crossovers[2])),
            _ => format!(
                "{} to {}",
                format_frequency(*crossovers.get(band - 1).expect("middle band lower edge")),
                format_frequency(*crossovers.get(band).expect("middle band upper edge"))
            ),
        };
        let mut controls = Column::new()
            .push(heading)
            .push(
                text(range)
                    .size(theme::LABEL_SIZE)
                    .line_height(theme::LINE_HEIGHT)
                    .color(theme::GREY_MIDDLE_LIGHT)
                    .height(layout::BAND_HEADER_HEIGHT)
                    .align_y(iced::alignment::Vertical::Center),
            )
            .push(Space::new().height(sections.band_header_gap));
        for (field, label) in layout::FIELD_NAMES.into_iter().enumerate() {
            let id = band_parameter_id(band, field).expect("fixed band field id");
            controls = controls.push(
                Column::new()
                    .push(
                        text(label)
                            .size(theme::LABEL_SIZE)
                            .line_height(theme::LINE_HEIGHT)
                            .height(layout::SLIDER_LABEL_HEIGHT)
                            .align_y(iced::alignment::Vertical::Bottom)
                            .color(theme::GREY_LIGHT),
                    )
                    .push(self.parameter_slider(id, layout::VALUE_WIDTH))
                    .height(sections.band_row),
            );
        }
        container(controls)
            .padding(layout::BAND_INSET)
            .style(theme::panel)
            .height(sections.band)
            .width(Length::FillPortion(1))
            .into()
    }

    /// Compact disclosure retains only the C++ Mid/Side/Width/Pan values, never Recover, heading, Solo, or Link.
    fn compact_band(&self, band: usize, strip_width: f32) -> Element<'_, Message> {
        let fields = [
            (0, "M", " MID", 49.0),
            (1, "S", " SIDE", 49.0),
            (2, "W", " WIDTH", 40.0),
            (4, "", " PAN", 32.0),
        ];
        let available = strip_width - 10.0;
        let name_widths = [
            compact_text_width("-60.0 MID"),
            compact_text_width("-60.0 SIDE"),
            compact_text_width("2.00 WIDTH"),
            compact_text_width("100L PAN"),
        ];
        let total: f32 = name_widths.iter().sum();
        let full_names = available >= total;
        let mut used = 0.0;
        let mut values = Row::new().spacing(2.0);
        for (index, (field, identity, full_name, weight)) in fields.into_iter().enumerate() {
            let suffix = if full_names { full_name } else { identity };
            let measured = *name_widths.get(index).expect("fixed compact field");
            let width = if index == 3 {
                available - used
            } else if full_names {
                measured + ((available - total) * measured / total).floor()
            } else {
                (available * weight / 170.0).floor()
            };
            used += width;
            let id = band_parameter_id(band, field).expect("compact band field");
            let spec = parameter_spec(id).expect("compact band field");
            let value = self.parameters.get(id).unwrap_or(spec.default_normalized);
            let label = format_compact_value(spec, value, suffix);
            let draft = self.numeric_draft.as_ref().filter(|draft| draft.id == id);
            let field: Element<'_, Message> = draft.map_or_else(
                || {
                    container(stack![
                        parameter_input(id, value).style(theme::compact_slider).on_numeric(id),
                        mouse_area(
                            container(
                                text(label)
                                    .font(theme::MONO)
                                    .size(11)
                                    .align_x(iced::alignment::Horizontal::Center)
                            )
                            .center(Length::Fill)
                        )
                        .on_double_click(Message::NumericFocus(id)),
                    ])
                    .width(width)
                    .height(24.0)
                    .into()
                },
                |draft| {
                    numeric_input::input(
                        id,
                        &draft.text,
                        width,
                        24.0,
                        draft.invalid,
                        true,
                        self.settings_shown || self.presets.shown,
                    )
                },
            );
            values = values.push(field);
        }
        container(values)
            .padding(2.0)
            .style(theme::panel)
            .height(28.0)
            .width(strip_width)
            .into()
    }

    /// Uses normalized sliders while the registry alone owns plain-unit conversion and defaults.
    fn parameter_slider(&self, id: u32, value_width: f32) -> Element<'_, Message> {
        let height = if id == GAIN_PARAMETER {
            28.0
        } else {
            layout::SLIDER_HEIGHT
        };
        let value_height = if id == GAIN_PARAMETER { 20.0 } else { 22.0 };
        let spec = parameter_spec(id).expect("visible control has registry metadata");
        let value = self.parameters.get(id).unwrap_or(spec.default_normalized);
        let slider = container(parameter_input(id, value).height(height))
            .padding([0.0, 6.0])
            .width(Length::Fill);
        Row::new()
            .push(slider)
            .push({
                let formatted = format_value(spec, value);
                let draft = self.numeric_draft.as_ref().filter(|draft| draft.id == id);
                numeric_input::input(
                    id,
                    draft.map_or(formatted.as_str(), |draft| draft.text.as_str()),
                    value_width,
                    value_height,
                    draft.is_some_and(|draft| draft.invalid),
                    draft.is_some(),
                    self.settings_shown || self.presets.shown,
                )
            })
            .height(height)
            .align_y(Alignment::Center)
            .into()
    }

    /// Retains native button order and selection colors while sending one complete host gesture per click.
    fn toggle<'a>(&self, label: &'a str, id: u32, width: f32, accent: iced::Color) -> Element<'a, Message> {
        control(
            label,
            width,
            Some(Message::Toggle(id)),
            self.parameters.get(id).is_some_and(|value| value >= 0.5),
            accent,
        )
    }
}

/// Names custom vector actions without changing their retained focus or pointer behavior.
fn chrome_action(chrome: Chrome, action: &'static str) -> Element<'static, Message> {
    tooltip(chrome, text(action).size(theme::LABEL_SIZE), tooltip::Position::Bottom).into()
}
