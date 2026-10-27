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
//! Loading a preset replaces every slot, and the chain remembers the preset's name until its slots change.
//! The settings of the instances are captured when processing starts, an editor closes, and the app quits.
//! Each slot can show its plugin's editor in a native window.
//! The window only hides when the user closes it,
//! and the next housekeeping tick captures the settings so they can be saved, then destroys the window.

use std::path::PathBuf;

use anyhow::{Context as _, Result};
use audiobatch_plugins::{
    ChainSlot, PluginChain, PluginDescription, PreviewChain, chain_tail_frames, prepare_for_preview,
};
use raw_window_handle::RawWindowHandle;
use vst3_host::{EditorWindow, Module, PluginInstance, UiDispatcher, UiThread};

/// Most frames the preview passes to the live instances in one call.
pub const PREVIEW_BLOCK_FRAMES: usize = 1024;

/// Identifies a slot of the live chain for as long as the slot exists. Ids are never reused.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SlotId(u64);

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
    /// Name of the preset the chain was loaded from or saved as,
    /// until a slot is added, removed, moved, enabled, or bypassed.
    preset_name: Option<String>,
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
            Ok(instance) => {
                self.instance = Some(instance);
                self.prepare(ui, sample_rate);
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

    /// Captures the instance's current settings into the saved slot.
    fn capture_state(&mut self, ui: &UiThread) {
        if let Some(instance) = &self.instance {
            instance.flush_parameter_edits(ui);
            if let Ok(state) = instance.state(ui) {
                self.slot.set_state(state);
            }
        }
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
        }
    }

    /// Loads the chain saved in `path`, without loading its plugins yet.
    /// `load_slots` and `open_editor` load them when the preview, the Plugin Chain window, or an editor needs them.
    /// A chain file that cannot be read gives an empty chain and the error,
    /// and one that cannot be parsed is moved aside first, so saving the empty chain cannot destroy it.
    #[must_use]
    pub fn load(path: PathBuf, dispatcher: UiDispatcher, sample_rate: u32) -> (Self, Option<String>) {
        let (saved, error) = match PluginChain::load_recovering(&path) {
            Ok((saved, message)) => (saved, message),
            Err(error) => (PluginChain::default(), Some(format!("{error:#}"))),
        };
        let mut chain = Self::new(path, dispatcher, sample_rate);
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

    /// Returns the name of the preset the chain matches, when it was loaded from or saved as one
    /// and its slots have not changed since.
    #[must_use]
    pub fn preset_name(&self) -> Option<&str> {
        self.preset_name.as_deref()
    }

    /// Records the preset the chain now matches, after it was saved as one.
    pub fn set_preset_name(&mut self, name: Option<String>) {
        self.preset_name = name;
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
        self.preset_name = None;
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
    }

    /// Removes a slot. Returns false when there is no such slot.
    pub fn remove(&mut self, id: SlotId) -> bool {
        let count = self.slots.len();
        self.slots.retain(|slot| slot.id != id);
        let removed = self.slots.len() != count;
        if removed {
            self.preset_name = None;
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
                self.preset_name = None;
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
                self.preset_name = None;
                true
            }
            _ => false,
        }
    }

    /// Removes every slot.
    pub fn clear(&mut self) {
        self.slots.clear();
        self.preset_name = None;
    }

    /// Prepares every instance again at another rate.
    pub fn set_sample_rate(&mut self, ui: &UiThread, sample_rate: u32) {
        if self.sample_rate == sample_rate {
            return;
        }
        self.sample_rate = sample_rate;
        for slot in &mut self.slots {
            slot.prepare(ui, sample_rate);
        }
    }

    /// Returns the slots whose instances asked for a new bus layout, a reload, or reported a new latency,
    /// and clears their requests.
    /// The caller takes their processors back from the preview before preparing them again.
    pub fn take_restart_requests(&self) -> Vec<SlotId> {
        self.slots
            .iter()
            .filter(|slot| {
                slot.instance
                    .as_ref()
                    .is_some_and(|instance| instance.take_restart_flags().needs_reconfiguration())
            })
            .map(|slot| slot.id)
            .collect()
    }

    /// Prepares the instances of some slots again at the current rate.
    pub fn prepare_again(&mut self, ui: &UiThread, ids: &[SlotId]) {
        let sample_rate = self.sample_rate;
        for slot in self.slots.iter_mut().filter(|slot| ids.contains(&slot.id)) {
            slot.prepare(ui, sample_rate);
        }
    }

    /// Builds the preview chain of the enabled slots, or returns `None` when no slot is enabled.
    ///
    /// # Errors
    ///
    /// Returns "<name>: <error>" for the first enabled slot whose plugin is not loaded or not prepared,
    /// because previewing without it would not be what the chain sounds like.
    pub fn preview_chain(&self, ui: &UiThread) -> Result<Option<PreviewChain>, String> {
        let mut plugins = Vec::new();
        for slot in self.slots.iter().filter(|slot| slot.is_enabled()) {
            let name = slot.slot.plugin.name.as_str();
            match (&slot.instance, &slot.error) {
                (_, Some(error)) => return Err(format!("{name}: {error}")),
                (Some(instance), None) => plugins.push((name, instance)),
                (None, None) => return Err(format!("{name}: The plugin is not loaded")),
            }
        }
        if plugins.is_empty() {
            return Ok(None);
        }
        PreviewChain::new(ui, &plugins).map(Some)
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

    /// Captures the current settings of every instance into the saved slots.
    /// Edits made in an editor reach an instance's state only through processing,
    /// so the caller takes the processors back from a paused preview first.
    pub fn capture_states(&mut self, ui: &UiThread) {
        for slot in &mut self.slots {
            slot.capture_state(ui);
        }
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
        if !slot.load_attempted {
            slot.load(ui, &self.dispatcher, self.sample_rate);
        }
        if let Some(error) = &slot.error {
            anyhow::bail!("{error}");
        }
        let instance = slot.instance.as_ref().context("The plugin is not loaded")?;
        slot.editor = Some(EditorWindow::open(ui, instance, &slot.slot.plugin.name, owner)?);
        Ok(())
    }

    /// Captures the settings of the slots whose editors the user closed, and destroys those windows.
    /// Returns true when any editor was closed, so the chain should be saved.
    pub fn reap_closed_editors(&mut self, ui: &UiThread) -> bool {
        let mut reaped = false;
        for slot in &mut self.slots {
            if slot.editor.as_ref().is_some_and(EditorWindow::is_closed) {
                slot.capture_state(ui);
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
    ///
    /// # Errors
    ///
    /// Returns an error when the file cannot be written.
    pub fn save(&self) -> Result<()> {
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
        }
    }
}

/// Loads the plugin of a slot and creates an instance with the slot's saved settings.
/// An instance that refuses the settings is not kept,
/// because capturing its state later would replace the saved settings with its defaults.
fn load_instance(ui: &UiThread, dispatcher: &UiDispatcher, slot: &ChainSlot) -> Result<PluginInstance> {
    let module = Module::load(ui, &slot.plugin.bundle)?;
    let mut instance = PluginInstance::create(ui, &module, slot.plugin.class_id, Some(dispatcher.clone()))?;
    instance
        .set_state(ui, &slot.state())
        .context("Could not restore the saved settings")?;
    Ok(instance)
}

/// Prepares an instance for the preview at the output rate, in blocks of up to `PREVIEW_BLOCK_FRAMES`.
fn prepare_instance(ui: &UiThread, instance: &mut PluginInstance, sample_rate: u32) -> Result<()> {
    prepare_for_preview(ui, instance, f64::from(sample_rate), PREVIEW_BLOCK_FRAMES)
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
    fn structural_changes_forget_the_preset_name() {
        let mut chain = chain_of(&["A", "B"]);
        let first = chain.slots().first().map(LiveSlot::id).expect("first slot");
        let preset = || Some(String::from("Mastering"));

        chain.set_preset_name(preset());
        assert_eq!(chain.preset_name(), Some("Mastering"));
        assert!(!chain.move_slot(first, -1));
        assert!(!chain.set_enabled(first, true));
        assert_eq!(chain.preset_name(), Some("Mastering"));

        assert!(chain.move_slot(first, 1));
        assert_eq!(chain.preset_name(), None);
        chain.set_preset_name(preset());
        assert!(chain.set_enabled(first, false));
        assert_eq!(chain.preset_name(), None);
        chain.set_preset_name(preset());
        assert!(chain.remove(first));
        assert_eq!(chain.preset_name(), None);
        chain.set_preset_name(preset());
        chain.clear();
        assert_eq!(chain.preset_name(), None);
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
    fn dispatcher() -> &'static UiDispatcher {
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
