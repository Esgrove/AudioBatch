//! The Plugin Chain window: the slots of the live chain in processing order.
//!
//! Each row has an enable checkbox, the slot label with its position, the latency,
//! and buttons that open the editor, move the slot, and remove it.
//! A slot whose plugin could not be loaded shows its error instead of the latency.
//! Below the rows a choice adds a known effect at the end, and the footer shows the total latency
//! and the rate the live instances run at.
//! The main view owns the chain: this view sends every change as a `ChainEvent`,
//! and the main view applies it and shows the window a new snapshot.

use audiobatch_plugins::PluginDescription;
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use gpui_kit::component::{Disableable as _, IndexPath, Sizable as _};
use gpui_kit::{
    AppContext as _, Context, Div, Entity, EventEmitter, FocusHandle, Focusable, InteractiveElement as _, IntoElement,
    ParentElement as _, Render, SharedString, StatefulInteractiveElement as _, Styled as _, Subscription, Window, div,
    px,
};

use super::live_chain::SlotId;
use super::{ROW_SPACING, SMALL_FONT_SIZE, WINDOW_FONT_SIZE, WINDOW_PADDING, hint, panel};
use crate::menus::{ClosePluginWindow, PLUGIN_WINDOW_KEY_CONTEXT};
use crate::theme::{MONO_FONT, TEXT_FONT, color, palette, tooltip};

/// Title of the window.
pub const CHAIN_TITLE: &str = "Plugin Chain";

/// Initial width of the window.
pub const CHAIN_WIDTH: f32 = 640.0;

/// Initial height of the window.
pub const CHAIN_HEIGHT: f32 = 420.0;

/// Width of the latency column.
const LATENCY_WIDTH: f32 = 150.0;

/// Tooltip of the enable checkbox.
const ENABLE_TOOLTIP: &str = "Bypass the plugin without losing its settings.";

/// Tooltip of the Edit button.
const EDIT_TOOLTIP: &str = "Open the plugin's editor window.";

/// Something the user did in the chain window.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ChainEvent {
    /// A slot was enabled or bypassed.
    SetEnabled(SlotId, bool),
    /// A slot's editor was asked for.
    Edit(SlotId),
    /// A slot was moved by an offset, negative towards the start.
    Move(SlotId, isize),
    /// A slot was removed.
    Remove(SlotId),
    /// A plugin was added at the end.
    Add(PluginDescription),
    /// Every slot was removed.
    Clear,
}

/// One slot as the window shows it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChainRow {
    /// The slot.
    pub id: SlotId,
    /// Label with the position, for example "1. Vendor Delay (VST3)".
    pub label: String,
    /// True when the slot processes audio.
    pub enabled: bool,
    /// Latency text, or the error of a plugin that could not be loaded or prepared.
    pub detail: String,
    /// True when `detail` is an error.
    pub failed: bool,
    /// True when the plugin has an editor to open.
    pub can_edit: bool,
}

/// What the window shows, collected by the main view.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ChainSnapshot {
    /// The slots in processing order.
    pub rows: Vec<ChainRow>,
    /// Summed latency of the enabled slots.
    pub total_latency: String,
    /// Rate the live instances run at.
    pub sample_rate: String,
    /// The effects that can be added, with their labels.
    pub available: Vec<(String, PluginDescription)>,
}

/// The content of the Plugin Chain window.
pub struct ChainView {
    focus_handle: FocusHandle,
    snapshot: ChainSnapshot,
    add_select: Entity<SelectState<Vec<SharedString>>>,
    _subscriptions: Vec<Subscription>,
}

impl ChainView {
    /// Creates the window content showing a snapshot.
    pub fn new(snapshot: ChainSnapshot, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let add_select = cx.new(|cx| SelectState::new(Vec::new(), None, window, cx));
        let subscriptions = vec![cx.subscribe_in(&add_select, window, Self::on_add_selected)];
        let focus_handle = cx.focus_handle();
        window.focus(&focus_handle, cx);
        let mut view = Self {
            focus_handle,
            snapshot: ChainSnapshot::default(),
            add_select,
            _subscriptions: subscriptions,
        };
        view.sync(snapshot, window, cx);
        view
    }

    /// Shows a new snapshot.
    pub fn sync(&mut self, snapshot: ChainSnapshot, window: &mut Window, cx: &mut Context<Self>) {
        if snapshot.available != self.snapshot.available {
            let labels = snapshot
                .available
                .iter()
                .map(|(label, _)| SharedString::from(label.clone()))
                .collect();
            self.add_select.update(cx, |state, cx| {
                state.set_items(labels, window, cx);
                state.set_selected_index(None, window, cx);
            });
        }
        self.snapshot = snapshot;
        cx.notify();
    }

