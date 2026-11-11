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
                .padding(plot_overlay::plot_padding(self.maximize_spectrogram))
                .width(Length::Fill)
                .height(Length::Fill),
            plot_overlay::PlotOverlay::new(self.crossovers(), plot_message)
                .maximized(self.maximize_spectrogram)
                .display(self.shared.display_preferences().mel_scale, self.parameters.snapshot())
                .interaction_epoch(self.parameters.with_editing(|editing, _| editing.interaction_epoch()))
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
                    text(format!("{:.0} FPS", self.refresh_rate))
                        .font(theme::MONO)
                        .size(11)
                        .color(theme::GREY_MIDDLE_LIGHT),
                )
                .align_right(Length::Fill)
                .padding(8.0),
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
        let ranges = self.band_frequency_ranges();
        let headings = layout::BandHeadings::new(sections.strip_width, &ranges);
        let sections = sections.with_band_headings(headings);
        let mut content = stack![
            at(self.header(size.width), layout::rect(0.0, 0.0, size.width, 36.0)),
            at(self.plot_section(sections.plot.width), sections.plot),
        ]
        .width(Length::Fill)
        .height(Length::Fill);
        let inline_toolbar = layout::Header::new(size.width).processing_toolbar;
        let toolbar_bounds = inline_toolbar.unwrap_or_else(|| layout::rect(8.0, 44.0, size.width - 16.0, 32.0));
        // Retaining this stack slot prevents a responsive toolbar move from replacing focused parameter trees.
        let toolbar = if inline_toolbar.is_some() || self.toolbar_shown {
            processing_toolbar(
                self.parameters
                    .with_editing(|editing, snapshot| editing.comparison(snapshot))
                    .active,
                self.preset_label(),
            )
        } else {
            Space::new().into()
        };
        content = content.push(at(toolbar, toolbar_bounds));
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
                    correlation_meter(std::sync::Arc::clone(&self.shared), band),
                    layout::rect(
                        x,
                        sections.correlation.y,
                        sections.strip_width,
                        sections.correlation.height,
                    ),
                ));
            }
            content = content.push(at(
                self.band_strip(band, sections, headings, ranges.get(band).expect("fixed band range")),
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
                control(
                    "Correlation",
                    sections.correlation.width,
                    Some(Message::Correlation),
                    false,
                    theme::BLUE,
                ),
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
                        self.maximize_spectrogram,
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
        ] {
            header = header.push(at(
                chrome_action(
                    Chrome::new(symbol, target.size(), message, false).id(format!("header-{id}")),
                    action,
                ),
                target,
            ));
        }
        // Keep the chevron slot even while inline, preserving the Output field's retained native focus.
        header = header.push(at(self.toolbar_disclosure(bounds), bounds.toolbar));
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

    /// Hides only the inline toolbar's disclosure, without changing the saved narrow-layout preference.
    fn toolbar_disclosure(&self, bounds: layout::Header) -> Element<'static, Message> {
        if bounds.processing_toolbar.is_some() {
            Space::new().into()
        } else {
            chrome_action(
                Chrome::new(
                    Symbol::Chevron(self.toolbar_shown),
                    bounds.toolbar.size(),
                    Some(Message::Toolbar),
                    false,
                )
                .id("header-processing-toolbar-disclosure"),
                "Processing toolbar",
            )
        }
    }

    /// Formats ordered crossover boundaries once for the shared four-band heading fit decision.
    fn band_frequency_ranges(&self) -> [String; 4] {
        let crossovers = self.crossovers();
        std::array::from_fn(|band| match band {
            0 => format!("Below {}", format_frequency(crossovers[0])),
            3 => format!("Above {}", format_frequency(crossovers[2])),
            _ => format!(
                "{} to {}",
                format_frequency(*crossovers.get(band - 1).expect("middle band lower edge")),
                format_frequency(*crossovers.get(band).expect("middle band upper edge"))
            ),
        })
    }

    /// Moves frequency annotations without changing child identities or replacing a focused parameter tree.
    fn band_strip(
        &self,
        band: usize,
        sections: layout::Sections,
        headings: layout::BandHeadings,
        range: &str,
    ) -> Element<'_, Message> {
        if self.compact_bands {
            return self.compact_band(band, sections.strip_width);
        }
        let solo = band_parameter_id(band, 5).expect("fixed band Solo id");
        let name = *layout::BAND_NAMES.get(band).expect("fixed band name");
        let title_width = theme::text_width(name, theme::SEMIBOLD, theme::BAND_TITLE_SIZE);
        let inner_width = layout::BAND_INSET.mul_add(-2.0, sections.strip_width);
        let action_width = layout::BAND_SOLO_SIZE + layout::BAND_HEADING_GAP + headings.link_width;
        let first_row_height = if headings.inline {
            layout::BAND_SOLO_SIZE
        } else {
            layout::BAND_HEADER_HEIGHT
        };
        let frequency_x = if headings.inline {
            title_width + layout::BAND_HEADING_GAP
        } else {
            0.0
        };
        let frequency_y = if headings.inline {
            0.0
        } else {
            layout::BAND_HEADER_HEIGHT
        };
        let frequency_width = if headings.inline {
            inner_width - frequency_x - action_width - layout::BAND_HEADING_GAP
        } else {
            inner_width
        };
        let actions = self.band_heading_actions(band, solo, headings.link_width);
        let heading = stack![
            at(
                text(name)
                    .font(theme::SEMIBOLD)
                    .size(theme::BAND_TITLE_SIZE)
                    .line_height(theme::LINE_HEIGHT)
                    .height(Length::Fill)
                    .align_y(iced::alignment::Vertical::Center)
                    .color(theme::GREY_SUPER_LIGHT),
                layout::rect(0.0, 0.0, title_width, first_row_height),
            ),
            at(
                text(range.to_owned())
                    .size(theme::LABEL_SIZE)
                    .line_height(theme::LINE_HEIGHT)
                    .wrapping(iced::advanced::text::Wrapping::None)
                    .height(Length::Fill)
                    .align_y(iced::alignment::Vertical::Center)
                    .color(theme::GREY_MIDDLE_LIGHT),
                layout::rect(frequency_x, frequency_y, frequency_width, first_row_height),
            ),
            at(
                actions,
                layout::rect(
                    inner_width - action_width,
                    (first_row_height - layout::BAND_SOLO_SIZE) * 0.5,
                    action_width,
                    layout::BAND_SOLO_SIZE,
                ),
            ),
        ]
        .width(Length::Fill)
        .height(headings.height());
        let mut controls = Column::new()
            .push(heading)
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

    /// Gives Solo a larger vector target and measures Link independently from the smaller frequency annotation.
    fn band_heading_actions(&self, band: usize, solo: u32, link_width: f32) -> Element<'_, Message> {
        Row::new()
            .push(chrome_action(
                Chrome::new(
                    Symbol::Headphones,
                    Size::new(layout::BAND_SOLO_SIZE, layout::BAND_SOLO_SIZE),
                    Some(Message::Toggle(solo)),
                    self.parameters.get(solo).is_some_and(|value| value >= 0.5),
                )
                .id(format!("band-{band}-solo")),
                "Solo band",
            ))
            .push(Space::new().width(layout::BAND_HEADING_GAP))
            .push(
                container(
                    checkbox(self.linked.get(band).copied().unwrap_or(false))
                        .label("Link")
                        .text_size(theme::BAND_LINK_SIZE)
                        .size(theme::CHECKBOX_SIZE)
                        .spacing(layout::BAND_HEADING_GAP)
                        .style(theme::check)
                        .on_toggle(move |value| Message::Link(band, value)),
                )
                .width(link_width),
            )
            .align_y(Alignment::Center)
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

