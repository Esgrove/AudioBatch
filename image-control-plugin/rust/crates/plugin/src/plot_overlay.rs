//! GPU-rendered plot guides and real crossover hit testing over the persistent spectrogram shader.

use iced::advanced::Renderer as _;
use iced::advanced::text::{Paragraph as _, Renderer as _};
use iced::advanced::{Clipboard, Layout, Shell, Widget, layout, renderer, text, widget::tree};
use iced::{Color, Element, Event, Length, Point, Rectangle, Size, Theme, mouse};
use iced_baseview as iced;

use super::{layout::PLOT_INSETS, theme};

/// Bounds shared by the logarithmic axis, hit testing, and DSP crossover controls.
const MIN_FREQUENCY: f64 = 20.0;
const MAX_FREQUENCY: f64 = 20_000.0;
const GRAB_DISTANCE: f32 = 6.0;
const LABEL_GAP: f32 = 4.0;
const PILL_BOTTOM_INSET: f32 = 7.5;
const GRID_ALPHA: f32 = 0.04;
const CENTER_GRID_ALPHA: f32 = 0.1;
const GRID_FREQUENCIES: [f64; 10] = [
    20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10_000.0, 20_000.0,
];

/// Editor messages retain only an index and a plain frequency, not references to renderer state.
#[derive(Clone, Copy, Debug)]
pub(super) enum PlotEvent {
    Begin(usize),
    Change(usize, f64),
    End,
}

/// A foreground guide widget has no image data and leaves analysis entirely in the shader beneath it.
pub(super) struct PlotOverlay<Message> {
    crossovers: [f64; 3],
    on_event: fn(PlotEvent) -> Message,
}

/// Pointer ownership survives view rebuilding and releases even when the pointer leaves the plot.
#[derive(Default)]
struct Interaction {
    dragging: Option<usize>,
    selected: Option<usize>,
}

impl<Message> PlotOverlay<Message> {
    /// Uses authoritative crossover readback for every new view.
    pub(super) const fn new(crossovers: [f64; 3], on_event: fn(PlotEvent) -> Message) -> Self {
        Self { crossovers, on_event }
    }
}

