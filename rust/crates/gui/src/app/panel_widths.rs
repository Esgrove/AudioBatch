//! Independent preview-column geometry, native resize handles, and committed width preferences.

use std::rc::Rc;

use gpui_kit::base::{InteractiveElementExt as _, ResizeHandleContext, ResizeHandleRenderer, ResizeHandleState};
use gpui_kit::component::ActiveTheme as _;
use gpui_kit::component::resizable::resize_handle;
use gpui_kit::{
    AccessibleAction, App, AppContext as _, Axis, Context, Div, DragMoveEvent, Empty, FocusHandle, Global,
    InteractiveElement as _, IntoElement, KeyBinding, MouseButton, Orientation, ParentElement as _, Render, Role,
    StatefulInteractiveElement as _, Styled as _, Window, div, px,
};

use super::{AppView, SECTION_SPACING};
use crate::settings::{
    DEFAULT_CONTROLS_WIDTH_SCALE, DEFAULT_INFORMATION_WIDTH_SCALE, MAXIMUM_PANEL_WIDTH_SCALE,
    MINIMUM_CONTROLS_WIDTH_SCALE, MINIMUM_INFORMATION_WIDTH_SCALE,
};
use crate::theme::tooltip;

/// Width reserved for each divider, with its native nine-pixel hit band entirely inside the gap.
const COLUMN_GAP: f32 = 10.0;
/// Unscaled responsive controls minimum.
const CONTROLS_MINIMUM_WIDTH: f32 = 209.0;
/// Unscaled responsive controls maximum.
const CONTROLS_MAXIMUM_WIDTH: f32 = 228.0;
/// Unscaled responsive controls share of the content width.
const CONTROLS_WIDTH_PROPORTION: f32 = 0.14;
/// Unscaled responsive information-panel minimum.
const INFORMATION_MINIMUM_WIDTH: f32 = 234.0;
/// Unscaled responsive information-panel maximum.
const INFORMATION_MAXIMUM_WIDTH: f32 = 270.0;
/// Unscaled responsive information-panel share of the content width.
const INFORMATION_WIDTH_PROPORTION: f32 = 0.22;
/// Content width that switches both columns to their wide layouts.
const WIDE_BREAKPOINT: f32 = 1580.0;
/// Share of the wide-layout left half occupied by the controls.
const WIDE_CONTROLS_PROPORTION: f32 = 0.48;
/// Unscaled controls maximum in the wide layout.
const WIDE_CONTROLS_MAXIMUM_WIDTH: f32 = 500.0;
/// Unscaled information-panel maximum in the wide layout.
const WIDE_INFORMATION_MAXIMUM_WIDTH: f32 = 540.0;
/// Smallest waveform width, unless the whole content area is narrower than it.
const WAVEFORM_MINIMUM_WIDTH: f32 = 140.0;
/// Keyboard movement of a divider in resolved window pixels.
const KEYBOARD_STEP: f32 = 4.0;
/// Accelerated keyboard movement while Shift is held, matching the panel-width interaction contract.
const FAST_KEYBOARD_STEP: f32 = 16.0;
/// Key context shared by the two separately focused preview dividers.
const WIDTH_KEY_CONTEXT: &str = "PreviewPanelWidth";

/// Which column a divider changes, without changing its sibling's preference.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum PreviewPanel {
    /// The leading controls column, controlled by the first divider.
    Controls,
    /// The middle information column, controlled by the second divider.
    Information,
}

/// One pending native gesture, kept out of persisted settings until release.
#[derive(Clone, Copy)]
pub(super) struct PanelWidthDrag {
    panel: PreviewPanel,
    pointer_start: f32,
    initial_width: f32,
    scale: f32,
}

/// Fully resolved expanded-preview columns, derived without modifying the saved scales.
#[non_exhaustive]
#[derive(Clone, Copy, Debug)]
pub(super) struct PreviewWidths {
    /// Resolved leading controls width in logical window pixels.
    pub controls: f32,
    /// Resolved middle information width in logical window pixels.
    pub information: f32,
    /// Remaining waveform width after both columns and the two gaps.
    pub waveform: f32,
    /// Gap occupied by each native resize band, never additional to column spacing.
    pub gap: f32,
    /// Whether the unchanged responsive breakpoint selects the wide content layout.
    pub wide: bool,
    constrained: bool,
}

/// Application-lifetime marker, avoiding duplicate keyboard registrations when the root view is recreated.
struct PanelWidthBindings;

