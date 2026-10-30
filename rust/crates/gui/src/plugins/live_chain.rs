//! The plugin chain the user edits, with a live instance for each slot.
//!
//! A slot gets an instance prepared for stereo realtime processing at the preview rate
//! when it is added or a preset replaces the chain,
//! so the chain window can show its latency and the editor and the preview can use it.
//! The slots of the chain restored at startup create their instances when first needed:
//! when the preview plays through the chain, an editor opens, or the chain window shows them.
//! Slots are referred to by a `SlotId` that is never reused,
//! so an id held by a window or an event cannot point at another slot after the chain changed.
//! A slot whose plugin cannot be loaded keeps its saved settings and shows its error,
//! so a plugin that is missing for now does not lose its settings.
//! The caller saves the chain after every structural change.
//! Loading a preset replaces every slot, and the chain remembers the preset's name until it is cleared.
//! Changing a slot or a plugin's settings marks the chain modified, until it is saved as a preset again.
//! The settings of the instances are captured when processing starts, an editor closes, and the app quits.
//! Each slot can show its plugin's editor in a native window.
//! The window only hides when the user closes it,
//! and the next housekeeping tick captures the settings so they can be saved, then destroys the window.

use std::path::PathBuf;

use anyhow::{Context as _, Result};
use audiobatch_plugins::{
    ChainSlot, PluginChain, PluginDescription, PreviewChain, chain_tail_frames, prepare_for_preview, reload_for_preview,
};
use raw_window_handle::RawWindowHandle;
use vst3_host::{EditorWindow, Module, PluginInstance, RestartAction, UiDispatcher, UiThread};

/// Most frames the preview passes to the live instances in one call.
pub const PREVIEW_BLOCK_FRAMES: usize = 1024;

/// Identifies a slot of the live chain for as long as the slot exists. Ids are never reused.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SlotId(u64);

/// Lifecycle action retained until the preview has retired the slot's processor handles.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SlotRestart {
    /// Stable slot identity, independent of chain order.
    id: SlotId,
    /// Full reload or lighter reconfiguration.
    action: RestartAction,
}

/// One slot of the live chain.
pub struct LiveSlot {
    /// The slot's id.
    id: SlotId,
    /// The saved slot: the plugin, whether it is enabled, and its last captured settings.
    slot: ChainSlot,
    /// The window showing the plugin's editor.
    /// It comes before the instance, so it is destroyed first when the slot is dropped.
    editor: Option<EditorWindow>,
    /// The live instance, missing when the plugin could not be loaded.
    instance: Option<PluginInstance>,
    /// Latency of the prepared instance, in samples.
    latency_samples: u32,
    /// Why the plugin could not be loaded or prepared.
    error: Option<String>,
    /// True once loading the plugin was tried.
    /// The chain restored at startup loads its plugins only when they are first needed, so startup does not wait.
    load_attempted: bool,
    /// True when the plugin refused its saved settings, for example before its license is ready.
    /// The saved settings are kept instead of the plugin's defaults until the user opens its editor.
    keep_saved_state: bool,
    /// Strongest requested lifecycle action, retained until processor retirement and successful application.
    pending_restart: Option<RestartAction>,
}

/// The plugin chain the user edits, saved to the chain file.
pub struct LiveChain {
    /// The slots, in processing order.
    slots: Vec<LiveSlot>,
    /// Id of the next slot.
    next_id: u64,
    /// The chain file.
    path: PathBuf,
    /// Runs plugin calls from other threads on the UI thread, given to every instance.
    dispatcher: UiDispatcher,
    /// Rate the instances are prepared at, the preview's output rate.
    sample_rate: u32,
    /// Name of the preset the chain was loaded from or saved as, until the chain is cleared or the preset deleted.
    preset_name: Option<String>,
    /// True when the chain differs from the preset it was loaded from or saved as,
    /// because a slot was added, removed, moved, enabled, or bypassed, or a plugin's settings changed.
    modified: bool,
    /// True when the chain file exists but could not be read, such as one saved by a newer version.
    /// Saving leaves it alone while the chain is empty, and moves it aside before writing a new chain over it.
    unread_file: bool,
}

impl LiveSlot {
    /// Returns the slot's id.
    #[must_use]
    pub const fn id(&self) -> SlotId {
        self.id
    }

    /// Returns the plugin.
    #[must_use]
    pub const fn plugin(&self) -> &PluginDescription {
        &self.slot.plugin
    }

    /// Returns true when the slot processes audio.
    #[must_use]
    pub const fn is_enabled(&self) -> bool {
        self.slot.enabled
    }

    /// Returns the latency of the prepared instance in samples, 0 without one.
    #[must_use]
    pub const fn latency_samples(&self) -> u32 {
        self.latency_samples
    }

    /// Returns why the plugin could not be loaded or prepared.
    #[must_use]
    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    /// Returns true when the instance is loaded and prepared.
    #[must_use]
    pub const fn is_ready(&self) -> bool {
        self.instance.is_some() && self.error.is_none()
    }

    /// Returns true when the instance is loaded, so its editor can be opened.
    #[must_use]
    pub const fn has_instance(&self) -> bool {
        self.instance.is_some()
    }

