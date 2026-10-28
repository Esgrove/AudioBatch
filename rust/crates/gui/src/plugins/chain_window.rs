//! The Plugin Chain window: the saved presets and the slots of the live chain in processing order.
//!
//! The presets panel at the top loads a saved preset as soon as it is picked, deletes one,
//! and saves the chain under a name.
//! Deleting a preset and replacing one with the same name ask for confirmation first.
//! Each row has an enable checkbox, the slot label with its position, the latency,
//! and buttons that open the editor, move the slot, and remove it.
//! A slot whose plugin could not be loaded shows its error instead of the latency.
//! Below the rows the Add Plugin menu adds a known effect at the end, from one submenu per vendor,
//! and the footer shows the total latency and the rate the live instances run at.
//! The main view owns the chain: this view sends every change as a `ChainEvent`,
//! and the main view applies it and shows the window a new snapshot.

use audiobatch_plugins::{PluginDescription, sanitize_preset_name};
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::menu::{DropdownMenu as _, PopupMenu, PopupMenuItem};
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use gpui_kit::component::{Disableable as _, IconName, Sizable as _};
use gpui_kit::{
    AppContext as _, Context, Div, Entity, EventEmitter, FocusHandle, Focusable, InteractiveElement as _, IntoElement,
    ParentElement as _, PromptLevel, Render, SharedString, StatefulInteractiveElement as _, Styled as _, Subscription,
    WeakEntity, Window, div, px,
};

use super::labels::VendorGroup;
use super::live_chain::SlotId;
use super::{ROW_SPACING, SMALL_FONT_SIZE, WINDOW_FONT_SIZE, WINDOW_PADDING, hint, panel};
use crate::menus::{ClosePluginWindow, PLUGIN_WINDOW_KEY_CONTEXT};
use crate::theme::{MONO_FONT, TEXT_FONT, color, palette, tooltip};

/// Title of the window.
pub const CHAIN_TITLE: &str = "Plugin Chain";

/// Initial width of the window.
pub const CHAIN_WIDTH: f32 = 720.0;

/// Initial height of the window.
pub const CHAIN_HEIGHT: f32 = 540.0;

/// Width of the latency column, enough for "Latency: 1024 samples (21.33 ms)".
const LATENCY_WIDTH: f32 = 230.0;

/// Tooltip of the Add Plugin button.
const ADD_PLUGIN_TOOLTIP: &str = "Add a known effect at the end of the chain.";

/// Tooltip of the enable checkbox.
const ENABLE_TOOLTIP: &str = "Bypass the plugin without losing its settings.";

/// Tooltip of the Edit button.
const EDIT_TOOLTIP: &str = "Open the plugin's editor window.";

/// Tooltip of the preset Save button.
const SAVE_PRESET_TOOLTIP: &str = "Save the plugins, their order, and their settings under this name.";

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
    /// The chain was saved as the named preset, replacing a preset of the same name.
    SavePreset(String),
    /// The named preset was loaded in place of the chain.
    LoadPreset(String),
    /// The named preset was deleted.
    DeletePreset(String),
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
    /// The effects that can be added, grouped by vendor.
    pub plugin_groups: Vec<VendorGroup>,
    /// Names of the saved presets, in the order they are listed.
    pub presets: Vec<String>,
    /// Name of the preset the chain matches, when it was loaded from or saved as one and has not changed since.
    pub current_preset: Option<String>,
}

/// The content of the Plugin Chain window.
pub struct ChainView {
    focus_handle: FocusHandle,
    snapshot: ChainSnapshot,
    preset_select: Entity<SelectState<Vec<SharedString>>>,
    preset_name: Entity<InputState>,
    _subscriptions: Vec<Subscription>,
}

