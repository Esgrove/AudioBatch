//! Native numeric text inputs expose edit lifetimes without replacing Iced's caret, clipboard, or focus operations.

use iced::advanced::widget::{Operation, tree};
use iced::advanced::{Clipboard, Layout, Shell, Widget, layout, renderer, text};
use iced::widget::text_input::{self, TextInput};
use iced::{Element, Event, Font, Length, Rectangle, Size, Theme, Vector, keyboard, mouse};
use iced_baseview as iced;

use super::{Message, theme};

/// Builds an owned native text input whose messages address the authoritative editor parameter.
/// `active` identifies the editor's current draft, so rebuilding releases focus from superseded edits.
/// All allocation and event processing belong to the editor thread, never the audio callback.
pub(super) fn input<'a>(
    id: u32,
    value: &str,
    width: f32,
    height: f32,
    invalid: bool,
    active: bool,
    blocked: bool,
) -> Element<'a, Message> {
    let mut input = NumericInput::<iced::Renderer>::new(id, value, width, height, invalid, active);
    input.blocked = blocked;
    Element::new(input)
}

/// A single native child retains text editing state while the wrapper observes its focus transitions.
struct NumericInput<'a, Renderer: text::Renderer> {
    id: u32,
    active: bool,
    blocked: bool,
    child: TextInput<'a, Message, Theme, Renderer>,
}

/// Last published focus survives view rebuilding and detects changes made by native focus operations.
#[derive(Default)]
struct Interaction {
    focused: bool,
}

impl<Renderer: text::Renderer<Font = Font>> NumericInput<'_, Renderer> {
    /// Uses native padding for the complete target height, without an extra layout or painted box.
    fn new(id: u32, value: &str, width: f32, height: f32, invalid: bool, active: bool) -> Self {
        Self {
            id,
            active,
            blocked: false,
            child: TextInput::new("", value)
                .id(format!("numeric-{id}"))
                .on_input(move |value| Message::NumericDraft(id, value))
                .on_submit(Message::NumericCommit(id))
                .font(theme::MONO)
                .size(theme::VALUE_SIZE)
                .line_height(theme::LINE_HEIGHT)
                .align_x(iced::alignment::Horizontal::Center)
                .width(width)
                .padding([(height - theme::VALUE_SIZE).max(0.0) * 0.5, 4.0])
                .style(move |_, status| theme::numeric_input(status, invalid)),
        }
    }
}

impl<Renderer: text::Renderer> NumericInput<'_, Renderer> {
    /// Publishes operation-driven focus changes before any native input message can reach the editor.
    fn synchronize_focus(&self, tree: &mut tree::Tree, shell: &mut Shell<'_, Message>) {
        let focused = child_tree(tree)
            .state
            .downcast_ref::<text_input::State<Renderer::Paragraph>>()
            .is_focused();
        let interaction = tree.state.downcast_mut::<Interaction>();
        if interaction.focused != focused {
            interaction.focused = focused;
            shell.publish(if focused {
                Message::NumericFocus(self.id)
            } else {
                Message::NumericBlur(self.id)
            });
            shell.request_redraw();
        }
    }
}