    /// Loads the plugin with the slot's saved settings and prepares it at a rate.
    fn load(&mut self, ui: &UiThread, dispatcher: &UiDispatcher, sample_rate: u32) {
        self.load_attempted = true;
        match load_instance(ui, dispatcher, &self.slot) {
            Ok((instance, accepted)) => {
                if !accepted {
                    tracing::warn!(
                        "Plugin \"{}\" did not accept its saved settings, which are kept until its editor opens",
                        self.slot.plugin.name
                    );
                }
                self.keep_saved_state = !accepted;
                self.instance = Some(instance);
                self.prepare(ui, sample_rate);
                // Restoring the saved settings is not a change the user made.
                self.take_settings_change();
            }
            Err(error) => self.error = Some(format!("{error:#}")),
        }
    }

    /// Prepares the instance for stereo realtime processing at a rate, and reads its latency.
    fn prepare(&mut self, ui: &UiThread, sample_rate: u32) {
        let Some(instance) = &mut self.instance else {
            return;
        };
        match prepare_instance(ui, instance, sample_rate) {
            Ok(()) => {
                self.latency_samples = instance.latency_samples(ui);
                self.error = None;
            }
            Err(error) => {
                self.latency_samples = 0;
                self.error = Some(format!("Could not be prepared: {error:#}"));
            }
        }
    }

    /// Returns and clears whether the plugin reported a change to its settings since this was last called.
    fn take_settings_change(&self) -> bool {
        self.instance.as_ref().is_some_and(PluginInstance::take_dirty)
    }

    /// Captures complete settings, leaving the previous saved state unchanged on delivery failure.
    fn capture_state(&mut self, ui: &UiThread) -> Result<()> {
        if !self.keep_saved_state
            && let Some(instance) = &self.instance
        {
            let state = instance
                .state(ui)
                .with_context(|| format!("{}: Could not capture settings", self.slot.plugin.name))?;
            self.slot.set_state(state);
        }
        Ok(())
    }
}

impl LiveChain {
    /// Creates an empty chain saved to `path`, with instances prepared at `sample_rate`.
    #[must_use]
    pub const fn new(path: PathBuf, dispatcher: UiDispatcher, sample_rate: u32) -> Self {
        Self {
            slots: Vec::new(),
            next_id: 0,
            path,
            dispatcher,
            sample_rate,
            preset_name: None,
            modified: false,
            unread_file: false,
        }
    }

    /// Loads the chain saved in `path`, without loading its plugins yet.
    /// `load_slots` and `open_editor` load them when the preview, the Plugin Chain window, or an editor needs them.
    /// A chain file that cannot be parsed is moved aside first, so saving the empty chain cannot destroy it.
    /// One that cannot be read at all, or was saved by a newer version, gives an empty chain and the error,
    /// and stays in place until the user builds a new chain, see `save`.
    #[must_use]
    pub fn load(path: PathBuf, dispatcher: UiDispatcher, sample_rate: u32) -> (Self, Option<String>) {
        let (saved, error, unread_file) = match PluginChain::load_recovering(&path) {
            Ok((saved, message)) => (saved, message, false),
            Err(error) => (
                PluginChain::default(),
                Some(format!("{:#}", anyhow::Error::from(error))),
                true,
            ),
        };
        let mut chain = Self::new(path, dispatcher, sample_rate);
        chain.unread_file = unread_file;
        for slot in saved.slots {
            let live = chain.new_slot(slot);
            chain.slots.push(live);
        }
        (chain, error)
    }

    /// Loads and prepares the plugins of the slots that have not been loaded yet, only the enabled ones when asked.
    /// Returns true when it loaded any.
    pub fn load_slots(&mut self, ui: &UiThread, enabled_only: bool) -> bool {
        let sample_rate = self.sample_rate;
        let mut loaded = false;
        for slot in &mut self.slots {
            if !slot.load_attempted && (slot.is_enabled() || !enabled_only) {
                slot.load(ui, &self.dispatcher, sample_rate);
                loaded = true;
            }
        }
        loaded
    }

    /// Returns the slots in processing order.
    #[must_use]
    pub fn slots(&self) -> &[LiveSlot] {
        &self.slots
    }

    /// Returns the slot with an id.
    #[must_use]
    pub fn slot(&self, id: SlotId) -> Option<&LiveSlot> {
        self.slots.iter().find(|slot| slot.id == id)
    }

