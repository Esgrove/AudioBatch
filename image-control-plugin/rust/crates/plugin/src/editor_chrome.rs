//! Font-independent C++ header symbols, contextual section disclosures, divider, and captured resize grip.

use std::time::Instant;

use iced::advanced::graphics::geometry;
use iced::advanced::widget::{Id, Operation, operation::Focusable, tree};
use iced::advanced::{Clipboard, Layout, Shell, Widget, layout, renderer};
use iced::widget::canvas::{Frame, LineCap, LineJoin, Path, Stroke, path};
use iced::{Color, Element, Event, Length, Point, Rectangle, Size, Theme, Vector, keyboard, mouse};
use iced_baseview as iced;

use super::{Message, theme};

/// Section disclosures share the native editor's centered artwork, proximity, and finite fade durations.
const DISCLOSURE_SIZE: Size = Size::new(36.0, 8.0);
const DISCLOSURE_PROXIMITY: Size = Size::new(72.0, 20.0);
const DISCLOSURE_ALPHA: f32 = 0.18;
const DISCLOSURE_PRESSED_ALPHA: f32 = 0.3;
const DISCLOSURE_FADE_IN_SECONDS: f32 = 0.14;
const DISCLOSURE_FADE_OUT_SECONDS: f32 = 0.1;

/// Retained display-only fade advances on Iced redraws without timers or application messages.
#[derive(Default)]
struct DisclosureReveal {
    revealed: bool,
    progress: f32,
    last_frame: Option<Instant>,
}

impl DisclosureReveal {
    /// Immediately changes the proximity target while preserving a partially completed fade.
    const fn retarget(&mut self, revealed: bool, now: Instant) {
        if self.revealed != revealed {
            self.revealed = revealed;
            self.last_frame = Some(now);
        }
    }

    /// Advances the bounded linear progress using elapsed time, including the full exit duration.
    fn advance(&mut self, now: Instant) {
        if let Some(previous) = self.last_frame {
            let elapsed = now.saturating_duration_since(previous).as_secs_f32();
            let step = elapsed
                / if self.revealed {
                    DISCLOSURE_FADE_IN_SECONDS
                } else {
                    DISCLOSURE_FADE_OUT_SECONDS
                };
            self.progress = (self.progress + if self.revealed { step } else { -step }).clamp(0.0, 1.0);
        }
        self.last_frame = Some(now);
    }

    /// Requests frames only while the finite transition has not reached its current endpoint.
    fn animating(&self) -> bool {
        if self.revealed {
            self.progress < 1.0
        } else {
            self.progress > 0.0
        }
    }

    /// Applies the native smoothstep easing independently of pointer-acquired or keyboard focus.
    fn opacity(&self, pressed: bool) -> f32 {
        let eased = self.progress * self.progress * 2.0_f32.mul_add(-self.progress, 3.0);
        eased
            * if pressed {
                DISCLOSURE_PRESSED_ALPHA
            } else {
                DISCLOSURE_ALPHA
            }
    }
}

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
}

/// One production widget supports both click-driven icons and raw-motion drag acquisition.
pub(super) struct Chrome {
    symbol: Symbol,
    size: Size,
    message: Option<Message>,
    active: bool,
    id: Id,
}

/// Focus operations own keyboard focus, while pointer acquisition keeps its outline quiet.
#[derive(Default)]
struct Interaction {
    pressed: bool,
    focused: bool,
    keyboard_focus: bool,
    window_inactive: bool,
    disclosure: DisclosureReveal,
}

impl Focusable for Interaction {
    fn is_focused(&self) -> bool {
        self.focused
    }

    fn focus(&mut self) {
        self.focused = true;
        self.keyboard_focus = true;
    }

    fn unfocus(&mut self) {
        self.focused = false;
        self.keyboard_focus = false;
    }
}

impl Symbol {
    /// Symbol identity does not depend on the current expanded or selected presentation.
    const fn default_id(self) -> &'static str {
        match self {
            Self::Settings => "settings",
            Self::Undo => "undo",
            Self::Redo => "redo",
            Self::Chevron(_) => "processing-toolbar-disclosure",
            Self::Headphones => "solo",
            Self::Disclosure(_) => "section-disclosure",
            Self::Divider => "split-divider",
            Self::Resize => "resize-grip",
        }
    }
}

impl Chrome {
    pub(super) fn new(symbol: Symbol, size: Size, message: Option<Message>, active: bool) -> Self {
        Self {
            symbol,
            size,
            message,
            active,
            id: symbol.default_id().into(),
        }
    }