impl<Renderer: text::Renderer> Widget<Message, Theme, Renderer> for NumericInput<'_, Renderer> {
    /// Keeps wrapper bookkeeping distinct from the native child's unchanged state tag.
    fn tag(&self) -> tree::Tag {
        tree::Tag::of::<Interaction>()
    }

    /// Retains only the focus already reported to the editor, not a second text or caret model.
    fn state(&self) -> tree::State {
        tree::State::new(Interaction::default())
    }

    /// Gives the native input its own ordinary retained tree.
    fn children(&self) -> Vec<tree::Tree> {
        vec![tree::Tree::new(&self.child as &dyn Widget<Message, Theme, Renderer>)]
    }

    /// Preserves native editing state for the current draft and releases superseded input focus.
    fn diff(&self, tree: &mut tree::Tree) {
        tree.diff_children(&[&self.child as &dyn Widget<Message, Theme, Renderer>]);
        // An operation can focus a field before an event reports NumericFocus to the editor.
        // Preserve that pending focus through rebuilds, but never revive an acknowledged, finished draft.
        if !self.active && tree.state.downcast_ref::<Interaction>().focused {
            child_tree(tree)
                .state
                .downcast_mut::<text_input::State<Renderer::Paragraph>>()
                .unfocus();
            tree.state.downcast_mut::<Interaction>().focused = false;
        }
    }

    /// Exposes exactly the native child's layout request.
    fn size(&self) -> Size<Length> {
        Widget::size(&self.child)
    }

    /// Preserves native size hints for surrounding retained containers.
    fn size_hint(&self) -> Size<Length> {
        self.child.size_hint()
    }

    /// Returns the native layout directly, without introducing another target or inset.
    fn layout(&mut self, tree: &mut tree::Tree, renderer: &Renderer, limits: &layout::Limits) -> layout::Node {
        Widget::layout(&mut self.child, child_tree(tree), renderer, limits)
    }

    /// Forwards stable-id focus and selection operations so native traversal remains available.
    fn operate(
        &mut self,
        tree: &mut tree::Tree,
        layout: Layout<'_>,
        renderer: &Renderer,
        operation: &mut dyn Operation,
    ) {
        if !self.blocked {
            self.child.operate(child_tree(tree), layout, renderer, operation);
        }
    }

    /// Observes actual native focus before and after editing, preserving all child shell requests.
    /// Operation-driven transitions are reported on the next event, before native drafts or submissions.
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
        if self.blocked {
            return;
        }
        self.synchronize_focus(tree, shell);
        let focused_before = tree.state.downcast_ref::<Interaction>().focused;
        let mut messages = Vec::new();
        let mut child_shell = Shell::new(&mut messages);
        self.child.update(
            child_tree(tree),
            event,
            layout,
            cursor,
            renderer,
            clipboard,
            &mut child_shell,
            viewport,
        );

        // Native text inputs keep their focus identity while the window is inactive.
        // Numeric editing closes instead, so activation cannot silently resume a committed draft.
        if matches!(event, Event::Window(iced::window::Event::Unfocused)) {
            child_tree(tree)
                .state
                .downcast_mut::<text_input::State<Renderer::Paragraph>>()
                .unfocus();
        }
        let focused_after = child_tree(tree)
            .state
            .downcast_ref::<text_input::State<Renderer::Paragraph>>()
            .is_focused();
        if !focused_before && focused_after {
            self.synchronize_focus(tree, shell);
        }
        shell.merge(child_shell, std::convert::identity);
        if focused_before && !focused_after {
            if matches!(
                event,
                Event::Keyboard(keyboard::Event::KeyPressed {
                    key: keyboard::Key::Named(keyboard::key::Named::Escape),
                    ..
                })
            ) {
                tree.state.downcast_mut::<Interaction>().focused = false;
                shell.publish(Message::NumericCancel(self.id));
                shell.request_redraw();
            } else {
                self.synchronize_focus(tree, shell);
            }
        }
    }

    /// Draws only the native field, including its caret, selection, and rejected-draft styling.
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
        Widget::draw(
            &self.child,
            child_tree_ref(tree),
            renderer,
            theme,
            style,
            layout,
            cursor,
            viewport,
        );
    }

    /// Retains the platform text pointer instead of imposing a numeric drag affordance.
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

    /// Passes through any native input overlay without changing its coordinate system.
    fn overlay<'a>(
        &'a mut self,
        tree: &'a mut tree::Tree,
        layout: Layout<'a>,
        renderer: &Renderer,
        viewport: &Rectangle,
        translation: Vector,
    ) -> Option<iced::advanced::overlay::Element<'a, Message, Theme, Renderer>> {
        self.child
            .overlay(child_tree(tree), layout, renderer, viewport, translation)
    }
}

