//! Detached sound, chronological local history, and comparison ownership shared across editor lifetimes.
//!
//! Call only from UI or non-audio state callbacks. No method invokes a host, touches DSP, or accesses a preset file.

use serde::{Deserialize, Serialize};

use crate::params::{BYPASS_PARAMETER, DELTA_PARAMETER, PARAMETER_COUNT, Parameters, Snapshot};

/// Sound controls exclude four Solo switches and the global Bypass and Delta audition switches.
pub const SOUND_PARAMETER_COUNT: usize = 25;
/// Bounded local history is transient and never included in saved projects.
#[cfg(any(feature = "editor", test))]
const HISTORY_CAPACITY: usize = 128;

/// Independent Rust sound schema, in stable host-id order with audition controls omitted.
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SoundSnapshot {
    /// Exact normalized values preserve accepted readback and legacy off-grid settings.
    pub values: [f64; SOUND_PARAMETER_COUNT],
}

/// Project-owned identity and baseline never retain a library path or an accepted filesystem revision.
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PresetOrigin {
    /// Stable preset identity, separate from its library revision.
    pub id: String,
    /// Display name survives removal of the library entry.
    pub name: String,
    /// Source sound remains intact across normalized recall conversion.
    pub baseline: SoundSnapshot,
    /// Actual accepted recall representation avoids false modified status from legal value conversion.
    #[serde(default)]
    pub effective: Option<SoundSnapshot>,
}

/// A and B remain independently editable, including their optional selected-preset baselines.
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ComparisonState {
    /// Stored sounds. Capture refreshes the active slot from authoritative live atomics.
    pub slots: [SoundSnapshot; 2],
    /// Active slot, zero for A and one for B.
    pub active: usize,
    /// Optional project provenance, not transient permission to mutate a library file.
    pub origins: [Option<PresetOrigin>; 2],
}

/// One explicit user command changes only touched parameters and optional comparison metadata.
#[cfg(any(feature = "editor", test))]
#[derive(Clone, Debug)]
pub struct Command {
    /// Before and after values for touched ids. Host automation never adds entries by itself.
    pub changes: Vec<(u32, f64, f64)>,
    /// Before and after comparison selection, sounds, and provenance for metadata commands.
    metadata: Option<(ComparisonState, ComparisonState)>,
}

/// Non-audio owner is protected by the parameter allocation's short editing mutex.
pub struct EditingState {
    /// Last synchronized host-restore identity.
    generation: u64,
    /// Persistent comparisons, independent of transient history.
    comparison: ComparisonState,
    /// Chronological commands including ordinary edits, A/B, copy, and preset recall.
    #[cfg(any(feature = "editor", test))]
    history: Vec<Command>,
    /// Next redo position within the retained history.
    #[cfg(any(feature = "editor", test))]
    position: usize,
    /// Nested local gesture depth.
    #[cfg(any(feature = "editor", test))]
    depth: usize,
    /// First touched values for the outer gesture.
    #[cfg(any(feature = "editor", test))]
    pending: Vec<(u32, f64)>,
    /// Captured metadata before a comparison command.
    #[cfg(any(feature = "editor", test))]
    metadata_before: Option<ComparisonState>,
}

impl SoundSnapshot {
    /// Captures all sound values, leaving live audition settings outside the detached sound.
    pub fn capture(snapshot: Snapshot) -> Self {
        let mut values = [0.0; SOUND_PARAMETER_COUNT];
        for (target, id) in values.iter_mut().zip(sound_ids()) {
            *target = snapshot_value(snapshot, id);
        }
        Self { values }
    }

    /// Rejects malformed complete snapshots before any sound or metadata publication.
    pub fn is_valid(self) -> bool {
        self.values
            .iter()
            .all(|value| value.is_finite() && (0.0..=1.0).contains(value))
    }

    /// Lists exact target values without including Solo, Delta, Bypass, or editor choices.
    #[cfg(feature = "editor")]
    pub fn parameters(self) -> impl Iterator<Item = (u32, f64)> {
        sound_ids().zip(self.values)
    }
}

impl ComparisonState {
    /// Initializes legacy states from their restored live sound, selecting A without inventing provenance.
    pub const fn from_sound(sound: SoundSnapshot) -> Self {
        Self {
            slots: [sound; 2],
            active: 0,
            origins: [None, None],
        }
    }

