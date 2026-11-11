//! Retained Iced editor, balanced host gestures, and the compact `ImageControl` processing layout.

use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant};

use iced::widget::{
    Column, Row, Space, button, checkbox, container, mouse_area, pick_list, pin, scrollable, slider, stack, text,
};
use iced::{Alignment, Element, Length, Point, Rectangle, Size, Subscription, Task, Theme};

#[path = "editor_chrome.rs"]
mod chrome;
use chrome::{Chrome, Symbol};
use iced_baseview as iced;
use image_control_dsp::{AnalysisStatus, AnalysisTap, ColourMode, CorrelationSnapshot, SharedState};

use crate::params::{
    BYPASS_PARAMETER, CROSSOVER_PARAMETERS, DELTA_PARAMETER, GAIN_PARAMETER, MONO_PARAMETER, ParameterKind,
    ParameterSpec, Parameters, band_parameter_id, parameter_spec,
};
use crate::spectrogram;

#[path = "layout.rs"]
mod layout;
#[path = "plot_overlay.rs"]
mod plot_overlay;
#[path = "theme.rs"]
mod theme;

/// Initial window size, shared with the native bridge.
pub const EDITOR_SIZE: (f32, f32) = layout::DEFAULT_SIZE;
/// Minimum dimensions shared with the host's resize constraints.
pub const MIN_EDITOR_SIZE: (f32, f32) = layout::MIN_SIZE;
/// Wheel edits have no release event, so their host gesture closes after a bounded idle period.
const GESTURE_IDLE: Duration = Duration::from_millis(200);

pub use crate::params::EditorBinding;

/// Messages carry stable parameter ids instead of borrowed widget or host objects.
#[derive(Clone, Copy, Debug)]
pub enum Message {
    Change(u32, f64),
    Begin(u32),
    EndGesture,
    Toggle(u32),
    Link(usize, bool),
    Toolbar,
    Correlation,
    Settings,
    Tap(AnalysisTap),
    Persistence(f32),
    Contrast(f32),
    Colour(ColourMode),
    Refresh,
    Resize,
    Hold,
    DividerStart(Point),
    ResizeStart(Point),
    Pointer(Point),
    CompactBands,
    CloseSettings,
    ResetAppearance,
    Ignore,
}

/// Captured drag origin is independent of host-accepted resize updates during raw pointer motion.
struct GeometryDrag {
    pointer: Point,
    size: Size,
}

/// Captured linked values preserve source-trim offsets and balanced automation when Link changes mid-edit.
struct Gesture {
    id: u32,
    linked: Option<(u32, f64, f64)>,
    last_change: Instant,
}

/// Native window construction retains immutable inputs, with all interactive state created at boot.
struct EditorProgram {
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    binding: Rc<dyn EditorBinding>,
    window_size: iced::shell::SharedWindowSize,
    notifier: Option<iced::PollSubNotifier>,
    analysis: Rc<std::cell::RefCell<Option<image_control_dsp::AnalysisShutdown>>>,
}

/// Window-owned state keeps parameter atomics authoritative and never holds mutable audio-processing state.
pub struct EditorState {
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    settings_shown: bool,
    correlation: CorrelationObservation,
    binding: Rc<dyn EditorBinding>,
    spectrogram: Result<spectrogram::Spectrogram, std::io::Error>,
    window_size: Option<iced::shell::SharedWindowSize>,
    notifier: Option<iced::PollSubNotifier>,
    gesture: Option<Gesture>,
    linked: [bool; 4],
    toolbar_shown: bool,
    correlation_shown: bool,
    input_held: bool,
    band_height: f32,
    divider_drag: Option<GeometryDrag>,
    resize_drag: Option<GeometryDrag>,
    compact_bands: bool,
}

impl iced_program::Program for EditorProgram {
    type State = EditorState;
    type Message = Message;
    type Theme = Theme;
    type Renderer = iced::Renderer;
    type Executor = crate::iced_executor::Executor;

    fn name() -> &'static str {
        "image_control"
    }

    fn settings(&self) -> iced::Settings {
        iced::Settings {
            fonts: vec![theme::ROBOTO.into(), theme::ROBOTO_MONO.into()],
            default_font: theme::TEXT,
            default_text_size: 14.0.into(),
            vsync: false,
            ..iced::Settings::default()
        }
    }

    fn window(&self) -> Option<iced::window::Settings> {
        Some(iced::window::Settings {
            size: iced::Size::new(EDITOR_SIZE.0, EDITOR_SIZE.1),
            min_size: Some(iced::Size::new(MIN_EDITOR_SIZE.0, MIN_EDITOR_SIZE.1)),
            ..iced::window::Settings::default()
        })
    }

    fn boot(&self) -> (EditorState, Task<Message>) {
        if let Some(notifier) = &self.notifier {
            notifier.notify();
        }
        self.shared.set_correlation_enabled(true);
        let spectrogram = spectrogram::Spectrogram::new(Arc::clone(&self.shared));
        if let Ok(spectrogram) = &spectrogram {
            *self.analysis.borrow_mut() = Some(spectrogram.shutdown_handle());
        }
        (
            EditorState {
                parameters: Arc::clone(&self.parameters),
                shared: Arc::clone(&self.shared),
                settings_shown: false,
                correlation: CorrelationObservation::default(),
                binding: Rc::clone(&self.binding),
                spectrogram,
                window_size: Some(self.window_size.clone()),
                notifier: self.notifier.clone(),
                gesture: None,
                linked: [false; 4],
                toolbar_shown: true,
                correlation_shown: true,
                input_held: false,
                band_height: layout::BAND_HEIGHT,
                divider_drag: None,
                resize_drag: None,
                compact_bands: false,
            },
            Task::none(),
        )
    }

    fn update(&self, state: &mut EditorState, message: Message) -> Task<Message> {
        state.update(message)
    }

    fn view<'a>(&self, state: &'a EditorState, _: iced::window::Id) -> Element<'a, Message> {
        state.view()
    }

    fn subscription(&self, _: &EditorState) -> Subscription<Message> {
        EditorState::subscription()
    }

    fn theme(&self, _: &EditorState, _: iced::window::Id) -> Option<Theme> {
        Some(Theme::Dark)
    }

    fn title(&self, _: &EditorState, _: iced::window::Id) -> String {
        "ImageControl".into()
    }
}