/// Borrows the one retained native child without exposing its storage to the editor.
fn child_tree(tree: &mut tree::Tree) -> &mut tree::Tree {
    tree.children
        .first_mut()
        .expect("numeric input retains one native child")
}

/// Borrows native drawing and pointer state without mutating focus bookkeeping.
fn child_tree_ref(tree: &tree::Tree) -> &tree::Tree {
    tree.children.first().expect("numeric input retains one native child")
}

#[cfg(test)]
mod tests {
    use iced::advanced::widget::operation;
    use iced::{Color, Pixels, Point};

    use super::*;

    /// Exercises the production native text input with its real text shaping, but without a GPU or window.
    struct EventRenderer;

    impl iced::advanced::Renderer for EventRenderer {
        fn start_layer(&mut self, _: Rectangle) {}
        fn end_layer(&mut self) {}
        fn start_transformation(&mut self, _: iced::Transformation) {}
        fn end_transformation(&mut self) {}
        fn fill_quad(&mut self, _: renderer::Quad, _: impl Into<iced::Background>) {}
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

        fn default_font(&self) -> Font {
            theme::MONO
        }
        fn default_size(&self) -> Pixels {
            theme::VALUE_SIZE.into()
        }
        fn fill_paragraph(&mut self, _: &Self::Paragraph, _: Point, _: Color, _: Rectangle) {}
        fn fill_editor(&mut self, _: &Self::Editor, _: Point, _: Color, _: Rectangle) {}
        fn fill_text(&mut self, _: text::Text, _: Point, _: Color, _: Rectangle) {}
    }

    /// A deterministic clipboard retains the same native copy/cut/paste paths as the embedded editor.
    #[derive(Default)]
    struct ClipboardText(Option<String>);

    impl Clipboard for ClipboardText {
        fn read(&self, _: iced::advanced::clipboard::Kind) -> Option<String> {
            self.0.clone()
        }
        fn write(&mut self, _: iced::advanced::clipboard::Kind, contents: String) {
            self.0 = Some(contents);
        }
    }

    /// Editor messages and native capture from one event sent directly to the numeric input.
    struct InputOutcome {
        messages: Vec<Message>,
        captured: bool,
    }

    /// Retains the production wrapper and native tree while tests send actual Iced events and operations.
    struct Driver {
        input: NumericInput<'static, EventRenderer>,
        tree: tree::Tree,
        node: layout::Node,
        clipboard: ClipboardText,
    }

    impl Driver {
        /// Loads the same font and native layout used by the production numeric field.
        fn new(value: &str) -> Self {
            iced_widget::graphics::text::font_system()
                .write()
                .expect("test font system")
                .load_font(std::borrow::Cow::Borrowed(theme::ROBOTO_MONO));
            let mut input = NumericInput::new(7, value, 80.0, 22.0, false, false);
            let mut tree = tree::Tree::new(&input as &dyn Widget<Message, Theme, EventRenderer>);
            let node = Widget::layout(
                &mut input,
                &mut tree,
                &EventRenderer,
                &layout::Limits::new(Size::ZERO, Size::new(80.0, 22.0)),
            );
            Self {
                input,
                tree,
                node,
                clipboard: ClipboardText::default(),
            }
        }

        /// Forwards a real event and returns native event capture along with editor messages.
        fn send(&mut self, event: &Event, cursor: mouse::Cursor) -> InputOutcome {
            let mut messages = Vec::new();
            let mut shell = Shell::new(&mut messages);
            self.input.update(
                &mut self.tree,
                event,
                Layout::new(&self.node),
                cursor,
                &EventRenderer,
                &mut self.clipboard,
                &mut shell,
                &Rectangle::with_size(Size::new(200.0, 100.0)),
            );
            let captured = shell.is_event_captured();
            InputOutcome { messages, captured }
        }

        /// Sends keyboard and window events without inventing a pointer position.
        fn event(&mut self, event: &Event) -> Vec<Message> {
            self.send(event, mouse::Cursor::Unavailable).messages
        }