    /// Returns the number of slots.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.slots.len()
    }

    /// Returns the number of enabled slots.
    #[must_use]
    pub fn enabled_count(&self) -> usize {
        self.slots.iter().filter(|slot| slot.is_enabled()).count()
    }

    /// Returns the summary of the chain for the menus and the controls, for example "Pro-Q 4 > (Limiter)".
    #[must_use]
    pub fn summary(&self) -> String {
        super::labels::chain_summary(
            self.slots
                .iter()
                .map(|slot| (slot.plugin().name.as_str(), slot.is_enabled())),
        )
    }

    /// Returns the rate the instances are prepared at.
    #[must_use]
    pub const fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// Returns the name of the preset the chain was loaded from or saved as,
    /// even when it was modified since, see `is_modified`.
    #[must_use]
    pub fn preset_name(&self) -> Option<&str> {
        self.preset_name.as_deref()
    }

    /// Returns true when the chain was modified since it was loaded from or saved as its preset.
    #[must_use]
    pub const fn is_modified(&self) -> bool {
        self.modified
    }

    /// Records the preset the chain now matches, after it was saved as one, or forgets it, after it was deleted.
    /// The changes the plugins reported until now belong to the saved preset, so they are cleared.
    pub fn set_preset_name(&mut self, name: Option<String>) {
        self.preset_name = name;
        self.modified = false;
        for slot in &self.slots {
            slot.take_settings_change();
        }
    }

    /// Returns true when a plugin reported a change to its settings since this was last called,
    /// and marks the chain modified then.
    /// The changes the plugins report are cleared, so each is counted once.
    pub fn take_settings_changes(&mut self) -> bool {
        let mut changed = false;
        for slot in &self.slots {
            changed |= slot.take_settings_change();
        }
        if changed {
            self.modified = true;
        }
        changed
    }

    /// Returns the summed latency of the enabled slots whose instances are ready, in samples.
    #[must_use]
    pub fn total_latency_samples(&self) -> u64 {
        self.slots
            .iter()
            .filter(|slot| slot.is_enabled() && slot.is_ready())
            .map(|slot| u64::from(slot.latency_samples))
            .sum()
    }

    /// Adds a plugin at the end of the chain and returns its slot id.
    pub fn add(&mut self, ui: &UiThread, plugin: PluginDescription) -> SlotId {
        let live = self.create_slot(ui, ChainSlot::new(plugin));
        let id = live.id;
        self.slots.push(live);
        self.modified = true;
        id
    }

    /// Replaces every slot with the slots of a saved chain, such as a preset, and creates their instances.
    /// The new slots get new ids, so windows and events of the old slots no longer find them.
    /// The caller takes the processors back from the preview first, because the old instances are destroyed.
    pub fn replace(&mut self, ui: &UiThread, chain: PluginChain, preset_name: Option<String>) {
        self.slots.clear();
        for slot in chain.slots {
            let live = self.create_slot(ui, slot);
            self.slots.push(live);
        }
        self.preset_name = preset_name;
        self.modified = false;
    }

    /// Removes a slot. Returns false when there is no such slot.
    pub fn remove(&mut self, id: SlotId) -> bool {
        let count = self.slots.len();
        self.slots.retain(|slot| slot.id != id);
        let removed = self.slots.len() != count;
        if removed {
            self.modified = true;
        }
        removed
    }

    /// Moves a slot by an offset, negative towards the start.
    /// Returns false when there is no such slot or the move would leave the chain.
    pub fn move_slot(&mut self, id: SlotId, offset: isize) -> bool {
        let Some(index) = self.index_of(id) else {
            return false;
        };
        match index
            .checked_add_signed(offset)
            .filter(|&target| target < self.slots.len())
        {
            Some(target) if target != index => {
                let slot = self.slots.remove(index);
                self.slots.insert(target, slot);
                self.modified = true;
                true
            }
            _ => false,
        }
    }

    /// Enables or bypasses a slot. Returns true when it changed.
    pub fn set_enabled(&mut self, id: SlotId, enabled: bool) -> bool {
        match self.slots.iter_mut().find(|slot| slot.id == id) {
            Some(slot) if slot.slot.enabled != enabled => {
                slot.slot.enabled = enabled;
                self.modified = true;
                true
            }
            _ => false,
        }
    }

    /// Removes every slot, and forgets the preset the chain was loaded from or saved as.
    pub fn clear(&mut self) {
        self.slots.clear();
        self.preset_name = None;
        self.modified = false;
    }

    /// Collects typed requests without forgetting work while processors are still being retired.
    pub fn take_restart_requests(&mut self) -> Vec<SlotRestart> {
        for slot in &mut self.slots {
            if let Some(action) = slot
                .instance
                .as_ref()
                .and_then(|instance| instance.take_restart_flags().action())
            {
                slot.pending_restart = Some(match (slot.pending_restart, action) {
                    (Some(RestartAction::Reload), _) | (_, RestartAction::Reload) => RestartAction::Reload,
                    _ => RestartAction::Reconfigure,
                });
            }
        }
        self.slots
            .iter()
            .filter_map(|slot| slot.pending_restart.map(|action| SlotRestart { id: slot.id, action }))
            .collect()
    }

    /// Returns true while lifecycle work is waiting for processor retirement.
    #[must_use]
    pub fn has_pending_restarts(&self) -> bool {
        self.slots.iter().any(|slot| slot.pending_restart.is_some())
    }

    /// Applies restart actions and device rate changes together, with full reload taking precedence.
    /// Failed retirement retains the request for the next tick, and failed reloads keep their saved settings.
    ///
    /// # Errors
    ///
    /// Returns the first retirement, capture, reconstruction, preparation or editor reopening failure.
    pub fn apply_restarts(
        &mut self,
        ui: &UiThread,
        sample_rate: u32,
        requests: &[SlotRestart],
        owner: Option<RawWindowHandle>,
    ) -> Result<()> {
        let rate_changed = self.sample_rate != sample_rate;
        let mut first_error = None;
        for slot in &mut self.slots {
            let action = requests
                .iter()
                .find(|request| request.id == slot.id)
                .map(|request| request.action);
            if !rate_changed && action.is_none() {
                continue;
            }
            if slot
                .instance
                .as_ref()
                .is_some_and(PluginInstance::has_processor_handles)
            {
                slot.pending_restart = Some(action.unwrap_or(RestartAction::Reconfigure));
                first_error
                    .get_or_insert_with(|| anyhow::anyhow!("{}: The plugin is still in use", slot.slot.plugin.name));
                continue;
            }
            if action == Some(RestartAction::Reload) {
                let reopen = slot.editor.as_ref().is_some_and(|editor| !editor.is_closed());
                // Destroy the view before state capture and component/controller teardown.
                slot.editor = None;
                let result = slot.capture_state(ui).and_then(|()| {
                    if let Some(instance) = slot.instance.take() {
                        let instance = reload_for_preview(
                            ui,
                            instance,
                            &slot.slot.state(),
                            f64::from(sample_rate),
                            PREVIEW_BLOCK_FRAMES,
                        )?;
                        slot.latency_samples = instance.latency_samples(ui);
                        slot.instance = Some(instance);
                    } else {
                        slot.load(ui, &self.dispatcher, sample_rate);
                        if let Some(error) = &slot.error {
                            anyhow::bail!("{error}");
                        }
                    }
                    slot.error = None;
                    Ok(())
                });
                if let Err(error) = result {
                    // Never let a later device-rate change revive an old component whose reload failed.
                    // The last complete saved state remains available for an explicit load retry.
                    slot.instance = None;
                    slot.error = Some(format!("Could not reload: {error:#}"));
                    slot.latency_samples = 0;
                    slot.pending_restart = None;
                    first_error.get_or_insert(error);
                    continue;
                }
                if reopen && let Some(instance) = &slot.instance {
                    match EditorWindow::open(ui, instance, &slot.slot.plugin.name, owner) {
                        Ok(editor) => slot.editor = Some(editor),
                        Err(error) => {
                            first_error.get_or_insert_with(|| anyhow::Error::from(error));
                        }
                    }
                }
            } else {
                slot.prepare(ui, sample_rate);
                if let Some(error) = &slot.error {
                    first_error.get_or_insert_with(|| anyhow::anyhow!("{}: {error}", slot.slot.plugin.name));
                }
            }
            slot.pending_restart = None;
        }
        self.sample_rate = sample_rate;
        first_error.map_or(Ok(()), Err)
    }

    /// Builds the preview chain of the enabled slots, or returns `None` when no slot is enabled.
    ///
    /// # Errors
    ///
    /// Returns "<name>: <error>" for the first enabled slot whose plugin is not loaded or not prepared,
    /// because previewing without it would not be what the chain sounds like.
    pub fn preview_chain(&self, ui: &UiThread) -> Result<Option<PreviewChain>> {
        let mut plugins = Vec::new();
        for slot in self.slots.iter().filter(|slot| slot.is_enabled()) {
            let name = slot.slot.plugin.name.as_str();
            if slot.pending_restart.is_some() {
                anyhow::bail!("{name}: The plugin is waiting for its processor to retire");
            }
            match (&slot.instance, &slot.error) {
                (_, Some(error)) => anyhow::bail!("{name}: {error}"),
                (Some(instance), None) => plugins.push((name, instance)),
                (None, None) => anyhow::bail!("{name}: The plugin is not loaded"),
            }
        }
        if plugins.is_empty() {
            return Ok(None);
        }
        Ok(Some(PreviewChain::new(ui, &plugins)?))
    }

    /// Returns the sum of the tails the enabled, prepared instances report now, each limited, in frames.
    #[must_use]
    pub fn preview_tail_frames(&self, ui: &UiThread) -> usize {
        let instances = self
            .slots
            .iter()
            .filter(|slot| slot.is_enabled() && slot.is_ready())
            .filter_map(|slot| slot.instance.as_ref());
        chain_tail_frames(ui, instances, f64::from(self.sample_rate))
    }

    /// Captures the current settings of every instance into the saved slots atomically.
    /// Every processor must be retired first, so queued edits can be completely delivered.
    ///
    /// # Errors
    ///
    /// Returns an error without changing saved slots when any instance cannot flush its queued edits.
    pub fn capture_states(&mut self, ui: &UiThread) -> Result<()> {
        // Stage every blob before publishing any, so a failed slot cannot produce a mixed saved snapshot.
        let states = self
            .slots
            .iter()
            .map(|slot| {
                if slot.keep_saved_state {
                    Ok(None)
                } else {
                    slot.instance
                        .as_ref()
                        .map(|instance| {
                            instance
                                .state(ui)
                                .with_context(|| format!("{}: Could not capture settings", slot.slot.plugin.name))
                        })
                        .transpose()
                }
            })
            .collect::<Result<Vec<_>>>()?;
        for (slot, state) in self.slots.iter_mut().zip(states) {
            if let Some(state) = state {
                slot.slot.set_state(state);
            }
        }
        Ok(())
    }

    /// Shows the editor of a slot, opening a window titled with the plugin's name when it has none.
    /// On Windows, the window is owned by `owner`.
    ///
    /// # Errors
    ///
    /// Returns an error when there is no such slot, its plugin is not loaded, or its editor cannot be opened.
    pub fn open_editor(&mut self, ui: &UiThread, id: SlotId, owner: Option<RawWindowHandle>) -> Result<()> {
        let slot = self
            .slots
            .iter_mut()
            .find(|slot| slot.id == id)
            .context("The plugin is no longer in the chain")?;
        if let Some(editor) = &slot.editor {
            editor.show(ui);
            return Ok(());
        }
        if !slot.load_attempted || slot.instance.is_none() {
            slot.load(ui, &self.dispatcher, self.sample_rate);
        }
        // A plugin that loaded but could not be prepared for the preview still opens, so its settings can be changed.
        let Some(instance) = slot.instance.as_ref() else {
            anyhow::bail!("{}", slot.error.as_deref().unwrap_or("The plugin is not loaded"));
        };
        slot.editor = Some(EditorWindow::open(ui, instance, &slot.slot.plugin.name, owner)?);
        // The user sees and edits the settings the plugin has, so they are captured from now on.
        slot.keep_saved_state = false;
        Ok(())
    }

    /// Destroys the editor windows the user closed, on the UI thread.
    /// Returns true when any editor was closed, so the caller detaches processors and captures all slots atomically.
    pub fn reap_closed_editors(&mut self, _ui: &UiThread) -> bool {
        let mut reaped = false;
        for slot in &mut self.slots {
            if slot.editor.as_ref().is_some_and(EditorWindow::is_closed) {
                slot.editor = None;
                reaped = true;
            }
        }
        reaped
    }

    /// Returns the chain as it is saved, with the settings captured last.
    #[must_use]
    pub fn chain(&self) -> PluginChain {
        PluginChain {
            slots: self.slots.iter().map(|slot| slot.slot.clone()).collect(),
        }
    }

    /// Writes the chain to the chain file.
    /// When the file could not be read at startup, an empty chain is not saved, so the file stays as it is,
    /// and a new chain moves it aside as `<name>.unread.json` first, so it is never overwritten.
    ///
    /// # Errors
    ///
    /// Returns an error when the unread file cannot be moved aside or the chain file cannot be written.
    pub fn save(&mut self) -> Result<()> {
        if self.unread_file {
            if self.slots.is_empty() {
                return Ok(());
            }
            if self.path.exists() {
                let aside = self.path.with_extension("unread.json");
                std::fs::rename(&self.path, &aside)
                    .with_context(|| format!("Could not move the unread plugin chain {} aside", self.path.display()))?;
                tracing::warn!("Moved the unread plugin chain to {}", aside.display());
            }
            self.unread_file = false;
        }
        self.chain()
            .save_to(&self.path)
            .with_context(|| format!("Could not save the plugin chain to {}", self.path.display()))
    }

    /// Returns the index of a slot.
    fn index_of(&self, id: SlotId) -> Option<usize> {
        self.slots.iter().position(|slot| slot.id == id)
    }

    /// Creates the live slot of a saved slot, with a new id, and loads and prepares its instance.
    fn create_slot(&mut self, ui: &UiThread, slot: ChainSlot) -> LiveSlot {
        let mut live = self.new_slot(slot);
        live.load(ui, &self.dispatcher, self.sample_rate);
        live
    }

    /// Creates the live slot of a saved slot, with a new id, without loading its plugin yet.
    const fn new_slot(&mut self, slot: ChainSlot) -> LiveSlot {
        let id = SlotId(self.next_id);
        self.next_id += 1;
        LiveSlot {
            id,
            slot,
            editor: None,
            instance: None,
            latency_samples: 0,
            error: None,
            load_attempted: false,
            keep_saved_state: false,
            pending_restart: None,
        }
    }
}

