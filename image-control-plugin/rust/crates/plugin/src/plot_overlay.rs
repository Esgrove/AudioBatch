//! GPU-rendered plot guides and real crossover hit testing over the persistent spectrogram shader.

use iced::advanced::Renderer as _;
use iced::advanced::text::Renderer as _;
use iced::advanced::{Clipboard, Layout, Shell, Widget, layout, renderer, text, widget::tree};
use iced::{Color, Element, Event, Length, Point, Rectangle, Size, Theme, mouse};
use iced_baseview as iced;

use super::{layout::PLOT_INSETS, theme};

/// Bounds shared by the logarithmic axis, hit testing, and DSP crossover controls.
const MIN_FREQUENCY: f64 = 20.0;
const MAX_FREQUENCY: f64 = 20_000.0;
const GRAB_DISTANCE: f32 = 6.0;
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
        let bounds = plot_bounds(layout.bounds());
        let interaction = tree.state.downcast_ref::<Interaction>();
        let hovered = interaction
            .dragging
            .or_else(|| hit_test(cursor, bounds, self.crossovers));
        for frequency in GRID_FREQUENCIES {
            let x = frequency_x(frequency, bounds);
            line(
                renderer,
                Rectangle::new(Point::new(x, bounds.y), Size::new(1.0, bounds.height)),
                theme::GREY_LIGHT.scale_alpha(0.1),
            );
            let label = if frequency >= 1000.0 {
                format!("{:.0}k", frequency * 0.001)
            } else {
                format!("{frequency:.0}")
            };
            label_at(
                renderer,
                label,
                Point::new(x - 20.0, bounds.y + bounds.height + 1.0),
                Size::new(40.0, 17.0),
                theme::GREY_MIDDLE_LIGHT,
                viewport,
            );
        }
        for (share, label) in [(0.0, "L"), (0.25, ""), (0.5, "C"), (0.75, ""), (1.0, "R")] {
            let y = f32::mul_add(share, bounds.height, bounds.y);
            line(
                renderer,
                Rectangle::new(Point::new(bounds.x, y), Size::new(bounds.width, 1.0)),
                theme::GREY_LIGHT.scale_alpha(if label == "C" { 0.22 } else { 0.1 }),
            );
            label_at(
                renderer,
                label.into(),
                Point::new(bounds.x - 26.0, y - 8.0),
                Size::new(22.0, 16.0),
                theme::GREY_MIDDLE_LIGHT,
                viewport,
            );
        }
        let mut left = bounds.x;
        for (band, name) in super::layout::BAND_NAMES.into_iter().enumerate() {
            let right = self
                .crossovers
                .get(band)
                .map_or(bounds.x + bounds.width, |frequency| frequency_x(*frequency, bounds));
            label_at(
                renderer,
                name.into(),
                Point::new(left, bounds.y + 4.0),
                Size::new((right - left).max(0.0), 16.0),
                theme::GREY_MIDDLE_LIGHT,
                viewport,
            );
            left = right;
        }
        for (index, frequency) in self.crossovers.into_iter().enumerate() {
            let x = frequency_x(frequency, bounds);
            let active = hovered == Some(index);
            line(
                renderer,
                Rectangle::new(
                    Point::new(x, bounds.y),
                    Size::new(if active { 2.0 } else { 1.0 }, bounds.height),
                ),
                theme::YELLOW.scale_alpha(if active { 0.9 } else { 0.4 }),
            );
            let label = super::format_frequency(frequency);
            let position = Point::new(
                (x + 4.0).min(bounds.x + bounds.width - 68.0),
                bounds.y + bounds.height - 24.0,
            );
            label_at(
                renderer,
                label,
                position,
                Size::new(68.0, 18.0),
                theme::YELLOW.scale_alpha(if active { 1.0 } else { 0.55 }),
                viewport,
            );
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
                let direction = match key {
                    iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowLeft) => -1.0,
                    iced::keyboard::Key::Named(iced::keyboard::key::Named::ArrowRight) => 1.0,
                    _ => return Vec::new(),
                };
                let Some(index) = self.selected else { return Vec::new() };
                let octaves = if modifiers.shift() { 1.0 / 120.0 } else { 1.0 / 12.0 };
                vec![
                    PlotEvent::Begin(index),
                    PlotEvent::Change(
                        index,
                        crossovers.get(index).copied().unwrap_or(MIN_FREQUENCY) * f64::exp2(direction * octaves),
                    ),
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

/// Emits only a geometric quad into Iced's renderer, never a CPU-generated pixel buffer.
fn line(renderer: &mut iced::Renderer, bounds: Rectangle, color: Color) {
    renderer.fill_quad(
        renderer::Quad {
            bounds,
            ..renderer::Quad::default()
        },
        color,
    );
}

/// Draws a short axis label with the shared font through the same GPU text renderer as other controls.
fn label_at(
    renderer: &mut iced::Renderer,
    content: String,
    position: Point,
    bounds: Size,
    color: Color,
    viewport: &Rectangle,
) {
    renderer.fill_text(
        text::Text {
            content,
            bounds,
            size: 11.0.into(),
            line_height: text::LineHeight::Relative(1.0),
            font: theme::TEXT,
            align_x: text::Alignment::Center,
            align_y: iced::alignment::Vertical::Center,
            shaping: text::Shaping::Basic,
            wrapping: text::Wrapping::None,
        },
        position,
        color,
        *viewport,
    );
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn logarithmic_axis_roundtrips_frequency() {
        let bounds = Rectangle::new(Point::new(28.0, 10.0), Size::new(900.0, 300.0));
        for frequency in GRID_FREQUENCIES {
            assert!((x_frequency(frequency_x(frequency, bounds), bounds) - frequency).abs() < frequency * 0.000_001);
        }
    }
}
