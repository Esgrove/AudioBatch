//! Font-independent C++ header symbols, contextual section disclosures, divider, and captured resize grip.

use iced::advanced::{Clipboard, Layout, Shell, Widget, layout, renderer, widget::tree};
use iced::{Color, Element, Event, Length, Point, Rectangle, Size, Theme, mouse};
use iced_baseview as iced;

use super::{Message, theme};

/// Vector glyphs use the same small paths and logical targets as the C++ editor.
#[derive(Clone, Copy)]
pub(super) enum Symbol {
    Settings,
    Undo,
    Redo,
    Chevron(bool),
    Headphones,
    Disclosure(bool),
    Divider,
    Resize,
    DisabledSlider,
}

/// One production widget supports both click-driven icons and raw-motion drag acquisition.
pub(super) struct Chrome {
    symbol: Symbol,
    size: Size,
    message: Option<Message>,
    active: bool,
}

#[derive(Default)]
struct Interaction {
    pressed: bool,
}

impl Chrome {
    pub(super) const fn new(symbol: Symbol, size: Size, message: Option<Message>, active: bool) -> Self {
        Self {
            symbol,
            size,
            message,
            active,
        }
    }
}

impl<Renderer: iced::advanced::Renderer> Widget<Message, Theme, Renderer> for Chrome {
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<Interaction>()
    }
    fn state(&self) -> tree::State {
        tree::State::new(Interaction::default())
    }
    fn size(&self) -> Size<Length> {
        Size::new(self.size.width.into(), self.size.height.into())
    }
    fn layout(&mut self, _: &mut tree::Tree, _: &Renderer, _: &layout::Limits) -> layout::Node {
        layout::Node::new(self.size)
    }
    fn update(
        &mut self,
        tree: &mut tree::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _: &Renderer,
        _: &mut dyn Clipboard,
        shell: &mut Shell<'_, Message>,
        _: &Rectangle,
    ) {
        if self.message.is_none() {
            return;
        }
        let state = tree.state.downcast_mut::<Interaction>();
        match event {
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) if cursor.is_over(layout.bounds()) => {
                state.pressed = true;
                if let Some(position) = cursor.position() {
                    match self.symbol {
                        Symbol::Resize => shell.publish(Message::ResizeStart(position)),
                        Symbol::Divider => shell.publish(Message::DividerStart(position)),
                        _ => {}
                    }
                }
                shell.capture_event();
            }
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)) if state.pressed => {
                state.pressed = false;
                if matches!(self.symbol, Symbol::Resize | Symbol::Divider) {
                    shell.publish(Message::EndGesture);
                } else if cursor.is_over(layout.bounds())
                    && let Some(message) = self.message.clone()
                {
                    shell.publish(message);
                }
                shell.capture_event();
            }
            Event::Window(iced::window::Event::Unfocused) => state.pressed = false,
            _ => {}
        }
    }
    fn mouse_interaction(
        &self,
        tree: &tree::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _: &Rectangle,
        _: &Renderer,
    ) -> mouse::Interaction {
        if self.message.is_some()
            && (tree.state.downcast_ref::<Interaction>().pressed || cursor.is_over(layout.bounds()))
        {
            match self.symbol {
                Symbol::Resize => mouse::Interaction::ResizingDiagonallyDown,
                Symbol::Divider => mouse::Interaction::ResizingVertically,
                _ => mouse::Interaction::Pointer,
            }
        } else {
            mouse::Interaction::None
        }
    }
    fn draw(
        &self,
        tree: &tree::Tree,
        renderer: &mut Renderer,
        _: &Theme,
        _: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        _: &Rectangle,
    ) {
        let bounds = layout.bounds();
        let hovered = cursor.is_over(bounds) || tree.state.downcast_ref::<Interaction>().pressed;
        let color = if self.message.is_none() {
            theme::GREY_MIDDLE_LIGHT.scale_alpha(0.3)
        } else if self.active && matches!(self.symbol, Symbol::Headphones) {
            theme::YELLOW
        } else if hovered {
            theme::GREY_SUPER_LIGHT
        } else {
            theme::GREY_MIDDLE_LIGHT
        };
        let center = bounds.center();
        match self.symbol {
            Symbol::Settings => settings_icon(renderer, center, color),
            Symbol::Undo | Symbol::Redo => history_icon(renderer, center, color, matches!(self.symbol, Symbol::Redo)),
            Symbol::Chevron(expanded) => {
                let sign: f32 = if expanded { -1.0 } else { 1.0 };
                let tip = Point::new(center.x, sign.mul_add(1.5, center.y));
                stroke(
                    renderer,
                    Point::new(center.x - 3.5, sign.mul_add(-1.5, center.y)),
                    tip,
                    1.0,
                    color,
                );
                stroke(
                    renderer,
                    tip,
                    Point::new(center.x + 3.5, sign.mul_add(-1.5, center.y)),
                    1.0,
                    color,
                );
            }
            Symbol::Headphones => {
                let radius = bounds.height * 0.29;
                let mut previous = Point::new(center.x - radius, center.y + 1.0);
                for step in 1..=20 {
                    let angle = std::f32::consts::PI + step as f32 * std::f32::consts::PI / 20.0;
                    let next = Point::new(
                        radius.mul_add(angle.cos(), center.x),
                        radius.mul_add(angle.sin(), center.y + 1.0),
                    );
                    stroke(renderer, previous, next, 1.5, color);
                    previous = next;
                }
                for x in [center.x - radius - 0.7, center.x + radius - 2.9] {
                    quad(
                        renderer,
                        Rectangle::new(Point::new(x, center.y), Size::new(3.6, 6.2)),
                        color,
                        1.2,
                    );
                }
            }
            Symbol::Disclosure(down) if hovered => disclosure_icon(renderer, center, down),
            Symbol::Divider if hovered => {
                quad(
                    renderer,
                    Rectangle::new(Point::new(center.x - 18.0, center.y - 1.0), Size::new(36.0, 2.0)),
                    theme::GREY_MIDDLE_LIGHT.scale_alpha(0.5),
                    1.0,
                );
            }
            Symbol::Resize => {
                for offset in [4.0, 8.0, 12.0] {
                    stroke(
                        renderer,
                        Point::new(bounds.x + bounds.width - offset, bounds.y + bounds.height - 2.0),
                        Point::new(bounds.x + bounds.width - 2.0, bounds.y + bounds.height - offset),
                        1.0,
                        color.scale_alpha(if hovered { 0.8 } else { 0.3 }),
                    );
                }
            }
            Symbol::DisabledSlider => {
                quad(
                    renderer,
                    Rectangle::new(
                        Point::new(bounds.x + 12.0, center.y - 3.0),
                        Size::new((bounds.width - 24.0).max(0.0), 6.0),
                    ),
                    theme::GREY_MEDIUM,
                    3.0,
                );
                quad(
                    renderer,
                    Rectangle::new(Point::new(center.x - 6.0, center.y - 6.0), Size::new(12.0, 12.0)),
                    theme::GREY_MIDDLE,
                    6.0,
                );
            }
            Symbol::Disclosure(_) | Symbol::Divider => {}
        }
    }
}

