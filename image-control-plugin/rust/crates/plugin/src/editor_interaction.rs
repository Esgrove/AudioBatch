//! Native slider interruption and modal background barriers preserve Iced's production widget behavior.

use iced::advanced::widget::{Id, Operation, operation::Focusable, tree};
use iced::advanced::{Clipboard, Layout, Shell, Widget, layout, renderer};
use iced::widget::slider::{self, Slider};
use iced::{Element, Event, Length, Pixels, Rectangle, Size, Theme, Vector, keyboard, mouse};
use iced_baseview as iced;

/// Keeps background layout and drawing while excluding every event,
/// focus operation, and overlay during a modal callout.
/// The callout, outside-click shield, and Settings opener belong above this element in the editor's stack.
pub fn modal_background<'a, Message: 'a, Renderer: iced::advanced::Renderer + 'a>(
    content: impl Into<Element<'a, Message, Theme, Renderer>>,
    blocked: bool,
) -> Element<'a, Message, Theme, Renderer> {
    Element::new(ModalBackground {
        content: content.into(),
        blocked,
    })
}

/// Retains the entire ordinary background without exposing its controls to modal input dispatch.
struct ModalBackground<'a, Message, Renderer: iced::advanced::Renderer> {
    content: Element<'a, Message, Theme, Renderer>,
    blocked: bool,
}

impl<Message, Renderer: iced::advanced::Renderer> Widget<Message, Theme, Renderer>
    for ModalBackground<'_, Message, Renderer>
{
    /// Retains one normal background subtree regardless of the modal state.
    fn children(&self) -> Vec<tree::Tree> {
        vec![tree::Tree::new(&self.content)]
    }

    /// Reconciles the same subtree so opening Settings never replaces retained controls or GPU resources.
    fn diff(&self, tree: &mut tree::Tree) {
        tree.diff_children(std::slice::from_ref(&self.content));
    }

    /// Preserves the background's ordinary size request.
    fn size(&self) -> Size<Length> {
        self.content.as_widget().size()
    }

    /// Preserves native container sizing hints.
    fn size_hint(&self) -> Size<Length> {
        self.content.as_widget().size_hint()
    }

    /// Adds no inset or target, retaining the child's original coordinate system.
    fn layout(&mut self, tree: &mut tree::Tree, renderer: &Renderer, limits: &layout::Limits) -> layout::Node {
        self.content.as_widget_mut().layout(child_tree(tree), renderer, limits)
    }

    /// Excludes background controls from every operation pass, including native Tab traversal and explicit focus.
    fn operate(
        &mut self,
        tree: &mut tree::Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn Operation,
    ) {
        if !self.blocked {
            self.content
                .as_widget_mut()
                .operate(child_tree(tree), layout, renderer, operation);
        }
    }

    /// Rejects background input while forwarding frame and lifecycle events so live monitoring never freezes.
    fn update(
        &mut self,
        tree: &mut tree::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &Renderer,
        clipboard: &mut dyn Clipboard,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        if self.blocked && !matches!(event, Event::Window(_)) {
            return;
        }
        self.content.as_widget_mut().update(
            child_tree(tree),
            event,
            layout,
            cursor,
            renderer,
            clipboard,
            shell,
            viewport,
        );
    }

    /// Draws the unchanged background beneath the modal content.
    fn draw(
        &self,
        tree: &tree::Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        self.content
            .as_widget()
            .draw(child_tree_ref(tree), renderer, theme, style, layout, cursor, viewport);
    }

    /// Leaves the modal shield and foreground controls in sole charge of pointer affordances.
    fn mouse_interaction(
        &self,
        tree: &tree::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        if self.blocked {
            mouse::Interaction::None
        } else {
            self.content
                .as_widget()
                .mouse_interaction(child_tree_ref(tree), layout, cursor, viewport, renderer)
        }
    }

    /// Prevents background menus from escaping above the modal while retaining ordinary overlays when unblocked.
    fn overlay<'a>(
        &'a mut self,
        tree: &'a mut tree::Tree,
        layout: Layout<'a>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<iced::advanced::overlay::Element<'a, Message, Theme, Renderer>> {
        if self.blocked {
            None
        } else {
            self.content
                .as_widget_mut()
                .overlay(child_tree(tree), layout, renderer, viewport, translation)
        }
    }
}

/// Wraps the production native slider, releasing its private drag state when the embedded window loses focus.
/// Native layout, pointer hit testing, keyboard increments, wheel steps, drawing, and operations remain authoritative.
pub struct FocusSafeSlider<'a, Value, Message> {
    child: Slider<'a, Value, Message>,
    id: Option<Id>,
    on_release: Option<Message>,
    on_numeric: Option<Message>,
}

/// Retains only adapter input state. The native child remains authoritative for values, defaults, and dragging.
#[derive(Default)]
struct SliderInteraction {
    modifiers: keyboard::Modifiers,
    last_click: Option<iced::advanced::mouse::Click>,
    reset_pending: bool,
    focused: bool,
    keyboard_focus: bool,
    window_inactive: bool,
}

/// Selects native event adaptations without reading or mirroring the slider's private state.
#[derive(Default)]
struct SliderDispatch {
    modifiers: Option<keyboard::Modifiers>,
    finish_reset: bool,
    interrupt: bool,
    numeric: bool,
}