impl ChainView {
    /// Creates the window content showing a snapshot.
    pub fn new(snapshot: ChainSnapshot, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let preset_select = cx.new(|cx| SelectState::new(Vec::new(), None, window, cx));
        let preset_name = cx.new(|cx| InputState::new(window, cx).placeholder("Preset name"));
        let subscriptions = vec![
            cx.subscribe_in(&preset_select, window, Self::on_preset_selected),
            cx.subscribe_in(&preset_name, window, Self::on_preset_name_input),
        ];
        let focus_handle = cx.focus_handle();
        window.focus(&focus_handle, cx);
        let mut view = Self {
            focus_handle,
            snapshot: ChainSnapshot::default(),
            preset_select,
            preset_name,
            _subscriptions: subscriptions,
        };
        view.sync(snapshot, window, cx);
        view
    }

    /// Shows a new snapshot.
    pub fn sync(&mut self, snapshot: ChainSnapshot, window: &mut Window, cx: &mut Context<Self>) {
        if snapshot.presets != self.snapshot.presets {
            let names = snapshot.presets.iter().cloned().map(SharedString::from).collect();
            self.preset_select
                .update(cx, |state, cx| state.set_items(names, window, cx));
        }
        if snapshot.current_preset != self.snapshot.current_preset
            && let Some(current) = &snapshot.current_preset
        {
            let current = SharedString::from(current.clone());
            self.preset_select
                .update(cx, |state, cx| state.set_selected_value(&current, window, cx));
            self.preset_name
                .update(cx, |state, cx| state.set_value(current, window, cx));
        }
        self.snapshot = snapshot;
        cx.notify();
    }

    /// Returns the preset chosen in the preset select.
    fn selected_preset(&self, cx: &Context<Self>) -> Option<String> {
        self.preset_select.read(cx).selected_value().map(ToString::to_string)
    }

    /// Loads the picked preset in place of the chain,
    /// and puts its name in the name field, so saving again replaces it.
    fn on_preset_selected(
        &mut self,
        _select: &Entity<SelectState<Vec<SharedString>>>,
        event: &SelectEvent<Vec<SharedString>>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if let SelectEvent::Confirm(Some(name)) = event {
            let name = name.clone();
            self.preset_name
                .update(cx, |state, cx| state.set_value(name.clone(), window, cx));
            cx.emit(ChainEvent::LoadPreset(name.to_string()));
        }
        cx.notify();
    }

    /// Saves the preset when Enter is pressed in the name field, and refreshes the Save button while typing.
    fn on_preset_name_input(
        &mut self,
        _input: &Entity<InputState>,
        event: &InputEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match event {
            InputEvent::PressEnter { .. } => self.save_preset(window, cx),
            InputEvent::Change => cx.notify(),
            InputEvent::Focus | InputEvent::Blur => {}
        }
    }

    /// Saves the chain under the typed name, asking first when a preset of that name exists.
    fn save_preset(&self, window: &mut Window, cx: &mut Context<Self>) {
        let name = sanitize_preset_name(&self.preset_name.read(cx).value());
        if name.is_empty() || self.snapshot.rows.is_empty() {
            return;
        }
        let exists = self
            .snapshot
            .presets
            .iter()
            .any(|preset| preset.to_lowercase() == name.to_lowercase());
        if !exists {
            cx.emit(ChainEvent::SavePreset(name));
            return;
        }
        let detail = format!("A preset named \"{name}\" already exists. Replace it with the current chain?");
        let answer = window.prompt(
            PromptLevel::Warning,
            "Replace Preset",
            Some(&detail),
            &["Replace", "Cancel"],
            cx,
        );
        cx.spawn_in(window, async move |this, cx| {
            if answer.await == Ok(0) {
                let _ = this.update(cx, |_, cx| cx.emit(ChainEvent::SavePreset(name)));
            }
        })
        .detach();
    }

    /// Deletes the chosen preset after a confirmation.
    fn delete_preset(&self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(name) = self.selected_preset(cx) else {
            return;
        };
        let detail = format!("Delete the preset \"{name}\"? The current chain is not changed.");
        let answer = window.prompt(
            PromptLevel::Warning,
            "Delete Preset",
            Some(&detail),
            &["Delete", "Cancel"],
            cx,
        );
        cx.spawn_in(window, async move |this, cx| {
            if answer.await == Ok(0) {
                let _ = this.update(cx, |_, cx| cx.emit(ChainEvent::DeletePreset(name)));
            }
        })
        .detach();
    }

