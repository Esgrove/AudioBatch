//! Retained correlation meters sample atomic audio publications on native redraws without editor messages.

use std::sync::Arc;
use std::time::{Duration, Instant};

use iced::advanced::{Clipboard, Layout, Shell, Widget, layout, renderer, text, widget::tree};
use iced::{Color, Element, Event, Length, Point, Rectangle, Size, Theme, mouse};
use iced_baseview as iced;
use image_control_dsp::SharedState;

use super::super::{layout as editor_layout, theme};
use super::CorrelationObservation;
use crate::correlation_display::{MeterHistory, RANGE_INTENSITY_SAMPLES};

const HORIZONTAL_PADDING: f32 = 10.0;
const VERTICAL_PADDING: f32 = 2.0;
const LABEL_HEIGHT: f32 = 17.0;
const TRACK_HEIGHT: f32 = 5.0;
const SCALE_GAP: f32 = 2.0;
const TEXT_SIZE: f32 = 11.0;
const TEXT_LINE_HEIGHT: f32 = 1.3;

/// Immutable view description. Presence in the retained tree controls the meter's visible lifetime.
pub(super) struct CorrelationMeter {
    shared: Arc<SharedState>,
    band: usize,
}

/// UI-owned, bounded presentation state survives ordinary view rebuilding and never touches audio history.
struct MeterState {
    shared: Arc<SharedState>,
    band: usize,
    observation: CorrelationObservation,
    history: MeterHistory,
    updated: Option<Instant>,
    generation: Option<u64>,
}

impl CorrelationMeter {
    /// Retains only shared atomic publication ownership, without reading or locking mutable audio processing.
    pub(super) const fn new(shared: Arc<SharedState>, band: usize) -> Self {
        Self { shared, band }
    }
}

impl MeterState {
    /// Starts unavailable so a new or reinserted meter must observe actual publication progress.
    fn new(meter: &CorrelationMeter) -> Self {
        Self {
            shared: Arc::clone(&meter.shared),
            band: meter.band,
            observation: CorrelationObservation::default(),
            history: MeterHistory::default(),
            updated: None,
            generation: None,
        }
    }

    /// Drops transient display data when clipped out, without changing optional audio measurement enablement.
    fn hide(&mut self) {
        self.observation = CorrelationObservation::default();
        self.history.clear();
        self.updated = None;
        self.generation = None;
    }

    /// Samples partition-rate atomics on a native frame and advances display history using its actual timestamp.
    fn redraw(&mut self, now: Instant) {
        let snapshot = self.shared.correlation_snapshot();
        if self.generation != Some(snapshot.generation) {
            self.history.clear();
            self.updated = None;
            self.generation = Some(snapshot.generation);
        }
        self.observation.observe(snapshot, now);
        let reading = self
            .observation
            .readings(now, Duration::from_secs_f32(self.shared.correlation_expiry_seconds()))
            .get(self.band)
            .copied()
            .flatten();
        let elapsed = self
            .updated
            .map_or(0.0, |updated| now.saturating_duration_since(updated).as_secs_f64());
        self.updated = Some(now);
        self.history.update(
            reading,
            elapsed,
            f64::from(self.shared.display_preferences().persistence_seconds),
        );
    }
}