impl EditorState {
    /// Applies only local user messages, leaving host automation as atomics read directly by each view.
    pub fn update(&mut self, message: Message) -> Task<Message> {
        match message {
            Message::Begin(id) => {
                self.input_held = true;
                self.begin_gesture(id);
            }
            Message::Hold => self.input_held = true,
            Message::DividerStart(pointer) => self.start_divider(pointer),
            Message::ResizeStart(pointer) => self.start_resize(pointer),
            Message::Pointer(pointer) => self.move_pointer(pointer),
            Message::Change(id, value) => self.change(id, value),
            Message::EndGesture => {
                self.divider_drag = None;
                self.resize_drag = None;
                self.input_held = false;
                self.end_gesture();
            }
            Message::Resize => {
                self.input_held = false;
                self.end_gesture();
            }
            Message::CompactBands => {
                self.end_gesture();
                self.divider_drag = None;
                self.compact_bands = !self.compact_bands;
            }
            Message::CloseSettings => self.settings_shown = false,
            Message::ResetAppearance => {
                self.shared
                    .set_display_preferences(image_control_dsp::DisplayPreferences::default());
            }
            Message::Ignore => {}
            Message::Toggle(id) => {
                self.end_gesture();
                if let Some(value) = self.parameters.get(id) {
                    self.binding.begin_edit(id);
                    self.write(id, f64::from(value < 0.5));
                    self.binding.end_edit(id);
                }
            }
            Message::Link(band, linked) => {
                if let Some(value) = self.linked.get_mut(band) {
                    *value = linked;
                }
            }
            Message::Toolbar => {
                self.end_gesture();
                self.toolbar_shown = !self.toolbar_shown;
            }
            Message::Correlation => {
                self.correlation_shown = !self.correlation_shown;
                self.shared.set_correlation_enabled(self.correlation_shown);
                self.correlation = CorrelationObservation::default();
            }
            Message::Settings => self.settings_shown = !self.settings_shown,
            Message::Tap(tap) => self.shared.set_analysis_tap(tap),
            Message::Persistence(persistence_seconds) => {
                let mut display = self.shared.display_preferences();
                display.persistence_seconds = persistence_seconds;
                self.shared.set_display_preferences(display);
            }
            Message::Contrast(contrast) => {
                let mut display = self.shared.display_preferences();
                display.contrast = contrast;
                self.shared.set_display_preferences(display);
            }
            Message::Colour(color_mode) => {
                let mut display = self.shared.display_preferences();
                display.color_mode = color_mode;
                self.shared.set_display_preferences(display);
            }
            Message::Refresh => {
                if self.correlation_shown {
                    self.correlation
                        .observe(self.shared.correlation_snapshot(), Instant::now());
                }
                if let Ok(spectrogram) = &mut self.spectrogram {
                    spectrogram.tick();
                }
                if !self.input_held
                    && self
                        .gesture
                        .as_ref()
                        .is_some_and(|gesture| gesture.last_change.elapsed() > GESTURE_IDLE)
                {
                    self.end_gesture();
                }
                if let Some(notifier) = &self.notifier {
                    notifier.notify();
                }
            }
        }
        Task::none()
    }

    /// Subscribes to the bridge's timer and raw releases, including events already handled by a child widget.
    pub fn subscription() -> Subscription<Message> {
        Subscription::batch([
            iced::poll_events().map(|()| Message::Refresh),
            iced::window_resized().map(|()| Message::Resize),
            iced::event::listen_raw(|event, _, _| release_message(&event)),
        ])
    }

    fn start_divider(&mut self, pointer: Point) {
        self.end_gesture();
        let size = self.size();
        self.band_height = self.sections(size).band;
        self.divider_drag = Some(GeometryDrag {
            pointer,
            size: Size::new(size.width, self.band_height),
        });
    }

    fn start_resize(&mut self, pointer: Point) {
        self.end_gesture();
        self.resize_drag = Some(GeometryDrag {
            pointer,
            size: self.size(),
        });
    }

    /// Continues captured drags outside their widget bounds. Host acceptance remains authoritative.
    fn move_pointer(&mut self, pointer: Point) {
        if let Some(drag) = &self.divider_drag {
            self.band_height = (drag.size.height + drag.pointer.y - pointer.y).max(layout::MIN_BAND_HEIGHT);
        }
        if let Some(drag) = &self.resize_drag {
            let width = (drag.size.width + pointer.x - drag.pointer.x)
                .round()
                .clamp(layout::MIN_SIZE.0, layout::MAX_SIZE.0);
            let height = (drag.size.height + pointer.y - drag.pointer.y)
                .round()
                .clamp(layout::MIN_SIZE.1, layout::MAX_SIZE.1);
            self.binding.request_resize(width as u32, height as u32);
        }
    }

    /// Uses host-accepted logical dimensions, not a locally optimistic resize mirror.
    fn size(&self) -> Size {
        self.window_size
            .as_ref()
            .map_or(Size::new(EDITOR_SIZE.0, EDITOR_SIZE.1), |window| {
                let logical = window.get().logical;
                Size::new(logical.width as f32, logical.height as f32)
            })
    }

    fn sections(&self, size: Size) -> layout::Sections {
        layout::Sections::new(
            size,
            self.toolbar_shown,
            self.correlation_shown,
            self.band_height,
            self.compact_bands,
        )
    }