#[cfg(test)]
mod responsive_tests {
    use std::cell::Cell;
    use std::rc::Rc;
    use std::sync::Arc;

    use iced::advanced::widget::{Tree, tree};

    use super::*;
    use crate::params::{EditorBinding, Parameters};

    /// Counts any accidental host edit while responsive view trees are reconciled.
    #[derive(Default)]
    struct Binding {
        edits: Cell<usize>,
    }

    impl EditorBinding for Binding {
        fn begin_edit(&self, _: u32) {
            self.edits.set(self.edits.get() + 1);
        }
        fn set_value(&self, _: u32, _: f64) {
            self.edits.set(self.edits.get() + 1);
        }
        fn end_edit(&self, _: u32) {
            self.edits.set(self.edits.get() + 1);
        }
    }

    type NativeInputState =
        iced::widget::text_input::State<<iced::Renderer as iced::advanced::text::Renderer>::Paragraph>;

    /// Focuses native fields directly, without a device or replacing production widget state.
    fn focus_native_inputs(tree: &mut Tree) -> usize {
        let own = usize::from(tree.tag == tree::Tag::of::<NativeInputState>());
        if own > 0 {
            tree.state.downcast_mut::<NativeInputState>().focus();
        }
        own + tree.children.iter_mut().map(focus_native_inputs).sum::<usize>()
    }