    /// Adds the chosen effect and clears the choice, so the same effect can be added again.
    fn on_add_selected(
        &mut self,
        select: &Entity<SelectState<Vec<SharedString>>>,
        event: &SelectEvent<Vec<SharedString>>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let SelectEvent::Confirm(Some(label)) = event else {
            return;
        };
        let plugin = self
            .snapshot
            .available
            .iter()
            .find(|(candidate, _)| candidate.as_str() == label.as_ref())
            .map(|(_, plugin)| plugin.clone());
        select.update(cx, |state, cx| state.set_selected_index(None::<IndexPath>, window, cx));
        if let Some(plugin) = plugin {
            cx.emit(ChainEvent::Add(plugin));
        }
    }

    /// Renders one slot row.
    fn render_row(row: &ChainRow, index: usize, count: usize, cx: &Context<Self>) -> Div {
        let id = row.id;
        let enable = Checkbox::new(("chain-enabled", index))
            .checked(row.enabled)
            .tooltip(ENABLE_TOOLTIP)
            .on_change(cx.listener(move |_, checked: &bool, _, cx| cx.emit(ChainEvent::SetEnabled(id, *checked))));
        let label = div()
            .id(("chain-label", index))
            .flex_1()
            .min_w_0()
            .truncate()
            .text_color(color(if row.enabled {
                palette::GREY_LIGHT
            } else {
                palette::GREY_MIDDLE
            }))
            .tooltip(tooltip(row.label.clone()))
            .child(row.label.clone());
        let detail = div()
            .id(("chain-detail", index))
            .w(px(LATENCY_WIDTH))
            .flex_none()
            .truncate()
            .font_family(MONO_FONT)
            .text_size(px(SMALL_FONT_SIZE))
            .text_color(color(if row.failed {
                palette::RED
            } else {
                palette::GREY_MIDDLE_LIGHT
            }))
            .tooltip(tooltip(row.detail.clone()))
            .child(row.detail.clone());
        div()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(ROW_SPACING))
            .child(enable)
            .child(label)
            .child(detail)
            .child(
                Button::new(("chain-edit", index))
                    .label("Edit")
                    .small()
                    .tooltip(EDIT_TOOLTIP)
                    .disabled(!row.can_edit)
                    .on_click(cx.listener(move |_, _, _, cx| cx.emit(ChainEvent::Edit(id)))),
            )
            .child(
                Button::new(("chain-up", index))
                    .label("Up")
                    .small()
                    .disabled(index == 0)
                    .on_click(cx.listener(move |_, _, _, cx| cx.emit(ChainEvent::Move(id, -1)))),
            )
            .child(
                Button::new(("chain-down", index))
                    .label("Down")
                    .small()
                    .disabled(index + 1 >= count)
                    .on_click(cx.listener(move |_, _, _, cx| cx.emit(ChainEvent::Move(id, 1)))),
            )
            .child(
                Button::new(("chain-remove", index))
                    .label("Remove")
                    .small()
                    .danger()
                    .on_click(cx.listener(move |_, _, _, cx| cx.emit(ChainEvent::Remove(id)))),
            )
    }
}

impl EventEmitter<ChainEvent> for ChainView {}

impl Focusable for ChainView {
    fn focus_handle(&self, _cx: &gpui_kit::App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for ChainView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let count = self.snapshot.rows.len();
        let mut slots = panel(format!("Slots in processing order ({count})"));
        if count == 0 {
            slots = slots.child(hint(
                "The chain is empty. Add an effect below or from the Plugins menu.",
            ));
        }
        for (index, row) in self.snapshot.rows.iter().enumerate() {
            slots = slots.child(Self::render_row(row, index, count, cx));
        }
        let add_row = div()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(ROW_SPACING))
            .child(div().flex_none().child("Add effect"))
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .child(Select::new(&self.add_select).small().placeholder("Choose a plugin…")),
            )
            .child(
                Button::new("chain-clear")
                    .label("Clear Chain")
                    .small()
                    .disabled(count == 0)
                    .on_click(cx.listener(|_, _, _, cx| cx.emit(ChainEvent::Clear))),
            );
        let available_hint = self
            .snapshot
            .available
            .is_empty()
            .then(|| hint("No effects are known yet. Scan for plugins in the Plugin Manager."));
        let footer = div()
            .flex()
            .flex_col()
            .gap(px(2.0))
            .text_size(px(SMALL_FONT_SIZE))
            .text_color(color(palette::GREY_MIDDLE_LIGHT))
            .child(format!("Total latency: {}", self.snapshot.total_latency))
            .child(format!("Live instances run at {}", self.snapshot.sample_rate));
        div()
            .id("plugin-chain")
            .key_context(PLUGIN_WINDOW_KEY_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(|_, _: &ClosePluginWindow, window, _| window.remove_window()))
            .size_full()
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap(px(WINDOW_PADDING))
            .p(px(WINDOW_PADDING + 4.0))
            .bg(color(palette::GREY_DARK))
            .text_color(color(palette::GREY_LIGHT))
            .font_family(TEXT_FONT)
            .text_size(px(WINDOW_FONT_SIZE))
            .child(slots)
            .child(add_row)
            .children(available_hint)
            .child(footer)
    }
}