/// Moves the focused vertical divider toward the leading edge.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
struct NarrowPreviewPanel;
/// Moves the focused vertical divider toward the trailing edge.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
struct WidenPreviewPanel;
/// Moves the focused vertical divider toward the leading edge by the accelerated keyboard step.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
struct NarrowPreviewPanelFast;
/// Moves the focused vertical divider toward the trailing edge by the accelerated keyboard step.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
struct WidenPreviewPanelFast;
/// Restores only the focused divider's default relative width.
#[derive(Clone, Default, PartialEq, Eq, Debug, gpui_kit::Action)]
#[action(namespace = audiobatch)]
struct ResetPreviewPanelWidth;

impl Global for PanelWidthBindings {}

impl PreviewPanel {
    /// Returns this panel's stored default and lower bound, relative to its responsive baseline.
    const fn scale_limits(self) -> (f32, f32) {
        match self {
            Self::Controls => (DEFAULT_CONTROLS_WIDTH_SCALE, MINIMUM_CONTROLS_WIDTH_SCALE),
            Self::Information => (DEFAULT_INFORMATION_WIDTH_SCALE, MINIMUM_INFORMATION_WIDTH_SCALE),
        }
    }

    /// Selects one column from resolved or baseline geometry.
    const fn width(self, widths: PreviewWidths) -> f32 {
        match self {
            Self::Controls => widths.controls,
            Self::Information => widths.information,
        }
    }

    /// Selects the other independent column from resolved geometry.
    const fn sibling_width(self, widths: PreviewWidths) -> f32 {
        match self {
            Self::Controls => widths.information,
            Self::Information => widths.controls,
        }
    }
}

impl Render for PreviewPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        Empty
    }
}

impl AppView {
    /// Creates stable Tab targets and registers only divider-scoped keyboard commands.
    pub(super) fn new_panel_width_focus(cx: &mut Context<Self>) -> [FocusHandle; 2] {
        if !cx.has_global::<PanelWidthBindings>() {
            cx.bind_keys([
                KeyBinding::new("left", NarrowPreviewPanel, Some(WIDTH_KEY_CONTEXT)),
                KeyBinding::new("right", WidenPreviewPanel, Some(WIDTH_KEY_CONTEXT)),
                KeyBinding::new("shift-left", NarrowPreviewPanelFast, Some(WIDTH_KEY_CONTEXT)),
                KeyBinding::new("shift-right", WidenPreviewPanelFast, Some(WIDTH_KEY_CONTEXT)),
                KeyBinding::new("home", ResetPreviewPanelWidth, Some(WIDTH_KEY_CONTEXT)),
            ]);
            cx.set_global(PanelWidthBindings);
        }
        [cx.focus_handle().tab_stop(true), cx.focus_handle().tab_stop(true)]
    }

    /// Resolves live drag feedback without writing pending changes into canonical settings.
    pub(super) fn panel_widths(&self, content_width: f32) -> PreviewWidths {
        let mut controls = self.settings.controls_width_scale;
        let mut information = self.settings.information_width_scale;
        if let Some(drag) = self.panel_width_drag {
            match drag.panel {
                PreviewPanel::Controls => controls = drag.scale,
                PreviewPanel::Information => information = drag.scale,
            }
        }
        resolve_widths(content_width, controls, information)
    }