/// Loads the plugin of a slot and creates an instance with the slot's saved settings.
/// Returns the instance and whether the plugin accepted the settings.
fn load_instance(ui: &UiThread, dispatcher: &UiDispatcher, slot: &ChainSlot) -> Result<(PluginInstance, bool)> {
    let module = Module::load(ui, &slot.plugin.bundle)?;
    let mut instance = PluginInstance::create(ui, &module, slot.plugin.class_id, Some(dispatcher.clone()))?;
    let accepted = instance
        .set_state(ui, &slot.state())
        .context("Could not restore the saved settings")?;
    Ok((instance, accepted))
}

/// Prepares an instance for the preview at the output rate, in blocks of up to `PREVIEW_BLOCK_FRAMES`.
fn prepare_instance(ui: &UiThread, instance: &mut PluginInstance, sample_rate: u32) -> Result<()> {
    Ok(prepare_for_preview(
        ui,
        instance,
        f64::from(sample_rate),
        PREVIEW_BLOCK_FRAMES,
    )?)
}

#[cfg(test)]
mod test_slots {
    use vst3_host::ui_channel;

    use super::*;
    use crate::test_helpers::effect;

    /// Returns a chain of slots without instances, as if their plugins were missing, with a latency each.
    fn chain_of(names: &[&str]) -> LiveChain {
        let (dispatcher, _jobs) = ui_channel();
        let mut chain = LiveChain::new(PathBuf::from("/data/plugin-chain.json"), dispatcher, 48_000);
        for name in names {
            push_slot(&mut chain, name);
        }
        chain
    }