impl<Message> Widget<Message, Theme, iced::Renderer> for PlotOverlay<Message> {
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<Interaction>()
    }

    fn state(&self) -> tree::State {
        tree::State::new(Interaction::default())
    }

    fn size(&self) -> Size<Length> {
        Size::new(Length::Fill, Length::Fill)
    }

    fn layout(&mut self, _: &mut tree::Tree, _: &iced::Renderer, limits: &layout::Limits) -> layout::Node {
        layout::Node::new(limits.resolve(Length::Fill, Length::Fill, Size::ZERO))
    }

    fn update(
        &mut self,
        tree: &mut tree::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _: &iced::Renderer,
        _: &mut dyn Clipboard,
        shell: &mut Shell<'_, Message>,
        _: &Rectangle,
    ) {
        let interaction = tree.state.downcast_mut::<Interaction>();
        let bounds = plot_bounds(layout.bounds());
        let events = interaction.update(event, cursor, bounds, self.crossovers);
        if !events.is_empty() {
            shell.capture_event();
            for event in events {
                shell.publish((self.on_event)(event));
            }
        }
    }

    fn mouse_interaction(
        &self,
        tree: &tree::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _: &Rectangle,
        _: &iced::Renderer,
    ) -> mouse::Interaction {
        let interaction = tree.state.downcast_ref::<Interaction>();
        if interaction.dragging.is_some() || hit_test(cursor, plot_bounds(layout.bounds()), self.crossovers).is_some() {
            mouse::Interaction::ResizingHorizontally
        } else {
            mouse::Interaction::None
        }
    }

    fn draw(
        &self,
        tree: &tree::Tree,
        renderer: &mut iced::Renderer,
        _: &Theme,
        _: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        let outer = layout.bounds();
        let bounds = plot_bounds(outer);
        if bounds.width <= 0.0 || bounds.height <= 0.0 {
            return;
        }
        let interaction = tree.state.downcast_ref::<Interaction>();
        let hovered = interaction
            .dragging
            .or_else(|| hit_test(cursor, bounds, self.crossovers));
        for frequency in GRID_FREQUENCIES {
            line(
                renderer,
                Rectangle::new(
                    Point::new(frequency_x(frequency, bounds), bounds.y),
                    Size::new(1.0, bounds.height),
                ),
                bounds,
                theme::GREY_LIGHT.scale_alpha(GRID_ALPHA),
            );
        }
        let frequency_labels = frequency_labels(bounds, outer);
        let mut occupied = frequency_labels.iter().map(|(_, bounds)| *bounds).collect::<Vec<_>>();
        for (label, label_bounds) in frequency_labels {
            label_at(
                renderer,
                label,
                label_bounds,
                theme::GREY_LIGHT.scale_alpha(0.55),
                viewport,
            );
        }
        for (share, label) in [(0.0, "L"), (0.25, ""), (0.5, "C"), (0.75, ""), (1.0, "R")] {
            let y = f32::mul_add(share, bounds.height, bounds.y);
            line(
                renderer,
                Rectangle::new(Point::new(bounds.x, y), Size::new(bounds.width, 1.0)),
                bounds,
                theme::GREY_LIGHT.scale_alpha(if label == "C" { CENTER_GRID_ALPHA } else { GRID_ALPHA }),
            );
            if !label.is_empty()
                && let Some(label_bounds) = position_label_bounds(y, bounds, outer, label_size(label))
                    .filter(|candidate| !overlaps_labels(*candidate, &occupied))
            {
                label_at(renderer, label.into(), label_bounds, theme::GREY_MIDDLE_LIGHT, viewport);
                occupied.push(label_bounds);
            }
        }
        let mut left = bounds.x;
        for (band, name) in super::layout::BAND_NAMES.into_iter().enumerate() {
            let right = self
                .crossovers
                .get(band)
                .map_or(bounds.x + bounds.width, |frequency| frequency_x(*frequency, bounds));
            if let Some(label_bounds) = band_label_bounds(left, right, bounds, label_size(name))
                .filter(|candidate| !overlaps_labels(*candidate, &occupied))
            {
                label_at(
                    renderer,
                    name.into(),
                    label_bounds,
                    theme::GREY_LIGHT.scale_alpha(0.7),
                    viewport,
                );
                occupied.push(label_bounds);
            }
            left = right;
        }
        for (index, frequency) in self.crossovers.into_iter().enumerate() {
            let active = hovered == Some(index);
            line(
                renderer,
                Rectangle::new(
                    Point::new(frequency_x(frequency, bounds), bounds.y),
                    Size::new(if active { 2.0 } else { 1.0 }, bounds.height),
                ),
                bounds,
                theme::YELLOW.scale_alpha(if active { 0.9 } else { 0.4 }),
            );
        }
        // The captured marker's pill wins when adjacent crossovers leave room for only one annotation.
        let mut indices = [0, 1, 2];
        indices.sort_by_key(|index| hovered != Some(*index));
        for index in indices {
            let Some(frequency) = self.crossovers.get(index).copied() else {
                continue;
            };
            let label = super::format_frequency(frequency);
            if let Some(pill) =
                crossover_label_bounds(frequency_x(frequency, bounds), bounds, label_size(&label), &occupied)
            {
                crossover_label(renderer, label, pill, hovered == Some(index), viewport);
                occupied.push(pill);
            }
        }
    }
}

impl<'a, Message: 'a> From<PlotOverlay<Message>> for Element<'a, Message> {
    fn from(overlay: PlotOverlay<Message>) -> Self {
        Self::new(overlay)
    }
}