    /// Gives repeated disclosures and per-band Solo controls independent stable focus targets.
    pub(super) fn id(mut self, id: impl Into<Id>) -> Self {
        self.id = id.into();
        self
    }

    /// Only actionable controls participate in native traversal. Host resize remains pointer-owned.
    const fn focusable(&self) -> bool {
        self.message.is_some() && !matches!(self.symbol, Symbol::Resize)
    }
}

impl<Renderer: geometry::Renderer> Widget<Message, Theme, Renderer> for Chrome {
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<Interaction>()
    }
    fn state(&self) -> tree::State {
        tree::State::new(Interaction::default())
    }
    /// Disabled actions cannot retain a keyboard target or revive an unfinished pointer activation.
    fn diff(&self, tree: &mut tree::Tree) {
        if self.message.is_none() {
            *tree.state.downcast_mut::<Interaction>() = Interaction::default();
        }
    }
    fn size(&self) -> Size<Length> {
        Size::new(self.size.width.into(), self.size.height.into())
    }
    fn layout(&mut self, _: &mut tree::Tree, _: &Renderer, _: &layout::Limits) -> layout::Node {
        layout::Node::new(self.size)
    }
    /// Registers the real retained focus state with Iced's focus and Tab traversal operations.
    fn operate(&mut self, tree: &mut tree::Tree, layout: Layout<'_>, _: &Renderer, operation: &mut dyn Operation) {
        if self.focusable() {
            operation.focusable(
                Some(&self.id),
                layout.bounds(),
                tree.state.downcast_mut::<Interaction>(),
            );
        }
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
                state.focused = self.focusable();
                state.keyboard_focus = false;
                shell.request_redraw();
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
                shell.request_redraw();
                if matches!(self.symbol, Symbol::Resize | Symbol::Divider) {
                    shell.publish(Message::EndGesture);
                } else if cursor.is_over(layout.bounds())
                    && let Some(message) = self.message.clone()
                {
                    shell.publish(message);
                }
                shell.capture_event();
            }
            Event::Mouse(mouse::Event::ButtonPressed(_)) => {
                state.unfocus();
                shell.request_redraw();
            }
            Event::Keyboard(keyboard::Event::KeyPressed {
                key, modifiers, repeat, ..
            }) if state.focused
                && !state.window_inactive
                && !modifiers
                    .intersects(keyboard::Modifiers::CTRL | keyboard::Modifiers::ALT | keyboard::Modifiers::LOGO) =>
            {
                if matches!(self.symbol, Symbol::Divider) && !state.pressed {
                    let step = if modifiers.shift() { 1.0 } else { 8.0 };
                    let delta = match key {
                        keyboard::Key::Named(keyboard::key::Named::ArrowUp) => Some(-step),
                        keyboard::Key::Named(keyboard::key::Named::ArrowDown) => Some(step),
                        _ => None,
                    };
                    if let Some(delta) = delta {
                        shell.publish(Message::DividerNudge(delta));
                        shell.capture_event();
                    }
                } else if !matches!(self.symbol, Symbol::Resize | Symbol::Divider)
                    && matches!(
                        key,
                        keyboard::Key::Named(keyboard::key::Named::Space | keyboard::key::Named::Enter)
                    )
                {
                    if !repeat && let Some(message) = self.message.clone() {
                        shell.publish(message);
                    }
                    shell.capture_event();
                }
            }
            Event::Window(iced::window::Event::Unfocused) => {
                state.pressed = false;
                state.window_inactive = true;
                shell.request_redraw();
            }
            Event::Window(iced::window::Event::Focused) => {
                state.window_inactive = false;
                shell.request_redraw();
            }
            Event::Mouse(mouse::Event::CursorMoved { .. } | mouse::Event::CursorLeft) => {
                shell.request_redraw();
            }
            _ => {}
        }
        if matches!(self.symbol, Symbol::Disclosure(_)) {
            let now = if let Event::Window(iced::window::Event::RedrawRequested(now)) = event {
                state.disclosure.advance(*now);
                *now
            } else {
                Instant::now()
            };
            let revealed = !state.window_inactive
                && !matches!(event, Event::Mouse(mouse::Event::CursorLeft))
                && cursor.is_over(disclosure_proximity(layout.bounds()));
            let changed = state.disclosure.revealed != revealed;
            state.disclosure.retarget(revealed, now);
            if changed || state.disclosure.animating() {
                shell.request_redraw();
            }
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
        let state = tree.state.downcast_ref::<Interaction>();
        let focused = self.focusable() && state.focused && state.keyboard_focus && !state.window_inactive;
        let hovered = cursor.is_over(bounds) || state.pressed || focused;
        if matches!(self.symbol, Symbol::Undo | Symbol::Redo) {
            quad(
                renderer,
                bounds.shrink(1.0),
                history_background(self.message.is_some(), hovered, state.pressed),
                3.0,
            );
        }
        if focused {
            renderer.fill_quad(
                renderer::Quad {
                    bounds: bounds.shrink(1.0),
                    border: iced::Border {
                        color: theme::GREY_MIDDLE_LIGHT.scale_alpha(0.55),
                        width: 1.0,
                        radius: 3.0.into(),
                    },
                    ..renderer::Quad::default()
                },
                Color::TRANSPARENT,
            );
        }
        let color = if self.message.is_none() {
            theme::GREY_MIDDLE_LIGHT.scale_alpha(0.3)
        } else if self.active && matches!(self.symbol, Symbol::Headphones) {
            theme::YELLOW
        } else if hovered {
            theme::GREY_SUPER_LIGHT
        } else {
            theme::GREY_MIDDLE_LIGHT
        };
        draw_symbol(
            renderer,
            self.symbol,
            bounds,
            color,
            hovered,
            state.disclosure.opacity(state.pressed),
        );
    }
}