        /// Sends native press/release events so focus and caret hit testing follow the production path.
        fn click(&mut self, x: f32) -> Vec<Message> {
            let cursor = mouse::Cursor::Available(Point::new(x, 11.0));
            let messages = self
                .send(&Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)), cursor)
                .messages;
            let _ = self.send(&Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)), cursor);
            messages
        }

        /// Applies the same stable-id focus and selection operations available to the editor.
        fn operate(&mut self, operation: &mut dyn Operation) {
            self.input
                .operate(&mut self.tree, Layout::new(&self.node), &EventRenderer, operation);
        }

        /// Reads only native focus for assertions, never a separate test interaction model.
        fn focused(&self) -> bool {
            child_tree_ref(&self.tree)
                .state
                .downcast_ref::<text_input::State<<EventRenderer as text::Renderer>::Paragraph>>()
                .is_focused()
        }
    }

    /// Editor messages and native dispatch statuses from one event sent through the retained stack.
    struct StackOutcome {
        messages: Vec<Message>,
        statuses: Vec<iced::event::Status>,
    }

    /// Real absolute stack layers reproduce the editor's early return when an upper control captures an event.
    struct StackDriver {
        interface: Option<iced::runtime::UserInterface<'static, Message, Theme, EventRenderer>>,
        clipboard: ClipboardText,
    }

    impl StackDriver {
        /// Places two native inputs and a native button on distinct absolute stack layers.
        fn view(active: Option<u32>) -> iced::core::Element<'static, Message, Theme, EventRenderer> {
            let mut stack = iced::widget::Stack::new().width(280.0).height(22.0);
            for (id, x) in [(7, 0.0), (8, 100.0)] {
                let input = iced::core::Element::new(NumericInput::<EventRenderer>::new(
                    id,
                    "12",
                    80.0,
                    22.0,
                    false,
                    active == Some(id),
                ));
                stack = stack.push(iced::widget::pin(input).x(x));
            }
            stack
                .push(
                    iced::widget::pin(
                        iced::widget::button(iced::widget::Space::new())
                            .width(80.0)
                            .height(22.0)
                            .padding(0)
                            .on_press(Message::Toolbar),
                    )
                    .x(200.0),
                )
                .into()
        }

        /// Starts with neither input owning an editor draft or native focus.
        fn new() -> Self {
            Self {
                interface: Some(iced::runtime::UserInterface::build(
                    Self::view(None),
                    Size::new(280.0, 22.0),
                    iced::runtime::user_interface::Cache::new(),
                    &mut EventRenderer,
                )),
                clipboard: ClipboardText::default(),
            }
        }

        /// Reconciles real retained input state after the parent changes its authoritative active draft.
        fn rebuild(&mut self, active: Option<u32>) {
            let cache = self.interface.take().expect("retained stack interface").into_cache();
            self.interface = Some(iced::runtime::UserInterface::build(
                Self::view(active),
                Size::new(280.0, 22.0),
                cache,
                &mut EventRenderer,
            ));
        }

        /// Sends real events through stack dispatch instead of calling a numeric child directly.
        fn send(&mut self, event: Event, cursor: mouse::Cursor) -> StackOutcome {
            let mut messages = Vec::new();
            let (_, statuses) = self.interface.as_mut().expect("retained stack interface").update(
                &[event],
                cursor,
                &mut EventRenderer,
                &mut self.clipboard,
                &mut messages,
            );
            StackOutcome { messages, statuses }
        }

        /// Drives native button press and release while retaining the exact stack capture behavior.
        fn click(&mut self, x: f32) -> Vec<Message> {
            let cursor = mouse::Cursor::Available(Point::new(x, 11.0));
            let mut messages = self
                .send(Event::Mouse(mouse::Event::ButtonPressed(mouse::Button::Left)), cursor)
                .messages;
            let released = self.send(Event::Mouse(mouse::Event::ButtonReleased(mouse::Button::Left)), cursor);
            messages.extend(released.messages);
            messages
        }

        /// Applies every pass of the real native focus operation, including its counting phase.
        fn operate(&mut self, mut operation: Box<dyn Operation>) {
            loop {
                self.interface
                    .as_mut()
                    .expect("retained stack interface")
                    .operate(&EventRenderer, operation.as_mut());
                match operation.finish() {
                    operation::Outcome::Chain(next) => operation = next,
                    operation::Outcome::None | operation::Outcome::Some(()) => break,
                }
            }
        }

        /// Queries native focus by stable id without reaching through the stack's private child structure.
        fn focused(&mut self, id: u32) -> bool {
            let mut operation = operation::focusable::is_focused(format!("numeric-{id}").into());
            self.interface
                .as_mut()
                .expect("retained stack interface")
                .operate(&EventRenderer, &mut operation::black_box(&mut operation));
            matches!(operation.finish(), operation::Outcome::Some(true))
        }
    }

    /// Creates the native key event used by text editing and submission, including its physical identity.
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

    /// Creates named navigation and editing keys without text insertion.
    fn named(key: keyboard::key::Named, code: keyboard::key::Code) -> Event {
        self::key(keyboard::Key::Named(key), code, None)
    }

    #[test]
    fn modal_blocked_numeric_field_is_excluded_from_focus_operations_and_keyboard_input() {
        let mut driver = Driver::new("12");
        driver.input.blocked = true;
        driver.operate(&mut operation::focusable::focus("numeric-7".into()));
        assert!(!driver.focused());
        assert!(driver.click(4.0).is_empty());
        assert!(
            driver
                .event(&key(
                    keyboard::Key::Character("3".into()),
                    keyboard::key::Code::Digit3,
                    Some("3")
                ))
                .is_empty()
        );
    }

    #[test]
    fn pointer_focus_native_caret_draft_and_submit_preserve_order_and_target_height() {
        let mut driver = Driver::new("12");
        assert_eq!(driver.node.size(), Size::new(80.0, 22.0));
        assert!(matches!(driver.click(4.0).as_slice(), [Message::NumericFocus(7)]));
        assert!(driver.focused());
        let messages = driver.event(&key(
            keyboard::Key::Character("3".into()),
            keyboard::key::Code::Digit3,
            Some("3"),
        ));
        assert!(matches!(messages.as_slice(), [Message::NumericDraft(7, value)] if value == "312"));
        let InputOutcome { messages, captured } = driver.send(
            &named(keyboard::key::Named::Enter, keyboard::key::Code::Enter),
            mouse::Cursor::Unavailable,
        );
        assert!(captured);
        assert!(matches!(messages.as_slice(), [Message::NumericCommit(7)]));
        assert!(driver.focused());
    }

    #[test]
    fn native_selection_clipboard_and_view_rebuild_retain_editing_state() {
        let mut driver = Driver::new("12");
        let _ = driver.click(4.0);
        driver.operate(&mut operation::text_input::select_all("numeric-7".into()));
        let _ = driver.event(&Event::Keyboard(keyboard::Event::ModifiersChanged(
            keyboard::Modifiers::COMMAND,
        )));
        assert!(
            driver
                .event(&key(
                    keyboard::Key::Character("c".into()),
                    keyboard::key::Code::KeyC,
                    None
                ))
                .is_empty()
        );
        assert_eq!(driver.clipboard.0.as_deref(), Some("12"));
        driver.clipboard.0 = Some("-6.5\n".into());
        let messages = driver.event(&key(
            keyboard::Key::Character("v".into()),
            keyboard::key::Code::KeyV,
            None,
        ));
        assert!(matches!(messages.as_slice(), [Message::NumericDraft(7, value)] if value == "-6.5"));

        driver.input = NumericInput::new(7, "-6.5", 80.0, 22.0, false, true);
        driver
            .tree
            .diff(&driver.input as &dyn Widget<Message, Theme, EventRenderer>);
        let _ = driver.event(&Event::Keyboard(keyboard::Event::ModifiersChanged(
            keyboard::Modifiers::NONE,
        )));
        let _ = driver.event(&named(keyboard::key::Named::ArrowLeft, keyboard::key::Code::ArrowLeft));
        let messages = driver.event(&key(
            keyboard::Key::Character("2".into()),
            keyboard::key::Code::Digit2,
            Some("2"),
        ));
        assert!(matches!(messages.as_slice(), [Message::NumericDraft(7, value)] if value == "-6.25"));
    }

    #[test]
    fn escape_cancels_without_blur_and_stops_following_native_text() {
        let mut driver = Driver::new("12");
        let _ = driver.click(4.0);
        let InputOutcome { messages, captured } = driver.send(
            &named(keyboard::key::Named::Escape, keyboard::key::Code::Escape),
            mouse::Cursor::Unavailable,
        );
        assert!(captured);
        assert!(matches!(messages.as_slice(), [Message::NumericCancel(7)]));
        assert!(!driver.focused());
        assert!(driver.event(&Event::Window(iced::window::Event::Unfocused)).is_empty());
        assert!(
            driver
                .event(&key(
                    keyboard::Key::Character("3".into()),
                    keyboard::key::Code::Digit3,
                    Some("3")
                ))
                .is_empty()
        );
    }

    #[test]
    fn outside_pointer_and_window_deactivation_publish_one_blur_without_resuming_focus() {
        for window in [false, true] {
            let mut driver = Driver::new("12");
            let _ = driver.click(4.0);
            let messages = if window {
                driver.event(&Event::Window(iced::window::Event::Unfocused))
            } else {
                driver.click(120.0)
            };
            assert!(matches!(messages.as_slice(), [Message::NumericBlur(7)]));
            assert!(!driver.focused());
            assert!(driver.event(&Event::Window(iced::window::Event::Unfocused)).is_empty());
            assert!(driver.event(&Event::Window(iced::window::Event::Focused)).is_empty());
            assert!(!driver.focused());
        }
    }

    #[test]
    fn inactive_rebuild_clears_ghost_focus_after_upper_stack_input_captures_click() {
        let mut driver = StackDriver::new();
        assert!(matches!(driver.click(4.0).as_slice(), [Message::NumericFocus(7)]));
        driver.rebuild(Some(7));
        assert!(driver.focused(7));
        assert!(!driver.focused(8));

        assert!(matches!(driver.click(104.0).as_slice(), [Message::NumericFocus(8)]));
        // The upper input captured the event before the lower input could observe the outside click.
        assert!(driver.focused(7));
        assert!(driver.focused(8));
        driver.rebuild(Some(8));
        assert!(!driver.focused(7));
        assert!(driver.focused(8));

        let StackOutcome { messages, .. } = driver.send(
            key(
                keyboard::Key::Character("3".into()),
                keyboard::key::Code::Digit3,
                Some("3"),
            ),
            mouse::Cursor::Unavailable,
        );
        assert!(matches!(messages.as_slice(), [Message::NumericDraft(8, value)] if value == "312"));
        driver.rebuild(Some(8));
        let StackOutcome { messages, .. } = driver.send(
            named(keyboard::key::Named::Escape, keyboard::key::Code::Escape),
            mouse::Cursor::Unavailable,
        );
        assert!(matches!(messages.as_slice(), [Message::NumericCancel(8)]));
        driver.rebuild(None);
        let StackOutcome { messages, .. } = driver.send(
            key(
                keyboard::Key::Character("3".into()),
                keyboard::key::Code::Digit3,
                Some("3"),
            ),
            mouse::Cursor::Unavailable,
        );
        assert!(messages.is_empty());
    }

    #[test]
    fn clearing_parent_draft_after_captured_stack_action_removes_native_focus_without_blur() {
        let mut driver = StackDriver::new();
        let _ = driver.click(4.0);
        driver.rebuild(Some(7));
        assert!(matches!(driver.click(240.0).as_slice(), [Message::Toolbar]));
        assert!(driver.focused(7));
        driver.rebuild(None);
        assert!(!driver.focused(7));
        assert!(!driver.focused(8));
        for event in [
            Event::Window(iced::window::Event::RedrawRequested(iced::core::time::Instant::now())),
            key(
                keyboard::Key::Character("3".into()),
                keyboard::key::Code::Digit3,
                Some("3"),
            ),
            named(keyboard::key::Named::Enter, keyboard::key::Code::Enter),
        ] {
            assert!(driver.send(event, mouse::Cursor::Unavailable).messages.is_empty());
        }
    }

    #[test]
    fn native_tab_operations_survive_rebuild_before_focus_notification_and_preserve_caret() {
        let mut driver = StackDriver::new();
        let _ = driver.click(4.0);
        driver.rebuild(Some(7));
        let StackOutcome { messages, statuses } = driver.send(
            named(keyboard::key::Named::Tab, keyboard::key::Code::Tab),
            mouse::Cursor::Unavailable,
        );
        assert!(messages.is_empty());
        assert_eq!(statuses, [iced::event::Status::Ignored]);
        // Keep the current draft through the pre-operation view rebuild so traversal retains its origin.
        driver.rebuild(Some(7));
        driver.operate(Box::new(operation::focusable::focus_next::<()>()));
        assert!(!driver.focused(7));
        assert!(driver.focused(8));
        // The target's focus has not yet produced an editor message, so active still identifies the old draft.
        driver.rebuild(Some(7));
        assert!(driver.focused(8));
        let StackOutcome { messages, .. } = driver.send(
            key(
                keyboard::Key::Character("3".into()),
                keyboard::key::Code::Digit3,
                Some("3"),
            ),
            mouse::Cursor::Unavailable,
        );
        assert!(
            matches!(messages.as_slice(), [Message::NumericFocus(8), Message::NumericDraft(8, value)] if value == "123")
        );
        driver.rebuild(Some(8));
        driver.operate(Box::new(operation::focusable::focus_previous::<()>()));
        driver.rebuild(Some(8));
        let StackOutcome { messages, .. } = driver.send(
            Event::Window(iced::window::Event::RedrawRequested(iced::core::time::Instant::now())),
            mouse::Cursor::Unavailable,
        );
        assert!(matches!(
            messages.as_slice(),
            [Message::NumericBlur(8), Message::NumericFocus(7)]
        ));
        driver.rebuild(Some(7));
        assert!(driver.focused(7));
        assert!(!driver.focused(8));
    }

    #[test]
    fn native_focus_operations_publish_focus_before_first_draft_and_blur_after_tab_traversal() {
        let mut driver = Driver::new("12");
        driver.operate(&mut operation::focusable::focus("numeric-7".into()));
        let messages = driver.event(&key(
            keyboard::Key::Character("3".into()),
            keyboard::key::Code::Digit3,
            Some("3"),
        ));
        assert!(
            matches!(messages.as_slice(), [Message::NumericFocus(7), Message::NumericDraft(7, value)] if value == "123")
        );

        let InputOutcome { messages, captured } = driver.send(
            &named(keyboard::key::Named::Tab, keyboard::key::Code::Tab),
            mouse::Cursor::Unavailable,
        );
        assert!(messages.is_empty());
        assert!(!captured);
        // The editor routes uncaptured Tab to Iced's traversal task, which performs this native operation.
        let mut next = operation::focusable::focus_next::<()>();
        driver.operate(&mut next);
        let operation::Outcome::Chain(mut next) = next.finish() else {
            unreachable!("focus traversal counts widgets before applying focus")
        };
        driver.operate(next.as_mut());
        assert!(!driver.focused());
        let messages = driver.event(&Event::Window(iced::window::Event::RedrawRequested(
            iced::core::time::Instant::now(),
        )));
        assert!(matches!(messages.as_slice(), [Message::NumericBlur(7)]));
    }
}