impl Interaction {
    /// Routes pointer and keyboard edits through exactly the same begin/change/end messages.
    fn update(
        &mut self,
        event: &Event,
        cursor: mouse::Cursor,
        bounds: Rectangle,
        crossovers: [f64; 3],
    ) -> Vec<PlotEvent> {
        match event {
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) => {
                self.selected = hit_test(cursor, bounds, crossovers);
                self.dragging = self.selected;
                self.dragging
                    .map_or_else(Vec::new, |index| vec![PlotEvent::Begin(index)])
            }
            Event::Mouse(mouse::Event::CursorMoved { position }) => self.dragging.map_or_else(Vec::new, |index| {
                vec![PlotEvent::Change(index, x_frequency(position.x, bounds))]
            }),
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))
            | Event::Window(iced::window::Event::Unfocused) => {
                if self.dragging.take().is_some() {
                    vec![PlotEvent::End]
                } else {
                    Vec::new()
                }
            }
            Event::Keyboard(iced::keyboard::Event::KeyPressed { key, modifiers, .. }) if cursor.is_over(bounds) => {
                let direction: f64 = match key {
                    iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowLeft) => -1.0,
                    iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowRight) => 1.0,
                    _ => return Vec::new(),
                };
                let Some(index) = self.selected else { return Vec::new() };
                let frequency = crossovers.get(index).copied().unwrap_or(MIN_FREQUENCY);
                let step = (frequency * if modifiers.shift() { 0.01 } else { 0.1 }).clamp(1.0, 1000.0);
                vec![
                    PlotEvent::Begin(index),
                    PlotEvent::Change(index, direction.mul_add(step, frequency)),
                ]
            }
            Event::Keyboard(iced::keyboard::Event::KeyReleased { .. }) if self.selected.is_some() => {
                vec![PlotEvent::End]
            }
            _ => Vec::new(),
        }
    }
}

/// Returns the same inner bounds used to inset the GPU shader behind these guides.
fn plot_bounds(bounds: Rectangle) -> Rectangle {
    Rectangle {
        x: bounds.x + PLOT_INSETS[3],
        y: bounds.y + PLOT_INSETS[0],
        width: (bounds.width - PLOT_INSETS[1] - PLOT_INSETS[3]).max(0.0),
        height: (bounds.height - PLOT_INSETS[0] - PLOT_INSETS[2]).max(0.0),
    }
}

/// Projects frequency without conflating the host's skewed normalized value with logarithmic screen position.
fn frequency_x(frequency: f64, bounds: Rectangle) -> f32 {
    let share =
        (frequency.clamp(MIN_FREQUENCY, MAX_FREQUENCY) / MIN_FREQUENCY).log(MAX_FREQUENCY / MIN_FREQUENCY) as f32;
    share.mul_add(bounds.width, bounds.x)
}

/// Clamps out-of-bounds pointer motion so a captured marker remains editable outside the plot.
fn x_frequency(x: f32, bounds: Rectangle) -> f64 {
    let share = f64::from(((x - bounds.x) / bounds.width.max(1.0)).clamp(0.0, 1.0));
    MIN_FREQUENCY * (MAX_FREQUENCY / MIN_FREQUENCY).powf(share)
}

/// Restricts marker ownership to the narrow existing crossover target rather than adding a separate control row.
fn hit_test(cursor: mouse::Cursor, bounds: Rectangle, crossovers: [f64; 3]) -> Option<usize> {
    let position = cursor.position().filter(|point| bounds.contains(*point))?;
    crossovers.into_iter().enumerate().find_map(|(index, frequency)| {
        ((position.x - frequency_x(frequency, bounds)).abs() <= GRAB_DISTANCE).then_some(index)
    })
}

/// Shares annotation shaping and alignment between measured geometry and the production GPU text call.
fn label_text<Content>(content: Content, bounds: Size) -> text::Text<Content> {
    text::Text {
        content,
        bounds,
        size: theme::ANNOTATION_SIZE.into(),
        line_height: text::LineHeight::Relative(1.0),
        font: theme::TEXT,
        align_x: text::Alignment::Center,
        align_y: iced::alignment::Vertical::Center,
        shaping: text::Shaping::Basic,
        wrapping: text::Wrapping::None,
    }
}

