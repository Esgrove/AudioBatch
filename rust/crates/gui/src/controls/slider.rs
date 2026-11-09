//! Focusable slider composition with shared pointer, keyboard, and accessibility value changes.
//! Base owns dragging and stepping, while the application supplies the existing track and thumb presentation.

use std::rc::Rc;

use gpui_kit::base::spring;

use gpui_kit::base::slider::{Slider as BaseSlider, SliderIndicator, SliderThumb, SliderTrack};
use gpui_kit::component::slider::SliderState;
use gpui_kit::component::{ActiveTheme as _, ThemeStyled as _};
use gpui_kit::{
    App, Div, Entity, FocusHandle, InteractiveElement as _, IntoElement, KeyBinding, MouseButton, ParentElement as _,
    RenderOnce, SharedString, Stateful, StatefulInteractiveElement as _, Styled as _, Window, div,
    prelude::FluentBuilder as _, px, relative,
};

use super::{ControlsEvent, EventCallback, SliderControl};

/// Context shared by focused preview sliders, without intercepting root application commands.
const KEY_CONTEXT: &str = "AudioBatchSlider";

/// Larger keyboard movement in configured slider steps.
const COARSE_STEPS: f32 = 10.0;

/// Width of a thumb's pointer and focus ring, matching the styled slider's outward feedback.
const THUMB_RING_WIDTH: f32 = 3.0;

/// Opacity of the thumb's outward ring.
const THUMB_RING_ALPHA: f32 = 0.5;

/// Moves the focused slider down by one configured step.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = preview_slider)]
struct Decrease;

/// Moves the focused slider up by one configured step.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = preview_slider)]
struct Increase;

/// Moves the focused slider down by the larger keyboard increment.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = preview_slider)]
struct DecreaseFast;

/// Moves the focused slider up by the larger keyboard increment.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = preview_slider)]
struct IncreaseFast;

/// Moves the focused slider to its lower bound.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = preview_slider)]
struct Minimum;

/// Moves the focused slider to its upper bound.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = preview_slider)]
struct Maximum;

/// Restores the focused slider's domain default.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = preview_slider)]
struct Reset;

/// Value movement requested by the focused slider's keyboard command.
#[derive(Clone, Copy)]
enum Movement {
    Steps(f32),
    Minimum,
    Maximum,
}

/// Pointer feedback retained by the thumb across rendered element trees.
#[derive(Default)]
struct ThumbInteraction {
    hovered: bool,
    pressed: bool,
}

/// A single-value slider whose semantic node owns focus and whose owner retains its value and focus handle.
#[derive(IntoElement)]
pub(super) struct FocusSlider {
    state: Entity<SliderState>,
    focus: FocusHandle,
    label: SharedString,
    enabled: bool,
    control: SliderControl,
    on_event: EventCallback,
    content: Option<BaseSlider>,
}

impl FocusSlider {
    /// Creates a horizontal slider, optionally using caller-supplied Base content for the target bar.
    pub(super) fn new(
        state: &Entity<SliderState>,
        focus: &FocusHandle,
        label: &'static str,
        enabled: bool,
        control: SliderControl,
        on_event: &EventCallback,
    ) -> Self {
        Self {
            state: state.clone(),
            focus: focus.clone(),
            label: label.into(),
            enabled,
            control,
            on_event: Rc::clone(on_event),
            content: None,
        }
    }

    /// Keeps the normalization target's application-owned bar presentation inside the focusable semantic root.
    pub(super) fn content(mut self, content: BaseSlider) -> Self {
        self.content = Some(content);
        self
    }
}

impl RenderOnce for FocusSlider {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let enabled = self.enabled;
        let focus = self.focus.clone().tab_stop(enabled);
        let focus_on_press = focus.clone();
        let focused = enabled && focus.is_focused(window) && window.last_input_was_keyboard();
        let content = self
            .content
            .unwrap_or_else(|| standard_slider(&self.state, &focus, enabled, focused, window, cx));
        let reset = Rc::clone(&self.on_event);
        let control = self.control;
        let wrapper = div()
            .id(("slider-keyboard", self.state.entity_id()))
            .key_context(KEY_CONTEXT)
            .size_full()
            .capture_any_mouse_down(move |event, window, cx| {
                if enabled && event.button == MouseButton::Left {
                    window.focus(&focus_on_press, cx);
                }
            })
            .on_action(move |_: &Reset, window, cx| {
                if enabled {
                    reset(&ControlsEvent::ResetSlider(control), window, cx);
                }
            });
        let wrapper = movement_handler::<Decrease>(wrapper, &self.state, enabled, Movement::Steps(-1.0));
        let wrapper = movement_handler::<Increase>(wrapper, &self.state, enabled, Movement::Steps(1.0));
        let wrapper = movement_handler::<DecreaseFast>(wrapper, &self.state, enabled, Movement::Steps(-COARSE_STEPS));
        let wrapper = movement_handler::<IncreaseFast>(wrapper, &self.state, enabled, Movement::Steps(COARSE_STEPS));
        let wrapper = movement_handler::<Minimum>(wrapper, &self.state, enabled, Movement::Minimum);
        let wrapper = movement_handler::<Maximum>(wrapper, &self.state, enabled, Movement::Maximum);
        wrapper
            .rounded(cx.theme().radius)
            .when(focused, |wrapper| wrapper.focus_ring_style(window, cx))
            .child(
                content
                    .track_focus(&focus)
                    .accessibility_label(self.label)
                    .disabled(!enabled),
            )
    }
}