    /// Validates all metadata before a host restore publishes any values.
    pub fn is_valid(&self) -> bool {
        self.active < 2
            && self.slots.iter().all(|sound| sound.is_valid())
            && self.origins.iter().flatten().all(|origin| {
                valid_identity(&origin.id)
                    && valid_name(&origin.name)
                    && origin.baseline.is_valid()
                    && origin.effective.is_none_or(SoundSnapshot::is_valid)
            })
    }
}

impl EditingState {
    /// Allocates the non-audio service before exposing the parameter allocation to a host.
    pub fn new(snapshot: Snapshot) -> Self {
        Self {
            generation: 0,
            comparison: ComparisonState::from_sound(SoundSnapshot::capture(snapshot)),
            #[cfg(any(feature = "editor", test))]
            history: Vec::new(),
            #[cfg(any(feature = "editor", test))]
            position: 0,
            #[cfg(any(feature = "editor", test))]
            depth: 0,
            #[cfg(any(feature = "editor", test))]
            pending: Vec::new(),
            #[cfg(any(feature = "editor", test))]
            metadata_before: None,
        }
    }

    /// Lazily clears transient edits after host recall, without imposing a history lock on audio automation.
    pub fn synchronize(&mut self, generation: u64, snapshot: Snapshot) {
        if self.generation != generation {
            self.generation = generation;
            self.comparison = ComparisonState::from_sound(SoundSnapshot::capture(snapshot));
            #[cfg(any(feature = "editor", test))]
            {
                self.history.clear();
                self.position = 0;
                self.depth = 0;
                self.pending.clear();
                self.metadata_before = None;
            }
        }
    }

    /// Installs independently validated project comparisons after sound recall invalidates local history.
    pub fn restore_comparison(&mut self, comparison: ComparisonState) {
        self.comparison = comparison;
    }

    /// Captures live automation for the active slot without treating it as a local history action.
    pub fn comparison(&self, snapshot: Snapshot) -> ComparisonState {
        let mut comparison = self.comparison.clone();
        *comparison
            .slots
            .get_mut(comparison.active)
            .expect("validated active slot") = SoundSnapshot::capture(snapshot);
        comparison
    }

    /// Opens a nested transaction. No-op transactions preserve the existing redo branch.
    #[cfg(any(feature = "editor", test))]
    pub fn begin(&mut self) {
        if self.depth == 0 {
            self.pending.clear();
            self.metadata_before = None;
        }
        self.depth += 1;
    }

    /// Captures a touched value before its first local write, never after a host callback has changed it.
    #[cfg(any(feature = "editor", test))]
    pub fn touch(&mut self, id: u32, value: f64) {
        if self.depth > 0 && !self.pending.iter().any(|(existing, _)| *existing == id) {
            self.pending.push((id, value));
        }
    }

    /// Closes the outer command using actual readback, with no host callbacks while ownership is borrowed.
    #[cfg(any(feature = "editor", test))]
    pub fn end(&mut self, snapshot: Snapshot) {
        if self.depth == 0 {
            return;
        }
        self.depth -= 1;
        if self.depth != 0 {
            return;
        }
        let changes = std::mem::take(&mut self.pending)
            .into_iter()
            .filter_map(|(id, before)| {
                let after = snapshot_value(snapshot, id);
                (before.to_bits() != after.to_bits()).then_some((id, before, after))
            })
            .collect::<Vec<_>>();
        let metadata = self.metadata_before.take().and_then(|before| {
            let after = self.comparison(snapshot);
            (before != after).then_some((before, after))
        });
        if changes.is_empty() && metadata.is_none() {
            return;
        }
        self.history.truncate(self.position);
        if self.history.len() == HISTORY_CAPACITY {
            self.history.remove(0);
        }
        self.history.push(Command { changes, metadata });
        self.position = self.history.len();
    }

    /// Publishes comparison metadata before parameter callbacks can reenter project save.
    #[cfg(any(feature = "editor", test))]
    pub fn switch(&mut self, slot: usize, snapshot: Snapshot) -> Option<SoundSnapshot> {
        if slot >= 2 || slot == self.comparison.active {
            return None;
        }
        let before = self.comparison(snapshot);
        let target = *before.slots.get(slot)?;
        self.metadata_before = Some(before.clone());
        self.comparison = before;
        self.comparison.active = slot;
        Some(target)
    }