impl SliderInteraction {
    /// Tracks adapter-only input and chooses transient native modifiers for the current event.
    fn prepare(
        &mut self,
        event: &Event,
        cursor: mouse::Cursor,
        bounds: Rectangle,
        focusable: bool,
        reset_enabled: bool,
        numeric_enabled: bool,
    ) -> SliderDispatch {
        let mut dispatch = SliderDispatch::default();
        let hovered = cursor.is_over(bounds);
        match event {
            Event::Keyboard(keyboard::Event::ModifiersChanged(modifiers)) => self.modifiers = *modifiers,
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)) if hovered => {
                self.focused = focusable;
                self.keyboard_focus = false;
                if let Some(position) = cursor.position() {
                    let click = iced::advanced::mouse::Click::new(position, mouse::Button::Left, self.last_click);
                    self.last_click = Some(click);
                    // Native defaults are private. Completion configuration marks reset-capable parameter sliders.
                    let double = reset_enabled && matches!(click.kind(), iced::advanced::mouse::click::Kind::Double);
                    self.reset_pending = double || self.modifiers.command();
                    if double {
                        dispatch.modifiers = Some(self.modifiers | keyboard::Modifiers::COMMAND);
                    }
                }
            }
            Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right)) if hovered && numeric_enabled => {
                dispatch.numeric = true;
            }
            Event::Mouse(mouse::Event::ButtonPressed(_)) => self.unfocus(),
            Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)) => {
                dispatch.finish_reset = std::mem::take(&mut self.reset_pending);
            }
            Event::Mouse(mouse::Event::WheelScrolled { .. }) if hovered => {
                dispatch.modifiers = Some(self.modifiers | keyboard::Modifiers::CTRL);
            }
            Event::Keyboard(keyboard::Event::KeyPressed {
                key, modifiers, repeat, ..
            }) if self.focused
                && !self.window_inactive
                && numeric_enabled
                && !modifiers
                    .intersects(keyboard::Modifiers::CTRL | keyboard::Modifiers::ALT | keyboard::Modifiers::LOGO)
                && matches!(
                    key,
                    keyboard::Key::Named(keyboard::key::Named::Enter | keyboard::key::Named::Space)
                ) =>
            {
                dispatch.numeric = !repeat;
            }
            Event::Window(iced::window::Event::Unfocused) => {
                dispatch.interrupt = true;
                dispatch.finish_reset = std::mem::take(&mut self.reset_pending);
                self.modifiers = keyboard::Modifiers::NONE;
                self.last_click = None;
                self.window_inactive = true;
            }
            Event::Window(iced::window::Event::Focused) => self.window_inactive = false,
            _ => {}
        }
        dispatch
    }
}

impl Focusable for SliderInteraction {
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

impl<'a, Value: Copy + From<u8> + PartialOrd, Message: Clone> FocusSafeSlider<'a, Value, Message> {
    /// Adopts a fully configured native slider without introducing another value or gesture model.
    pub const fn new(child: Slider<'a, Value, Message>) -> Self {
        Self {
            child,
            id: None,
            on_release: None,
            on_numeric: None,
        }
    }

    /// Exposes an ordinary native-operation focus target without replacing the slider's input state.
    pub fn id(mut self, id: impl Into<Id>) -> Self {
        self.id = Some(id.into());
        self
    }

    /// Gives native drag releases and synthesized default resets the same gesture completion message.
    pub fn on_release(mut self, message: Message) -> Self {
        self.child = self.child.on_release(message.clone());
        self.on_release = Some(message);
        self
    }

    /// Keeps the compact and expanded production builders' native height customization available.
    pub fn height(mut self, height: impl Into<Pixels>) -> Self {
        self.child = self.child.height(height);
        self
    }

    /// Keeps the compact production builder's native slider styling available.
    pub fn style(mut self, style: impl Fn(&Theme, slider::Status) -> slider::Style + 'a) -> Self {
        self.child = self.child.style(style);
        self
    }
}

impl<Value: Copy + From<u8> + PartialOrd> FocusSafeSlider<'_, Value, crate::editor::Message> {
    /// Lets compact controls request the parent's ordinary numeric overlay using its stable parameter id.
    pub fn on_numeric(mut self, id: u32) -> Self {
        self.on_numeric = Some(crate::editor::Message::NumericFocus(id));
        if self.id.is_none() {
            self.id = Some(format!("parameter-slider-{id}").into());
        }
        self
    }
}

impl<'a, Value, Message: Clone, Renderer: iced::advanced::Renderer> Widget<Message, Theme, Renderer>
    for FocusSafeSlider<'a, Value, Message>