    /// Renders a native resize band inside an existing gap, with focus and commands owned by the root view.
    pub(super) fn render_panel_width_divider(
        &self,
        panel: PreviewPanel,
        content_width: f32,
        cx: &Context<Self>,
    ) -> Div {
        let (id, label, focus) = match panel {
            PreviewPanel::Controls => ("controls-width-divider", "Resize controls", &self.panel_width_focus[0]),
            PreviewPanel::Information => (
                "information-width-divider",
                "Resize information panel",
                &self.panel_width_focus[1],
            ),
        };
        let drag_owner = cx.weak_entity();
        let focus_on_press = focus.clone();
        let focus_accessible = focus.clone();
        let widths = self.panel_widths(content_width);
        div().relative().w(px(widths.gap)).h_full().flex_none().child(
            div().absolute().left(px(widths.gap * 0.5)).h_full().child(
                resize_handle(id, Axis::Horizontal)
                    .with_appearance(width_divider_appearance(focus.clone()))
                    .track_focus(focus)
                    .key_context(WIDTH_KEY_CONTEXT)
                    .role(Role::Splitter)
                    .aria_label(label)
                    .aria_orientation(Orientation::Vertical)
                    .aria_value(format!("{:.0} pixels", panel.width(widths)))
                    .on_a11y_action(AccessibleAction::Focus, move |_, window, cx| {
                        window.focus(&focus_accessible, cx);
                    })
                    .focus_visible(|style| style.bg(cx.theme().ring.opacity(0.2)))
                    .tooltip(tooltip(
                        "Drag or use Left/Right to resize. Hold Shift for larger steps. Double-click or Home to reset.",
                    ))
                    .on_mouse_down(
                        MouseButton::Left,
                        cx.listener(move |view, event: &gpui_kit::MouseDownEvent, window, cx| {
                            window.focus(&focus_on_press, cx);
                            let widths = view.panel_widths(content_width);
                            let scale = match panel {
                                PreviewPanel::Controls => view.settings.controls_width_scale,
                                PreviewPanel::Information => view.settings.information_width_scale,
                            };
                            view.panel_width_drag = Some(PanelWidthDrag {
                                panel,
                                pointer_start: f32::from(event.position.x),
                                initial_width: panel.width(widths),
                                scale,
                            });
                        }),
                    )
                    .on_drag(panel, move |panel, _, window, cx| {
                        let _ = drag_owner.update(cx, |view, cx| {
                            view.drag_panel_width_at(f32::from(window.mouse_position().x), content_width, cx);
                        });
                        cx.new(|_| *panel)
                    })
                    .on_double_click(cx.listener(move |view, _, _, cx| view.reset_panel_width(panel, cx)))
                    .on_action(cx.listener(move |view, _: &NarrowPreviewPanel, _, cx| {
                        view.step_panel_width(panel, content_width, -KEYBOARD_STEP, cx);
                    }))
                    .on_action(cx.listener(move |view, _: &WidenPreviewPanel, _, cx| {
                        view.step_panel_width(panel, content_width, KEYBOARD_STEP, cx);
                    }))
                    .on_action(cx.listener(move |view, _: &NarrowPreviewPanelFast, _, cx| {
                        view.step_panel_width(panel, content_width, -FAST_KEYBOARD_STEP, cx);
                    }))
                    .on_action(cx.listener(move |view, _: &WidenPreviewPanelFast, _, cx| {
                        view.step_panel_width(panel, content_width, FAST_KEYBOARD_STEP, cx);
                    }))
                    .on_action(
                        cx.listener(move |view, _: &ResetPreviewPanelWidth, _, cx| view.reset_panel_width(panel, cx)),
                    ),
            ),
        )
    }

    /// Updates only the pending width while the native payload is active, even outside its original band.
    pub(super) fn drag_panel_width(
        &mut self,
        event: &DragMoveEvent<PreviewPanel>,
        content_width: f32,
        cx: &mut Context<Self>,
    ) {
        let Some(drag) = self.panel_width_drag else { return };
        if *event.drag(cx) != drag.panel {
            return;
        }
        self.drag_panel_width_at(f32::from(event.event.position.x), content_width, cx);
    }

    /// Resolves both native drag initiation and subsequent movement from the original pressed geometry.
    fn drag_panel_width_at(&mut self, pointer_x: f32, content_width: f32, cx: &mut Context<Self>) {
        let Some(drag) = self.panel_width_drag else { return };
        let requested = drag.initial_width + pointer_x - drag.pointer_start;
        if let Some(scale) = requested_scale(drag.panel, self.panel_widths(content_width), content_width, requested)
            && (scale - drag.scale).abs() > f32::EPSILON
        {
            self.panel_width_drag = Some(PanelWidthDrag { scale, ..drag });
            cx.notify();
        }
    }

    /// Commits one completed pointer gesture. Unchanged clicks do not create a settings write.
    pub(super) fn finish_panel_width_drag(&mut self, cx: &mut Context<Self>) {
        if let Some(drag) = self.panel_width_drag.take() {
            self.commit_panel_width(drag.panel, drag.scale, cx);
        }
    }

    /// Applies a keyboard move to only its focused column and saves that explicit edit immediately.
    fn step_panel_width(&mut self, panel: PreviewPanel, content_width: f32, delta: f32, cx: &mut Context<Self>) {
        let widths = self.panel_widths(content_width);
        if let Some(scale) = requested_scale(panel, widths, content_width, panel.width(widths) + delta) {
            self.commit_panel_width(panel, scale, cx);
        }
    }