    /// Copies sound only into the other slot, preserving that slot's selected-preset identity and baseline.
    #[cfg(any(feature = "editor", test))]
    pub fn copy(&mut self, snapshot: Snapshot) {
        let before = self.comparison(snapshot);
        self.metadata_before = Some(before.clone());
        self.comparison = before;
        let other = 1 - self.comparison.active;
        *self.comparison.slots.get_mut(other).expect("other slot") = SoundSnapshot::capture(snapshot);
    }

    /// Selects a preset before recall, retaining its baseline independently from a transient library binding.
    #[cfg(feature = "editor")]
    pub fn select_preset(&mut self, origin: PresetOrigin, snapshot: Snapshot) {
        let before = self.comparison(snapshot);
        self.metadata_before = Some(before.clone());
        self.comparison = before;
        *self
            .comparison
            .origins
            .get_mut(self.comparison.active)
            .expect("active origin") = Some(origin);
    }

    /// Updates only the selected source name, preserving sound, identity, and modified baseline.
    #[cfg(feature = "editor")]
    pub fn rename_origin(&mut self, id: &str, name: String, snapshot: Snapshot) {
        let before = self.comparison(snapshot);
        if let Some(Some(origin)) = self.comparison.origins.get_mut(self.comparison.active) && origin.id == id {
            origin.name = name;
            self.metadata_before = Some(before);
        }
    }

    /// Finishes recall with exact accepted sound while preserving the independent source baseline.
    #[cfg(feature = "editor")]
    pub fn set_effective_origin(&mut self, sound: SoundSnapshot) {
        if let Some(Some(origin)) = self.comparison.origins.get_mut(self.comparison.active) {
            origin.effective = Some(sound);
        }
    }

    /// Reports availability without modifying the chronological history.
    #[cfg(any(feature = "editor", test))]
    pub const fn can_undo(&self) -> bool {
        self.depth == 0 && self.position > 0
    }

    /// Reports the redo branch without manufacturing entries from host automation.
    #[cfg(any(feature = "editor", test))]
    pub const fn can_redo(&self) -> bool {
        self.depth == 0 && self.position < self.history.len()
    }

    /// Records exact replay readback only for the direction just applied, preventing normalization drift on repeats.
    #[cfg(feature = "editor")]
    pub fn complete_replay(&mut self, undo: bool, snapshot: Snapshot) {
        let index = if undo {
            self.position
        } else {
            self.position.saturating_sub(1)
        };
        if let Some(command) = self.history.get_mut(index) {
            for (id, before, after) in &mut command.changes {
                if undo {
                    *before = snapshot_value(snapshot, *id);
                } else {
                    *after = snapshot_value(snapshot, *id);
                }
            }
        }
    }

    /// Captures replay targets and updates metadata before reentrant parameter notifications.
    #[cfg(any(feature = "editor", test))]
    pub fn replay(&mut self, undo: bool) -> Option<Command> {
        if (undo && !self.can_undo()) || (!undo && !self.can_redo()) {
            return None;
        }
        if undo {
            self.position -= 1;
        }
        let command = self.history.get(self.position)?.clone();
        if !undo {
            self.position += 1;
        }
        if let Some((before, after)) = &command.metadata {
            self.comparison = if undo { before } else { after }.clone();
        }
        Some(command)
    }
}

impl Parameters {
    /// Runs a short non-audio editing operation. Host callbacks must happen after this returns.
    pub(crate) fn with_editing<T>(&self, operation: impl FnOnce(&mut EditingState, Snapshot) -> T) -> T {
        let mut editing = self.editing.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        let snapshot = self.snapshot();
        editing.synchronize(self.restore_generation(), snapshot);
        let result = operation(&mut editing, snapshot);
        drop(editing);
        result
    }
}

/// Reads a validated id from a detached snapshot without constructing a temporary live parameter allocation.
fn snapshot_value(snapshot: Snapshot, id: u32) -> f64 {
    match id {
        0 => snapshot.gain,
        1..=3 => *snapshot.crossovers.get((id - 1) as usize).expect("crossover"),
        4..=27 => {
            let index = (id - 4) as usize;
            let band = snapshot.bands.get(index / 6).expect("band");
            match index % 6 {
                0 => band.mid,
                1 => band.side,
                2 => band.width,
                3 => band.recover,
                4 => band.pan,
                _ => band.solo,
            }
        }
        28 => snapshot.bypass,
        29 => snapshot.delta,
        30 => snapshot.mono,
        _ => 0.0,
    }
}