where
    Slider<'a, Value, Message>: Widget<Message, Theme, Renderer>,
{
    /// Separates adapter focus and click tracking from the native child's private drag state.
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<SliderInteraction>()
    }

    /// Retains adapter bookkeeping without copying private native slider fields.
    fn state(&self) -> tree::State {
        tree::State::new(SliderInteraction::default())
    }

    /// Leaves native value and drag state in one unchanged child tree.
    fn children(&self) -> Vec<tree::Tree> {
        vec![tree::Tree::new(&self.child as &dyn Widget<Message, Theme, Renderer>)]
    }

    /// Preserves both native interaction and adapter focus through ordinary view rebuilds.
    fn diff(&self, tree: &mut tree::Tree) {
        tree.diff_children(&[&self.child as &dyn Widget<Message, Theme, Renderer>]);
        if self.id.is_none() {
            tree.state.downcast_mut::<SliderInteraction>().unfocus();
        }
    }

    /// Preserves the native slider's size request.
    fn size(&self) -> Size<Length> {
        Widget::<Message, Theme, Renderer>::size(&self.child)
    }

    /// Preserves the native slider's size hints.
    fn size_hint(&self) -> Size<Length> {
        Widget::<Message, Theme, Renderer>::size_hint(&self.child)
    }

    /// Returns the native slider bounds without inserting another layout node.
    fn layout(&mut self, tree: &mut tree::Tree, renderer: &Renderer, limits: &layout::Limits) -> layout::Node {
        self.child.layout(child_tree(tree), renderer, limits)
    }

    /// Preserves native widget operations instead of replacing the slider with a custom interaction model.
    fn operate(
        &mut self,
        tree: &mut tree::Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn Operation,
    ) {
        if let Some(id) = &self.id {
            operation.focusable(
                Some(id),
                layout.bounds(),
                tree.state.downcast_mut::<SliderInteraction>(),
            );
        }
        self.child.operate(child_tree(tree), layout, renderer, operation);
    }

    /// Adapts ordinary wheel and double-click reset into native modifier-driven paths, restoring modifiers afterwards.
    /// Keyboard focus routes native arrow increments without requiring the pointer to remain over the rail.
    /// Deactivation releases native dragging and clears missed modifier releases.
    fn update(
        &mut self,
        tree: &mut tree::Tree,
        event: &Event,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        renderer: &Renderer,
        clipboard: &mut dyn Clipboard,
        shell: &mut Shell<'_, Message>,
        viewport: &Rectangle,
    ) {
        let state = tree.state.downcast_mut::<SliderInteraction>();
        let focus_before = (state.focused, state.keyboard_focus, state.window_inactive);
        let dispatch = state.prepare(
            event,
            cursor,
            layout.bounds(),
            self.id.is_some(),
            self.on_release.is_some(),
            self.on_numeric.is_some(),
        );
        if focus_before != (state.focused, state.keyboard_focus, state.window_inactive) {
            shell.request_redraw();
        }
        if dispatch.numeric
            && let Some(message) = self.on_numeric.clone()
        {
            shell.publish(message);
            shell.capture_event();
            return;
        }
        let modifiers = state.modifiers;
        let cursor = if dispatch.interrupt {
            mouse::Cursor::Unavailable
        } else if state.focused
            && !state.window_inactive
            && matches!(event, Event::Keyboard(keyboard::Event::KeyPressed { .. }))
        {
            mouse::Cursor::Available(layout.bounds().center())
        } else {
            cursor
        };
        // Synthesized modifiers live only for this native dispatch. Shift remains authoritative for fine steps.
        for adapted in [
            dispatch
                .interrupt
                .then_some(Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left))),
            (dispatch.interrupt || dispatch.modifiers.is_some()).then(|| {
                Event::Keyboard(keyboard::Event::ModifiersChanged(
                    dispatch.modifiers.unwrap_or(modifiers),
                ))
            }),
            Some(event.clone()),
            dispatch
                .modifiers
                .map(|_| Event::Keyboard(keyboard::Event::ModifiersChanged(modifiers))),
        ]
        .into_iter()
        .flatten()
        {
            self.child.update(
                child_tree(tree),
                &adapted,
                layout,
                cursor,
                renderer,
                clipboard,
                shell,
                viewport,
            );
        }
        // Native command-click ends its drag immediately, so its default-reset path does not emit on_release itself.
        if dispatch.finish_reset
            && let Some(message) = self.on_release.clone()
        {
            shell.publish(message);
        }
    }

    /// Draws the real native slider with its ordinary theme and retained state.
    fn draw(
        &self,
        tree: &tree::Tree,
        renderer: &mut Renderer,
        theme: &Theme,
        style: &renderer::Style,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
    ) {
        self.child
            .draw(child_tree_ref(tree), renderer, theme, style, layout, cursor, viewport);
        let state = tree.state.downcast_ref::<SliderInteraction>();
        if state.focused && state.keyboard_focus && !state.window_inactive {
            renderer.fill_quad(
                renderer::Quad {
                    bounds: layout.bounds().shrink(1.0),
                    border: iced::Border {
                        color: crate::editor::theme::GREY_MIDDLE_LIGHT.scale_alpha(0.55),
                        width: 1.0,
                        radius: 3.0.into(),
                    },
                    ..renderer::Quad::default()
                },
                iced::Color::TRANSPARENT,
            );
        }
    }

    /// Uses the native slider's pointer feedback after interrupted drags have been released.
    fn mouse_interaction(
        &self,
        tree: &tree::Tree,
        layout: Layout<'_>,
        cursor: mouse::Cursor,
        viewport: &Rectangle,
        renderer: &Renderer,
    ) -> mouse::Interaction {
        self.child
            .mouse_interaction(child_tree_ref(tree), layout, cursor, viewport, renderer)
    }
}

impl<'a, Value: 'a, Message: Clone + 'a, Renderer: iced::advanced::Renderer + 'a>
    From<FocusSafeSlider<'a, Value, Message>> for Element<'a, Message, Theme, Renderer>