/// Measures the same embedded face and line height used to draw every plot annotation.
fn label_size(label: &str) -> Size {
    let paragraph = <iced::Renderer as text::Renderer>::Paragraph::with_text(label_text(
        label,
        Size::new(f32::INFINITY, theme::ANNOTATION_SIZE),
    ));
    Size::new(paragraph.min_width().ceil(), paragraph.min_height().ceil())
}

/// Keeps a complete annotation in its available area without squeezing or clipping its glyphs.
fn contained_label(center: Point, size: Size, area: Rectangle) -> Option<Rectangle> {
    if size.width <= 0.0 || size.height <= 0.0 || size.width > area.width || size.height > area.height {
        return None;
    }
    Some(Rectangle::new(
        Point::new(
            size.width
                .mul_add(-0.5, center.x)
                .clamp(area.x, area.x + area.width - size.width),
            size.height
                .mul_add(-0.5, center.y)
                .clamp(area.y, area.y + area.height - size.height),
        ),
        size,
    ))
}

/// Centers each band name between its markers, hiding names that cannot fit with a clear side inset.
fn band_label_bounds(left: f32, right: f32, plot: Rectangle, size: Size) -> Option<Rectangle> {
    let area = Rectangle::new(
        Point::new(left + LABEL_GAP, plot.y + LABEL_GAP),
        Size::new(
            LABEL_GAP.mul_add(-2.0, right - left).max(0.0),
            LABEL_GAP.mul_add(-2.0, plot.height).max(0.0),
        ),
    );
    contained_label(
        Point::new(left.midpoint(right), size.height.mul_add(0.5, area.y)),
        size,
        area,
    )
}

/// Keeps both endpoints readable before admitting non-overlapping interior ticks on compressed axes.
fn frequency_labels(plot: Rectangle, outer: Rectangle) -> Vec<(String, Rectangle)> {
    let mut labels = Vec::new();
    let mut occupied = Vec::new();
    let endpoints = [MIN_FREQUENCY, MAX_FREQUENCY];
    for frequency in endpoints.into_iter().chain(
        GRID_FREQUENCIES
            .into_iter()
            .filter(|frequency| *frequency > MIN_FREQUENCY && *frequency < MAX_FREQUENCY),
    ) {
        let label = if frequency >= 1000.0 {
            format!("{:.0}k", frequency * 0.001)
        } else {
            format!("{frequency:.0}")
        };
        if let Some(bounds) = frequency_label_bounds(frequency, plot, outer, label_size(&label))
            .filter(|candidate| !overlaps_labels(*candidate, &occupied))
        {
            occupied.push(bounds);
            labels.push((label, bounds));
        }
    }
    labels
}

/// Anchors ticks to their frequency and turns endpoint labels inward, leaving the stereo-letter gutter clear.
fn frequency_label_bounds(frequency: f64, plot: Rectangle, outer: Rectangle, size: Size) -> Option<Rectangle> {
    let area = Rectangle::new(
        Point::new(plot.x, plot.y + plot.height),
        Size::new(plot.width, (outer.y + outer.height - plot.y - plot.height).max(0.0)),
    );
    contained_label(Point::new(frequency_x(frequency, plot), area.center_y()), size, area)
}

/// Right-aligns L/C/R to a common gutter edge and keeps endpoint letters within the widget.
fn position_label_bounds(y: f32, plot: Rectangle, outer: Rectangle, size: Size) -> Option<Rectangle> {
    let area = Rectangle::new(
        outer.position(),
        Size::new((plot.x - outer.x - LABEL_GAP).max(0.0), outer.height),
    );
    contained_label(Point::new(size.width.mul_add(-0.5, area.x + area.width), y), size, area)
}

/// Reserves a small gap around all visible labels, including annotations in neighboring rows.
fn overlaps_labels(candidate: Rectangle, occupied: &[Rectangle]) -> bool {
    occupied.iter().any(|other| {
        candidate.x < other.x + other.width + LABEL_GAP
            && candidate.x + candidate.width + LABEL_GAP > other.x
            && candidate.y < other.y + other.height + LABEL_GAP
            && candidate.y + candidate.height + LABEL_GAP > other.y
    })
}