/// Enumerates sound ids without making the serialized schema depend on editor widget order.
fn sound_ids() -> impl Iterator<Item = u32> {
    (0..PARAMETER_COUNT as u32).filter(|id| {
        *id != BYPASS_PARAMETER && *id != DELTA_PARAMETER && !(*id >= 4 && *id <= 27 && (*id - 4) % 6 == 5)
    })
}

/// Accepts only nonzero lowercase 128-bit identities, never filesystem paths.
pub fn valid_identity(identity: &str) -> bool {
    identity.len() == 32
        && identity
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
        && identity.bytes().any(|byte| byte != b'0')
}

/// Portable display names reject control characters and filesystem syntax on both supported platforms.
pub fn valid_name(name: &str) -> bool {
    let basename = name.split('.').next().unwrap_or_default().to_ascii_uppercase();
    let reserved = ["CON", "PRN", "AUX", "NUL"].contains(&basename.as_str())
        || basename
            .strip_prefix("COM")
            .or_else(|| basename.strip_prefix("LPT"))
            .is_some_and(|number| number.len() == 1 && matches!(number.as_bytes().first(), Some(b'1'..=b'9')));
    !reserved
        && !name.is_empty()
        && name.len() <= 120
        && name.trim() == name
        && !name
            .chars()
            .any(|character| character.is_control() || "<>:\"/\\|?*".contains(character))
        && !name.ends_with('.')
        && ![".", ".."].contains(&name)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sound_omits_audition_but_keeps_mono_and_all_imaging_controls() {
        let before = SoundSnapshot::capture(Snapshot::default());
        let mut snapshot = Snapshot {
            bypass: 1.0,
            delta: 1.0,
            ..Snapshot::default()
        };
        for band in &mut snapshot.bands {
            band.solo = 1.0;
        }
        assert_eq!(SoundSnapshot::capture(snapshot), before);
        snapshot.mono = 1.0;
        assert_ne!(SoundSnapshot::capture(snapshot), before);
        assert_eq!(sound_ids().count(), SOUND_PARAMETER_COUNT);
    }

    #[test]
    fn touched_history_is_nested_bounded_and_noops_keep_redo() {
        let mut snapshot = Snapshot::default();
        let mut state = EditingState::new(snapshot);
        state.begin();
        state.begin();
        state.touch(0, snapshot.gain);
        snapshot.gain = 0.25;
        state.end(snapshot);
        assert!(!state.can_undo());
        state.end(snapshot);
        assert!(state.can_undo());
        let undo = state.replay(true).expect("undo");
        assert_eq!(undo.changes, vec![(0, Snapshot::default().gain, 0.25)]);
        state.begin();
        state.end(snapshot);
        assert!(state.can_redo());
        state.synchronize(1, snapshot);
        assert!(!state.can_undo());
        assert!(!state.can_redo());
        for index in 0..200 {
            state.begin();
            state.touch(0, snapshot.gain);
            snapshot.gain = f64::from(index) / 200.0;
            state.end(snapshot);
        }
        assert_eq!(state.history.len(), HISTORY_CAPACITY);
    }

    #[test]
    fn comparisons_capture_automation_and_copy_is_undoable_metadata() {
        let mut snapshot = Snapshot::default();
        let mut state = EditingState::new(snapshot);
        snapshot.gain = 0.25;
        state.begin();
        let target = state.switch(1, snapshot).expect("switch B");
        assert_eq!(target, SoundSnapshot::capture(Snapshot::default()));
        state.end(snapshot);
        assert_eq!(state.comparison.active, 1);
        assert_eq!(state.comparison.slots[0], SoundSnapshot::capture(snapshot));
        state.replay(true).expect("undo switch");
        assert_eq!(state.comparison.active, 0);
        state.begin();
        state.copy(snapshot);
        state.end(snapshot);
        assert_eq!(state.comparison.slots[1], SoundSnapshot::capture(snapshot));
        state.replay(true).expect("undo copy");
        assert_eq!(state.comparison.slots[1], SoundSnapshot::capture(Snapshot::default()));
    }
}