    /// Adds a slot without an instance, the way `create_slot` numbers it.
    fn push_slot(chain: &mut LiveChain, name: &str) -> SlotId {
        let id = SlotId(chain.next_id);
        chain.next_id += 1;
        chain.slots.push(LiveSlot {
            id,
            slot: ChainSlot::new(effect("Acme", name)),
            editor: None,
            instance: None,
            latency_samples: 100,
            error: Some(String::from("Missing")),
            load_attempted: true,
            keep_saved_state: false,
            pending_restart: None,
        });
        id
    }

    /// Returns the plugin names in chain order.
    fn names(chain: &LiveChain) -> Vec<String> {
        chain.slots().iter().map(|slot| slot.plugin().name.clone()).collect()
    }

    #[test]
    fn slots_move_within_the_chain() {
        let mut chain = chain_of(&["A", "B", "C"]);
        let first = chain.slots().first().map(LiveSlot::id).expect("first slot");
        assert!(chain.move_slot(first, 1));
        assert_eq!(names(&chain), ["B", "A", "C"]);
        assert!(!chain.move_slot(first, -2));
        assert!(chain.move_slot(first, -1));
        assert!(!chain.move_slot(first, -1));
        assert_eq!(names(&chain), ["A", "B", "C"]);
    }

    #[test]
    fn removed_ids_are_never_reused() {
        let mut chain = chain_of(&["A", "B"]);
        let second = chain.slots().get(1).map(LiveSlot::id).expect("second slot");
        assert!(chain.remove(second));
        assert!(!chain.remove(second));
        let added = push_slot(&mut chain, "C");
        assert_ne!(added, second);
        assert!(chain.slot(second).is_none());
        assert_eq!(names(&chain), ["A", "C"]);
    }