    /// Counts retained native focus after Iced reconciles a breakpoint-dependent production view.
    fn focused_native_inputs(tree: &Tree) -> usize {
        let own = usize::from(
            tree.tag == tree::Tag::of::<NativeInputState>()
                && tree.state.downcast_ref::<NativeInputState>().is_focused(),
        );
        own + tree.children.iter().map(focused_native_inputs).sum::<usize>()
    }

    #[test]
    fn responsive_header_retains_output_focus_and_the_saved_toolbar_preference() {
        let binding = Rc::new(Binding::default());
        let mut state = EditorState::for_host_test(
            Arc::new(Parameters::default()),
            image_control_dsp::Imager::new().shared(),
            binding.clone(),
        );
        state.toolbar_shown = false;
        let _ = state.update(Message::NumericDraft(GAIN_PARAMETER, "6 dB".into()));
        let mut tree = Tree::new(state.header(1000.0).as_widget());
        assert_eq!(focus_native_inputs(&mut tree), 1);
        for width in [1600.0, 760.0, 4096.0, 1000.0] {
            tree.diff(state.header(width).as_widget());
            assert_eq!(focused_native_inputs(&tree), 1, "header width {width}");
            assert!(!state.toolbar_shown);
        }
        let _ = state.update(Message::Resize);
        assert_eq!(state.numeric_draft.as_ref().expect("retained draft").text, "6 dB");
        assert_eq!(binding.edits.get(), 0);
    }

    #[test]
    fn responsive_band_heading_retains_native_fields_when_width_and_crossovers_change() {
        {
            let mut system = iced::advanced::graphics::text::font_system()
                .write()
                .expect("font system");
            for bytes in [theme::ROBOTO, theme::ROBOTO_SEMIBOLD] {
                system.load_font(bytes.into());
            }
        }
        let binding = Rc::new(Binding::default());
        let mut state = EditorState::for_host_test(
            Arc::new(Parameters::default()),
            image_control_dsp::Imager::new().shared(),
            binding.clone(),
        );
        let id = band_parameter_id(0, 0).expect("band Mid");
        let _ = state.update(Message::NumericDraft(id, "6 dB".into()));
        let ranges = state.band_frequency_ranges();
        let sections = state.sections(Size::new(1000.0, 764.0));
        let headings = layout::BandHeadings::new(sections.strip_width, &ranges);
        assert!(!headings.inline);
        let mut tree = Tree::new(
            state
                .band_strip(
                    0,
                    sections.with_band_headings(headings),
                    headings,
                    ranges.first().expect("first band range"),
                )
                .as_widget(),
        );
        assert_eq!(focus_native_inputs(&mut tree), 5);
        for width in [1600.0, 760.0, 4096.0, 1000.0] {
            let sections = state.sections(Size::new(width, 764.0));
            let headings = layout::BandHeadings::new(sections.strip_width, &ranges);
            tree.diff(
                state
                    .band_strip(
                        0,
                        sections.with_band_headings(headings),
                        headings,
                        ranges.first().expect("first band range"),
                    )
                    .as_widget(),
            );
            assert_eq!(focused_native_inputs(&tree), 5, "band width {width}");
        }
        let threshold = (760..=4096)
            .find(|width| {
                let sections = state.sections(Size::new(*width as f32, 764.0));
                layout::BandHeadings::new(sections.strip_width, &ranges).inline
            })
            .expect("headings fit a supported width");
        let sections = state.sections(Size::new(threshold as f32, 764.0));
        let headings = layout::BandHeadings::new(sections.strip_width, &ranges);
        assert!(headings.inline);
        tree.diff(
            state
                .band_strip(
                    0,
                    sections.with_band_headings(headings),
                    headings,
                    ranges.first().expect("first band range"),
                )
                .as_widget(),
        );
        for (id, frequency) in crate::params::CROSSOVER_PARAMETERS
            .into_iter()
            .zip([1000.0, 4000.0, 15_000.0])
        {
            let spec = parameter_spec(id).expect("crossover");
            state.parameters.set(id, spec.plain_to_normalized(frequency));
        }
        let ranges = state.band_frequency_ranges();
        let sections = state.sections(Size::new(threshold as f32, 764.0));
        let headings = layout::BandHeadings::new(sections.strip_width, &ranges);
        assert!(!headings.inline);
        tree.diff(
            state
                .band_strip(
                    0,
                    sections.with_band_headings(headings),
                    headings,
                    ranges.first().expect("first band range"),
                )
                .as_widget(),
        );
        assert_eq!(focused_native_inputs(&tree), 5);
        let _ = state.update(Message::Refresh);
        assert_eq!(state.numeric_draft.as_ref().expect("retained draft").text, "6 dB");
        assert_eq!(binding.edits.get(), 0);
    }
}