    /// Restores just one default, leaving the other scale and the preview height untouched.
    fn reset_panel_width(&mut self, panel: PreviewPanel, cx: &mut Context<Self>) {
        self.panel_width_drag = None;
        self.commit_panel_width(panel, panel.scale_limits().0, cx);
    }

    /// Routes completed width edits through the canonical preference owner and its settings-window synchronization.
    fn commit_panel_width(&mut self, panel: PreviewPanel, scale: f32, cx: &mut Context<Self>) {
        let mut settings = self.settings.clone();
        let target = match panel {
            PreviewPanel::Controls => &mut settings.controls_width_scale,
            PreviewPanel::Information => &mut settings.information_width_scale,
        };
        if (*target - scale).abs() <= f32::EPSILON {
            return;
        }
        *target = scale;
        self.apply_settings(settings, cx);
    }
}

/// Returns the unscaled responsive widths. Compact controls retain this calculation independently of saved scales.
pub(super) fn baseline_widths(content_width: f32) -> PreviewWidths {
    let half = content_width * 0.5;
    let wide = content_width >= WIDE_BREAKPOINT;
    let (controls, information) = if wide {
        let shared = half - SECTION_SPACING;
        (
            (shared * WIDE_CONTROLS_PROPORTION).min(WIDE_CONTROLS_MAXIMUM_WIDTH),
            (shared * (1.0 - WIDE_CONTROLS_PROPORTION)).min(WIDE_INFORMATION_MAXIMUM_WIDTH),
        )
    } else {
        (
            compact_controls_width(content_width),
            (content_width * INFORMATION_WIDTH_PROPORTION).clamp(INFORMATION_MINIMUM_WIDTH, INFORMATION_MAXIMUM_WIDTH),
        )
    };
    PreviewWidths {
        controls,
        information,
        waveform: 0.0,
        gap: COLUMN_GAP,
        wide,
        constrained: false,
    }
}

/// Returns the original compact playback width, unaffected by either expanded-panel preference.
pub(super) fn compact_controls_width(content_width: f32) -> f32 {
    (content_width * CONTROLS_WIDTH_PROPORTION).clamp(CONTROLS_MINIMUM_WIDTH, CONTROLS_MAXIMUM_WIDTH)
}

/// Fits preferred columns into the available space without ever writing constrained geometry back to preferences.
fn resolve_widths(content_width: f32, controls_scale: f32, information_scale: f32) -> PreviewWidths {
    let content_width = content_width.max(0.0);
    let mut widths = baseline_widths(content_width);
    widths.gap = COLUMN_GAP.min((content_width - WAVEFORM_MINIMUM_WIDTH).max(0.0) * 0.5);
    widths.controls *= controls_scale;
    widths.information *= information_scale;
    let available = (widths.gap.mul_add(-2.0, content_width) - WAVEFORM_MINIMUM_WIDTH).max(0.0);
    let preferred = widths.controls + widths.information;
    widths.constrained = preferred > available;
    if widths.constrained {
        let shrink = available / preferred;
        widths.controls *= shrink;
        widths.information *= shrink;
    }
    widths.waveform = widths
        .gap
        .mul_add(-2.0, content_width - widths.controls - widths.information)
        .max(0.0);
    widths
}

/// Constrains an explicit edit against its baseline limits and the unchanged sibling's resolved width.
/// A temporarily cramped window cannot replace a saved preference with an impossible minimum.
fn requested_scale(panel: PreviewPanel, widths: PreviewWidths, content_width: f32, requested: f32) -> Option<f32> {
    if widths.constrained {
        return None;
    }
    let baseline = panel.width(baseline_widths(content_width));
    let minimum = baseline * panel.scale_limits().1;
    let maximum = (widths.gap.mul_add(-2.0, content_width) - WAVEFORM_MINIMUM_WIDTH - panel.sibling_width(widths))
        .min(baseline * MAXIMUM_PANEL_WIDTH_SCALE);
    (maximum >= minimum).then(|| {
        (requested.clamp(minimum, maximum) / baseline).clamp(panel.scale_limits().1, MAXIMUM_PANEL_WIDTH_SCALE)
    })
}