impl<'a, Renderer: iced::advanced::Renderer + 'a> From<Chrome> for Element<'a, Message, Theme, Renderer> {
    fn from(chrome: Chrome) -> Self {
        Self::new(chrome)
    }
}

/// Keeps collapsed and expanded disclosure triangles centered on the corresponding section border.
fn disclosure_icon(renderer: &mut impl iced::advanced::Renderer, center: Point, down: bool) {
    for row in 0..8 {
        let share = if down { 1.0 - row as f32 / 8.0 } else { row as f32 / 8.0 };
        quad(
            renderer,
            Rectangle::new(
                Point::new(share.mul_add(-18.0, center.x), center.y - 4.0 + row as f32),
                Size::new(share * 36.0, 1.0),
            ),
            theme::GREY_LIGHT.scale_alpha(0.18),
            0.0,
        );
    }
}

/// Reuses the C++ gear polygon and central circular cutout without relying on a Unicode font glyph.
fn settings_icon(renderer: &mut impl iced::advanced::Renderer, center: Point, color: Color) {
    for point in 0..32 {
        let angle = point as f32 * std::f32::consts::TAU / 32.0;
        let next = (point + 1) as f32 * std::f32::consts::TAU / 32.0;
        let radius: f32 = if point % 4 < 2 { 8.0 } else { 6.5 };
        let next_radius: f32 = if (point + 1) % 4 < 2 { 8.0 } else { 6.5 };
        stroke(
            renderer,
            Point::new(
                radius.mul_add(angle.cos(), center.x),
                radius.mul_add(angle.sin(), center.y),
            ),
            Point::new(
                next_radius.mul_add(next.cos(), center.x),
                next_radius.mul_add(next.sin(), center.y),
            ),
            1.5,
            color,
        );
    }
    circle_outline(renderer, center, 3.0, 1.5, color);
}

