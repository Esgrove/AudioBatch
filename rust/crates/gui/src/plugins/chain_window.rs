//! The Plugin Chain window: the saved presets and the slots of the live chain in processing order.
//!
//! The presets panel at the top loads a saved preset as soon as it is picked, deletes one,
//! and saves the chain under a name.
//! Deleting a preset and replacing another one with the same name ask for confirmation first.
//! The panel says when the chain was modified since its preset was loaded or saved,
//! and saving under that preset's name updates it without asking.
//! Each row has an enable checkbox, the slot label with its position above its latency,
//! and buttons that open the editor, move the slot with two arrows, and remove it.
//! A slot whose plugin could not be loaded shows its error instead of the latency.
//! Below the rows the Add Plugin menu adds a known effect at the end, from one submenu per vendor,
//! and the buttons that rescan the plugins and clear the chain sit together at the right.
//! The footer shows the total latency and the rate the live instances run at.
//! The main view owns the chain: this view sends every change as a `ChainEvent`,
//! and the main view applies it and shows the window a new snapshot.

use std::cell::Cell;
use std::rc::Rc;

use audiobatch_plugins::{PluginDescription, sanitize_preset_name};
use gpui_kit::base::ElementExt as _;
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::menu::{DropdownMenu as _, PopupMenu, PopupMenuItem};
use gpui_kit::component::select::{Select, SelectEvent, SelectState};
use gpui_kit::component::{Disableable as _, IconName, Sizable as _};
use gpui_kit::{
    AppContext as _, Context, Div, Entity, EventEmitter, FocusHandle, Focusable, InteractiveElement as _, IntoElement,
    ParentElement as _, Pixels, PromptLevel, Render, SharedString, StatefulInteractiveElement as _, Styled as _,
    Subscription, WeakEntity, Window, div, px,
};

use super::labels::VendorGroup;
use super::live_chain::SlotId;
use super::{ROW_SPACING, SMALL_FONT_SIZE, WINDOW_FONT_SIZE, WINDOW_PADDING, hint, panel};
use crate::info_panel::CORNER_RADIUS;
use crate::menus::{ClosePluginWindow, PLUGIN_WINDOW_KEY_CONTEXT, RescanPlugins};
use crate::theme::{MONO_FONT, TEXT_FONT, color, palette, tooltip};

/// Title of the window.
pub const CHAIN_TITLE: &str = "Plugin Chain";

/// Initial width of the window.
pub const CHAIN_WIDTH: f32 = 720.0;

/// Initial height of the window.
pub const CHAIN_HEIGHT: f32 = 540.0;

/// Height of the buttons of a slot row, and the width of its square arrow buttons.
const ROW_BUTTON_HEIGHT: f32 = 32.0;

/// Padding inside a slot row.
const ROW_PADDING: f32 = 8.0;

/// Space between the two arrow buttons of a slot row.
const ARROW_SPACING: f32 = 4.0;

/// Font size of the latency line under the slot label.
const LATENCY_FONT_SIZE: f32 = 11.0;

/// Width of the Add Plugin button, wide enough to read as the main action of the row.
const ADD_PLUGIN_WIDTH: f32 = 200.0;

/// Height of one popup menu row, the default item height of the gpui-kit popup menu.
const MENU_ITEM_HEIGHT: f32 = 26.0;

/// Padding around the rows of a popup menu in rems, on each side.
const MENU_PADDING_REMS: f32 = 0.25;

/// Gap between the rows of a popup menu in rems.
const MENU_ROW_GAP_REMS: f32 = 0.125;

/// Gap between the Add Plugin button and its menu in rems, the default offset of the gpui-kit popover.
const MENU_OFFSET_REMS: f32 = 0.25;

/// Space the plugin menus keep from the window edges, the margin gpui-kit snaps submenus to.
const MENU_WINDOW_MARGIN: f32 = 4.0;

/// Tooltip of the Add Plugin button.
const ADD_PLUGIN_TOOLTIP: &str = "Add a known effect at the end of the chain.";

/// Tooltip of the enable checkbox.
const ENABLE_TOOLTIP: &str = "Bypass the plugin without losing its settings.";

/// Tooltip of the Edit button.
const EDIT_TOOLTIP: &str = "Open the plugin's editor window.";

/// Tooltip of the up arrow button.
const MOVE_UP_TOOLTIP: &str = "Move earlier";

/// Tooltip of the down arrow button.
const MOVE_DOWN_TOOLTIP: &str = "Move later";

