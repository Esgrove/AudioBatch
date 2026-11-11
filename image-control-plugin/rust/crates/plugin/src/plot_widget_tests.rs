//! Production-widget event and semantic drawing tests without GPU resources or pixel assertions.

use super::*;
use iced_widget::graphics::geometry;

/// Records GPU primitives and text while exercising the production generic Widget implementation.
#[derive(Default)]
struct EventRenderer {
    quads: Vec<(Rectangle, iced::Background)>,
    labels: Vec<String>,
    geometry_count: usize,
    frame_builds: std::cell::Cell<usize>,
}

/// Tessellates production paths with the GPU backend's CPU frame, without creating a device or surface.
impl geometry::Renderer for EventRenderer {
    type Geometry = <iced::Renderer as geometry::Renderer>::Geometry;
    type Frame = <iced::Renderer as geometry::Renderer>::Frame;

    fn new_frame(&self, bounds: Rectangle) -> Self::Frame {
        self.frame_builds.set(self.frame_builds.get() + 1);
        iced_widget::renderer::wgpu::geometry::Frame::new(bounds)
    }

    fn draw_geometry(&mut self, _: Self::Geometry) {
        self.geometry_count += 1;
    }
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

/// Records production messages and redraw demand without introducing a native scheduler.
struct EventOutcome {
    messages: Vec<PlotEvent>,
    redraw: iced::window::RedrawRequest,
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
        self.dispatch(event, cursor).messages
    }

    /// Exposes the widget's own redraw request for deterministic frame-local animation tests.
    fn dispatch(&mut self, event: &Event, cursor: mouse::Cursor) -> EventOutcome {
        let mut messages = Vec::new();
        let mut shell = Shell::new(&mut messages);
        self.element.as_widget_mut().update(
            &mut self.tree,
            event,
            Layout::new(&self.node),
            cursor,
            &EventRenderer::default(),
            &mut iced::advanced::clipboard::Null,
            &mut shell,
            &Rectangle::with_size(self.node.size()),
        );
        let redraw = shell.redraw_request();
        EventOutcome {
            messages: messages
                .into_iter()
                .filter(|event| !matches!(event, PlotEvent::HoverBand(_)))
                .collect(),
            redraw,
        }
    }

    /// Settles a hover using supplied redraw timestamps while keeping path drawing separate from animation.
    fn settle_hover(&mut self, cursor: mouse::Cursor, now: Instant) {
        for frame in 0..=6 {
            self.send(
                &Event::Window(iced::window::Event::RedrawRequested(
                    now + std::time::Duration::from_millis(frame * 100),
                )),
                cursor,
            );
        }
    }