/// Tries both sides of a marker before omitting a pill that would overlap a neighboring annotation.
fn crossover_label_bounds(line_x: f32, plot: Rectangle, text_size: Size, occupied: &[Rectangle]) -> Option<Rectangle> {
    let size = Size::new(LABEL_GAP.mul_add(2.0, text_size.width), text_size.height + LABEL_GAP);
    let center_y = size.height.mul_add(-0.5, plot.y + plot.height - PILL_BOTTOM_INSET);
    let right = size.width.mul_add(0.5, line_x + LABEL_GAP);
    let left = size.width.mul_add(-0.5, line_x - LABEL_GAP);
    let centers = if line_x + LABEL_GAP + size.width <= plot.x + plot.width {
        [right, left]
    } else {
        [left, right]
    };
    centers
        .into_iter()
        .filter_map(|center_x| contained_label(Point::new(center_x, center_y), size, plot))
        .find(|candidate| !overlaps_labels(*candidate, occupied))
}

/// Draws a complete, measured crossover pill without changing its marker's pointer target.
fn crossover_label(renderer: &mut iced::Renderer, label: String, pill: Rectangle, active: bool, viewport: &Rectangle) {
    renderer.fill_quad(
        renderer::Quad {
            bounds: pill,
            border: iced::Border {
                radius: theme::CONTROL_RADIUS.into(),
                ..iced::Border::default()
            },
            ..renderer::Quad::default()
        },
        theme::GREY_DARK.scale_alpha(0.8),
    );
    label_at(
        renderer,
        label,
        pill,
        theme::YELLOW.scale_alpha(if active { 1.0 } else { 0.55 }),
        viewport,
    );
}

/// Keeps the complete thickness of endpoint guides inside the plot instead of extending into its gutters.
fn guide_bounds(guide: Rectangle, plot: Rectangle) -> Rectangle {
    let size = Size::new(guide.width.min(plot.width), guide.height.min(plot.height));
    Rectangle::new(
        Point::new(
            guide.x.clamp(plot.x, plot.x + plot.width - size.width),
            guide.y.clamp(plot.y, plot.y + plot.height - size.height),
        ),
        size,
    )
}

/// Emits only a contained geometric quad into Iced's renderer, never a CPU-generated pixel buffer.
fn line(renderer: &mut iced::Renderer, bounds: Rectangle, plot: Rectangle, color: Color) {
    renderer.fill_quad(
        renderer::Quad {
            bounds: guide_bounds(bounds, plot),
            ..renderer::Quad::default()
        },
        color,
    );
}

/// Couples centered text with its matching renderer anchor so placement tests exercise the production geometry.
fn label_placement(content: String, bounds: Rectangle) -> (text::Text, Point) {
    (label_text(content, bounds.size()), bounds.center())
}