impl<Message, Renderer> Widget<Message, Theme, Renderer> for CorrelationMeter
where
    Renderer: text::Renderer<Font = iced::Font>,
{
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<MeterState>()
    }

    fn state(&self) -> tree::State {
        tree::State::new(MeterState::new(self))
    }

    /// Stable publication ownership and band identity preserve history, not numeric-mode preferences.
    fn diff(&self, tree: &mut tree::Tree) {
        let state = tree.state.downcast_mut::<MeterState>();
        if state.band != self.band || !Arc::ptr_eq(&state.shared, &self.shared) {
            *state = MeterState::new(self);
        }
    }

    fn size(&self) -> Size<Length> {
        Size::new(Length::Fill, Length::Fixed(editor_layout::CORRELATION_HEIGHT))
    }

    fn layout(&mut self, _: &mut tree::Tree, _: &Renderer, limits: &layout::Limits) -> layout::Node {
        layout::Node::new(limits.resolve(Length::Fill, editor_layout::CORRELATION_HEIGHT, Size::ZERO))
    }

    /// Requests the next native frame while visible, independently of plot visibility and application refreshes.
    fn update(
        &mut self,
        tree: &mut tree::Tree,
        event: &Event,
        layout: Layout<'_>,
        _: mouse::Cursor,
        _: &Renderer,
        _: &mut dyn Clipboard,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        let state = tree.state.downcast_mut::<MeterState>();
        if !visible(layout.bounds(), viewport) {
            state.hide();
            return;
        }
        if let Event::Window(iced::window::Event::RedrawRequested(now)) = event {
            state.redraw(*now);
            shell.request_redraw();
        }
    }

    /// Draws the same track, bounded range cells, markers, and labels directly, with no per-cell widget tree.
    fn draw(
        &self,
        tree: &tree::Tree,
        renderer: &mut Renderer,
        _: &Theme,
        _: &renderer::Style,
        layout: Layout<'_>,
        _: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let bounds = layout.bounds();
        let Some(clip) = bounds.intersection(viewport).filter(|_| visible(bounds, viewport)) else {
            return;
        };
        let state = tree.state.downcast_ref::<MeterState>();
        let reading = state.history.snapshot();
        let content = Rectangle::new(
            Point::new(bounds.x + HORIZONTAL_PADDING, bounds.y + VERTICAL_PADDING),
            Size::new(HORIZONTAL_PADDING.mul_add(-2.0, bounds.width).max(0.0), bounds.height),
        );
        let label = Rectangle {
            height: LABEL_HEIGHT,
            ..content
        };
        let number = reading.value(self.shared.display_preferences().smooth_correlation);
        label_at(
            renderer,
            "Correlation".into(),
            label,
            theme::TEXT,
            text::Alignment::Left,
            &clip,
        );
        label_at(
            renderer,
            number.map_or_else(String::new, |value| format!("{value:+.2}")),
            label,
            theme::MONO,
            text::Alignment::Right,
            &clip,
        );
        let track = Rectangle {
            y: content.y + LABEL_HEIGHT,
            height: TRACK_HEIGHT,
            ..content
        };
        renderer.with_layer(clip, |renderer| {
            quad(renderer, track, theme::GREY_MEDIUM);
            for (sample, intensity) in reading.range_intensity.into_iter().enumerate() {
                let left = sample as f32 / RANGE_INTENSITY_SAMPLES as f32;
                let right = (sample + 1) as f32 / RANGE_INTENSITY_SAMPLES as f32;
                quad(
                    renderer,
                    Rectangle {
                        x: left.mul_add(track.width, track.x),
                        width: (right - left) * track.width,
                        ..track
                    },
                    theme::BLUE.scale_alpha(intensity * 0.35),
                );
            }
            if let Some(value) = reading.trend {
                quad(renderer, marker(track, value, 9.0), theme::YELLOW.scale_alpha(0.6));
            }
            if let Some(value) = reading.live {
                quad(
                    renderer,
                    marker(track, value, 3.0),
                    if value < 0.0 { theme::ORANGE } else { theme::BLUE },
                );
            }
        });
        let scale = Rectangle {
            y: track.y + TRACK_HEIGHT + SCALE_GAP,
            height: TEXT_SIZE * TEXT_LINE_HEIGHT,
            ..content
        };
        for (label, alignment) in [
            ("-1", text::Alignment::Left),
            ("0", text::Alignment::Center),
            ("+1", text::Alignment::Right),
        ] {
            label_at(renderer, label.into(), scale, theme::TEXT, alignment, &clip);
        }
    }
}