where
    Slider<'a, Value, Message>: Widget<Message, Theme, Renderer>,
{
    /// Converts to any native renderer, including the hardware-free production event renderer used by tests.
    fn from(slider: FocusSafeSlider<'a, Value, Message>) -> Self {
        Self::new(slider)
    }
}

/// Borrows the retained native child without changing its coordinate system.
fn child_tree(tree: &mut tree::Tree) -> &mut tree::Tree {
    tree.children
        .first_mut()
        .expect("interaction wrapper retains one native child")
}

/// Borrows the retained child for unchanged native drawing and pointer queries.
fn child_tree_ref(tree: &tree::Tree) -> &tree::Tree {
    tree.children
        .first()
        .expect("interaction wrapper retains one native child")
}

#[cfg(test)]
mod tests {
    use iced::advanced::text;
    use iced::advanced::widget::operation;
    use iced::widget::{Column, Space, Stack, container, mouse_area, pin, slider, text_input};
    use iced::{Color, Font, Point, touch};

    use super::*;
    use crate::editor::chrome::{Chrome, Symbol};
    use crate::editor::controls::parameter_input;
    use crate::editor::{Message, theme};
    use crate::params::GAIN_PARAMETER;

    /// Runs real native widget layout and event dispatch without a GPU, native window, or pixel assertions.
    struct EventRenderer;

    impl iced_widget::graphics::geometry::Renderer for EventRenderer {
        type Geometry = <iced::Renderer as iced_widget::graphics::geometry::Renderer>::Geometry;
        type Frame = <iced::Renderer as iced_widget::graphics::geometry::Renderer>::Frame;

        fn new_frame(&self, bounds: Rectangle) -> Self::Frame {
            iced_widget::renderer::wgpu::geometry::Frame::new(bounds)
        }

        fn draw_geometry(&mut self, _: Self::Geometry) {}
    }

    impl iced::advanced::Renderer for EventRenderer {
        /// Keeps layer setup inert because event tests never rasterize a widget.
        fn start_layer(&mut self, _: Rectangle) {}
        /// Keeps layer teardown inert because event tests never rasterize a widget.
        fn end_layer(&mut self) {}
        /// Retains the event-only renderer contract without applying drawing transforms.
        fn start_transformation(&mut self, _: iced::Transformation) {}
        /// Completes an event-only transform without allocating drawing resources.
        fn end_transformation(&mut self) {}
        /// Discards drawing commands because assertions inspect native events rather than pixels.
        fn fill_quad(&mut self, _: renderer::Quad, _: impl Into<iced::Background>) {}
        /// Requires no frame state between native event batches.
        fn reset(&mut self, _: Rectangle) {}
        /// Rejects image allocation explicitly because no test needs graphics resources.
        fn allocate_image(
            &mut self,
            _: &iced::core::image::Handle,
            callback: impl FnOnce(Result<iced::core::image::Allocation, iced::core::image::Error>) + Send + 'static,
        ) {
            callback(Err(iced::core::image::Error::Unsupported));
        }
    }

    impl text::Renderer for EventRenderer {
        type Font = Font;
        type Paragraph = <iced::Renderer as text::Renderer>::Paragraph;
        type Editor = <iced::Renderer as text::Renderer>::Editor;

        const ICON_FONT: Font = <iced::Renderer as text::Renderer>::ICON_FONT;
        const CHECKMARK_ICON: char = <iced::Renderer as text::Renderer>::CHECKMARK_ICON;
        const ARROW_DOWN_ICON: char = <iced::Renderer as text::Renderer>::ARROW_DOWN_ICON;
        const SCROLL_UP_ICON: char = <iced::Renderer as text::Renderer>::SCROLL_UP_ICON;
        const SCROLL_DOWN_ICON: char = <iced::Renderer as text::Renderer>::SCROLL_DOWN_ICON;
        const SCROLL_LEFT_ICON: char = <iced::Renderer as text::Renderer>::SCROLL_LEFT_ICON;
        const SCROLL_RIGHT_ICON: char = <iced::Renderer as text::Renderer>::SCROLL_RIGHT_ICON;
        const ICED_LOGO: char = <iced::Renderer as text::Renderer>::ICED_LOGO;

        /// Uses the same numeric face as the editor's native fields.
        fn default_font(&self) -> Font {
            theme::MONO
        }
        /// Uses the editor's production numeric text size for real native layout.
        fn default_size(&self) -> Pixels {
            theme::VALUE_SIZE.into()
        }
        /// Keeps shaping available while discarding rasterization.
        fn fill_paragraph(&mut self, _: &Self::Paragraph, _: Point, _: Color, _: Rectangle) {}
        /// Discards editable-text rasterization while retaining native focus and editing state.
        fn fill_editor(&mut self, _: &Self::Editor, _: Point, _: Color, _: Rectangle) {}
        /// Discards immediate text drawing because no test inspects pixels.
        fn fill_text(&mut self, _: text::Text, _: Point, _: Color, _: Rectangle) {}
    }

    /// Retains the native widget tree across real event batches and modal view rebuilds.
    struct Driver {
        interface: Option<iced::runtime::UserInterface<'static, Message, Theme, EventRenderer>>,
    }

