//! Production-widget event and semantic drawing tests without GPU resources or pixel assertions.

use super::*;

/// Records GPU primitives and text while exercising the production generic Widget implementation.
#[derive(Default)]
struct EventRenderer {
    quads: Vec<(Rectangle, iced::Background)>,
    labels: Vec<String>,
}

impl iced::advanced::Renderer for EventRenderer {
    fn start_layer(&mut self, _: Rectangle) {}
    fn end_layer(&mut self) {}
    fn start_transformation(&mut self, _: iced::Transformation) {}
    fn end_transformation(&mut self) {}
    fn fill_quad(&mut self, quad: renderer::Quad, background: impl Into<iced::Background>) {
        self.quads.push((quad.bounds, background.into()));
    }
    fn reset(&mut self, _: Rectangle) {}
    fn allocate_image(
        &mut self,
        _: &iced::core::image::Handle,
        callback: impl FnOnce(Result<iced::core::image::Allocation, iced::core::image::Error>) + Send + 'static,
    ) {
        callback(Err(iced::core::image::Error::Unsupported));
    }
}

impl text::Renderer for EventRenderer {
    type Font = iced::Font;
    type Paragraph = <iced::Renderer as text::Renderer>::Paragraph;
    type Editor = <iced::Renderer as text::Renderer>::Editor;
    const ICON_FONT: iced::Font = theme::TEXT;
    const CHECKMARK_ICON: char = 'x';
    const ARROW_DOWN_ICON: char = 'v';
    const SCROLL_UP_ICON: char = '^';
    const SCROLL_DOWN_ICON: char = 'v';
    const SCROLL_LEFT_ICON: char = '<';
    const SCROLL_RIGHT_ICON: char = '>';
    const ICED_LOGO: char = 'i';
    fn default_font(&self) -> iced::Font {
        theme::TEXT
    }
    fn default_size(&self) -> iced::Pixels {
        theme::ANNOTATION_SIZE.into()
    }
    fn fill_paragraph(&mut self, _: &Self::Paragraph, _: Point, _: Color, _: Rectangle) {}
    fn fill_editor(&mut self, _: &Self::Editor, _: Point, _: Color, _: Rectangle) {}
    fn fill_text(&mut self, text: text::Text, _: Point, _: Color, _: Rectangle) {
        self.labels.push(text.content);
    }
}

/// Reconciles the actual widget between events to exercise retained drag and restore cancellation.
struct Driver {
    element: iced::core::Element<'static, PlotEvent, Theme, EventRenderer>,
    tree: tree::Tree,
    node: layout::Node,
}

impl Driver {
    /// Creates retained widget state at a fixed logical size without a native window.
    fn new(overlay: PlotOverlay<PlotEvent>, size: Size) -> Self {
        let element = overlay.into();
        let tree = tree::Tree::new(&element);
        Self {
            element,
            tree,
            node: layout::Node::new(size),
        }
    }
    /// Reconciles production widget state so tests exercise retention across view updates.
    fn rebuild(&mut self, overlay: PlotOverlay<PlotEvent>) {
        self.element = overlay.into();
        self.tree.diff(self.element.as_widget());
    }
    /// Delivers a real input event and returns commands other than incidental hover updates.
    fn send(&mut self, event: &Event, cursor: mouse::Cursor) -> Vec<PlotEvent> {
        let mut messages = Vec::new();
        self.element.as_widget_mut().update(
            &mut self.tree,
            event,
            Layout::new(&self.node),
            cursor,
            &EventRenderer::default(),
            &mut iced::advanced::clipboard::Null,
            &mut Shell::new(&mut messages),
            &Rectangle::with_size(self.node.size()),
        );
        messages
            .into_iter()
            .filter(|event| !matches!(event, PlotEvent::HoverBand(_)))
            .collect()
    }
}