impl<'a, Message: 'a, Renderer> From<CorrelationMeter> for Element<'a, Message, Theme, Renderer>
where
    Renderer: text::Renderer<Font = iced::Font> + 'a,
{
    fn from(meter: CorrelationMeter) -> Self {
        Self::new(meter)
    }
}

/// Rejects empty and fully clipped meters before scheduling further native frames.
fn visible(bounds: Rectangle, viewport: &Rectangle) -> bool {
    bounds.width > HORIZONTAL_PADDING * 2.0
        && bounds.height > 0.0
        && bounds
            .intersection(viewport)
            .is_some_and(|clip| clip.width > 0.0 && clip.height > 0.0)
}

/// Preserves the retained row's rounded proportional marker positions, including its one-part endpoint inset.
fn marker(track: Rectangle, value: f32, width: f32) -> Rectangle {
    let position = ((value + 1.0) * 500.0).round();
    let before = position.max(1.0);
    let after = (1000.0 - position).max(1.0);
    Rectangle {
        x: (track.width - width)
            .max(0.0)
            .mul_add(before / (before + after), track.x),
        width: width.min(track.width),
        ..track
    }
}

/// Emits geometric fields into the renderer, never a CPU image or retained child widget.
fn quad(renderer: &mut impl iced::advanced::Renderer, bounds: Rectangle, color: Color) {
    renderer.fill_quad(
        renderer::Quad {
            bounds,
            ..renderer::Quad::default()
        },
        color,
    );
}