/// Tooltip of the Remove button.
const REMOVE_TOOLTIP: &str = "Remove the plugin from the chain.";

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
    /// The configured VST plugin folders were asked to be rescanned.
    RescanPlugins,
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
    /// Name of the preset the chain was loaded from or saved as.
    pub current_preset: Option<String>,
    /// True when the chain was modified since it was loaded from or saved as `current_preset`.
    pub modified: bool,
    /// True while plugin discovery is running, disabling duplicate scans.
    pub scanning: bool,
}

/// The content of the Plugin Chain window.
pub struct ChainView {
    focus_handle: FocusHandle,
    snapshot: ChainSnapshot,
    preset_select: Entity<SelectState<Vec<SharedString>>>,
    preset_name: Entity<InputState>,
    /// Bottom edge of the Add Plugin button from its last prepaint,
    /// so its menu can grow down to the bottom of the window.
    add_plugin_bottom: Rc<Cell<Option<Pixels>>>,
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
            add_plugin_bottom: Rc::new(Cell::new(None)),
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

    /// Returns true when the typed name is the preset the chain was loaded from or saved as,
    /// so saving updates that preset.
    fn updates_current_preset(&self, name: &str) -> bool {
        self.snapshot
            .current_preset
            .as_ref()
            .is_some_and(|current| current.to_lowercase() == name.to_lowercase())
    }