/// Installs contextual slider commands once with the application's other bindings.
pub(super) fn install(cx: &mut App) {
    let context = Some(KEY_CONTEXT);
    cx.bind_keys([
        KeyBinding::new("left", Decrease, context),
        KeyBinding::new("down", Decrease, context),
        KeyBinding::new("right", Increase, context),
        KeyBinding::new("up", Increase, context),
        KeyBinding::new("shift-left", DecreaseFast, context),
        KeyBinding::new("shift-down", DecreaseFast, context),
        KeyBinding::new("shift-right", IncreaseFast, context),
        KeyBinding::new("shift-up", IncreaseFast, context),
        KeyBinding::new("pagedown", DecreaseFast, context),
        KeyBinding::new("pageup", IncreaseFast, context),
        KeyBinding::new("home", Minimum, context),
        KeyBinding::new("end", Maximum, context),
        KeyBinding::new(
            if cfg!(target_os = "macos") { "cmd-0" } else { "ctrl-0" },
            Reset,
            context,
        ),
    ]);
}

/// Adds one contextual command without duplicating the slider's snapping, event, or persistence policy.
fn movement_handler<A: gpui_kit::Action>(
    element: Stateful<Div>,
    state: &Entity<SliderState>,
    enabled: bool,
    movement: Movement,
) -> Stateful<Div> {
    let state = state.clone();
    element.on_action(move |_: &A, window, cx| {
        if !enabled {
            return;
        }
        state.update(cx, |state, cx| {
            let delta = match movement {
                Movement::Steps(steps) => steps * state.step_value(),
                Movement::Minimum => state.min_value() - state.value().start(),
                Movement::Maximum => state.max_value() - state.value().start(),
            };
            state.step_thumb(false, delta, window, cx);
        });
    })
}

/// Updates thumb press feedback only when its state changes.
fn set_pressed(interaction: &Entity<ThumbInteraction>, pressed: bool, cx: &mut App) {
    interaction.update(cx, |state, cx| {
        if state.pressed != pressed {
            state.pressed = pressed;
            cx.notify();
        }
    });
}

/// Composes the standard horizontal track and thumb from Base parts, preserving themed sizing and colors.
fn standard_slider(
    state: &Entity<SliderState>,
    focus: &FocusHandle,
    enabled: bool,
    focused: bool,
    window: &mut Window,
    cx: &mut App,
) -> BaseSlider {
    let percentage = state.read(cx).percentage().end;
    let bar = cx.theme().slider_bar;
    let interaction = window.use_keyed_state(("preview-slider-thumb", state.entity_id()), cx, |_, _| {
        ThumbInteraction::default()
    });
    let show_ring = enabled && (focused || interaction.read(cx).hovered || interaction.read(cx).pressed);
    let ring_width = THUMB_RING_WIDTH
        * spring(
            (("preview-slider-ring", state.entity_id()), "thumb"),
            if show_ring { 1.0 } else { 0.0 },
            cx.theme().motion_tokens().spring_control,
            window,
            cx,
        );
    let hover = interaction.clone();
    let pressed = interaction.clone();
    let released = interaction.clone();
    let released_out = interaction;
    let thumb_focus = focus.clone();
    let thumb = SliderThumb::new(state).disabled(!enabled).when(enabled, |thumb| {
        thumb
            .absolute()
            .top(px(-5.0))
            .left(relative(percentage))
            .ml(px(-8.0))
            .size_4()
            .p(px(1.0))
            .rounded_full()
            .bg(bar.opacity(0.5))
            .on_hover(move |hovered, _, cx| {
                hover.update(cx, |state, cx| {
                    state.hovered = *hovered;
                    cx.notify();
                });
            })
            // Endpoint thumbs overhang the track, so its wrapper cannot capture every valid thumb press.
            .capture_any_mouse_down(move |event, window, cx| {
                if event.button == MouseButton::Left {
                    window.focus(&thumb_focus, cx);
                }
                set_pressed(&pressed, true, cx);
            })
            .capture_any_mouse_up(move |_, _, cx| set_pressed(&released, false, cx))
            .on_mouse_up_out(MouseButton::Left, move |_, _, cx| set_pressed(&released_out, false, cx))
            .child(
                div()
                    .absolute()
                    .inset(px(-ring_width))
                    .rounded_full()
                    .border(px(ring_width))
                    .border_color(cx.theme().ring.opacity(THUMB_RING_ALPHA)),
            )
            .child(div().size_full().rounded_full().bg(cx.theme().slider_thumb))
    });
    BaseSlider::new(state)
        .flex()
        .flex_1()
        .items_center()
        .justify_center()
        .w_full()
        .child(
            SliderTrack::new(state)
                .disabled(!enabled)
                .flex()
                .items_center()
                .h_6()
                .w_full()
                .flex_shrink_0()
                .child(
                    SliderIndicator::new(state)
                        .relative()
                        .w_full()
                        .h_1p5()
                        .rounded_full()
                        .bg(bar.opacity(0.2))
                        .active(move |style| style.bg(bar.opacity(0.4)))
                        .child(
                            div()
                                .absolute()
                                .h_full()
                                .left_0()
                                .w(relative(percentage))
                                .bg(bar)
                                .rounded_full(),
                        )
                        .child(thumb),
                ),
        )
}