    /// Reads the same frame-local highlights used by response fills and region tints.
    fn highlights(&self) -> [f32; 4] {
        self.tree
            .state
            .downcast_ref::<PlotState<EventRenderer>>()
            .responses
            .borrow()
            .highlights()
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
fn hover_advances_only_on_redraw_and_stops_requesting_frames_after_fade_out() {
    let mut driver = Driver::new(overlay(), Size::new(932.0, 340.0));
    let cursor = mouse::Cursor::Available(Point::new(300.0, 100.0));
    let now = Instant::now();
    let first = driver.dispatch(&Event::Window(iced::window::Event::RedrawRequested(now)), cursor);
    assert_eq!(first.redraw, iced::window::RedrawRequest::NextFrame);
    assert_eq!(first.messages, []);
    assert_eq!(driver.highlights(), [0.0; 4]);
    let next = driver.dispatch(
        &Event::Window(iced::window::Event::RedrawRequested(
            now + std::time::Duration::from_millis(80),
        )),
        cursor,
    );
    assert_eq!(next.redraw, iced::window::RedrawRequest::NextFrame);
    let before = driver.highlights();
    assert!(
        before
            .get(1)
            .is_some_and(|highlight| *highlight > 0.63 && *highlight < 0.64)
    );
    let moved = driver.dispatch(
        &Event::Mouse(mouse::Event::CursorMoved {
            position: Point::new(600.0, 100.0),
        }),
        mouse::Cursor::Unavailable,
    );
    assert_eq!(moved.redraw, iced::window::RedrawRequest::NextFrame);
    assert_eq!(driver.highlights(), before);
    let neighbor = mouse::Cursor::Available(Point::new(600.0, 100.0));
    driver.settle_hover(neighbor, now + std::time::Duration::from_millis(80));
    assert_eq!(driver.highlights(), [0.0, 0.0, 1.0, 0.0]);
    let settled = driver.dispatch(
        &Event::Window(iced::window::Event::RedrawRequested(
            now + std::time::Duration::from_millis(780),
        )),
        neighbor,
    );
    assert_eq!(settled.redraw, iced::window::RedrawRequest::Wait);
    driver.rebuild(overlay().enabled(false));
    let fading = driver.dispatch(
        &Event::Window(iced::window::Event::RedrawRequested(
            now + std::time::Duration::from_millis(880),
        )),
        neighbor,
    );
    assert_eq!(fading.redraw, iced::window::RedrawRequest::NextFrame);
    assert_eq!(driver.highlights(), [0.0, 0.0, 1.0, 0.0]);
    driver.settle_hover(neighbor, now + std::time::Duration::from_millis(880));
    assert_eq!(driver.highlights(), [0.0; 4]);
    let settled = driver.dispatch(
        &Event::Window(iced::window::Event::RedrawRequested(
            now + std::time::Duration::from_secs(2),
        )),
        neighbor,
    );
    assert_eq!(settled.redraw, iced::window::RedrawRequest::Wait);
}

#[test]
fn animated_response_recolors_one_transient_cache_without_resampling_paths() {
    let cache = RefCell::new(responses::ResponseCache::<EventRenderer>::default());
    let bounds = Rectangle::new(Point::new(28.0, 40.0), Size::new(701.5, 200.0));
    let now = Instant::now();
    cache.borrow_mut().observe_hover(Some(1), now);
    let mut renderer = EventRenderer::default();
    let draw = |renderer: &mut EventRenderer| {
        responses::draw_bands(
            renderer,
            &cache,
            bounds,
            [100.0, 500.0, 5000.0],
            FrequencyScale::Mel,
            [false; 4],
        );
    };
    draw(&mut renderer);
    assert_eq!(renderer.frame_builds.get(), 0);
    cache
        .borrow_mut()
        .advance_hover(Some(1), now + std::time::Duration::from_millis(80));
    draw(&mut renderer);
    draw(&mut renderer);
    assert_eq!(renderer.frame_builds.get(), 1);
    assert_eq!(cache.borrow().builds, 1);
    let highlight = cache.borrow().highlights().get(1).copied().expect("highlighted band");
    assert!(renderer.quads.iter().any(|(_, background)| {
        matches!(background, iced::Background::Gradient(iced::Gradient::Linear(gradient))
            if gradient.stops.first().copied().flatten().is_some_and(|stop| 0.14_f32.mul_add(-highlight, stop.color.a).abs() < 1.0e-6))
    }));
    cache
        .borrow_mut()
        .advance_hover(Some(1), now + std::time::Duration::from_millis(160));
    draw(&mut renderer);
    assert_eq!(renderer.frame_builds.get(), 2);
    assert_eq!(cache.borrow().builds, 1);
    cache
        .borrow_mut()
        .observe_hover(None, now + std::time::Duration::from_millis(160));
    cache
        .borrow_mut()
        .advance_hover(None, now + std::time::Duration::from_millis(240));
    draw(&mut renderer);
    assert_eq!(renderer.frame_builds.get(), 3);
    assert_eq!(cache.borrow().builds, 1);
}

#[test]
fn position_geometry_reuses_hold_frames_and_only_recolors_paths_during_fade() {
    tests::load_annotation_font();
    let overlay = RefCell::new(responses::PositionOverlay::<EventRenderer>::default());
    let bounds = Rectangle::new(Point::new(28.0, 40.0), Size::new(701.5, 200.0));
    let now = Instant::now();
    overlay.borrow_mut().observe(Some((12.0, 32.0)), now);
    overlay.borrow_mut().observe(Some((10.0, 32.0)), now);
    let mut renderer = EventRenderer::default();
    for milliseconds in [0, 1000, 2000] {
        responses::draw_position_curve(
            &mut renderer,
            &overlay,
            Some((10.0, 32.0)),
            bounds,
            &bounds,
            now + std::time::Duration::from_millis(milliseconds),
        );
    }
    assert_eq!(renderer.geometry_count, 3);
    assert_eq!(renderer.frame_builds.get(), 1);
    let fading = now + std::time::Duration::from_millis(2750);
    for _ in 0..2 {
        responses::draw_position_curve(&mut renderer, &overlay, Some((10.0, 32.0)), bounds, &bounds, fading);
    }
    assert_eq!(renderer.geometry_count, 5);
    assert_eq!(renderer.frame_builds.get(), 2);
    responses::draw_position_curve(
        &mut renderer,
        &overlay,
        Some((10.0, 32.0)),
        bounds,
        &bounds,
        now + std::time::Duration::from_millis(3000),
    );
    assert_eq!(renderer.geometry_count, 5);
    assert_eq!(renderer.frame_builds.get(), 2);
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
    let mut driver = Driver::new(overlay().display(true, snapshot), size);
    let now = Instant::now();
    driver.settle_hover(cursor, now);
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
    assert!(renderer.quads.len() < 100);
    assert_eq!(renderer.geometry_count, 1);
    assert_eq!(renderer.frame_builds.get(), 1);
    assert_eq!(
        driver
            .tree
            .state
            .downcast_ref::<PlotState<EventRenderer>>()
            .responses
            .borrow()
            .builds,
        1
    );
    driver.element.as_widget().draw(
        &driver.tree,
        &mut renderer,
        &Theme::Dark,
        &renderer::Style::default(),
        Layout::new(&driver.node),
        cursor,
        &Rectangle::with_size(size),
    );
    assert_eq!(renderer.geometry_count, 2);
    assert_eq!(renderer.frame_builds.get(), 1);
    let neighbor = mouse::Cursor::Available(Point::new(frequency_x(8000.0, bounds, FrequencyScale::Mel), 100.0));
    for (index, cursor) in [neighbor, cursor].into_iter().enumerate() {
        driver.settle_hover(cursor, now + std::time::Duration::from_secs(index as u64 + 1));
        driver.element.as_widget().draw(
            &driver.tree,
            &mut renderer,
            &Theme::Dark,
            &renderer::Style::default(),
            Layout::new(&driver.node),
            cursor,
            &Rectangle::with_size(size),
        );
    }
    assert_eq!(renderer.frame_builds.get(), 2);
    snapshot.bands[1].width = 0.5;
    snapshot.bands[0].solo = 0.0;
    driver.rebuild(overlay().display(true, snapshot));
    driver.element.as_widget().draw(
        &driver.tree,
        &mut renderer,
        &Theme::Dark,
        &renderer::Style::default(),
        Layout::new(&driver.node),
        cursor,
        &Rectangle::with_size(size),
    );
    assert_eq!(renderer.frame_builds.get(), 2);
    assert_eq!(
        driver
            .tree
            .state
            .downcast_ref::<PlotState<EventRenderer>>()
            .responses
            .borrow()
            .builds,
        1
    );
}