    /// Composes the shader and plot guides without creating a second analysis image.
    /// Opening settings leaves the spectrum geometry unchanged.
    fn plot(&self) -> Element<'_, Message> {
        let plot_content = self.spectrogram.as_ref().map_or_else(
            |_| {
                container(
                    text("Spatial analysis unavailable")
                        .size(14)
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
            plot_overlay::PlotOverlay::new(self.crossovers(), plot_message),
        ];
        if let Ok(spectrogram) = &self.spectrogram
            && let Some(status) = analysis_status_text(spectrogram.status())
        {
            plot = plot.push(container(text(status).size(13).color(theme::GREY_MIDDLE_LIGHT)).center(Length::Fill));
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
            at(self.plot(), sections.plot),
        ]
        .width(Length::Fill)
        .height(Length::Fill);
        if self.toolbar_shown {
            content = content.push(at(
                processing_toolbar(),
                layout::rect(8.0, 44.0, size.width - 16.0, 32.0),
            ));
        }
        if !self.compact_bands {
            content = content.push(at(
                Chrome::new(Symbol::Divider, sections.divider.size(), Some(Message::Ignore), false),
                sections.divider,
            ));
        }
        let readings = self.correlation.readings(
            Instant::now(),
            Duration::from_secs_f32(self.shared.correlation_expiry_seconds()),
        );
        for band in 0..4 {
            let x = (band as f32).mul_add(sections.strip_width + layout::GAP, sections.bands.x);
            if self.correlation_shown {
                content = content.push(at(
                    correlation_meter(*readings.get(band).expect("fixed band")),
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
        if !self.correlation_shown {
            content = content.push(at(
                text("Correlation").size(11).color(theme::GREY_MIDDLE_LIGHT),
                sections.correlation,
            ));
        }
        let correlation_action = layout::rect((size.width - 72.0) * 0.5, sections.correlation.y - 6.0, 72.0, 20.0);
        content = content.push(at(
            Chrome::new(
                Symbol::Disclosure(self.correlation_shown),
                correlation_action.size(),
                Some(Message::Correlation),
                false,
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
            Chrome::new(
                Symbol::Disclosure(!self.compact_bands),
                band_action.size(),
                Some(Message::CompactBands),
                false,
            ),
            band_action,
        ));
        let grip = layout::rect(size.width - 18.0, size.height - 18.0, 18.0, 18.0);
        content = content.push(at(
            Chrome::new(Symbol::Resize, grip.size(), Some(Message::Ignore), false),
            grip,
        ));
        self.with_settings(content.into(), size)
    }

    /// Blocks underlying controls while retaining the release-driven Settings opener as the one exception.
    fn with_settings<'a>(&'a self, content: Element<'a, Message>, size: Size) -> Element<'a, Message> {
        let mut content = stack![content].width(Length::Fill).height(Length::Fill);
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
            content = content.push(at(mouse_area(self.settings()).on_press(Message::Ignore), callout));
            content = content.push(at(
                Chrome::new(Symbol::Settings, header.settings.size(), Some(Message::Settings), true),
                header.settings,
            ));
        }
        container(content)
            .width(Length::Fill)
            .height(Length::Fill)
            .style(theme::background)
            .into()
    }

    /// Captures linked source trims once, so incoming automation never recursively drives Link.
    fn begin_gesture(&mut self, id: u32) {
        if self.gesture.as_ref().is_some_and(|gesture| gesture.id == id) {
            return;
        }
        self.end_gesture();
        if parameter_spec(id).is_none() {
            return;
        }
        let linked = (0..4).find_map(|band| {
            if !self.linked.get(band).copied().unwrap_or(false) || band_parameter_id(band, 0) != Some(id) {
                return None;
            }
            let side = band_parameter_id(band, 1)?;
            Some((side, self.plain(id), self.plain(side)))
        });
        self.binding.begin_edit(id);
        if let Some((side, _, _)) = linked {
            self.binding.begin_edit(side);
        }
        self.gesture = Some(Gesture {
            id,
            linked,
            last_change: Instant::now(),
        });
    }

    /// Writes finite host-normalized values and moves linked Side by the same plain dB amount as Mid.
    fn change(&mut self, id: u32, value: f64) {
        if !value.is_finite() || !(0.0..=1.0).contains(&value) || parameter_spec(id).is_none() {
            return;
        }
        let value = CROSSOVER_PARAMETERS
            .iter()
            .position(|candidate| *candidate == id)
            .map_or(value, |index| {
                let crossovers = self.crossovers();
                let ratio = f64::from(1.259_921_1_f32);
                let lower = index
                    .checked_sub(1)
                    .and_then(|previous| crossovers.get(previous))
                    .map_or(20.0, |frequency| frequency * ratio);
                let upper = crossovers
                    .get(index + 1)
                    .map_or(20_000.0, |frequency| frequency / ratio);
                let spec = parameter_spec(id).expect("validated crossover");
                spec.plain_to_normalized(spec.normalized_to_plain(value).clamp(lower, upper.max(lower)))
            });
        self.begin_gesture(id);
        let Some(gesture) = &mut self.gesture else {
            return;
        };
        gesture.last_change = Instant::now();
        let linked = gesture.linked;
        self.write(id, value);
        if let Some((side, start_mid, start_side)) = linked
            && start_side > -60.0
            && let (Some(mid_spec), Some(side_spec)) = (parameter_spec(id), parameter_spec(side))
        {
            let side_db = start_side + mid_spec.normalized_to_plain(value) - start_mid;
            self.write(side, side_spec.plain_to_normalized(side_db));
        }
    }

    /// Closes every captured participant even if its Link checkbox changed during the edit.
    fn end_gesture(&mut self) {
        if let Some(gesture) = self.gesture.take() {
            if let Some((side, _, _)) = gesture.linked {
                self.binding.end_edit(side);
            }
            self.binding.end_edit(gesture.id);
        }
    }

    /// Updates the authoritative UI/host atomics before notifying the adapter, never a temporary UI mirror.
    fn write(&self, id: u32, value: f64) {
        if self.parameters.set(id, value) {
            self.binding.set_value(id, value);
        }
    }

    /// Reads a plain value for layout labels and linked dB offsets.
    fn plain(&self, id: u32) -> f64 {
        parameter_spec(id).map_or(0.0, |spec| {
            spec.normalized_to_plain(self.parameters.get(id).unwrap_or(spec.default_normalized))
        })
    }

    /// Reads all three visible markers from the same registry used by the host and sliders.
    fn crossovers(&self) -> [f64; 3] {
        CROSSOVER_PARAMETERS.map(|id| self.plain(id))
    }

    /// Preserves the header's exact left tools and right-anchored Mono/Delta/Bypass/Output ordering.
    fn header(&self, width: f32) -> Element<'_, Message> {
        let bounds = layout::Header::new(width);
        let mut header = stack![at(
            container(
                text("ImageControl")
                    .font(theme::BOLD)
                    .size(18)
                    .color(theme::GREY_SUPER_LIGHT)
            )
            .center_y(Length::Fill),
            bounds.title
        )]
        .width(Length::Fill)
        .height(36.0);
        for (symbol, target, message) in [
            (Symbol::Settings, bounds.settings, Some(Message::Settings)),
            (Symbol::Undo, bounds.undo, None),
            (Symbol::Redo, bounds.redo, None),
            (
                Symbol::Chevron(self.toolbar_shown),
                bounds.toolbar,
                Some(Message::Toolbar),
            ),
        ] {
            header = header.push(at(Chrome::new(symbol, target.size(), message, false), target));
        }
        for (label, target, tap) in [
            ("Pre", bounds.pre, AnalysisTap::Pre),
            ("Post", bounds.post, AnalysisTap::Post),
        ] {
            header = header.push(at(
                control(
                    label,
                    target.width,
                    Some(Message::Tap(tap)),
                    self.shared.analysis_tap() == tap,
                    theme::BLUE,
                ),
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
            container(text("Output").size(14)).center_y(Length::Fill),
            bounds.output_label,
        ));
        header
            .push(at(self.parameter_slider(GAIN_PARAMETER, 70.0), bounds.output))
            .into()
    }

    /// Matches the C++ callout's preference order. Unsupported renderer settings retain disabled native controls.
    fn settings(&self) -> Element<'_, Message> {
        let display = self.shared.display_preferences();
        let colour = if display.color_mode == ColourMode::Frequency {
            "Frequency"
        } else {
            "Phase coherence"
        };
        let choice = Row::new()
            .push(text("Colour").size(14).width(112.0))
            .push(
                pick_list(["Frequency", "Phase coherence"], Some(colour), |value| {
                    Message::Colour(if value == "Frequency" {
                        ColourMode::Frequency
                    } else {
                        ColourMode::Phase
                    })
                })
                .text_size(14)
                .padding([3.0, 8.0])
                .width(Length::Fill)
                .style(theme::choice),
            )
            .height(28.0)
            .align_y(Alignment::Center);
        let preferences = Column::new()
            .push(settings_heading("Appearance"))
            .push(choice)
            .push(disabled_choice("Frequency scale", "Logarithmic"))
            .push(disabled_choice("Rendering", "Particles"))
            .push(text("Contrast").size(14).height(18.0))
            .push(setting_slider(
                display.contrast,
                0.0..=1.0,
                0.01,
                Message::Contrast,
                format!("{:.0}%", display.contrast * 100.0),
            ))
            .push(Space::new().height(12.0))
            .push(settings_heading("Stereo position"))
            .push(text("Midpoint (50%)").size(14).height(18.0))
            .push(disabled_setting("12 dB"))
            .push(text("Edge (99%)").size(14).height(18.0))
            .push(disabled_setting("32 dB"))
            .push(Space::new().height(12.0))
            .push(settings_heading("Correlation"))
            .push(settings_check(
                "Show band correlation",
                self.correlation_shown,
                Some(Message::Correlation),
            ))
            .push(settings_check("Smooth correlation numbers", false, None))
            .push(Space::new().height(12.0))
            .push(settings_heading("Display behavior"))
            .push(text("Persistence").size(14).height(18.0))
            .push(setting_slider(
                display.persistence_seconds,
                0.05..=3.0,
                0.01,
                Message::Persistence,
                format!("{:.2} s", display.persistence_seconds),
            ))
            .push(settings_check("Hide spectrogram", false, None))
            .push(settings_check("Stop spectrogram analysis", false, None))
            .push(settings_check("Show frame rate", false, None))
            .push(Space::new().height(8.0))
            .push(control(
                "Reset display appearance",
                366.0,
                Some(Message::ResetAppearance),
                false,
                theme::BLUE,
            ))
            .push(Space::new().height(8.0))
            .push(control("Keyboard shortcuts", 366.0, None, false, theme::BLUE));
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

    /// Builds one quiet band surface with Solo/Link in its heading, followed by five stacked labelled rows.
    fn band_strip(&self, band: usize, sections: layout::Sections) -> Element<'_, Message> {
        if self.compact_bands {
            return self.compact_band(band, sections.strip_width);
        }
        let solo = band_parameter_id(band, 5).expect("fixed band Solo id");
        let heading = Row::new()
            .push(
                text(*layout::BAND_NAMES.get(band).expect("fixed band name"))
                    .font(theme::BOLD)
                    .size(16)
                    .color(theme::GREY_SUPER_LIGHT),
            )
            .push(Space::new().width(Length::Fill))
            .push(Chrome::new(
                Symbol::Headphones,
                Size::new(20.0, 20.0),
                Some(Message::Toggle(solo)),
                self.parameters.get(solo).is_some_and(|value| value >= 0.5),
            ))
            .push(Space::new().width(6.0))
            .push(
                container(
                    checkbox(self.linked.get(band).copied().unwrap_or(false))
                        .label("Link")
                        .text_size(14)
                        .size(13)
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
                    .size(14)
                    .color(theme::GREY_MIDDLE_LIGHT)
                    .height(layout::BAND_HEADER_HEIGHT),
            )
            .push(Space::new().height(sections.band_header_gap));
        for (field, label) in layout::FIELD_NAMES.into_iter().enumerate() {
            let id = band_parameter_id(band, field).expect("fixed band field id");
            controls = controls.push(
                Column::new()
                    .push(
                        text(label)
                            .size(14)
                            .height(layout::SLIDER_LABEL_HEIGHT)
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
            values = values.push(
                container(stack![
                    parameter_input(id, value).style(theme::compact_slider),
                    container(
                        text(label)
                            .font(theme::MONO)
                            .size(11)
                            .align_x(iced::alignment::Horizontal::Center)
                    )
                    .center(Length::Fill),
                ])
                .width(width)
                .height(24.0),
            );
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
            .push(
                container(
                    text(format_value(spec, value))
                        .font(theme::MONO)
                        .size(15)
                        .align_x(iced::alignment::Horizontal::Center),
                )
                .center_y(value_height)
                .width(value_width)
                .style(theme::value),
            )
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

impl Drop for EditorState {
    /// Ends host edits before releasing the analysis worker and GPU identity on the GUI thread.
    fn drop(&mut self) {
        self.end_gesture();
    }
}

/// Creates a direct Iced program for an adapter that supplies polling messages itself.
/// The native bridge must use `create_program_with_notifier` when relying on `iced::poll_events`.
#[expect(
    dead_code,
    reason = "four-argument factory retained for adapters with an external message sender"
)]
pub fn create_program(
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    binding: Rc<dyn EditorBinding>,
    window_size: iced::shell::SharedWindowSize,
) -> impl iced_program::Program<State = EditorState, Message = Message, Theme = Theme, Renderer = iced::Renderer> {
    EditorProgram {
        parameters,
        shared,
        binding,
        window_size,
        notifier: None,
        analysis: Rc::new(std::cell::RefCell::new(None)),
    }
}

/// Shares the window's poll notifier so each bounded timer callback rearms the next analyzer refresh.
pub fn create_program_with_notifier(
    parameters: Arc<Parameters>,
    shared: Arc<SharedState>,
    binding: Rc<dyn EditorBinding>,
    window_size: iced::shell::SharedWindowSize,
    notifier: iced::PollSubNotifier,
    analysis: Rc<std::cell::RefCell<Option<image_control_dsp::AnalysisShutdown>>>,
) -> impl iced_program::Program<State = EditorState, Message = Message, Theme = Theme, Renderer = iced::Renderer> {
    EditorProgram {
        parameters,
        shared,
        binding,
        window_size,
        notifier: Some(notifier),
        analysis,
    }
}

/// Converts real marker gestures to registry-normalized events without a separate crossover slider row.
fn plot_message(event: plot_overlay::PlotEvent) -> Message {
    match event {
        plot_overlay::PlotEvent::Begin(index) => {
            Message::Begin(*CROSSOVER_PARAMETERS.get(index).expect("fixed crossover id"))
        }
        plot_overlay::PlotEvent::Change(index, frequency) => {
            let id = *CROSSOVER_PARAMETERS.get(index).expect("fixed crossover id");
            let spec = parameter_spec(id).expect("fixed crossover id");
            Message::Change(id, spec.plain_to_normalized(frequency))
        }
        plot_overlay::PlotEvent::End => Message::EndGesture,
    }
}

/// Closes pointer, keyboard, touch, and interrupted gestures even when a child captured their release.
const fn release_message(event: &iced::Event) -> Option<Message> {
    match event {
        iced::Event::Mouse(iced::mouse::Event::CursorMoved { position }) => Some(Message::Pointer(*position)),
        iced::Event::Keyboard(iced::keyboard::Event::KeyPressed {
            key: iced::keyboard::Key::Named(iced::keyboard::key::Named::Escape),
            ..
        }) => Some(Message::CloseSettings),
        iced::Event::Keyboard(iced::keyboard::Event::KeyPressed { .. })
        | iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left))
        | iced::Event::Touch(iced::touch::Event::FingerPressed { .. }) => Some(Message::Hold),
        iced::Event::Keyboard(iced::keyboard::Event::KeyReleased { .. })
        | iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left))
        | iced::Event::Touch(iced::touch::Event::FingerLifted { .. } | iced::touch::Event::FingerLost { .. })
        | iced::Event::Window(iced::window::Event::Unfocused) => Some(Message::EndGesture),
        _ => None,
    }
}

/// Builds the same retained parameter widget for production views and hardware-free event tests.
fn parameter_input(id: u32, value: f64) -> slider::Slider<'static, f64, Message> {
    let spec = parameter_spec(id).expect("visible control has registry metadata");
    slider(0.0..=1.0, value, move |value| Message::Change(id, value))
        .step(0.001_f64)
        .shift_step(0.0001_f64)
        .default(spec.default_normalized)
        .on_release(Message::EndGesture)
        .height(layout::SLIDER_HEIGHT)
        .style(theme::parameter_slider)
}

/// Gives every button a compact, fixed target and disables unavailable features rather than simulating them.
fn control(
    label: &str,
    width: f32,
    message: Option<Message>,
    active: bool,
    accent: iced::Color,
) -> Element<'_, Message> {
    control_button(label, width, message, active, accent).into()
}

/// Shares the production button with event-only tests rather than simulating its messages.
fn control_button(
    label: &str,
    width: f32,
    message: Option<Message>,
    active: bool,
    accent: iced::Color,
) -> button::Button<'_, Message> {
    control_input(
        text(label).size(13).align_x(iced::alignment::Horizontal::Center),
        width,
        message,
        active,
        accent,
    )
}

/// Retains production click and selected styling behavior independently of glyph rasterization.
fn control_input<'a, Renderer: iced::advanced::Renderer>(
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

/// Preserves the actual comparison/preset row positions while absent history and library services remain disabled.
fn processing_toolbar<'a>() -> Element<'a, Message> {
    container(
        Row::new()
            .push(control("A", 32.0, None, true, theme::BLUE))
            .push(control("B", 32.0, None, false, theme::BLUE))
            .push(Space::new().width(4.0))
            .push(control("A → B", 72.0, None, false, theme::BLUE))
            .push(Space::new().width(12.0))
            .push(
                container(text("Unsaved sound").size(13).color(theme::GREY_MIDDLE))
                    .padding([3.0, 8.0])
                    .width(Length::Fill)
                    .height(24.0)
                    .style(theme::value),
            )
            .push(Space::new().width(160.0))
            .push(control("Presets", 72.0, None, false, theme::BLUE))
            .height(24.0)
            .align_y(Alignment::Center),
    )
    .padding([4.0, 0.0])
    .height(layout::TOOLBAR_HEIGHT)
    .into()
}

/// Pins a real retained widget into an independently testable logical target.
fn at<'a>(content: impl Into<Element<'a, Message>>, bounds: Rectangle) -> Element<'a, Message> {
    pin(container(content).width(bounds.width).height(bounds.height))
        .x(bounds.x)
        .y(bounds.y)
        .into()
}