    impl Driver {
        /// Builds a production widget using the same retained runtime dispatch as the embedded editor.
        fn new(content: Element<'static, Message, Theme, EventRenderer>) -> Self {
            Self {
                interface: Some(iced::runtime::UserInterface::build(
                    content,
                    Size::new(400.0, 160.0),
                    iced::runtime::user_interface::Cache::new(),
                    &mut EventRenderer,
                )),
            }
        }

        /// Reuses the real native cache so modal activation cannot hide a tree replacement bug.
        fn rebuild(&mut self, content: Element<'static, Message, Theme, EventRenderer>) {
            let cache = self
                .interface
                .take()
                .expect("retained interaction interface")
                .into_cache();
            self.interface = Some(iced::runtime::UserInterface::build(
                content,
                Size::new(400.0, 160.0),
                cache,
                &mut EventRenderer,
            ));
        }

        /// Dispatches one real event through the complete retained widget assembly.
        fn send(&mut self, event: Event, cursor: mouse::Cursor) -> Vec<Message> {
            let mut messages = Vec::new();
            self.interface.as_mut().expect("retained interaction interface").update(
                &[event],
                cursor,
                &mut EventRenderer,
                &mut iced::advanced::clipboard::Null,
                &mut messages,
            );
            messages
        }

        /// Sends both native pointer phases rather than manufacturing button or focus messages.
        fn click(&mut self, position: Point) -> Vec<Message> {
            let cursor = mouse::Cursor::Available(position);
            let mut messages = self.send(Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)), cursor);
            messages.extend(self.send(Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)), cursor));
            messages
        }

        /// Runs every native operation phase, including traversal's counting and chained focusing phases.
        fn operate(&mut self, mut operation: Box<dyn Operation>) {
            loop {
                self.interface
                    .as_mut()
                    .expect("retained interaction interface")
                    .operate(&EventRenderer, operation.as_mut());
                match operation.finish() {
                    operation::Outcome::Chain(next) => operation = next,
                    operation::Outcome::None | operation::Outcome::Some(()) => break,
                }
            }
        }

        /// Queries real native focus by id without depending on private tree shape or a separate focus model.
        fn focused(&mut self, id: &str) -> bool {
            let mut operation = operation::focusable::is_focused(id.to_owned().into());
            self.interface
                .as_mut()
                .expect("retained interaction interface")
                .operate(&EventRenderer, &mut operation::black_box(&mut operation));
            matches!(operation.finish(), operation::Outcome::Some(true))
        }
    }

    /// Builds the same stack layering as the production callout with native controls in front and behind its barrier.
    fn modal_view(blocked: bool) -> Element<'static, Message, Theme, EventRenderer> {
        let background = Column::new()
            .push(parameter_input(GAIN_PARAMETER, 0.5).height(26.0))
            .push(
                text_input("", "12")
                    .id("background-input")
                    .on_input(|_| Message::Ignore),
            )
            .width(200.0);
        let mut stack = Stack::new()
            .push(modal_background(background, blocked))
            .width(400.0)
            .height(160.0);
        if blocked {
            stack = stack
                .push(
                    mouse_area(container(Space::new()).width(Length::Fill).height(Length::Fill))
                        .on_press(Message::CloseSettings),
                )
                .push(
                    pin(mouse_area(
                        Column::new()
                            .push(FocusSafeSlider::new(
                                slider(0.0..=1.0, 0.5, Message::Contrast).step(0.01_f32).height(26.0),
                            ))
                            .push(text_input("", "34").id("settings-input").on_input(|_| Message::Ignore))
                            .width(150.0),
                    )
                    .on_press(Message::Ignore))
                    .x(220.0)
                    .y(40.0),
                )
                .push(
                    pin(Chrome::new(
                        Symbol::Settings,
                        Size::new(24.0, 24.0),
                        Some(Message::Settings),
                        true,
                    ))
                    .x(370.0),
                );
        }
        stack.into()
    }

    /// Creates a real key press with its logical and physical identity for native widget dispatch.
    fn key(key: keyboard::Key, code: keyboard::key::Code, text: Option<&str>) -> Event {
        Event::Keyboard(keyboard::Event::KeyPressed {
            modified_key: key.clone(),
            key,
            physical_key: keyboard::key::Physical::Code(code),
            location: keyboard::Location::Standard,
            modifiers: keyboard::Modifiers::NONE,
            text: text.map(Into::into),
            repeat: false,
        })
    }

    /// Creates a hovered arrow edit with the same native event shape as embedded keyboard input.
    fn arrow_up() -> Event {
        key(
            keyboard::Key::Named(keyboard::key::Named::ArrowUp),
            keyboard::key::Code::ArrowUp,
            None,
        )
    }

    /// Reuses the production parameter widget while testing its adapter's explicit completion and focus builders.
    fn configured_slider(value: f64) -> FocusSafeSlider<'static, f64, Message> {
        parameter_input(GAIN_PARAMETER, value)
            .id("test-parameter-slider")
            .on_release(Message::EndGesture)
            .height(26.0)
    }

    #[test]
    fn ordinary_and_shift_wheel_use_native_steps_without_retaining_synthetic_reset_modifiers() {
        let mut driver = Driver::new(configured_slider(0.5).into());
        let cursor = mouse::Cursor::Available(Point::new(100.0, 13.0));
        let wheel = Event::Mouse(mouse::Event::WheelScrolled {
            delta: mouse::ScrollDelta::Lines { x: 0.0, y: 1.0 },
        });
        assert!(matches!(
            driver.send(wheel.clone(), cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, value)] if (*value - 0.501).abs() < f64::EPSILON
        ));
        let _ = driver.send(
            Event::Keyboard(keyboard::Event::ModifiersChanged(keyboard::Modifiers::SHIFT)),
            cursor,
        );
        assert!(matches!(
            driver.send(wheel, cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, value)] if (*value - 0.5011).abs() < f64::EPSILON
        ));
        // A normal click after wheel adaptation must position the rail, not take native command-click reset.
        assert!(matches!(
            driver.click(Point::new(300.0, 13.0)).as_slice(),
            [Message::Change(GAIN_PARAMETER, value), Message::EndGesture] if (*value - 0.75).abs() < f64::EPSILON
        ));
    }

    #[test]
    fn double_click_resets_native_default_and_closes_once_without_resuming_a_drag() {
        let mut driver = Driver::new(configured_slider(0.5).into());
        let position = Point::new(300.0, 13.0);
        assert!(matches!(
            driver.click(position).as_slice(),
            [Message::Change(GAIN_PARAMETER, _), Message::EndGesture]
        ));
        let expected = crate::params::parameter_spec(GAIN_PARAMETER)
            .expect("output registry metadata")
            .default_normalized;
        let messages = driver.click(position);
        assert!(matches!(
            messages.as_slice(),
            [Message::Change(GAIN_PARAMETER, value), Message::EndGesture] if (*value - expected).abs() < f64::EPSILON
        ));
        assert!(
            driver
                .send(
                    Event::Mouse(mouse::Event::CursorMoved {
                        position: Point::new(200.0, 13.0)
                    }),
                    mouse::Cursor::Available(Point::new(200.0, 13.0)),
                )
                .is_empty()
        );
        assert!(
            driver
                .send(
                    Event::Window(iced::window::Event::Unfocused),
                    mouse::Cursor::Unavailable,
                )
                .is_empty()
        );
    }

    #[test]
    fn command_reset_interruption_closes_once_and_slider_focus_routes_arrows_and_numeric_requests() {
        let mut driver = Driver::new(configured_slider(0.5).on_numeric(GAIN_PARAMETER).into());
        let cursor = mouse::Cursor::Available(Point::new(100.0, 13.0));
        let _ = driver.send(
            Event::Keyboard(keyboard::Event::ModifiersChanged(keyboard::Modifiers::COMMAND)),
            cursor,
        );
        assert!(matches!(
            driver
                .send(Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)), cursor)
                .as_slice(),
            [Message::Change(GAIN_PARAMETER, _)]
        ));
        assert!(matches!(
            driver
                .send(
                    Event::Window(iced::window::Event::Unfocused),
                    mouse::Cursor::Unavailable
                )
                .as_slice(),
            [Message::EndGesture]
        ));
        assert!(
            driver
                .send(Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)), cursor,)
                .is_empty()
        );
        let _ = driver.send(Event::Window(iced::window::Event::Focused), mouse::Cursor::Unavailable);
        driver.operate(Box::new(operation::focusable::focus("test-parameter-slider".into())));
        assert!(driver.focused("test-parameter-slider"));
        assert!(matches!(
            driver.send(arrow_up(), mouse::Cursor::Unavailable).as_slice(),
            [Message::Change(GAIN_PARAMETER, _)]
        ));
        for event in [
            key(
                keyboard::Key::Named(keyboard::key::Named::Enter),
                keyboard::key::Code::Enter,
                None,
            ),
            key(
                keyboard::Key::Named(keyboard::key::Named::Space),
                keyboard::key::Code::Space,
                None,
            ),
        ] {
            assert!(matches!(
                driver.send(event, mouse::Cursor::Unavailable).as_slice(),
                [Message::NumericFocus(GAIN_PARAMETER)]
            ));
        }
        assert!(matches!(
            driver
                .send(Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Right)), cursor)
                .as_slice(),
            [Message::NumericFocus(GAIN_PARAMETER)]
        ));
    }

    #[test]
    fn chrome_native_traversal_stable_ids_skip_disabled_and_pointer_resize_and_activate_from_keyboard() {
        let view = iced::widget::Column::new()
            .push(Chrome::new(
                Symbol::Settings,
                Size::new(24.0, 24.0),
                Some(Message::Settings),
                false,
            ))
            .push(Chrome::new(Symbol::Undo, Size::new(24.0, 24.0), None, false))
            .push(Chrome::new(
                Symbol::Resize,
                Size::new(24.0, 24.0),
                Some(Message::Ignore),
                false,
            ))
            .push(
                Chrome::new(
                    Symbol::Disclosure(false),
                    Size::new(72.0, 20.0),
                    Some(Message::Correlation),
                    false,
                )
                .id("correlation-disclosure"),
            )
            .push(
                Chrome::new(
                    Symbol::Disclosure(true),
                    Size::new(72.0, 20.0),
                    Some(Message::CompactBands),
                    false,
                )
                .id("band-controls-disclosure"),
            );
        let mut driver = Driver::new(view.into());
        driver.operate(Box::new(operation::focusable::focus_next::<()>()));
        assert!(driver.focused("settings"));
        let enter = key(
            keyboard::Key::Named(keyboard::key::Named::Enter),
            keyboard::key::Code::Enter,
            None,
        );
        assert!(matches!(
            driver.send(enter, mouse::Cursor::Unavailable).as_slice(),
            [Message::Settings]
        ));
        let tab = key(
            keyboard::Key::Named(keyboard::key::Named::Tab),
            keyboard::key::Code::Tab,
            None,
        );
        assert!(driver.send(tab, mouse::Cursor::Unavailable).is_empty());
        driver.operate(Box::new(operation::focusable::focus_next::<()>()));
        assert!(driver.focused("correlation-disclosure"));
        let space = key(
            keyboard::Key::Named(keyboard::key::Named::Space),
            keyboard::key::Code::Space,
            None,
        );
        assert!(matches!(
            driver.send(space, mouse::Cursor::Unavailable).as_slice(),
            [Message::Correlation]
        ));
        driver.operate(Box::new(operation::focusable::focus_next::<()>()));
        assert!(driver.focused("band-controls-disclosure"));
        driver.operate(Box::new(operation::focusable::focus_previous::<()>()));
        assert!(driver.focused("correlation-disclosure"));
        assert!(!driver.focused("band-controls-disclosure"));
        assert!(!driver.focused("undo"));
        assert!(!driver.focused("resize-grip"));
    }

    #[test]
    fn focused_divider_uses_screen_space_steps_and_preserves_host_modifier_shortcuts() {
        let mut driver =
            Driver::new(Chrome::new(Symbol::Divider, Size::new(200.0, 10.0), Some(Message::Ignore), false).into());
        assert!(driver.send(arrow_up(), mouse::Cursor::Unavailable).is_empty());
        driver.operate(Box::new(operation::focusable::focus("split-divider".into())));
        for (named, code, expected) in [
            (keyboard::key::Named::ArrowUp, keyboard::key::Code::ArrowUp, -8.0),
            (keyboard::key::Named::ArrowDown, keyboard::key::Code::ArrowDown, 8.0),
        ] {
            let event = key(keyboard::Key::Named(named), code, None);
            assert!(matches!(
                driver.send(event.clone(), mouse::Cursor::Unavailable).as_slice(),
                [Message::DividerNudge(delta)] if (*delta - expected).abs() < f32::EPSILON
            ));
            let mut fine = event.clone();
            if let Event::Keyboard(keyboard::Event::KeyPressed { modifiers, .. }) = &mut fine {
                *modifiers = keyboard::Modifiers::SHIFT;
            }
            assert!(matches!(
                driver.send(fine, mouse::Cursor::Unavailable).as_slice(),
                [Message::DividerNudge(delta)] if (*delta - expected.signum()).abs() < f32::EPSILON
            ));
            for modifier in [
                keyboard::Modifiers::CTRL,
                keyboard::Modifiers::ALT,
                keyboard::Modifiers::LOGO,
            ] {
                let mut shortcut = event.clone();
                if let Event::Keyboard(keyboard::Event::KeyPressed { modifiers, .. }) = &mut shortcut {
                    *modifiers = modifier;
                }
                assert!(driver.send(shortcut, mouse::Cursor::Unavailable).is_empty());
            }
        }
        let _ = driver.send(
            Event::Window(iced::window::Event::Unfocused),
            mouse::Cursor::Unavailable,
        );
        assert!(driver.send(arrow_up(), mouse::Cursor::Unavailable).is_empty());
    }

    #[test]
    fn production_slider_deactivation_releases_mouse_and_touch_drags_before_any_following_motion() {
        for compact in [false, true] {
            for touch_drag in [false, true] {
                let mut slider = parameter_input(GAIN_PARAMETER, 0.5);
                if compact {
                    slider = slider.style(theme::compact_slider).height(14.0);
                }
                let mut driver = Driver::new(slider.into());
                let position = Point::new(300.0, 7.0);
                let press = if touch_drag {
                    Event::Touch(touch::Event::FingerPressed {
                        id: touch::Finger(1),
                        position,
                    })
                } else {
                    Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left))
                };
                let messages = driver.send(press, mouse::Cursor::Available(position));
                assert!(matches!(messages.as_slice(), [Message::Change(GAIN_PARAMETER, _)]));
                let messages = driver.send(
                    Event::Window(iced::window::Event::Unfocused),
                    mouse::Cursor::Unavailable,
                );
                assert!(matches!(messages.as_slice(), [Message::EndGesture]));
                assert!(
                    driver
                        .send(
                            Event::Window(iced::window::Event::Unfocused),
                            mouse::Cursor::Unavailable
                        )
                        .is_empty()
                );
                // The production slider survives an ordinary view rebuild with its released native state.
                driver.rebuild(parameter_input(GAIN_PARAMETER, 0.75).into());
                assert!(
                    driver
                        .send(Event::Window(iced::window::Event::Focused), mouse::Cursor::Unavailable)
                        .is_empty()
                );
                let position = Point::new(100.0, 7.0);
                for motion in [
                    Event::Mouse(mouse::Event::CursorMoved { position }),
                    Event::Touch(touch::Event::FingerMoved {
                        id: touch::Finger(1),
                        position,
                    }),
                    Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)),
                ] {
                    assert!(driver.send(motion, mouse::Cursor::Available(position)).is_empty());
                }
                assert!(matches!(
                    driver.click(position).as_slice(),
                    [Message::Change(GAIN_PARAMETER, _), Message::EndGesture]
                ));
            }
        }
    }

    #[test]
    fn production_slider_preserves_native_keyboard_wheel_and_modifier_reset_after_deactivation() {
        let mut driver = Driver::new(parameter_input(GAIN_PARAMETER, 0.5).height(26.0).into());
        let cursor = mouse::Cursor::Available(Point::new(100.0, 13.0));
        assert!(matches!(
            driver.send(arrow_up(), cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, value)] if (*value - 0.501).abs() < f64::EPSILON
        ));
        let modifiers = keyboard::Modifiers::CTRL | keyboard::Modifiers::SHIFT;
        let _ = driver.send(Event::Keyboard(keyboard::Event::ModifiersChanged(modifiers)), cursor);
        let wheel = Event::Mouse(mouse::Event::WheelScrolled {
            delta: mouse::ScrollDelta::Lines { x: 0.0, y: 1.0 },
        });
        assert!(matches!(
            driver.send(wheel.clone(), cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, value)] if (*value - 0.5011).abs() < f64::EPSILON
        ));
        assert!(
            driver
                .send(
                    Event::Window(iced::window::Event::Unfocused),
                    mouse::Cursor::Unavailable
                )
                .is_empty()
        );
        assert!(matches!(
            driver.send(wheel, cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, value)] if (*value - 0.502).abs() < f64::EPSILON
        ));
        assert!(matches!(
            driver.send(arrow_up(), cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, value)] if (*value - 0.503).abs() < f64::EPSILON
        ));
    }

    #[test]
    fn assembled_modal_blocks_background_keyboard_wheel_touch_and_pointer_but_keeps_callout_and_opener() {
        let mut driver = Driver::new(modal_view(false));
        let cursor = mouse::Cursor::Available(Point::new(100.0, 13.0));
        assert!(matches!(
            driver.send(arrow_up(), cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, _)]
        ));
        let modifiers = Event::Keyboard(keyboard::Event::ModifiersChanged(keyboard::Modifiers::CTRL));
        let _ = driver.send(modifiers.clone(), cursor);
        let wheel = Event::Mouse(mouse::Event::WheelScrolled {
            delta: mouse::ScrollDelta::Lines { x: 0.0, y: 1.0 },
        });
        assert!(matches!(
            driver.send(wheel.clone(), cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, _)]
        ));
        driver.rebuild(modal_view(true));
        assert!(
            driver
                .send(
                    Event::Window(iced::window::Event::RedrawRequested(std::time::Instant::now())),
                    cursor,
                )
                .is_empty()
        );
        // Retained native modifiers prove that wheel suppression comes from the barrier, not a fresh slider state.
        for event in [
            arrow_up(),
            modifiers,
            wheel,
            Event::Mouse(mouse::Event::CursorMoved {
                position: Point::new(50.0, 13.0),
            }),
        ] {
            assert!(driver.send(event, cursor).is_empty());
        }
        for event in [
            Event::Touch(touch::Event::FingerPressed {
                id: touch::Finger(1),
                position: Point::new(100.0, 13.0),
            }),
            Event::Touch(touch::Event::FingerMoved {
                id: touch::Finger(1),
                position: Point::new(150.0, 13.0),
            }),
            Event::Touch(touch::Event::FingerLifted {
                id: touch::Finger(1),
                position: Point::new(150.0, 13.0),
            }),
        ] {
            assert!(
                !driver
                    .send(event, cursor)
                    .iter()
                    .any(|message| matches!(message, Message::Change(..)))
            );
        }
        assert!(matches!(
            driver.click(Point::new(100.0, 13.0)).as_slice(),
            [Message::CloseSettings]
        ));
        assert!(matches!(
            driver.click(Point::new(380.0, 12.0)).as_slice(),
            [Message::Settings]
        ));
        assert!(matches!(
            driver
                .send(arrow_up(), mouse::Cursor::Available(Point::new(260.0, 53.0)))
                .as_slice(),
            [Message::Contrast(_)]
        ));
        let _ = driver.send(
            Event::Keyboard(keyboard::Event::ModifiersChanged(keyboard::Modifiers::NONE)),
            mouse::Cursor::Unavailable,
        );
        assert!(matches!(
            driver.click(Point::new(300.0, 53.0)).as_slice(),
            [Message::Contrast(_)]
        ));
        driver.rebuild(modal_view(false));
        assert!(matches!(
            driver.send(arrow_up(), cursor).as_slice(),
            [Message::Change(GAIN_PARAMETER, _)]
        ));
    }

    #[test]
    fn assembled_modal_excludes_background_native_focus_operations_but_retains_foreground_traversal() {
        let mut driver = Driver::new(modal_view(false));
        driver.operate(Box::new(operation::focusable::focus("background-input".into())));
        assert!(driver.focused("background-input"));
        driver.rebuild(modal_view(true));
        assert!(!driver.focused("background-input"));
        driver.operate(Box::new(operation::focusable::focus("background-input".into())));
        driver.operate(Box::new(operation::focusable::focus_next::<()>()));
        assert!(driver.focused("settings-input"));
        assert!(!driver.focused("background-input"));
        let input = key(
            keyboard::Key::Character("5".into()),
            keyboard::key::Code::Digit5,
            Some("5"),
        );
        assert!(matches!(
            driver.send(input, mouse::Cursor::Unavailable).as_slice(),
            [Message::Ignore]
        ));
    }
}