    #[test]
    fn bypassed_slots_are_saved_but_not_counted() {
        let mut chain = chain_of(&["A", "B"]);
        let first = chain.slots().first().map(LiveSlot::id).expect("first slot");
        assert!(chain.set_enabled(first, false));
        assert!(!chain.set_enabled(first, false));
        assert_eq!(chain.enabled_count(), 1);
        let saved = chain.chain();
        assert_eq!(saved.slots.len(), 2);
        assert!(!saved.slots.first().expect("saved slot").enabled);
        assert_eq!(chain.total_latency_samples(), 0);
        chain.clear();
        assert_eq!(chain.len(), 0);
    }

    #[test]
    fn structural_changes_mark_the_preset_modified() {
        let mut chain = chain_of(&["A", "B"]);
        let first = chain.slots().first().map(LiveSlot::id).expect("first slot");
        let preset = || Some(String::from("Mastering"));

        chain.set_preset_name(preset());
        assert_eq!(chain.preset_name(), Some("Mastering"));
        assert!(!chain.is_modified());
        assert!(!chain.move_slot(first, -1));
        assert!(!chain.set_enabled(first, true));
        assert!(!chain.take_settings_changes());
        assert!(!chain.is_modified());

        assert!(chain.move_slot(first, 1));
        assert!(chain.is_modified());
        assert_eq!(chain.preset_name(), Some("Mastering"));
        chain.set_preset_name(preset());
        assert!(chain.set_enabled(first, false));
        assert!(chain.is_modified());
        chain.set_preset_name(preset());
        assert!(chain.remove(first));
        assert!(chain.is_modified());
        assert_eq!(chain.preset_name(), Some("Mastering"));

        chain.clear();
        assert_eq!(chain.preset_name(), None);
        assert!(!chain.is_modified());
    }

    #[test]
    fn a_chain_file_from_a_newer_version_is_never_overwritten() {
        let directory = tempfile::tempdir().expect("temp dir");
        let path = directory.path().join("plugin-chain.json");
        let newer = r#"{"version": 99, "slots": []}"#;
        std::fs::write(&path, newer).expect("written");
        let (dispatcher, _jobs) = ui_channel();

        let (mut chain, error) = LiveChain::load(path.clone(), dispatcher, 48_000);
        assert!(error.is_some_and(|error| error.contains("newer version")));
        chain.save().expect("an empty chain is not saved");
        assert_eq!(std::fs::read_to_string(&path).expect("read"), newer);

        push_slot(&mut chain, "A");
        chain.save().expect("saved");
        let aside = path.with_extension("unread.json");
        assert_eq!(std::fs::read_to_string(&aside).expect("moved aside"), newer);
        assert_eq!(PluginChain::load_from(&path).expect("loads").slots.len(), 1);
    }
}

#[cfg(test)]
mod test_restarts {
    use audiobatch_test_plugin::{Kind, RELOAD_TRIGGER_PARAMETER};
    use vst3_host::ClassId;

    use super::*;
    use crate::test_helpers::effect;

    /// Adds an in-process fake instance without opening a native editor or loading a bundle.
    fn fake_slot(ui: &UiThread, chain: &mut LiveChain, kind: Kind) -> SlotId {
        let module = Module::from_factory(ui, "restart tests", audiobatch_test_plugin::factory());
        let mut instance =
            PluginInstance::create(ui, &module, ClassId::from_tuid(&kind.class_id()), None).expect("instance");
        prepare_instance(ui, &mut instance, chain.sample_rate).expect("prepare");
        let mut slot = chain.new_slot(ChainSlot::new(effect("Tests", kind.name())));
        slot.slot.set_state(instance.state(ui).expect("initial state"));
        slot.instance = Some(instance);
        slot.load_attempted = true;
        let id = slot.id;
        chain.slots.push(slot);
        id
    }