fn settings_check(label: &str, checked: bool, message: Option<Message>) -> Element<'_, Message> {
    container(
        checkbox(checked)
            .label(label)
            .text_size(14)
            .size(16)
            .style(theme::check)
            .on_toggle_maybe(message.map(|message| move |_| message)),
    )
    .center_y(28.0)
    .into()
}

fn settings_heading(label: &str) -> Element<'_, Message> {
    Column::new()
        .push(container(text(label).font(theme::BOLD).size(18).color(theme::GREY_MIDDLE_LIGHT)).height(24.0))
        .push(Space::new().height(8.0))
        .into()
}

fn disabled_choice<'a>(label: &'a str, value: &'a str) -> Element<'a, Message> {
    Row::new()
        .push(text(label).size(14).width(112.0))
        .push(
            container(text(value).size(14).color(theme::GREY_MIDDLE))
                .padding([3.0, 8.0])
                .width(Length::Fill)
                .height(24.0)
                .style(theme::value),
        )
        .height(28.0)
        .align_y(Alignment::Center)
        .into()
}

fn disabled_setting(value: &str) -> Element<'_, Message> {
    Row::new()
        .push(Chrome::new(Symbol::DisabledSlider, Size::new(290.0, 28.0), None, false))
        .push(
            container(text(value).font(theme::MONO).size(12).color(theme::GREY_MIDDLE))
                .center_y(22.0)
                .width(76.0)
                .style(theme::value),
        )
        .height(28.0)
        .align_y(Alignment::Center)
        .into()
}