/// Draws smooth local canvas paths while leaving focus and hit geometry in the retained widget.
fn draw_symbol<Renderer: geometry::Renderer>(
    renderer: &mut Renderer,
    symbol: Symbol,
    bounds: Rectangle,
    color: Color,
    hovered: bool,
    disclosure_opacity: f32,
) {
    if bounds.width <= 0.0 || bounds.height <= 0.0 {
        return;
    }
    let mut frame = Frame::new(renderer, bounds.size());
    let center = Point::new(bounds.width * 0.5, bounds.height * 0.5);
    match symbol {
        Symbol::Settings => {
            frame.stroke(&settings_path(center), icon_stroke(color, 1.5));
            frame.stroke(&Path::circle(center, 3.0), icon_stroke(color, 1.5));
        }
        Symbol::Undo | Symbol::Redo => {
            frame.stroke(
                &history_path(center, matches!(symbol, Symbol::Redo)),
                icon_stroke(color, 1.5),
            );
        }
        Symbol::Chevron(expanded) => {
            let sign: f32 = if expanded { -1.0 } else { 1.0 };
            let path = Path::new(|path| {
                path.move_to(Point::new(center.x - 3.5, sign.mul_add(-1.5, center.y)));
                path.line_to(Point::new(center.x, sign.mul_add(1.5, center.y)));
                path.line_to(Point::new(center.x + 3.5, sign.mul_add(-1.5, center.y)));
            });
            frame.stroke(&path, icon_stroke(color, 1.0));
        }
        Symbol::Headphones => {
            let radius = bounds.height * 0.29;
            let headband = Path::new(|path| {
                path.arc(path::Arc {
                    center: Point::new(center.x, center.y + 1.0),
                    radius,
                    start_angle: std::f32::consts::PI.into(),
                    end_angle: std::f32::consts::TAU.into(),
                });
            });
            frame.stroke(&headband, icon_stroke(color, 1.5));
            for x in [center.x - radius - 0.7, center.x + radius - 2.9] {
                frame.fill(
                    &Path::rounded_rectangle(Point::new(x, center.y), Size::new(3.6, 6.2), 1.2.into()),
                    color,
                );
            }
        }
        Symbol::Disclosure(down) if disclosure_opacity > 0.0 => {
            frame.fill(
                &disclosure_path(center, down),
                theme::GREY_LIGHT.scale_alpha(disclosure_opacity),
            );
        }
        Symbol::Divider if hovered => {
            frame.fill(
                &Path::rounded_rectangle(
                    Point::new(center.x - 18.0, center.y - 1.0),
                    Size::new(36.0, 2.0),
                    1.0.into(),
                ),
                theme::GREY_MIDDLE_LIGHT.scale_alpha(0.5),
            );
        }
        Symbol::Resize => {
            let path = Path::new(|path| {
                for offset in [4.0, 8.0, 12.0] {
                    path.move_to(Point::new(bounds.width - offset, bounds.height - 2.0));
                    path.line_to(Point::new(bounds.width - 2.0, bounds.height - offset));
                }
            });
            frame.stroke(
                &path,
                icon_stroke(color.scale_alpha(if hovered { 0.8 } else { 0.3 }), 1.0),
            );
        }
        Symbol::Disclosure(_) | Symbol::Divider => return,
    }
    let geometry = frame.into_geometry();
    renderer.with_translation(Vector::new(bounds.x, bounds.y), |renderer| {
        renderer.draw_geometry(geometry);
    });
}