    /// Runs a non-rendering lifecycle assertion on the process's existing test UI thread.
    fn on_ui(test: impl FnOnce(&UiThread, &mut LiveChain) + Send + 'static) {
        super::test_replace::dispatcher()
            .call(
                move |ui| {
                    let mut chain = LiveChain::new(
                        PathBuf::from("/unused/plugin-chain.json"),
                        super::test_replace::dispatcher().clone(),
                        48_000,
                    );
                    test(ui, &mut chain);
                },
                &|| false,
            )
            .expect("UI job");
    }

    #[test]
    fn reload_survives_device_rate_and_latency_flags_and_restores_the_state() {
        on_ui(|ui, chain| {
            let id = fake_slot(ui, chain, Kind::ReloadRequired);
            let instance = chain
                .slots
                .first()
                .expect("first slot")
                .instance
                .as_ref()
                .expect("instance");
            instance.set_parameter(ui, 0, 0.375).expect("gain");
            instance
                .set_parameter(ui, RELOAD_TRIGGER_PARAMETER, 1.0)
                .expect("trigger");
            let requests = chain.take_restart_requests();
            assert_eq!(
                requests,
                [SlotRestart {
                    id,
                    action: RestartAction::Reload
                }]
            );
            chain.apply_restarts(ui, 96_000, &requests, None).expect("reload");
            assert_eq!(chain.sample_rate(), 96_000);
            let instance = chain
                .slots
                .first()
                .expect("first slot")
                .instance
                .as_ref()
                .expect("recreated instance");
            assert_eq!(instance.state(ui).expect("state").component, 0.375_f64.to_le_bytes());
            let mut processor = instance.processor(ui).expect("processor");
            assert!((processor.sample_rate() - 96_000.0).abs() < f64::EPSILON);
            processor.process(1).expect("fresh component works");
        });
    }

    #[test]
    fn a_reload_request_is_retained_until_all_processor_handles_retire() {
        on_ui(|ui, chain| {
            let id = fake_slot(ui, chain, Kind::ReloadRequired);
            let instance = chain
                .slots
                .first()
                .expect("first slot")
                .instance
                .as_ref()
                .expect("instance");
            let processor = instance.processor(ui).expect("old processor");
            instance
                .set_parameter(ui, RELOAD_TRIGGER_PARAMETER, 1.0)
                .expect("trigger");
            let requests = chain.take_restart_requests();
            assert!(chain.apply_restarts(ui, 96_000, &requests, None).is_err());
            assert_eq!(
                chain.take_restart_requests(),
                [SlotRestart {
                    id,
                    action: RestartAction::Reload
                }]
            );
            drop(processor);
            let requests = chain.take_restart_requests();
            chain
                .apply_restarts(ui, 96_000, &requests, None)
                .expect("retired reload");
            let mut processor = chain
                .slots
                .first()
                .expect("first slot")
                .instance
                .as_ref()
                .expect("instance")
                .processor(ui)
                .expect("new processor");
            processor.process(1).expect("reload repaired component");
        });
    }

    #[test]
    fn failed_capture_and_failed_reload_preserve_the_previous_saved_settings() {
        on_ui(|ui, chain| {
            fake_slot(ui, chain, Kind::FlushFailing);
            let previous = chain.chain();
            chain
                .slots
                .first()
                .expect("first slot")
                .instance
                .as_ref()
                .expect("instance")
                .set_parameter(ui, 0, 0.5)
                .expect("edit");
            assert!(chain.capture_states(ui).is_err());
            assert_eq!(chain.chain(), previous);
            let id = chain.slots.first().expect("first slot").id;
            assert!(
                chain
                    .apply_restarts(
                        ui,
                        48_000,
                        &[SlotRestart {
                            id,
                            action: RestartAction::Reload
                        }],
                        None
                    )
                    .is_err()
            );
            assert_eq!(chain.chain(), previous);
            assert!(!chain.slots.first().expect("first slot").is_ready());
            assert!(chain.preview_chain(ui).is_err());
        });
    }

    #[test]
    fn one_failed_slot_prevents_publishing_a_partially_updated_chain_snapshot() {
        on_ui(|ui, chain| {
            fake_slot(ui, chain, Kind::Gain);
            fake_slot(ui, chain, Kind::FlushFailing);
            let previous = chain.chain();
            for slot in &chain.slots {
                slot.instance
                    .as_ref()
                    .expect("instance")
                    .set_parameter(ui, 0, 0.25)
                    .expect("edit");
            }
            assert!(chain.capture_states(ui).is_err());
            assert_eq!(chain.chain(), previous);
        });
    }

    #[test]
    fn rejected_reload_state_leaves_the_saved_blob_available_for_recovery() {
        on_ui(|ui, chain| {
            let id = fake_slot(ui, chain, Kind::Gain);
            let slot = chain.slots.first_mut().expect("slot");
            slot.slot.component_state = vec![1, 2, 3];
            slot.keep_saved_state = true;
            let previous = chain.chain();
            let requests = [SlotRestart {
                id,
                action: RestartAction::Reload,
            }];
            assert!(chain.apply_restarts(ui, 48_000, &requests, None).is_err());
            assert_eq!(chain.chain(), previous);
            assert!(!chain.slots.first().expect("slot").has_instance());
            assert!(chain.preview_chain(ui).is_err());
        });
    }