/// Uses the original correlation label fonts, line metrics, alignment anchors, and muted foreground color.
fn label_at(
    renderer: &mut impl text::Renderer<Font = iced::Font>,
    content: String,
    bounds: Rectangle,
    font: iced::Font,
    alignment: text::Alignment,
    clip: &Rectangle,
) {
    let x = match alignment {
        text::Alignment::Left | text::Alignment::Default | text::Alignment::Justified => bounds.x,
        text::Alignment::Center => bounds.center_x(),
        text::Alignment::Right => bounds.x + bounds.width,
    };
    renderer.fill_text(
        text::Text {
            content,
            bounds: bounds.size(),
            size: TEXT_SIZE.into(),
            line_height: text::LineHeight::Relative(TEXT_LINE_HEIGHT),
            font,
            align_x: alignment,
            align_y: iced::alignment::Vertical::Top,
            shaping: text::Shaping::Basic,
            wrapping: text::Wrapping::None,
        },
        Point::new(x, bounds.y),
        theme::GREY_MIDDLE_LIGHT,
        *clip,
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    use image_control_dsp::Imager;

    /// Records semantic primitives from the production generic widget without a GPU or pixels.
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
            TEXT_SIZE.into()
        }
        fn fill_paragraph(&mut self, _: &Self::Paragraph, _: Point, _: Color, _: Rectangle) {}
        fn fill_editor(&mut self, _: &Self::Editor, _: Point, _: Color, _: Rectangle) {}
        fn fill_text(&mut self, text: text::Text, _: Point, _: Color, _: Rectangle) {
            self.labels.push(text.content);
        }
    }

    /// Owns the actual retained widget and tree, with no application update loop or refresh message.
    struct Driver {
        element: Element<'static, (), Theme, EventRenderer>,
        tree: tree::Tree,
        node: layout::Node,
        viewport: Rectangle,
    }

    impl Driver {
        /// Creates the production generic widget and resolves its fill-width, fixed-height layout.
        fn new(shared: &Arc<SharedState>) -> Self {
            let mut element: Element<'static, (), Theme, EventRenderer> =
                CorrelationMeter::new(Arc::clone(shared), 0).into();
            let mut tree = tree::Tree::new(&element);
            let node = element.as_widget_mut().layout(
                &mut tree,
                &EventRenderer::default(),
                &layout::Limits::new(Size::ZERO, Size::new(240.0, 100.0)),
            );
            let viewport = Rectangle::with_size(node.size());
            Self {
                element,
                tree,
                node,
                viewport,
            }
        }

        /// Dispatches real widget events, checking that they never rebuild the application or publish messages.
        fn dispatch(&mut self, event: &Event) -> iced::window::RedrawRequest {
            let mut messages = Vec::new();
            let mut shell = Shell::new(&mut messages);
            self.element.as_widget_mut().update(
                &mut self.tree,
                event,
                Layout::new(&self.node),
                mouse::Cursor::Unavailable,
                &EventRenderer::default(),
                &mut iced::advanced::clipboard::Null,
                &mut shell,
                &self.viewport,
            );
            assert!(shell.is_empty());
            assert!(!shell.is_layout_invalid());
            assert!(!shell.are_widgets_invalid());
            assert!(!shell.is_event_captured());
            shell.redraw_request()
        }

        /// Sends a native frame timestamp directly, without ordinary editor polling.
        fn redraw(&mut self, now: Instant) -> iced::window::RedrawRequest {
            self.dispatch(&Event::Window(iced::window::Event::RedrawRequested(now)))
        }

        /// Exercises the real primitive path with a CPU recorder instead of rasterizing glyphs or pixels.
        fn draw(&self) -> EventRenderer {
            let mut renderer = EventRenderer::default();
            self.element.as_widget().draw(
                &self.tree,
                &mut renderer,
                &Theme::Dark,
                &renderer::Style {
                    text_color: theme::GREY_LIGHT,
                },
                Layout::new(&self.node),
                mouse::Cursor::Unavailable,
                &self.viewport,
            );
            renderer
        }

        /// Rebuilds only the immutable view description, preserving its one retained state node.
        fn rebuild(&mut self, shared: &Arc<SharedState>, band: usize) {
            self.element = CorrelationMeter::new(Arc::clone(shared), band).into();
            self.tree.diff(self.element.as_widget());
        }

        /// Borrows the widget-owned history used by production drawing.
        fn state(&self) -> &MeterState {
            self.tree.state.downcast_ref::<MeterState>()
        }
    }

    /// Generates actual band publications through the unchanged DSP, outside the widget event path.
    fn publish(imager: &mut Imager, begin: usize, frames: usize) {
        imager.begin_block();
        for sample in begin..begin + frames {
            let value = (sample as f32 * 0.0013)
                .sin()
                .mul_add(0.3, (sample as f32 * 0.13).sin() * 0.2);
            let _ = imager.process_frame([value, value]);
        }
    }

    /// Prepares real partition-rate measurement without an analysis worker or any hardware dependencies.
    fn measured_imager() -> Imager {
        let mut imager = Imager::new();
        assert!(imager.prepare(48_000.0));
        imager.shared().set_correlation_enabled(true);
        publish(&mut imager, 0, 16_384);
        imager
    }

    /// Establishes the generation boundary, then delivers a fresh actual publication to seed history.
    fn seed(driver: &mut Driver, imager: &mut Imager, now: Instant) {
        assert_eq!(driver.redraw(now), iced::window::RedrawRequest::NextFrame);
        assert_eq!(driver.state().history.live(), None);
        publish(imager, 16_384, 512);
        assert_eq!(driver.redraw(now), iced::window::RedrawRequest::NextFrame);
        assert_eq!(driver.state().history.live(), Some(1.0));
    }

    #[test]
    fn native_120_and_144_hz_sample_each_frame_without_refresh_or_plot() {
        for rate in [120, 144] {
            let mut imager = measured_imager();
            let shared = imager.shared();
            let mut driver = Driver::new(&shared);
            let now = Instant::now();
            seed(&mut driver, &mut imager, now);
            let history = &mut driver.tree.state.downcast_mut::<MeterState>().history;
            history.clear();
            history.update(Some(-1.0), 0.0, 0.3);
            let mut expected = -1.0_f64;
            let mut previous = now;
            for frame in 1..=rate {
                publish(&mut imager, 16_896 + frame * 512, 512);
                let time = now + Duration::from_secs_f64(frame as f64 / rate as f64);
                assert_eq!(driver.redraw(time), iced::window::RedrawRequest::NextFrame);
                let snapshot = driver.state().history.snapshot();
                assert_eq!(snapshot.live, Some(1.0));
                assert_eq!(
                    driver.state().observation.revision,
                    shared.correlation_snapshot().revision
                );
                let elapsed = time.saturating_duration_since(previous).as_secs_f64();
                let weight = -(-elapsed / f64::from(shared.display_preferences().persistence_seconds)).exp_m1();
                expected = weight.mul_add(1.0 - expected, expected);
                assert!((f64::from(snapshot.trend.expect("native frame has a trend")) - expected).abs() < 2.0e-6);
                previous = time;
            }
            assert!(driver.tree.children.is_empty());
            assert_eq!(driver.node.size(), Size::new(240.0, editor_layout::CORRELATION_HEIGHT));
        }
    }

    #[test]
    fn repeated_redraws_and_other_events_do_not_keep_stopped_publications_fresh() {
        let mut imager = measured_imager();
        let mut driver = Driver::new(&imager.shared());
        let now = Instant::now();
        seed(&mut driver, &mut imager, now);
        assert_eq!(
            driver.dispatch(&Event::Window(iced::window::Event::Focused)),
            iced::window::RedrawRequest::Wait
        );
        for milliseconds in [50, 100, 150, 200, 249] {
            driver.redraw(now + Duration::from_millis(milliseconds));
            assert_eq!(driver.state().history.live(), Some(1.0));
        }
        driver.redraw(now + Duration::from_millis(250));
        assert_eq!(
            driver.state().history.snapshot(),
            crate::correlation_display::MeterReading::default()
        );
        assert_eq!(driver.draw().labels, ["Correlation", "", "-1", "0", "+1"]);
    }

    /// Builds the production modal barrier beneath the same full-window dismissal shield and foreground callout.
    fn modal_meter(shared: &Arc<SharedState>, blocked: bool) -> Element<'static, (), Theme, EventRenderer> {
        use iced::widget::{Space, container, mouse_area, pin, stack};

        let background: Element<'static, (), Theme, EventRenderer> =
            CorrelationMeter::new(Arc::clone(shared), 0).into();
        let mut content = stack![crate::editor::controls::interaction::modal_background(
            background, blocked
        )]
        .width(240.0)
        .height(100.0);
        if blocked {
            content = content
                .push(mouse_area(container(Space::new()).width(Length::Fill).height(Length::Fill)).on_press(()))
                .push(pin(container(Space::new()).width(80.0).height(30.0)).x(150.0).y(60.0));
        }
        content.into()
    }

    /// Dispatches a native frame through the assembled retained interface without editor refresh messages.
    fn redraw_modal(
        interface: &mut iced::runtime::UserInterface<'_, (), Theme, EventRenderer>,
        now: Instant,
    ) -> EventRenderer {
        let mut renderer = EventRenderer::default();
        let mut messages = Vec::new();
        let (state, _) = interface.update(
            &[Event::Window(iced::window::Event::RedrawRequested(now))],
            mouse::Cursor::Unavailable,
            &mut renderer,
            &mut iced::advanced::clipboard::Null,
            &mut messages,
        );
        assert!(
            messages.is_empty(),
            "monitor frames must not publish application messages"
        );
        assert!(matches!(
            state,
            iced::runtime::user_interface::State::Updated {
                redraw_request: iced::window::RedrawRequest::NextFrame,
                ..
            }
        ));
        interface.draw(
            &mut renderer,
            &Theme::Dark,
            &renderer::Style {
                text_color: theme::GREY_LIGHT,
            },
            mouse::Cursor::Unavailable,
        );
        renderer
    }

    #[test]
    fn assembled_modal_keeps_live_meter_generation_and_expiry_advancing_without_refreshes() {
        let mut imager = measured_imager();
        let shared = imager.shared();
        let size = Size::new(240.0, 100.0);
        let mut interface = iced::runtime::UserInterface::build(
            modal_meter(&shared, false),
            size,
            iced::runtime::user_interface::Cache::new(),
            &mut EventRenderer::default(),
        );
        let now = Instant::now();
        let unavailable = ["Correlation", "", "-1", "0", "+1"];
        let available = ["Correlation", "+1.00", "-1", "0", "+1"];
        assert_eq!(redraw_modal(&mut interface, now).labels, unavailable);
        publish(&mut imager, 16_384, 512);
        assert_eq!(redraw_modal(&mut interface, now).labels, available);

        // Preserve the live native tree when opening the callout, just as the production editor does.
        let cache = interface.into_cache();
        let mut interface =
            iced::runtime::UserInterface::build(modal_meter(&shared, true), size, cache, &mut EventRenderer::default());
        assert_eq!(
            redraw_modal(&mut interface, now + Duration::from_millis(10)).labels,
            available
        );
        shared.invalidate_monitoring();
        assert_eq!(
            redraw_modal(&mut interface, now + Duration::from_millis(20)).labels,
            unavailable
        );
        publish(&mut imager, 16_896, 512);
        assert_eq!(
            redraw_modal(&mut interface, now + Duration::from_millis(30)).labels,
            available
        );
        assert_eq!(
            redraw_modal(&mut interface, now + Duration::from_millis(279)).labels,
            available
        );
        assert_eq!(
            redraw_modal(&mut interface, now + Duration::from_millis(280)).labels,
            unavailable
        );
        publish(&mut imager, 17_408, 512);
        assert_eq!(
            redraw_modal(&mut interface, now + Duration::from_millis(290)).labels,
            available
        );
        shared.set_correlation_enabled(false);
        assert_eq!(
            redraw_modal(&mut interface, now + Duration::from_millis(300)).labels,
            unavailable
        );
    }

    #[test]
    fn prepared_host_block_allowance_controls_expiry() {
        let mut imager = measured_imager();
        imager.shared().set_maximum_block_frames(24_000);
        let mut driver = Driver::new(&imager.shared());
        let now = Instant::now();
        seed(&mut driver, &mut imager, now);
        driver.redraw(now + Duration::from_millis(999));
        assert_eq!(driver.state().history.live(), Some(1.0));
        driver.redraw(now + Duration::from_secs(1));
        assert_eq!(
            driver.state().history.snapshot(),
            crate::correlation_display::MeterReading::default()
        );
    }

    #[test]
    fn generation_changes_clear_history_and_require_fresh_publication() {
        let mut imager = measured_imager();
        let shared = imager.shared();
        let mut driver = Driver::new(&shared);
        let now = Instant::now();
        seed(&mut driver, &mut imager, now);
        shared.invalidate_monitoring();
        driver.redraw(now + Duration::from_millis(10));
        assert_eq!(
            driver.state().history.snapshot(),
            crate::correlation_display::MeterReading::default()
        );
        publish(&mut imager, 16_896, 512);
        driver.redraw(now + Duration::from_millis(20));
        assert_eq!(driver.state().history.range(), Some((1.0, 1.0)));
        shared.set_correlation_enabled(false);
        driver.redraw(now + Duration::from_millis(30));
        assert_eq!(
            driver.state().history.snapshot(),
            crate::correlation_display::MeterReading::default()
        );
    }

    #[test]
    fn clipped_or_removed_widgets_drop_history_and_stop_requesting_frames() {
        let mut imager = measured_imager();
        let shared = imager.shared();
        let mut driver = Driver::new(&shared);
        let now = Instant::now();
        seed(&mut driver, &mut imager, now);
        driver.viewport = Rectangle::new(Point::new(1000.0, 1000.0), Size::new(240.0, 48.0));
        assert_eq!(driver.redraw(now), iced::window::RedrawRequest::Wait);
        assert_eq!(
            driver.state().history.snapshot(),
            crate::correlation_display::MeterReading::default()
        );
        driver.viewport = Rectangle::with_size(driver.node.size());
        driver.redraw(now);
        assert_eq!(driver.state().history.live(), None);
        publish(&mut imager, 16_896, 512);
        driver.redraw(now);
        assert_eq!(driver.state().history.live(), Some(1.0));
        driver.tree = tree::Tree::empty();
        driver.tree.diff(driver.element.as_widget());
        driver.redraw(now);
        assert_eq!(
            driver.state().history.snapshot(),
            crate::correlation_display::MeterReading::default()
        );
    }

    #[test]
    fn rebuild_retains_history_but_band_and_instance_identity_do_not() {
        let mut imager = measured_imager();
        let shared = imager.shared();
        let mut driver = Driver::new(&shared);
        let now = Instant::now();
        seed(&mut driver, &mut imager, now);
        let before = driver.state().history.snapshot();
        driver.rebuild(&shared, 0);
        assert_eq!(driver.state().history.snapshot(), before);
        driver.rebuild(&shared, 1);
        assert_eq!(
            driver.state().history.snapshot(),
            crate::correlation_display::MeterReading::default()
        );
        driver.rebuild(&Imager::new().shared(), 1);
        assert_eq!(
            driver.state().history.snapshot(),
            crate::correlation_display::MeterReading::default()
        );
    }

    #[test]
    fn direct_drawing_selects_live_numeric_preference_without_resetting_history() {
        let mut imager = measured_imager();
        let shared = imager.shared();
        let mut driver = Driver::new(&shared);
        let now = Instant::now();
        seed(&mut driver, &mut imager, now);
        driver
            .tree
            .state
            .downcast_mut::<MeterState>()
            .history
            .update(Some(-1.0), 0.3, 0.3);
        let before = driver.state().history.snapshot();
        let publication = shared.correlation_snapshot();
        for smooth in [false, true, false] {
            let mut preferences = shared.display_preferences();
            preferences.smooth_correlation = smooth;
            assert!(shared.set_display_preferences(preferences));
            let output = driver.draw();
            assert_eq!(
                output.labels,
                [
                    "Correlation".to_owned(),
                    format!("{:+.2}", before.value(smooth).expect("seeded number")),
                    "-1".to_owned(),
                    "0".to_owned(),
                    "+1".to_owned(),
                ]
            );
            assert_eq!(output.quads.len(), RANGE_INTENSITY_SAMPLES + 3);
            assert!(output.quads.iter().all(|(quad, _)| {
                quad.x >= 10.0
                    && quad.x + quad.width <= 230.001
                    && (quad.y - 19.0).abs() < f32::EPSILON
                    && (quad.height - 5.0).abs() < f32::EPSILON
            }));
            assert_eq!(driver.state().history.snapshot(), before);
            assert_eq!(shared.correlation_snapshot().revision, publication.revision);
            assert_eq!(shared.correlation_snapshot().generation, publication.generation);
        }
        assert!(driver.tree.children.is_empty());
    }

    #[test]
    fn redraw_and_draw_do_not_consume_or_change_audio_measurements() {
        let mut imager = measured_imager();
        let shared = imager.shared();
        let before = shared.correlation_snapshot();
        let mut driver = Driver::new(&shared);
        let now = Instant::now();
        driver.redraw(now);
        publish(&mut imager, 16_384, 512);
        let publication = shared.correlation_snapshot();
        driver.redraw(now);
        let _ = driver.draw();
        driver.rebuild(&shared, 0);
        assert!(publication.revision > before.revision);
        let after = shared.correlation_snapshot();
        assert_eq!(after.revision, publication.revision);
        assert_eq!(after.generation, publication.generation);
        assert_eq!(after.readings.map(f32::to_bits), publication.readings.map(f32::to_bits));
    }
}