impl<'a, Renderer: geometry::Renderer + 'a> From<Chrome> for Element<'a, Message, Theme, Renderer> {
    fn from(chrome: Chrome) -> Self {
        Self::new(chrome)
    }
}

/// Keeps a disclosure's narrow reveal neighbourhood independent of its unchanged pointer hit target.
fn disclosure_proximity(bounds: Rectangle) -> Rectangle {
    Rectangle::new(
        Point::new(
            DISCLOSURE_PROXIMITY.width.mul_add(-0.5, bounds.center_x()),
            DISCLOSURE_PROXIMITY.height.mul_add(-0.5, bounds.center_y()),
        ),
        DISCLOSURE_PROXIMITY,
    )
}

/// Gives history actions restrained boxes without adding selected-state or layout changes.
fn history_background(enabled: bool, hovered: bool, pressed: bool) -> Color {
    if !enabled {
        theme::GREY_MEDIUM_DARK.scale_alpha(0.5)
    } else if pressed {
        theme::GREY_MIDDLE.scale_alpha(0.3)
    } else if hovered {
        theme::GREY_MEDIUM
    } else {
        theme::GREY_MEDIUM_DARK
    }
}

/// Rounded joins and caps keep small vector symbols continuous at fractional backing scales.
fn icon_stroke(color: Color, width: f32) -> Stroke<'static> {
    Stroke::default()
        .with_color(color)
        .with_width(width)
        .with_line_cap(LineCap::Round)
        .with_line_join(LineJoin::Round)
}

/// Builds one filled triangle centered on the section border rather than a stack of opaque strips.
fn disclosure_path(center: Point, down: bool) -> Path {
    let half_width = DISCLOSURE_SIZE.width * 0.5;
    let half_height = DISCLOSURE_SIZE.height * 0.5;
    let tip_offset = if down { half_height } else { -half_height };
    Path::new(|path| {
        path.move_to(Point::new(center.x - half_width, center.y - tip_offset));
        path.line_to(Point::new(center.x, center.y + tip_offset));
        path.line_to(Point::new(center.x + half_width, center.y - tip_offset));
        path.close();
    })
}

/// Retains the native eight-tooth polygon as a single closed outline.
fn settings_path(center: Point) -> Path {
    Path::new(|path| {
        for point in 0..32 {
            let angle = point as f32 * std::f32::consts::TAU / 32.0;
            let radius: f32 = if point % 4 < 2 { 8.0 } else { 6.5 };
            let position = Point::new(
                radius.mul_add(angle.cos(), center.x),
                radius.mul_add(angle.sin(), center.y),
            );
            if point == 0 {
                path.move_to(position);
            } else {
                path.line_to(position);
            }
        }
        path.close();
    })
}

/// Preserves the native cubic and open arrowhead, with exact horizontal reflection for Redo.
fn history_path(center: Point, redo: bool) -> Path {
    let direction = if redo { -1.0 } else { 1.0 };
    let point = |x: f32, y: f32| Point::new((x - 7.0).mul_add(direction, center.x), center.y - 5.0 + y);
    Path::new(|path| {
        path.move_to(point(12.0, 11.0));
        path.bezier_curve_to(point(15.0, 4.0), point(7.0, 0.0), point(1.0, 6.0));
        path.move_to(point(1.0, 2.0));
        path.line_to(point(1.0, 6.0));
        path.line_to(point(5.0, 6.0));
    })
}