/// Constructs the production overlay with representative crossovers and unchanged event messages.
fn overlay() -> PlotOverlay<PlotEvent> {
    PlotOverlay::new([100.0, 500.0, 5000.0], std::convert::identity)
}
/// Supplies the primary-button press that begins production pointer gestures.
fn press() -> Event {
    Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
}
/// Supplies the primary-button release that ends retained pointer gestures.
fn release() -> Event {
    Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))
}
/// Supplies a right-arrow key press with explicit modifiers for coarse and fine crossover edits.
fn arrow(modifiers: iced::keyboard::Modifiers) -> Event {
    let key = iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowRight);
    Event::Keyboard(iced::keyboard::Event::KeyPressed {
        key: key.clone(),
        modified_key: key,
        physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowRight),
        location: iced::keyboard::Location::Standard,
        modifiers,
        text: None,
        repeat: false,
    })
}

#[test]
fn pointer_drag_captures_marker_rebuilds_and_releases_outside_plot() {
    let size = Size::new(932.0, 340.0);
    let bounds = plot_bounds(Rectangle::with_size(size));
    let cursor = mouse::Cursor::Available(Point::new(
        frequency_x(500.0, bounds, FrequencyScale::Logarithmic),
        bounds.center_y(),
    ));
    let mut driver = Driver::new(overlay(), size);
    assert_eq!(driver.send(&press(), cursor), [PlotEvent::Begin(1)]);
    driver.rebuild(overlay());
    let events = driver.send(
        &Event::Mouse(mouse::Event::CursorMoved {
            position: Point::new(2000.0, 900.0),
        }),
        mouse::Cursor::Unavailable,
    );
    assert!(
        matches!(events.as_slice(), [PlotEvent::Change(1, value)] if (*value - 5000.0 / f64::from(1.259_921_1_f32)).abs() < 0.001)
    );
    assert_eq!(driver.send(&release(), mouse::Cursor::Unavailable), [PlotEvent::End]);
}

#[test]
fn empty_plot_space_moves_band_but_empty_outer_gutter_does_not_open_edit() {
    let mut driver = Driver::new(overlay(), Size::new(932.0, 340.0));
    let cursor = mouse::Cursor::Available(Point::new(50.0, 100.0));
    assert_eq!(driver.send(&press(), cursor), [PlotEvent::BeginBand(0)]);
    assert_eq!(driver.send(&release(), cursor), [PlotEvent::End]);
    assert_eq!(
        driver.send(&press(), mouse::Cursor::Available(Point::new(1.0, 100.0))),
        []
    );
}

#[test]
fn hidden_narrow_pills_keep_marker_selection_and_keyboard_hover_gestures() {
    let size = Size::new(PLOT_INSETS[1] + PLOT_INSETS[3] + 32.0, 240.0);
    let bounds = plot_bounds(Rectangle::with_size(size));
    let x = frequency_x(500.0, bounds, FrequencyScale::Logarithmic);
    assert!(crossover_label_bounds(x, bounds, Size::new(32.0, 10.0), &[]).is_none());
    let cursor = mouse::Cursor::Available(Point::new(x, bounds.center_y()));
    let mut driver = Driver::new(overlay(), size);
    for (modifiers, expected) in [
        (iced::keyboard::Modifiers::default(), 550.0),
        (iced::keyboard::Modifiers::SHIFT, 505.0),
    ] {
        assert_eq!(
            driver.send(&arrow(modifiers), cursor),
            [PlotEvent::Begin(1), PlotEvent::Change(1, expected), PlotEvent::End]
        );
    }
    assert_eq!(driver.send(&press(), cursor), [PlotEvent::Begin(1)]);
    assert_eq!(driver.send(&release(), cursor), [PlotEvent::End]);
}