    /// Renders the presets panel: the preset choice, which loads the picked preset, with Delete,
    /// and the name field with Save.
    fn render_presets(&self, cx: &Context<Self>) -> Div {
        let selected = self.selected_preset(cx);
        let has_name = !sanitize_preset_name(&self.preset_name.read(cx).value()).is_empty();
        let title = self.snapshot.current_preset.as_ref().map_or_else(
            || String::from("Presets"),
            |current| format!("Presets (loaded: {current})"),
        );
        let choose_row = div()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(ROW_SPACING))
            .child(
                div().flex_1().min_w_0().child(
                    Select::new(&self.preset_select)
                        .small()
                        .placeholder("Choose a preset…")
                        .disabled(self.snapshot.presets.is_empty()),
                ),
            )
            .child(
                Button::new("preset-delete")
                    .label("Delete")
                    .small()
                    .danger()
                    .disabled(selected.is_none())
                    .on_click(cx.listener(|view, _, window, cx| view.delete_preset(window, cx))),
            );
        let save_row = div()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(ROW_SPACING))
            .child(div().flex_none().child("Save as"))
            .child(div().flex_1().min_w_0().child(Input::new(&self.preset_name).small()))
            .child(
                Button::new("preset-save")
                    .label("Save")
                    .small()
                    .tooltip(SAVE_PRESET_TOOLTIP)
                    .disabled(!has_name || self.snapshot.rows.is_empty())
                    .on_click(cx.listener(|view, _, window, cx| view.save_preset(window, cx))),
            );
        let mut presets = panel(title).child(choose_row).child(save_row);
        if self.snapshot.presets.is_empty() {
            presets = presets.child(hint("No presets are saved yet. Name the chain above and save it."));
        }
        presets
    }

    /// Renders the Add Plugin button, whose menu has a submenu of effects for each vendor.
    /// Picking an effect adds it at the end of the chain.
    fn render_add_plugin(&self, cx: &Context<Self>) -> impl IntoElement {
        let groups = self.snapshot.plugin_groups.clone();
        let view = cx.entity().downgrade();
        Button::new("chain-add-plugin")
            .label("Add Plugin")
            .icon(IconName::ChevronDown)
            .small()
            .tooltip(ADD_PLUGIN_TOOLTIP)
            .disabled(groups.is_empty())
            .dropdown_menu(move |menu, window, cx| {
                groups.iter().fold(menu.scrollable(true), |menu, group| {
                    let plugins = group.plugins.clone();
                    let view = view.clone();
                    menu.submenu(group.vendor.clone(), window, cx, move |submenu, _, _| {
                        vendor_submenu(submenu, &plugins, &view)
                    })
                })
            })
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
            .justify_between()
            .gap(px(ROW_SPACING))
            .child(self.render_add_plugin(cx))
            .child(
                Button::new("chain-clear")
                    .label("Clear Chain")
                    .small()
                    .disabled(count == 0)
                    .on_click(cx.listener(|_, _, _, cx| cx.emit(ChainEvent::Clear))),
            );
        let available_hint = self
            .snapshot
            .plugin_groups
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
            .child(self.render_presets(cx))
            .child(slots)
            .child(add_row)
            .children(available_hint)
            .child(footer)
    }
}

/// Fills the submenu of one vendor with its effects, each adding itself to the chain of the view when picked.
fn vendor_submenu(submenu: PopupMenu, plugins: &[PluginDescription], view: &WeakEntity<ChainView>) -> PopupMenu {
    plugins.iter().fold(submenu.scrollable(true), |submenu, plugin| {
        let plugin = plugin.clone();
        let view = view.clone();
        submenu.item(PopupMenuItem::new(plugin.name.clone()).on_click(move |_, _, cx| {
            let _ = view.update(cx, |_, cx| cx.emit(ChainEvent::Add(plugin.clone())));
        }))
    })
}