fn setting_slider(
    value: f32,
    range: std::ops::RangeInclusive<f32>,
    step: f32,
    change: fn(f32) -> Message,
    label: String,
) -> Element<'static, Message> {
    Row::new()
        .push(
            container(slider(range, value, change).step(step).style(theme::parameter_slider))
                .padding([0.0, 6.0])
                .width(Length::Fill),
        )
        .push(
            container(text(label).font(theme::MONO).size(12))
                .center_y(22.0)
                .width(76.0)
                .style(theme::value),
        )
        .height(28.0)
        .align_y(Alignment::Center)
        .into()
}

/// Measures worst-case numeric glyphs once per view so compact full names never depend on the current value.
fn compact_text_width(content: &str) -> f32 {
    use iced::advanced::text::Paragraph as _;
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

fn format_compact_value(spec: ParameterSpec, normalized: f64, suffix: &str) -> String {
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

/// Tracks the last observed publication on the UI thread, without audio-thread clock calls.
struct CorrelationObservation {
    readings: [f32; 4],
    generation: u64,
    revision: u64,
    updated: Option<Instant>,
}

impl Default for CorrelationObservation {
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
    const fn observe(&mut self, snapshot: CorrelationSnapshot, now: Instant) {
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
    fn readings(&self, now: Instant, expiry: Duration) -> [Option<f32>; 4] {
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
const fn analysis_status_text(status: AnalysisStatus) -> Option<&'static str> {
    match status {
        AnalysisStatus::Waiting => Some("Waiting for audio"),
        AnalysisStatus::UnsupportedRate => Some("Spatial analysis unavailable at this sample rate"),
        AnalysisStatus::Failed => Some("Spatial analysis failed"),
        AnalysisStatus::Disabled => Some("Spatial analysis disabled"),
        AnalysisStatus::Running => None,
    }
}

/// Displays real processed-band readings, with no marker or invented number while unavailable.
fn correlation_meter<'a>(reading: Option<f32>) -> Element<'a, Message> {
    let label = Row::new()
        .push(text("Correlation").size(11))
        .push(Space::new().width(Length::Fill))
        .push(
            text(reading.map_or_else(String::new, |value| format!("{value:+.2}")))
                .font(theme::MONO)
                .size(11),
        )
        .height(17.0);
    let mut track = stack![
        container(Space::new().height(5.0))
            .width(Length::Fill)
            .style(|_| container::Style::default().background(theme::GREY_MEDIUM)),
    ];
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

/// Formats band and header values using the same domains as their host metadata.
fn format_value(spec: ParameterSpec, normalized: f64) -> String {
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
fn format_frequency(frequency: f64) -> String {
    if frequency.round() < 1000.0 {
        format!("{frequency:.0} Hz")
    } else if frequency < 10_000.0 {
        format!("{:.2} kHz", frequency * 0.001)
    } else {
        format!("{:.1} kHz", frequency * 0.001)
    }
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use super::*;

    /// Records the real editor's host callbacks without creating a native host or GPU device.
    #[derive(Default)]
    struct RecordingBinding {
        events: RefCell<Vec<(u32, Option<f64>, bool)>>,
        resizes: RefCell<Vec<Size>>,
    }

    impl EditorBinding for RecordingBinding {
        fn begin_edit(&self, id: u32) {
            self.events.borrow_mut().push((id, None, true));
        }
        fn set_value(&self, id: u32, value: f64) {
            self.events.borrow_mut().push((id, Some(value), false));
        }
        fn end_edit(&self, id: u32) {
            self.events.borrow_mut().push((id, None, false));
        }
        fn request_resize(&self, width: u32, height: u32) -> bool {
            self.resizes.borrow_mut().push(Size::new(width as f32, height as f32));
            true
        }
    }

    /// Exercises the ordinary editor update path without opening the editor-owned analyzer consumer.
    fn editor() -> (EditorState, Rc<RecordingBinding>) {
        let binding = Rc::new(RecordingBinding::default());
        let state = EditorState {
            parameters: Arc::new(Parameters::default()),
            shared: image_control_dsp::Imager::new().shared(),
            settings_shown: false,
            correlation: CorrelationObservation::default(),
            binding: binding.clone(),
            spectrogram: Err(std::io::Error::other("headless interaction test")),
            window_size: None,
            notifier: None,
            gesture: None,
            linked: [false; 4],
            toolbar_shown: true,
            correlation_shown: true,
            input_held: false,
            band_height: layout::BAND_HEIGHT,
            divider_drag: None,
            resize_drag: None,
            compact_bands: false,
        };
        (state, binding)
    }

    #[test]
    fn linked_gesture_captures_participants_until_release_even_when_link_changes() {
        let (mut state, binding) = editor();
        let mid = band_parameter_id(0, 0).expect("Mid");
        let side = band_parameter_id(0, 1).expect("Side");
        let _ = state.update(Message::Link(0, true));
        let _ = state.update(Message::Change(mid, 0.75));
        let _ = state.update(Message::Link(0, false));
        let _ = state.update(Message::Change(mid, 0.5));
        let _ = state.update(Message::EndGesture);
        let events = binding.events.borrow();
        let starts: Vec<_> = events
            .iter()
            .filter(|(_, value, begin)| value.is_none() && *begin)
            .map(|(id, _, _)| *id)
            .collect();
        let ends: Vec<_> = events
            .iter()
            .filter(|(_, value, begin)| value.is_none() && !*begin)
            .map(|(id, _, _)| *id)
            .collect();
        assert_eq!(starts, [mid, side]);
        assert_eq!(ends, [side, mid]);
        assert!((state.plain(mid) - state.plain(side)).abs() < 0.000_001);
        assert!((state.plain(band_parameter_id(0, 2).expect("Width")) - 1.0).abs() < f64::EPSILON);
    }

    #[test]
    fn pointer_pause_never_splits_a_held_gesture_and_drop_closes_it() {
        let (mut state, binding) = editor();
        let _ = state.update(Message::Hold);
        let _ = state.update(Message::Change(GAIN_PARAMETER, 0.7));
        state.gesture.as_mut().expect("gesture").last_change = Instant::now()
            .checked_sub(GESTURE_IDLE * 2)
            .expect("recent test instant");
        let _ = state.update(Message::Refresh);
        assert!(state.gesture.is_some());
        drop(state);
        let events = binding.events.borrow();
        assert_eq!(events.iter().filter(|(_, value, _)| value.is_none()).count(), 2);
        assert_eq!(events.last(), Some(&(GAIN_PARAMETER, None, false)));
    }

    #[test]
    fn keyboard_release_closes_real_edit_and_toggle_is_balanced() {
        let (mut state, binding) = editor();
        let key = iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowUp);
        let event = iced::Event::Keyboard(iced::keyboard::Event::KeyReleased {
            key: key.clone(),
            modified_key: key,
            physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowUp),
            location: iced::keyboard::Location::Standard,
            modifiers: iced::keyboard::Modifiers::default(),
        });
        let _ = state.update(Message::Change(GAIN_PARAMETER, 0.8));
        let _ = state.update(release_message(&event).expect("key release"));
        assert!(state.gesture.is_none());
        let _ = state.update(Message::Toggle(MONO_PARAMETER));
        assert_eq!(binding.events.borrow().last(), Some(&(MONO_PARAMETER, None, false)));
        assert!(state.parameters.get(MONO_PARAMETER).expect("Mono") >= 0.5);
    }

    #[test]
    fn marker_cannot_cross_its_neighbors_or_change_another_parameter() {
        let (mut state, binding) = editor();
        let id = CROSSOVER_PARAMETERS[1];
        let _ = state.update(Message::Change(id, 1.0));
        assert!((state.plain(id) - 5000.0 / f64::from(1.259_921_1_f32)).abs() < 0.001);
        assert!((state.plain(CROSSOVER_PARAMETERS[0]) - 100.0).abs() < 0.001);
        assert!((state.plain(CROSSOVER_PARAMETERS[2]) - 5000.0).abs() < 0.001);
        let _ = state.update(Message::EndGesture);
        assert!(binding.events.borrow().iter().all(|(changed, _, _)| *changed == id));
    }

    #[test]
    fn divider_changes_layout_only_and_release_stops_dragging() {
        let (mut state, binding) = editor();
        let _ = state.update(Message::DividerStart(Point::new(500.0, 300.0)));
        let _ = state.update(Message::Pointer(Point::new(500.0, 280.0)));
        assert!((state.band_height - 324.0).abs() < f32::EPSILON);
        let _ = state.update(Message::EndGesture);
        let _ = state.update(Message::Pointer(Point::new(500.0, 200.0)));
        assert!((state.band_height - 324.0).abs() < f32::EPSILON);
        assert!(binding.events.borrow().is_empty());
    }

    #[test]
    fn invalid_changes_and_automated_readback_do_not_create_host_gestures() {
        let (mut state, binding) = editor();
        for value in [f64::NAN, f64::INFINITY, -0.1, 1.1] {
            let _ = state.update(Message::Change(GAIN_PARAMETER, value));
        }
        assert!(state.parameters.set(GAIN_PARAMETER, 0.1));
        let _ = state.update(Message::Refresh);
        assert!(binding.events.borrow().is_empty());
        assert!((state.plain(GAIN_PARAMETER) + 20.4).abs() < 0.000_001);
    }

    /// Event-only renderer keeps real production widget tests available in Release as well as Debug.
    /// It is never selected by the editor, and does not rasterize or generate any image.
    struct EventRenderer;

    impl iced::advanced::Renderer for EventRenderer {
        fn start_layer(&mut self, _: iced::Rectangle) {}
        fn end_layer(&mut self) {}
        fn start_transformation(&mut self, _: iced::Transformation) {}
        fn end_transformation(&mut self) {}
        fn fill_quad(&mut self, _: iced::advanced::renderer::Quad, _: impl Into<iced::Background>) {}
        fn reset(&mut self, _: iced::Rectangle) {}
        fn allocate_image(
            &mut self,
            _: &iced::core::image::Handle,
            callback: impl FnOnce(Result<iced::core::image::Allocation, iced::core::image::Error>) + Send + 'static,
        ) {
            callback(Err(iced::core::image::Error::Unsupported));
        }
    }

    /// Routes actual events through retained production chrome, then the same raw-event subscription mapping.
    fn drive_chrome(
        state: &mut EditorState,
        symbol: Symbol,
        message: Message,
        bounds: Rectangle,
        events: &[(iced::Event, iced::mouse::Cursor)],
    ) {
        let mut element: iced::core::Element<'_, Message, Theme, EventRenderer> =
            Chrome::new(symbol, bounds.size(), Some(message), false).into();
        let mut tree = iced::advanced::widget::Tree::new(&element);
        let node = iced::advanced::layout::Node::new(bounds.size()).move_to(bounds.position());
        for (event, cursor) in events {
            let mut messages = Vec::new();
            element.as_widget_mut().update(
                &mut tree,
                event,
                iced::advanced::Layout::new(&node),
                *cursor,
                &EventRenderer,
                &mut iced::advanced::clipboard::Null,
                &mut iced::advanced::Shell::new(&mut messages),
                &iced::Rectangle::with_size(Size::new(4096.0, 4096.0)),
            );
            for message in messages {
                let _ = state.update(message);
            }
            if let Some(message) = release_message(event) {
                let _ = state.update(message);
            }
        }
    }

    #[test]
    fn real_resize_grip_captures_motion_outside_bounds_and_host_resize_notifications() {
        let (mut state, binding) = editor();
        let bounds = layout::rect(982.0, 746.0, 18.0, 18.0);
        let press = iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left));
        let release = iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left));
        drive_chrome(
            &mut state,
            Symbol::Resize,
            Message::Ignore,
            bounds,
            &[
                (press, iced::mouse::Cursor::Available(Point::new(992.0, 754.0))),
                (
                    iced::Event::Mouse(iced::mouse::Event::CursorMoved {
                        position: Point::new(1092.0, 834.0),
                    }),
                    iced::mouse::Cursor::Unavailable,
                ),
            ],
        );
        assert_eq!(*binding.resizes.borrow(), [Size::new(1100.0, 844.0)]);
        let _ = state.update(Message::Resize);
        let _ = state.update(Message::Pointer(Point::new(1192.0, 934.0)));
        assert_eq!(binding.resizes.borrow().last(), Some(&Size::new(1200.0, 944.0)));
        let _ = state.update(release_message(&release).expect("raw release"));
        let _ = state.update(Message::Pointer(Point::new(1292.0, 1034.0)));
        assert_eq!(binding.resizes.borrow().len(), 2);
        assert!(binding.events.borrow().is_empty());
    }

    #[test]
    fn real_disclosure_pointer_click_compacts_without_changing_sound_or_window() {
        let (mut state, binding) = editor();
        let before = state.parameters.snapshot();
        let bounds = layout::rect(476.0, 446.0, 48.0, 12.0);
        let cursor = iced::mouse::Cursor::Available(Point::new(500.0, 452.0));
        drive_chrome(
            &mut state,
            Symbol::Disclosure(false),
            Message::CompactBands,
            bounds,
            &[
                (
                    iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left)),
                    cursor,
                ),
                (
                    iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left)),
                    cursor,
                ),
            ],
        );
        assert!(state.compact_bands);
        assert_eq!(
            state.sections(state.size()).bands,
            layout::rect(8.0, 728.0, 984.0, 28.0)
        );
        assert_eq!(
            serde_json::to_vec(&state.parameters.snapshot()).expect("sound"),
            serde_json::to_vec(&before).expect("sound")
        );
        assert!(binding.resizes.borrow().is_empty());
        assert!(binding.events.borrow().is_empty());
    }

    #[test]
    fn production_slider_routes_real_pointer_and_key_events_to_balanced_edits() {
        let (mut state, binding) = editor();
        let element: iced::core::Element<'_, Message, Theme, EventRenderer> =
            parameter_input(GAIN_PARAMETER, 0.5).into();
        let mut tree_widget = iced::advanced::widget::Tree::new(&element);
        let node = iced::advanced::layout::Node::new(iced::Size::new(200.0, 26.0));
        let bounds = iced::Rectangle::with_size(iced::Size::new(200.0, 26.0));
        let cursor = iced::mouse::Cursor::Available(iced::Point::new(150.0, 13.0));
        let mut element = element;
        let mut clipboard = iced::advanced::clipboard::Null;
        let key = iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowUp);
        let events = [
            iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left)),
            iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left)),
            iced::Event::Keyboard(iced::keyboard::Event::KeyPressed {
                key: key.clone(),
                modified_key: key.clone(),
                physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowUp),
                location: iced::keyboard::Location::Standard,
                modifiers: iced::keyboard::Modifiers::default(),
                text: None,
                repeat: false,
            }),
            iced::Event::Keyboard(iced::keyboard::Event::KeyReleased {
                key: key.clone(),
                modified_key: key,
                physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowUp),
                location: iced::keyboard::Location::Standard,
                modifiers: iced::keyboard::Modifiers::default(),
            }),
        ];
        for event in events {
            let mut messages = Vec::new();
            let mut shell = iced::advanced::Shell::new(&mut messages);
            element.as_widget_mut().update(
                &mut tree_widget,
                &event,
                iced::advanced::Layout::new(&node),
                cursor,
                &EventRenderer,
                &mut clipboard,
                &mut shell,
                &bounds,
            );
            for message in messages {
                let _ = state.update(message);
            }
            if let Some(message) = release_message(&event) {
                let _ = state.update(message);
            }
        }
        assert!(state.gesture.is_none());
        let callbacks = binding.events.borrow();
        assert_eq!(
            callbacks
                .iter()
                .filter(|(_, value, begin)| value.is_none() && *begin)
                .count(),
            2
        );
        assert_eq!(
            callbacks
                .iter()
                .filter(|(_, value, begin)| value.is_none() && !*begin)
                .count(),
            2
        );
        assert!(
            callbacks
                .iter()
                .any(|(_, value, _)| value.is_some_and(|value| (value - 0.75).abs() < 0.001))
        );
    }

    #[test]
    fn real_preference_buttons_select_shared_state_without_sound_gestures() {
        let (mut state, binding) = editor();
        let before = serde_json::to_vec(&state.parameters.snapshot()).expect("sound");
        for message in [
            Message::Tap(AnalysisTap::Pre),
            Message::Settings,
            Message::Colour(ColourMode::Phase),
        ] {
            let content: iced::core::Element<'_, Message, Theme, EventRenderer> = Space::new().into();
            let mut element: iced::core::Element<'_, Message, Theme, EventRenderer> =
                control_input(content, 80.0, Some(message), false, theme::BLUE).into();
            let mut tree = iced::advanced::widget::Tree::new(&element);
            let node = element.as_widget_mut().layout(
                &mut tree,
                &EventRenderer,
                &iced::advanced::layout::Limits::new(iced::Size::ZERO, iced::Size::new(80.0, 24.0)),
            );
            let bounds = iced::Rectangle::with_size(iced::Size::new(80.0, 24.0));
            for event in [
                iced::Event::Mouse(iced::mouse::Event::ButtonPressed(iced::mouse::Button::Left)),
                iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left)),
            ] {
                let mut messages = Vec::new();
                element.as_widget_mut().update(
                    &mut tree,
                    &event,
                    iced::advanced::Layout::new(&node),
                    iced::mouse::Cursor::Available(iced::Point::new(40.0, 12.0)),
                    &EventRenderer,
                    &mut iced::advanced::clipboard::Null,
                    &mut iced::advanced::Shell::new(&mut messages),
                    &bounds,
                );
                for message in messages {
                    let _ = state.update(message);
                }
            }
        }
        assert_eq!(state.shared.analysis_tap(), AnalysisTap::Pre);
        assert_eq!(state.shared.display_preferences().color_mode, ColourMode::Phase);
        assert!(state.settings_shown);
        assert!(binding.events.borrow().is_empty());
        assert_eq!(serde_json::to_vec(&state.parameters.snapshot()).expect("sound"), before);
        assert!(
            state
                .shared
                .set_display_preferences(image_control_dsp::DisplayPreferences::default())
        );
        state.shared.set_analysis_tap(AnalysisTap::Post);
        let _ = state.update(Message::Refresh);
        assert_eq!(state.shared.analysis_tap(), AnalysisTap::Post);
        assert_eq!(state.shared.display_preferences().color_mode, ColourMode::Frequency);
    }

    #[test]
    fn revision_expiry_and_generation_changes_remove_real_meter_readings() {
        let now = Instant::now();
        let mut meter = CorrelationObservation::default();
        meter.observe(
            CorrelationSnapshot {
                readings: [1.0; 4],
                generation: 1,
                revision: 1,
            },
            now,
        );
        assert_eq!(meter.readings(now, Duration::ZERO), [None; 4]);
        meter.observe(
            CorrelationSnapshot {
                readings: [-0.5; 4],
                generation: 1,
                revision: 2,
            },
            now,
        );
        assert_eq!(meter.readings(now, Duration::ZERO), [Some(-0.5); 4]);
        assert_eq!(
            meter.readings(now + Duration::from_millis(250), Duration::ZERO),
            [None; 4]
        );
        assert_eq!(
            meter.readings(now + Duration::from_millis(300), Duration::from_secs(1)),
            [Some(-0.5); 4]
        );
        meter.observe(
            CorrelationSnapshot {
                readings: [1.0; 4],
                generation: 2,
                revision: 3,
            },
            now,
        );
        assert_eq!(meter.readings(now, Duration::from_secs(1)), [None; 4]);
    }

    #[test]
    fn analysis_status_is_explicit_and_preference_validation_keeps_sound_unchanged() {
        assert_eq!(analysis_status_text(AnalysisStatus::Waiting), Some("Waiting for audio"));
        assert!(analysis_status_text(AnalysisStatus::UnsupportedRate).is_some());
        assert!(analysis_status_text(AnalysisStatus::Failed).is_some());
        assert!(analysis_status_text(AnalysisStatus::Running).is_none());
        let (mut state, binding) = editor();
        let display = state.shared.display_preferences();
        let _ = state.update(Message::Persistence(f32::NAN));
        let _ = state.update(Message::Contrast(2.0));
        assert_eq!(state.shared.display_preferences(), display);
        assert!(binding.events.borrow().is_empty());
    }

    #[test]
    fn value_readouts_follow_independent_parameter_domains() {
        for (id, normalized, expected) in [
            (GAIN_PARAMETER, 2.0 / 3.0, "0.0 dB"),
            (4, 0.0, "-inf dB"),
            (6, 0.5, "1.00"),
            (7, 0.0, "0%"),
            (8, 0.5, "C"),
            (8, 0.25, "50L"),
        ] {
            assert_eq!(
                format_value(parameter_spec(id).expect("parameter"), normalized),
                expected
            );
        }
    }

    #[test]
    fn raw_pointer_and_focus_loss_events_close_gestures() {
        for event in [
            iced::Event::Mouse(iced::mouse::Event::ButtonReleased(iced::mouse::Button::Left)),
            iced::Event::Window(iced::window::Event::Unfocused),
        ] {
            assert!(matches!(release_message(&event), Some(Message::EndGesture)));
        }
        assert!(release_message(&iced::Event::Mouse(iced::mouse::Event::CursorLeft)).is_none());
    }

    #[test]
    fn marker_messages_use_registry_conversions_not_screen_units() {
        let Message::Change(id, normalized) = plot_message(plot_overlay::PlotEvent::Change(1, 2000.0)) else {
            panic!("change message")
        };
        assert_eq!(id, CROSSOVER_PARAMETERS[1]);
        assert!((parameter_spec(id).expect("crossover").normalized_to_plain(normalized) - 2000.0).abs() < 0.000_001);
    }
}