/// Draws center-aligned text from its rectangle's center, as required by Iced's renderer anchor contract.
fn label_at(renderer: &mut iced::Renderer, content: String, bounds: Rectangle, color: Color, viewport: &Rectangle) {
    let (label, anchor) = label_placement(content, bounds);
    renderer.fill_text(label, anchor, color, *viewport);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Loads production font bytes into the renderer's shared shaper without a GPU or desktop window.
    fn load_annotation_font() {
        static LOADED: std::sync::Once = std::sync::Once::new();
        LOADED.call_once(|| {
            iced::advanced::graphics::text::font_system()
                .write()
                .expect("renderer font system")
                .load_font(std::borrow::Cow::Borrowed(theme::ROBOTO));
        });
    }

    /// Checks complete rectangles, not just centers, so guides and text cannot leak into neighboring regions.
    fn assert_contained(inner: Rectangle, outer: Rectangle) {
        let epsilon = 0.0001;
        assert!(inner.x >= outer.x - epsilon);
        assert!(inner.y >= outer.y - epsilon);
        assert!(inner.x + inner.width <= outer.x + outer.width + epsilon);
        assert!(inner.y + inner.height <= outer.y + outer.height + epsilon);
    }

    #[test]
    fn renderer_text_anchor_is_the_rectangle_center_for_names_and_pills() {
        for bounds in [
            Rectangle::new(Point::new(72.0, 31.0), Size::new(90.0, 11.0)),
            Rectangle::new(Point::new(400.0, 220.0), Size::new(42.0, 15.0)),
        ] {
            let (label, anchor) = label_placement("Low Mid".into(), bounds);
            assert_eq!(anchor, bounds.center());
            assert_eq!(label.bounds, bounds.size());
            assert_eq!(label.align_x, text::Alignment::Center);
            assert_eq!(label.align_y, iced::alignment::Vertical::Center);
            assert!((label.size.0 - theme::ANNOTATION_SIZE).abs() < f32::EPSILON);
        }
    }

    #[test]
    fn measured_band_names_center_between_markers_and_hide_in_narrow_bands() {
        load_annotation_font();
        let plot = Rectangle::new(Point::new(28.0, 30.0), Size::new(700.0, 200.0));
        let size = label_size("Low Mid");
        let label = band_label_bounds(100.0, 240.0, plot, size).expect("wide band");
        assert!((label.center_x() - 170.0).abs() < 0.0001);
        assert_contained(label, plot);
        assert!(band_label_bounds(100.0, 100.0 + size.width + LABEL_GAP, plot, size).is_none());
        assert!(band_label_bounds(100.0, 100.0, plot, size).is_none());
        assert!(band_label_bounds(100.0, 90.0, plot, size).is_none());
        let neighbor = band_label_bounds(240.0, 380.0, plot, label_size("High Mid")).expect("next band");
        assert!(!overlaps_labels(neighbor, &[label]));
    }

    #[test]
    fn frequency_labels_keep_endpoints_inside_and_suppress_overlapping_narrow_ticks() {
        load_annotation_font();
        for width in [80.0, 160.0, 744.0, 984.0] {
            let outer = Rectangle::new(Point::new(8.0, 84.0), Size::new(width, 160.0));
            let plot = plot_bounds(outer);
            let labels = frequency_labels(plot, outer);
            assert!(labels.iter().any(|(label, _)| label == "20"));
            assert!(labels.iter().any(|(label, _)| label == "20k"));
            let mut occupied = Vec::new();
            for (label, bounds) in &labels {
                assert_contained(*bounds, outer);
                assert!(bounds.y >= plot.y + plot.height);
                assert!(bounds.width >= label_size(label).width);
                assert!(!overlaps_labels(*bounds, &occupied));
                occupied.push(*bounds);
            }
            if width < 100.0 {
                assert!(labels.len() < GRID_FREQUENCIES.len());
            }
        }
        let outer = Rectangle::new(Point::new(8.0, 84.0), Size::new(744.0, 160.0));
        let plot = plot_bounds(outer);
        for frequency in [MIN_FREQUENCY, MAX_FREQUENCY] {
            let bounds = frequency_label_bounds(frequency, plot, outer, Size::new(40.0, 10.0)).expect("endpoint");
            assert_contained(bounds, outer);
        }
        let empty = Rectangle::with_size(Size::ZERO);
        assert_eq!(frequency_labels(empty, empty), Vec::new());
    }

    #[test]
    fn stereo_letters_share_a_right_aligned_gutter_and_clamp_at_both_ends() {
        let outer = Rectangle::new(Point::new(20.0, 40.0), Size::new(300.0, 90.0));
        let plot = Rectangle::new(Point::new(36.0, 42.0), Size::new(268.0, 86.0));
        let mut right_edges = Vec::new();
        for (y, width) in [(plot.y, 6.0), (plot.center_y(), 7.0), (plot.y + plot.height, 8.0)] {
            let bounds = position_label_bounds(y, plot, outer, Size::new(width, 12.0)).expect("gutter letter");
            assert_contained(bounds, outer);
            right_edges.push(bounds.x + bounds.width);
            assert!(bounds.x + bounds.width <= plot.x - LABEL_GAP);
        }
        assert!(right_edges.iter().all(|edge| (*edge - 32.0).abs() < 0.0001));
        let top = position_label_bounds(plot.y, plot, outer, Size::new(6.0, 12.0)).expect("top letter");
        let bottom =
            position_label_bounds(plot.y + plot.height, plot, outer, Size::new(8.0, 12.0)).expect("bottom letter");
        assert!((top.y - outer.y).abs() < 0.0001);
        assert!((bottom.y + bottom.height - outer.y - outer.height).abs() < 0.0001);
        assert!(position_label_bounds(plot.y, plot, outer, Size::new(20.0, 12.0)).is_none());
    }

    #[test]
    fn endpoint_guides_keep_their_full_thickness_inside_the_plot() {
        for size in [Size::new(700.0, 200.0), Size::new(0.5, 0.25), Size::ZERO] {
            let plot = Rectangle::new(Point::new(28.0, 30.0), size);
            for guide in [
                Rectangle::new(Point::new(plot.x + plot.width, plot.y), Size::new(2.0, plot.height)),
                Rectangle::new(Point::new(plot.x, plot.y + plot.height), Size::new(plot.width, 1.0)),
            ] {
                assert_contained(guide_bounds(guide, plot), plot);
            }
        }
    }

    #[test]
    fn adjacent_crossover_pills_try_opposite_sides_then_hide_instead_of_colliding() {
        let plot = Rectangle::new(Point::new(28.0, 30.0), Size::new(300.0, 200.0));
        let size = Size::new(32.0, 10.0);
        let first = crossover_label_bounds(180.0, plot, size, &[]).expect("first pill");
        let second = crossover_label_bounds(181.0, plot, size, &[first]).expect("opposite-side pill");
        assert!(first.x > 180.0);
        assert!(second.x + second.width < 181.0);
        assert!(!overlaps_labels(second, &[first]));
        assert!(crossover_label_bounds(182.0, plot, size, &[first, second]).is_none());
        for bounds in [first, second] {
            assert_contained(bounds, plot);
            let (_, anchor) = label_placement("1k".into(), bounds);
            assert_eq!(anchor, bounds.center());
        }
        let narrow = Rectangle::new(plot.position(), Size::new(32.0, 200.0));
        assert!(crossover_label_bounds(narrow.center_x(), narrow, size, &[]).is_none());
        let short = Rectangle::new(plot.position(), Size::new(300.0, 10.0));
        assert!(crossover_label_bounds(short.center_x(), short, size, &[]).is_none());
        let occupied = Rectangle::new(Point::new(plot.x, plot.y + 178.0), Size::new(plot.width, 15.0));
        assert!(crossover_label_bounds(180.0, plot, size, &[occupied]).is_none());
    }

    #[test]
    fn production_font_metrics_fit_complete_crossover_text_at_both_plot_edges() {
        load_annotation_font();
        let plot = Rectangle::new(Point::new(28.0, 30.0), Size::new(700.0, 200.0));
        for label in ["20 Hz", "100 Hz", "1.00 kHz", "20.0 kHz"] {
            let size = label_size(label);
            assert!(size.width > 0.0);
            assert!(size.height > 0.0);
            for line_x in [plot.x, plot.x + plot.width] {
                let pill = crossover_label_bounds(line_x, plot, size, &[]).expect("edge pill");
                assert_contained(pill, plot);
                assert!(pill.width >= LABEL_GAP.mul_add(2.0, size.width));
                assert!(pill.height >= size.height + LABEL_GAP);
                let (text, anchor) = label_placement(label.into(), pill);
                let paragraph = <iced::Renderer as text::Renderer>::Paragraph::with_text(text.as_ref());
                assert!(paragraph.min_width() <= pill.width);
                assert!(paragraph.min_height() <= pill.height);
                assert_eq!(anchor, pill.center());
            }
        }
    }

    #[test]
    fn pointer_drag_captures_only_a_marker_and_releases_outside_plot() {
        let bounds = Rectangle::new(Point::new(28.0, 10.0), Size::new(900.0, 300.0));
        let crossovers = [100.0, 500.0, 5000.0];
        let mut interaction = Interaction::default();
        let cursor = mouse::Cursor::Available(Point::new(frequency_x(500.0, bounds), 100.0));
        let events = interaction.update(
            &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
            cursor,
            bounds,
            crossovers,
        );
        assert!(matches!(events.as_slice(), [PlotEvent::Begin(1)]));
        let events = interaction.update(
            &Event::Mouse(mouse::Event::CursorMoved {
                position: Point::new(2000.0, 900.0),
            }),
            cursor,
            bounds,
            crossovers,
        );
        assert!(
            matches!(events.as_slice(), [PlotEvent::Change(1, frequency)] if (*frequency - MAX_FREQUENCY).abs() < 0.001)
        );
        let events = interaction.update(
            &Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
            mouse::Cursor::Unavailable,
            bounds,
            crossovers,
        );
        assert!(matches!(events.as_slice(), [PlotEvent::End]));
        assert!(interaction.dragging.is_none());
    }

    #[test]
    fn empty_plot_click_does_not_open_parameter_edit() {
        let bounds = Rectangle::with_size(Size::new(900.0, 300.0));
        let mut interaction = Interaction::default();
        let cursor = mouse::Cursor::Available(Point::new(50.0, 100.0));
        assert!(
            interaction
                .update(
                    &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
                    cursor,
                    bounds,
                    [100.0, 500.0, 5000.0]
                )
                .is_empty()
        );
        assert!(interaction.dragging.is_none());
    }

    #[test]
    fn hidden_narrow_pills_keep_marker_selection_and_keyboard_gestures() {
        let bounds = Rectangle::new(Point::new(28.0, 30.0), Size::new(32.0, 200.0));
        let crossovers = [100.0, 500.0, 5000.0];
        assert!(crossover_label_bounds(frequency_x(500.0, bounds), bounds, Size::new(32.0, 10.0), &[]).is_none());
        let cursor = mouse::Cursor::Available(Point::new(frequency_x(500.0, bounds), bounds.center_y()));
        let mut interaction = Interaction::default();
        assert!(matches!(
            interaction
                .update(
                    &Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)),
                    cursor,
                    bounds,
                    crossovers
                )
                .as_slice(),
            [PlotEvent::Begin(1)]
        ));
        let _ = interaction.update(
            &Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
            cursor,
            bounds,
            crossovers,
        );
        let key = iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowRight);
        for (modifiers, expected) in [
            (iced::keyboard::Modifiers::default(), 550.0),
            (iced::keyboard::Modifiers::SHIFT, 505.0),
        ] {
            let press = Event::Keyboard(iced::keyboard::Event::KeyPressed {
                key: key.clone(),
                modified_key: key.clone(),
                physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowRight),
                location: iced::keyboard::Location::Standard,
                modifiers,
                text: None,
                repeat: false,
            });
            assert!(matches!(
                interaction.update(&press, cursor, bounds, crossovers).as_slice(),
                [PlotEvent::Begin(1), PlotEvent::Change(1, frequency)] if (*frequency - expected).abs() < 0.0001
            ));
        }
        let release = Event::Keyboard(iced::keyboard::Event::KeyReleased {
            key: key.clone(),
            modified_key: key,
            physical_key: iced::keyboard::key::Physical::Code(iced::keyboard::key::Code::ArrowRight),
            location: iced::keyboard::Location::Standard,
            modifiers: iced::keyboard::Modifiers::default(),
        });
        assert!(matches!(
            interaction.update(&release, cursor, bounds, crossovers).as_slice(),
            [PlotEvent::End]
        ));
    }

    #[test]
    fn logarithmic_axis_roundtrips_frequency() {
        let bounds = Rectangle::new(Point::new(28.0, 10.0), Size::new(900.0, 300.0));
        for frequency in GRID_FREQUENCIES {
            assert!((x_frequency(frequency_x(frequency, bounds), bounds) - frequency).abs() < frequency * 0.000_001);
        }
    }
}