#[test]
fn restore_epoch_cancels_retained_widget_drag_before_next_pointer_write() {
    let size = Size::new(932.0, 340.0);
    let cursor = mouse::Cursor::Available(Point::new(300.0, 100.0));
    let mut driver = Driver::new(overlay(), size);
    assert_eq!(driver.send(&press(), cursor), [PlotEvent::BeginBand(1)]);
    driver.rebuild(overlay().interaction_epoch(1));
    assert_eq!(
        driver.send(
            &Event::Mouse(mouse::Event::CursorMoved {
                position: Point::new(500.0, 100.0)
            }),
            cursor
        ),
        [PlotEvent::End]
    );
    assert_eq!(driver.send(&release(), cursor), []);
}

#[test]
fn real_widget_modifier_gain_wheel_and_double_click_follow_public_event_contract() {
    let size = Size::new(932.0, 340.0);
    let bounds = plot_bounds(Rectangle::with_size(size));
    let cursor = mouse::Cursor::Available(Point::new(
        frequency_x(250.0, bounds, FrequencyScale::Logarithmic),
        100.0,
    ));
    let mut driver = Driver::new(overlay(), size);
    let _ = driver.send(
        &Event::Keyboard(iced::keyboard::Event::ModifiersChanged(iced::keyboard::Modifiers::CTRL)),
        cursor,
    );
    assert_eq!(driver.send(&press(), cursor), [PlotEvent::BeginGain(1, true)]);
    let events = driver.send(
        &Event::Mouse(mouse::Event::CursorMoved {
            position: Point::new(400.0, 90.0),
        }),
        cursor,
    );
    assert_eq!(events, [PlotEvent::ChangeGain(1, true, 1.2)]);
    assert_eq!(driver.send(&release(), cursor), [PlotEvent::End]);
    let _ = driver.send(
        &Event::Keyboard(iced::keyboard::Event::ModifiersChanged(
            iced::keyboard::Modifiers::default(),
        )),
        cursor,
    );
    assert_eq!(
        driver.send(
            &Event::Mouse(mouse::Event::WheelScrolled {
                delta: mouse::ScrollDelta::Lines { x: 0.0, y: 1.0 }
            }),
            cursor
        ),
        [
            PlotEvent::BeginGain(1, false),
            PlotEvent::ChangeGain(1, false, 0.1),
            PlotEvent::End
        ]
    );
    let marker = mouse::Cursor::Available(Point::new(
        frequency_x(500.0, bounds, FrequencyScale::Logarithmic),
        100.0,
    ));
    assert_eq!(driver.send(&press(), marker), [PlotEvent::Begin(1)]);
    assert_eq!(driver.send(&release(), marker), [PlotEvent::End]);
    assert_eq!(driver.send(&press(), marker), [PlotEvent::Reset(1)]);
}

#[test]
fn real_widget_draws_solo_veil_width_and_response_without_pixel_assertions() {
    tests::load_annotation_font();
    let mut snapshot = Snapshot::default();
    snapshot.bands[0].solo = 1.0;
    snapshot.bands[1].width = 0.75;
    let size = Size::new(932.0, 340.0);
    let bounds = plot_bounds(Rectangle::with_size(size));
    let cursor = mouse::Cursor::Available(Point::new(frequency_x(250.0, bounds, FrequencyScale::Mel), 100.0));
    let driver = Driver::new(overlay().display(true, snapshot), size);
    let mut renderer = EventRenderer::default();
    driver.element.as_widget().draw(
        &driver.tree,
        &mut renderer,
        &Theme::Dark,
        &renderer::Style::default(),
        Layout::new(&driver.node),
        cursor,
        &Rectangle::with_size(size),
    );
    assert!(renderer.labels.iter().any(|label| label == "Width 1.50"));
    assert!(renderer.quads.iter().any(|(_, background)| matches!(background, iced::Background::Color(colour) if *colour == theme::GREY_DARK.scale_alpha(0.55))));
    assert!(renderer.quads.len() > 100);
    assert_eq!(
        driver.tree.state.downcast_ref::<PlotState>().responses.borrow().builds,
        1
    );
}