    /// Saves the chain under the typed name, asking first when another preset of that name exists.
    /// Updating the preset the chain was loaded from or saved as does not ask.
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
        if !exists || self.updates_current_preset(&name) {
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
    /// and the name field with Save, which reads Update for the name of the loaded preset.
    /// The title names the loaded preset, and says when the chain was modified since.
    fn render_presets(&self, cx: &Context<Self>) -> Div {
        let selected = self.selected_preset(cx);
        let typed_name = sanitize_preset_name(&self.preset_name.read(cx).value());
        let has_name = !typed_name.is_empty();
        let title = match (&self.snapshot.current_preset, self.snapshot.modified) {
            (None, _) => String::from("Presets"),
            (Some(current), false) => format!("Presets (loaded: {current})"),
            (Some(current), true) => format!("Presets (loaded: {current}, modified)"),
        };
        let save_label = if has_name && self.updates_current_preset(&typed_name) {
            "Update"
        } else {
            "Save"
        };
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
                    .label(save_label)
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
    /// The vendor menu may grow down to the bottom of the window and each submenu to the full window height,
    /// and a menu scrolls only when its rows do not fit.
    /// The label sits at the left of the button, with a spacer after it.
    fn render_add_plugin(&self, cx: &Context<Self>) -> impl IntoElement {
        let groups = self.snapshot.plugin_groups.clone();
        let view = cx.entity().downgrade();
        let button_bottom = Rc::clone(&self.add_plugin_bottom);
        let measured_bottom = Rc::clone(&self.add_plugin_bottom);
        let button = Button::new("chain-add-plugin")
            .label("Add Plugin")
            .icon(IconName::ChevronDown)
            .child(div().flex_1())
            .small()
            .w(px(ADD_PLUGIN_WIDTH))
            .tooltip(ADD_PLUGIN_TOOLTIP)
            .disabled(groups.is_empty())
            .dropdown_menu(move |menu, window, cx| {
                let window_height = window.viewport_size().height;
                let menu_top = button_bottom.get().map_or(px(MENU_WINDOW_MARGIN), |bottom| {
                    bottom + window.rem_size() * MENU_OFFSET_REMS
                });
                let available = window_height - menu_top - px(MENU_WINDOW_MARGIN);
                let menu = fit_menu(menu, groups.len(), available, window);
                groups.iter().fold(menu, |menu, group| {
                    let plugins = group.plugins.clone();
                    let view = view.clone();
                    menu.submenu(group.vendor.clone(), window, cx, move |submenu, window, _| {
                        let available = window.viewport_size().height - px(MENU_WINDOW_MARGIN) * 2.0;
                        let submenu = fit_menu(submenu, plugins.len(), available, window);
                        vendor_submenu(submenu, &plugins, &view)
                    })
                })
            });
        div()
            .child(button)
            .on_prepaint(move |bounds, _, _| measured_bottom.set(Some(bounds.bottom())))
    }

    /// Renders one slot row on a rounded background,
    /// with the slot label above its latency or error, so the text leaves room for the buttons.
    fn render_row(row: &ChainRow, index: usize, count: usize, cx: &Context<Self>) -> Div {
        let id = row.id;
        let enable = Checkbox::new(("chain-enabled", index))
            .checked(row.enabled)
            .tooltip(ENABLE_TOOLTIP)
            .on_change(cx.listener(move |_, checked: &bool, _, cx| cx.emit(ChainEvent::SetEnabled(id, *checked))));
        let label = div()
            .id(("chain-label", index))
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
            .truncate()
            .font_family(MONO_FONT)
            .text_size(px(LATENCY_FONT_SIZE))
            .text_color(color(if row.failed {
                palette::RED
            } else {
                palette::GREY_MIDDLE_LIGHT
            }))
            .tooltip(tooltip(row.detail.clone()))
            .child(row.detail.clone());
        let arrows = div()
            .flex()
            .flex_row()
            .flex_none()
            .gap(px(ARROW_SPACING))
            .child(
                Button::new(("chain-up", index))
                    .icon(IconName::ArrowUp)
                    .w(px(ROW_BUTTON_HEIGHT))
                    .h(px(ROW_BUTTON_HEIGHT))
                    .tooltip(MOVE_UP_TOOLTIP)
                    .disabled(index == 0)
                    .on_click(cx.listener(move |_, _, _, cx| cx.emit(ChainEvent::Move(id, -1)))),
            )
            .child(
                Button::new(("chain-down", index))
                    .icon(IconName::ArrowDown)
                    .w(px(ROW_BUTTON_HEIGHT))
                    .h(px(ROW_BUTTON_HEIGHT))
                    .tooltip(MOVE_DOWN_TOOLTIP)
                    .disabled(index + 1 >= count)
                    .on_click(cx.listener(move |_, _, _, cx| cx.emit(ChainEvent::Move(id, 1)))),
            );
        div()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(ROW_SPACING))
            .p(px(ROW_PADDING))
            .rounded(px(CORNER_RADIUS))
            .bg(color(palette::GREY_MEDIUM_DARK))
            .child(enable)
            .child(
                div()
                    .flex()
                    .flex_col()
                    .flex_1()
                    .min_w_0()
                    .gap(px(2.0))
                    .child(label)
                    .child(detail),
            )
            .child(
                Button::new(("chain-edit", index))
                    .label("Edit")
                    .h(px(ROW_BUTTON_HEIGHT))
                    .tooltip(EDIT_TOOLTIP)
                    .disabled(!row.can_edit)
                    .on_click(cx.listener(move |_, _, _, cx| cx.emit(ChainEvent::Edit(id)))),
            )
            .child(arrows)
            .child(
                Button::new(("chain-remove", index))
                    .label("Remove")
                    .h(px(ROW_BUTTON_HEIGHT))
                    .tooltip(REMOVE_TOOLTIP)
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
        let mut slots = panel(format!("Plugins in processing order ({count})"));
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
            .child(self.render_add_plugin(cx))
            .child(div().flex_1())
            .child(
                Button::new("chain-rescan-plugins")
                    .label(if self.snapshot.scanning {
                        "Scanning…"
                    } else {
                        "Rescan VST plugins"
                    })
                    .small()
                    .disabled(self.snapshot.scanning)
                    .on_click(cx.listener(|_, _, _, cx| cx.emit(ChainEvent::RescanPlugins))),
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
            .plugin_groups
            .is_empty()
            .then(|| hint("No effects are known yet. Rescan VST plugins to find effects."));
        let footer = div()
            .flex()
            .flex_col()
            .gap(px(2.0))
            .text_size(px(SMALL_FONT_SIZE))
            .text_color(color(palette::GREY_MIDDLE_LIGHT))
            .child(
                div()
                    .text_size(px(WINDOW_FONT_SIZE))
                    .text_color(color(palette::GREY_SUPER_LIGHT))
                    .child(format!("Total latency: {}", self.snapshot.total_latency)),
            )
            .child(format!("Live instances run at {}", self.snapshot.sample_rate));
        div()
            .id(crate::control_ids::PLUGIN_CHAIN_WINDOW)
            .key_context(PLUGIN_WINDOW_KEY_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(|_, _: &ClosePluginWindow, window, _| window.remove_window()))
            .on_action(cx.listener(|view, _: &RescanPlugins, _, cx| {
                if !view.snapshot.scanning {
                    cx.emit(ChainEvent::RescanPlugins);
                }
            }))
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
    plugins.iter().fold(submenu, |submenu, plugin| {
        let plugin = plugin.clone();
        let view = view.clone();
        submenu.item(PopupMenuItem::new(plugin.name.clone()).on_click(move |_, _, cx| {
            let _ = view.update(cx, |_, cx| cx.emit(ChainEvent::Add(plugin.clone())));
        }))
    })
}

/// Limits a popup menu of `rows` items to the `available` height,
/// and makes it scrollable only when its rows are taller than that.
/// A scrollable menu always shows a scrollbar and allows some scrolling,
/// so short menus that fit must not be scrollable at all.
fn fit_menu(menu: PopupMenu, rows: usize, available: Pixels, window: &Window) -> PopupMenu {
    let available = available.max(px(MENU_ITEM_HEIGHT));
    let menu = menu.max_h(available);
    if menu_overflows(rows, available, window.rem_size()) {
        menu.scrollable(true)
    } else {
        menu
    }
}

/// Returns true when a popup menu of `rows` items is taller than the `available` height.
fn menu_overflows(rows: usize, available: Pixels, rem_size: Pixels) -> bool {
    menu_content_height(rows, rem_size) > available
}

/// Returns the height the rows of a popup menu take with its padding and the gaps between them.
fn menu_content_height(rows: usize, rem_size: Pixels) -> Pixels {
    let count = f32::from(u16::try_from(rows).unwrap_or(u16::MAX));
    let gaps = (count - 1.0).max(0.0);
    px(MENU_ITEM_HEIGHT) * count + rem_size * MENU_PADDING_REMS.mul_add(2.0, MENU_ROW_GAP_REMS * gaps)
}

#[cfg(test)]
mod integration_tests {
    use std::cell::RefCell;
    use std::rc::Rc;

    use gpui_kit::test::TestWindowExt as _;
    use gpui_kit::{TestAppContext, WindowOptions};

    use super::*;

    /// Chain view and its retained event subscription, kept alive for the driven workflow.
    struct OpenedChain {
        handle: gpui_kit::AnyWindowHandle,
        view: Entity<ChainView>,
        _subscription: gpui_kit::Subscription,
    }

    #[gpui_kit::test]
    fn rescan_button_emits_request_and_is_disabled_while_scanning(cx: &mut TestAppContext) {
        let events = Rc::new(RefCell::new(Vec::new()));
        let OpenedChain {
            handle,
            view,
            _subscription,
        } = cx.update(|cx| {
            gpui_kit::init(cx);
            crate::theme::install(cx);
            let (handle, view) = gpui_kit::open_window(WindowOptions::default(), cx, |window, cx| {
                cx.new(|cx| ChainView::new(ChainSnapshot::default(), window, cx))
            })
            .expect("opens the chain window");
            let events = Rc::clone(&events);
            let subscription = cx.subscribe(&view, move |_, event: &ChainEvent, _| {
                events.borrow_mut().push(event.clone());
            });
            OpenedChain {
                handle,
                view,
                _subscription: subscription,
            }
        });
        cx.update_window(handle, |_, window, cx| {
            window.render_frame(cx);
            window.click("chain-rescan-plugins", cx);
        })
        .expect("requests a scan");
        cx.run_until_parked();
        assert_eq!(*events.borrow(), vec![ChainEvent::RescanPlugins]);
        cx.update_window(handle, |_, window, cx| {
            view.update(cx, |view, cx| {
                view.sync(
                    ChainSnapshot {
                        scanning: true,
                        ..ChainSnapshot::default()
                    },
                    window,
                    cx,
                );
            });
            window.render_frame(cx);
            assert_eq!(window.find("chain-rescan-plugins").disabled(), Some(true));
            window.click("chain-rescan-plugins", cx);
        })
        .expect("disables duplicate scan requests");
        cx.run_until_parked();
        assert_eq!(events.borrow().len(), 1);
    }
}

#[cfg(test)]
mod test_menu_fit {
    use super::*;

    /// Root font size the tests measure menus with.
    const REM: f32 = 16.0;

    #[test]
    fn content_height_counts_rows_gaps_and_padding() {
        let one_row = menu_content_height(1, px(REM));
        let five_rows = menu_content_height(5, px(REM));
        assert!((f32::from(one_row) - 34.0).abs() < 1e-3);
        assert!((f32::from(five_rows) - 146.0).abs() < 1e-3);
    }

    #[test]
    fn short_listing_in_the_chain_window_does_not_scroll() {
        let available = px(CHAIN_HEIGHT) - px(MENU_WINDOW_MARGIN) * 2.0;
        for rows in 1..=5 {
            assert!(!menu_overflows(rows, available, px(REM)), "{rows} rows");
        }
    }

    #[test]
    fn listing_taller_than_the_window_scrolls() {
        let available = px(CHAIN_HEIGHT) - px(MENU_WINDOW_MARGIN) * 2.0;
        assert!(menu_overflows(40, available, px(REM)));
    }

    #[test]
    fn exact_fit_does_not_scroll() {
        let available = menu_content_height(12, px(REM));
        assert!(!menu_overflows(12, available, px(REM)));
        assert!(menu_overflows(13, available, px(REM)));
    }
}