/// Fills a rounded chrome rectangle through the editor's retained renderer.
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

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::*;
    use path::lyon_path::Event as PathEvent;

    /// Checks the continuous cubic path independently of renderer tessellation or pixels.
    #[test]
    fn history_is_one_cubic_with_a_separate_open_arrowhead() {
        let center = Point::new(12.0, 12.0);
        let events: Vec<_> = history_path(center, false).raw().iter().collect();
        let [
            PathEvent::Begin { .. },
            PathEvent::Cubic { from, ctrl1, ctrl2, to },
            PathEvent::End { close: false, .. },
            PathEvent::Begin { .. },
            PathEvent::Line { .. },
            PathEvent::Line { .. },
            PathEvent::End { close: false, .. },
        ] = events.as_slice()
        else {
            unreachable!("history path must preserve the cubic and open arrowhead");
        };
        assert_eq!(*from, path::lyon_path::math::point(17.0, 18.0));
        assert_eq!(*ctrl1, path::lyon_path::math::point(20.0, 11.0));
        assert_eq!(*ctrl2, path::lyon_path::math::point(12.0, 7.0));
        assert_eq!(*to, path::lyon_path::math::point(6.0, 13.0));
        let redo: Vec<_> = history_path(center, true).raw().iter().collect();
        let PathEvent::Cubic {
            from: redo_from,
            ctrl1: redo_first,
            ctrl2: redo_second,
            to: redo_to,
        } = redo.get(1).expect("redo cubic")
        else {
            unreachable!("redo must preserve the cubic");
        };
        for (undo, redo) in [
            (from, redo_from),
            (ctrl1, redo_first),
            (ctrl2, redo_second),
            (to, redo_to),
        ] {
            assert!(center.x.mul_add(-2.0, undo.x + redo.x).abs() < f32::EPSILON);
            assert!((undo.y - redo.y).abs() < f32::EPSILON);
        }
    }

    /// Filled disclosure triangles have precisely three edges and reverse their tip without moving.
    #[test]
    fn disclosure_is_a_centered_closed_triangle_in_each_direction() {
        let center = Point::new(50.0, 10.0);
        for (down, expected_tip) in [(true, 14.0), (false, 6.0)] {
            let events: Vec<_> = disclosure_path(center, down).raw().iter().collect();
            let [
                PathEvent::Begin { .. },
                PathEvent::Line { to, .. },
                PathEvent::Line { .. },
                PathEvent::End { close: true, .. },
            ] = events.as_slice()
            else {
                unreachable!("disclosure must have one closed triangle");
            };
            assert_eq!(*to, path::lyon_path::math::point(center.x, expected_tip));
        }
    }

    /// Proximity follows the small native reveal region, not an arbitrary full-width action target.
    #[test]
    fn disclosure_reveal_proximity_is_independent_of_hit_target_width() {
        let bounds = Rectangle::new(Point::new(100.0, 200.0), Size::new(800.0, 20.0));
        let proximity = disclosure_proximity(bounds);
        assert_eq!(proximity.size(), Size::new(72.0, 20.0));
        assert_eq!(proximity.center(), bounds.center());
        assert!(proximity.contains(bounds.center()));
        assert!(!proximity.contains(Point::new(bounds.x + 10.0, bounds.center_y())));
    }

    /// Native smoothstep fades finish within 140 ms in and 100 ms out, with no delayed release tail.
    #[test]
    fn disclosure_fade_uses_eased_alpha_and_bounded_exit() {
        let start = Instant::now();
        let mut reveal = DisclosureReveal::default();
        reveal.retarget(true, start);
        reveal.advance(start + Duration::from_millis(70));
        assert!((reveal.opacity(false) - 0.09).abs() < 0.000_001);
        assert!((reveal.opacity(true) - 0.15).abs() < 0.000_001);
        reveal.advance(start + Duration::from_millis(140));
        assert!((reveal.opacity(false) - 0.18).abs() < 0.000_001);
        assert!(!reveal.animating());
        reveal.retarget(false, start + Duration::from_millis(150));
        reveal.advance(start + Duration::from_millis(200));
        assert!((reveal.opacity(false) - 0.09).abs() < 0.000_001);
        reveal.advance(start + Duration::from_millis(250));
        assert!(reveal.opacity(false).abs() < f32::EPSILON);
        assert!(!reveal.animating());
    }

    /// Interrupted fades reverse immediately from their current progress instead of restarting.
    #[test]
    fn disclosure_fade_retargets_without_extending_the_exit_bound() {
        let start = Instant::now();
        let mut reveal = DisclosureReveal::default();
        reveal.retarget(true, start);
        reveal.advance(start + Duration::from_millis(35));
        let opacity = reveal.opacity(false);
        reveal.retarget(false, start + Duration::from_millis(40));
        assert!((reveal.opacity(false) - opacity).abs() < f32::EPSILON);
        reveal.advance(start + Duration::from_millis(140));
        assert!(reveal.opacity(false).abs() < f32::EPSILON);
        assert!(!reveal.animating());
    }

    /// Disabled history controls retain their box without adopting hover or pressed feedback.
    #[test]
    fn disabled_history_background_ignores_hover_and_press() {
        assert_eq!(
            history_background(false, true, true),
            history_background(false, false, false)
        );
        assert_ne!(
            history_background(true, false, false),
            history_background(true, true, false)
        );
        assert_ne!(
            history_background(true, true, false),
            history_background(true, true, true)
        );
    }
}