    #[test]
    fn lighter_reconfiguration_keeps_objects_and_recovers_from_failed_preparation() {
        on_ui(|ui, chain| {
            let id = fake_slot(ui, chain, Kind::Gain);
            let previous = chain.chain();
            let requests = [SlotRestart {
                id,
                action: RestartAction::Reconfigure,
            }];
            assert!(chain.apply_restarts(ui, 0, &requests, None).is_err());
            assert!(chain.slots.first().expect("first slot").has_instance());
            assert!(!chain.slots.first().expect("first slot").is_ready());
            assert_eq!(chain.chain(), previous);
            chain
                .apply_restarts(ui, 96_000, &requests, None)
                .expect("recover preparation");
            assert!(chain.slots.first().expect("first slot").is_ready());
            assert_eq!(chain.chain(), previous);
        });
    }
}

#[cfg(test)]
mod test_replace {
    use std::sync::OnceLock;

    use vst3_host::ui_channel;

    use super::*;
    use crate::test_helpers::effect;

    /// The dispatcher of the one UI thread these tests share, since a process can have only one.
    static DISPATCHER: OnceLock<UiDispatcher> = OnceLock::new();

    /// Returns the dispatcher of the shared test UI thread, starting the thread on first use.
    pub(super) fn dispatcher() -> &'static UiDispatcher {
        DISPATCHER.get_or_init(|| UiThread::spawn_dedicated().expect("no other UI thread in the test process"))
    }

    /// Returns a saved slot of a plugin whose bundle does not exist, with a state and an enabled flag.
    fn missing_slot(name: &str, component: &[u8], enabled: bool) -> ChainSlot {
        let mut plugin = effect("Acme", name);
        plugin.bundle = PathBuf::from(format!("/missing/{name}.vst3"));
        let mut slot = ChainSlot::new(plugin);
        slot.component_state = component.to_vec();
        slot.enabled = enabled;
        slot
    }

    #[test]
    fn replacing_loads_the_preset_slots_with_new_ids_and_keeps_missing_plugins_settings() {
        let preset = PluginChain {
            slots: vec![
                missing_slot("Width", b"wide", false),
                missing_slot("Gain", b"loud", true),
            ],
        };
        let expected = preset.clone();
        let (dispatcher_for_chain, _jobs) = ui_channel();
        let result = dispatcher()
            .call(
                move |ui| {
                    let mut chain =
                        LiveChain::new(PathBuf::from("/data/plugin-chain.json"), dispatcher_for_chain, 48_000);
                    let old = chain.add(ui, effect("Acme", "Old"));
                    chain.replace(ui, preset, Some(String::from("Wide")));
                    let ids: Vec<SlotId> = chain.slots().iter().map(LiveSlot::id).collect();
                    let errors = chain.slots().iter().filter(|slot| slot.error().is_some()).count();
                    (old, ids, errors, chain.chain(), chain.preset_name().map(str::to_owned))
                },
                &|| false,
            )
            .expect("the test UI thread runs the job");
        let (old, ids, errors, saved, preset_name) = result;
        assert_eq!(ids.len(), 2);
        assert!(!ids.contains(&old));
        assert_eq!(errors, 2);
        assert_eq!(saved, expected);
        assert_eq!(preset_name.as_deref(), Some("Wide"));
    }

    #[test]
    fn a_restored_chain_loads_its_plugins_only_when_they_are_needed() {
        let directory = tempfile::tempdir().expect("temporary directory");
        let path = directory.path().join("plugin-chain.json");
        PluginChain {
            slots: vec![
                missing_slot("Width", b"wide", false),
                missing_slot("Gain", b"loud", true),
            ],
        }
        .save_to(&path)
        .expect("saved");
        let (dispatcher_for_chain, _jobs) = ui_channel();
        let errors = |chain: &LiveChain| chain.slots().iter().filter(|slot| slot.error().is_some()).count();
        let result = dispatcher()
            .call(
                move |ui| {
                    let (mut chain, error) = LiveChain::load(path, dispatcher_for_chain, 48_000);
                    let errors_after_load = errors(&chain);
                    let enabled_loaded = chain.load_slots(ui, true);
                    let errors_after_enabled = errors(&chain);
                    let all_loaded = chain.load_slots(ui, false);
                    let again = chain.load_slots(ui, false);
                    (
                        error,
                        errors_after_load,
                        enabled_loaded,
                        errors_after_enabled,
                        all_loaded,
                        errors(&chain),
                        again,
                    )
                },
                &|| false,
            )
            .expect("the test UI thread runs the job");
        let (error, errors_after_load, enabled_loaded, errors_after_enabled, all_loaded, errors_after_all, again) =
            result;
        assert_eq!(error, None);
        assert_eq!(errors_after_load, 0);
        assert!(enabled_loaded);
        assert_eq!(errors_after_enabled, 1);
        assert!(all_loaded);
        assert_eq!(errors_after_all, 2);
        assert!(!again);
    }
}