/// Samples the native curved-arrow Bézier, mirroring the same geometry for Redo.
fn history_icon(renderer: &mut impl iced::advanced::Renderer, center: Point, color: Color, redo: bool) {
    let direction = if redo { -1.0 } else { 1.0 };
    let origin = Point::new(7.0_f32.mul_add(-direction, center.x), center.y - 5.0);
    let mut previous = Point::new(12.0_f32.mul_add(direction, origin.x), origin.y + 11.0);
    for step in 1..=20 {
        let t = step as f32 / 20.0;
        let u = 1.0 - t;
        let first = 3.0 * u.powi(2) * t;
        let second = 3.0 * u * t.powi(2);
        let x = second.mul_add(7.0, first.mul_add(15.0, u.powi(3) * 12.0)) + t.powi(3);
        let y = t.powi(3).mul_add(6.0, first.mul_add(4.0, u.powi(3) * 11.0));
        let next = Point::new(x.mul_add(direction, origin.x), origin.y + y);
        stroke(renderer, previous, next, 1.5, color);
        previous = next;
    }
    stroke(
        renderer,
        Point::new(origin.x + direction, origin.y + 2.0),
        previous,
        1.5,
        color,
    );
    stroke(
        renderer,
        previous,
        Point::new(5.0_f32.mul_add(direction, origin.x), origin.y + 6.0),
        1.5,
        color,
    );
}

fn quad(renderer: &mut impl iced::advanced::Renderer, bounds: Rectangle, color: Color, radius: f32) {
    renderer.fill_quad(
        renderer::Quad {
            bounds,
            border: iced::Border {
                radius: radius.into(),
                ..iced::Border::default()
            },
            ..renderer::Quad::default()
        },
        color,
    );
}

fn stroke(renderer: &mut impl iced::advanced::Renderer, start: Point, end: Point, width: f32, color: Color) {
    let delta = end - start;
    let steps = (delta.x.hypot(delta.y) * 2.0).ceil().max(1.0) as u32;
    for step in 0..=steps {
        let share = step as f32 / steps as f32;
        quad(
            renderer,
            Rectangle::new(
                Point::new(
                    width.mul_add(-0.5, delta.x.mul_add(share, start.x)),
                    width.mul_add(-0.5, delta.y.mul_add(share, start.y)),
                ),
                Size::new(width, width),
            ),
            color,
            width * 0.5,
        );
    }
}

fn circle_outline(
    renderer: &mut impl iced::advanced::Renderer,
    center: Point,
    radius: f32,
    thickness: f32,
    color: Color,
) {
    renderer.fill_quad(
        renderer::Quad {
            bounds: Rectangle::new(
                Point::new(center.x - radius, center.y - radius),
                Size::new(radius * 2.0, radius * 2.0),
            ),
            border: iced::Border {
                color,
                width: thickness,
                radius: radius.into(),
            },
            ..renderer::Quad::default()
        },
        Color::TRANSPARENT,
    );
}