/// Keeps a hairline quiet at rest and makes pointer ownership or retained keyboard focus visible with theme colors.
fn width_divider_appearance(focus: FocusHandle) -> ResizeHandleRenderer {
    Rc::new(move |handle: &ResizeHandleContext, window: &mut Window, cx: &mut App| {
        let color = match handle.state() {
            ResizeHandleState::Pressed | ResizeHandleState::Dragging => cx.theme().ring,
            ResizeHandleState::Hovered => cx.theme().muted_foreground,
            ResizeHandleState::Idle if focus.is_focused(window) => cx.theme().ring,
            ResizeHandleState::Idle => cx.theme().border.opacity(0.35),
        };
        Some(div().flex_none().h_full().w(px(1.0)).bg(color).into_any_element())
    })
}

#[cfg(test)]
mod test_preview_widths {
    use super::*;

    /// Compares derived geometry without requiring binary32 arithmetic to be exact.
    fn close(actual: f32, expected: f32) {
        assert!((actual - expected).abs() < 0.001, "{actual} != {expected}");
    }

    #[test]
    fn defaults_scale_the_original_responsive_baseline_at_both_layouts() {
        for width in [800.0, 1200.0, 1579.0, 1580.0, 1800.0, 3000.0] {
            let baseline = baseline_widths(width);
            let resolved = resolve_widths(width, DEFAULT_CONTROLS_WIDTH_SCALE, DEFAULT_INFORMATION_WIDTH_SCALE);
            close(resolved.controls, baseline.controls * 0.95);
            close(resolved.information, baseline.information * 0.90);
            assert_eq!(resolved.wide, width >= 1580.0);
            close(
                resolved
                    .gap
                    .mul_add(2.0, resolved.controls + resolved.information + resolved.waveform),
                width,
            );
        }
        close(baseline_widths(1200.0).controls, 209.0);
        close(baseline_widths(1200.0).information, 264.0);
        close(baseline_widths(1580.0).controls, (790.0 - SECTION_SPACING) * 0.48);
    }

    #[test]
    fn each_divider_changes_only_its_own_column_and_waveform() {
        for width in [1000.0, 1580.0, 2400.0] {
            let original = resolve_widths(width, 0.95, 0.90);
            for panel in [PreviewPanel::Controls, PreviewPanel::Information] {
                let scale =
                    requested_scale(panel, original, width, panel.width(original) + 12.0).expect("room to resize");
                let changed = match panel {
                    PreviewPanel::Controls => resolve_widths(width, scale, 0.90),
                    PreviewPanel::Information => resolve_widths(width, 0.95, scale),
                };
                close(panel.width(changed), panel.width(original) + 12.0);
                close(panel.sibling_width(changed), panel.sibling_width(original));
                close(changed.waveform, original.waveform - 12.0);
            }
        }
    }

    #[test]
    fn explicit_edits_respect_baseline_scale_limits() {
        let original = resolve_widths(1800.0, 0.95, 0.90);
        for panel in [PreviewPanel::Controls, PreviewPanel::Information] {
            close(
                requested_scale(panel, original, 1800.0, -1000.0).expect("valid range"),
                panel.scale_limits().1,
            );
            close(
                requested_scale(panel, original, 1800.0, 10000.0).expect("valid range"),
                1.10,
            );
        }
    }

    #[test]
    fn tiny_windows_keep_nonnegative_geometry_and_recover_preferences() {
        let restored = resolve_widths(1800.0, 1.10, 1.08);
        for width in [0.0, 60.0, 140.0, 150.0, 300.0, 500.0, 700.0] {
            let cramped = resolve_widths(width, 1.10, 1.08);
            assert!(cramped.controls >= 0.0 && cramped.information >= 0.0 && cramped.waveform >= 0.0);
            assert!(cramped.waveform + 0.001 >= width.min(WAVEFORM_MINIMUM_WIDTH));
            close(
                cramped
                    .gap
                    .mul_add(2.0, cramped.controls + cramped.information + cramped.waveform),
                width,
            );
            if width <= 500.0 {
                assert!(requested_scale(PreviewPanel::Controls, cramped, width, 10.0).is_none());
            }
        }
        let recovered = resolve_widths(1800.0, 1.10, 1.08);
        close(recovered.controls, restored.controls);
        close(recovered.information, restored.information);
    }

    #[test]
    fn waveform_minimum_clamps_only_the_requested_column() {
        let original = resolve_widths(610.0, 0.95, 0.90);
        let scale = requested_scale(PreviewPanel::Information, original, 610.0, 10000.0).expect("valid range");
        let changed = resolve_widths(610.0, 0.95, scale);
        close(changed.controls, original.controls);
        close(changed.waveform, WAVEFORM_MINIMUM_WIDTH);
    }
}
