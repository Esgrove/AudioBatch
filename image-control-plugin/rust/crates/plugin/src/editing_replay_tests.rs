//! Replay contracts exercise rejection, live automation, and comparison metadata through the editing owner.

use super::*;

/// Applies detached replay values without introducing live host machinery into history-owner tests.
fn apply(command: &Command, undo: bool, snapshot: &mut Snapshot) {
    for &(id, before, after) in &command.changes {
        let value = if undo { before } else { after };
        match id {
            0 => snapshot.gain = value,
            1..=3 => *snapshot.crossovers.get_mut((id - 1) as usize).expect("crossover") = value,
            4..=27 => {
                let index = (id - 4) as usize;
                let band = snapshot.bands.get_mut(index / 6).expect("band");
                match index % 6 {
                    0 => band.mid = value,
                    1 => band.side = value,
                    2 => band.width = value,
                    3 => band.recover = value,
                    4 => band.pan = value,
                    _ => band.solo = value,
                }
            }
            28 => snapshot.bypass = value,
            29 => snapshot.delta = value,
            30 => snapshot.mono = value,
            _ => unreachable!("validated parameter"),
        }
    }
}

/// Replays one accepted command with the same mutex-free apply boundary used by the editor.
fn replay(state: &mut EditingState, undo: bool, snapshot: &mut Snapshot) -> Command {
    let command = state.begin_replay(undo, *snapshot).expect("available replay");
    apply(&command, undo, snapshot);
    state.complete_replay(undo, *snapshot, true);
    command
}

/// Records an A/B switch with actual sound writes and a single chronological boundary.
fn switch(state: &mut EditingState, slot: usize, snapshot: &mut Snapshot) {
    state.begin();
    let target = state.switch(slot, *snapshot).expect("different slot");
    let changes = sound_ids()
        .zip(target.values)
        .map(|(id, target)| {
            let before = snapshot_value(*snapshot, id);
            state.touch(id, before);
            (id, before, target)
        })
        .collect();
    apply(
        &Command {
            changes,
            metadata: None,
        },
        false,
        snapshot,
    );
    state.end(*snapshot);
}

/// Supplies project provenance without depending on editor-only preset library APIs.
fn origin(id: &str, name: &str, snapshot: Snapshot) -> PresetOrigin {
    PresetOrigin {
        id: id.into(),
        name: name.into(),
        baseline: SoundSnapshot::capture(snapshot),
        effective: None,
    }
}

#[test]
fn rejected_undo_keeps_cursor_metadata_and_original_targets_for_retry() {
    let mut snapshot = Snapshot::default();
    let original = snapshot;
    let mut state = EditingState::new(snapshot);
    state.begin();
    state.touch(0, snapshot.gain);
    state.touch(30, snapshot.mono);
    snapshot.gain = 0.25;
    snapshot.mono = 1.0;
    state.end(snapshot);
    let comparison = state.comparison(snapshot);

    let command = state.begin_replay(true, snapshot).expect("undo");
    assert!(!state.can_undo());
    assert!(!state.can_redo());
    assert!(state.begin_replay(true, snapshot).is_none());
    assert!(state.begin_replay(false, snapshot).is_none());
    snapshot.gain = original.gain;
    state.complete_replay(true, snapshot, false);
    assert!(state.can_undo());
    assert!(!state.can_redo());
    assert_eq!(state.position, 1);
    assert_eq!(state.comparison, comparison);
    assert_eq!(state.history.first().expect("command").changes, command.changes);

    let retry = state.begin_replay(true, snapshot).expect("retry undo");
    assert_eq!(retry.changes, command.changes);
    apply(&retry, true, &mut snapshot);
    state.complete_replay(true, snapshot, true);
    assert!(!state.can_undo());
    assert!(state.can_redo());
    assert_eq!(SoundSnapshot::capture(snapshot), SoundSnapshot::capture(original));
    replay(&mut state, false, &mut snapshot);
    assert_eq!(snapshot.gain, 0.25);
    assert_eq!(snapshot.mono, 1.0);
}

#[test]
fn rejected_redo_restores_selection_and_preserves_intended_inactive_slot() {
    let mut snapshot = Snapshot::default();
    let mut state = EditingState::new(snapshot);
    snapshot.gain = 0.25;
    switch(&mut state, 1, &mut snapshot);
    replay(&mut state, true, &mut snapshot);
    let comparison = state.comparison(snapshot);
    let position = state.position;

    let command = state.begin_replay(false, snapshot).expect("redo switch");
    assert_eq!(state.comparison.active, 1);
    snapshot.gain = 0.125;
    state.complete_replay(false, snapshot, false);
    assert_eq!(state.position, position);
    assert_eq!(state.comparison, comparison);
    assert!(state.can_redo());
    let retry = state.begin_replay(false, snapshot).expect("retry redo");
    assert_eq!(retry.changes, command.changes);
    apply(&retry, false, &mut snapshot);
    state.complete_replay(false, snapshot, true);
    assert_eq!(state.comparison.active, 1);
    assert_eq!(state.comparison.slots.first(), comparison.slots.first());
    assert_eq!(snapshot.gain, Snapshot::default().gain);
}

#[test]
fn undo_and_redo_switch_preserve_outgoing_automation_and_provenance() {
    let mut snapshot = Snapshot::default();
    let mut state = EditingState::new(snapshot);
    let mut comparison = state.comparison(snapshot);
    *comparison.origins.first_mut().expect("A origin") =
        Some(origin("11111111111111111111111111111111", "A source", snapshot));
    *comparison.origins.get_mut(1).expect("B origin") =
        Some(origin("22222222222222222222222222222222", "B source", snapshot));
    state.restore_comparison(comparison.clone());
    snapshot.gain = 0.25;
    switch(&mut state, 1, &mut snapshot);

    snapshot.gain = 0.75;
    snapshot.mono = 1.0;
    let automated_b = SoundSnapshot::capture(snapshot);
    replay(&mut state, true, &mut snapshot);
    assert_eq!(snapshot.gain, 0.25);
    assert_eq!(snapshot.mono, 0.0);
    assert_eq!(state.comparison.slots.get(1), Some(&automated_b));
    assert_eq!(state.comparison.origins, comparison.origins);

    snapshot.gain = 0.125;
    let automated_a = SoundSnapshot::capture(snapshot);
    replay(&mut state, false, &mut snapshot);
    assert_eq!(SoundSnapshot::capture(snapshot), automated_b);
    assert_eq!(state.comparison.slots.first(), Some(&automated_a));
    assert_eq!(state.comparison.origins, comparison.origins);
    let saved = serde_json::to_vec(&state.comparison(snapshot)).expect("serialize comparison");
    let restored: ComparisonState = serde_json::from_slice(&saved).expect("restore comparison");
    assert!(restored.is_valid());
    assert_eq!(restored.slots.first(), Some(&automated_a));
    assert_eq!(restored.slots.get(1), Some(&automated_b));
    replay(&mut state, true, &mut snapshot);
    assert_eq!(SoundSnapshot::capture(snapshot), automated_a);
}

#[test]
fn switching_equal_sounds_still_recalls_later_automation_on_undo() {
    let mut snapshot = Snapshot::default();
    let mut state = EditingState::new(snapshot);
    switch(&mut state, 1, &mut snapshot);
    assert_eq!(state.history.first().expect("metadata switch").changes, []);
    snapshot.mono = 1.0;
    let automated_b = SoundSnapshot::capture(snapshot);
    replay(&mut state, true, &mut snapshot);
    assert_eq!(snapshot.mono, 0.0);
    replay(&mut state, false, &mut snapshot);
    assert_eq!(SoundSnapshot::capture(snapshot), automated_b);
}

#[test]
fn copy_undo_reverts_only_inactive_sound_and_preserves_active_automation() {
    let mut snapshot = Snapshot::default();
    let mut state = EditingState::new(snapshot);
    let mut comparison = state.comparison(snapshot);
    *comparison.origins.get_mut(1).expect("B origin") =
        Some(origin("22222222222222222222222222222222", "B source", snapshot));
    snapshot.gain = 0.25;
    *comparison.origins.first_mut().expect("A origin") =
        Some(origin("11111111111111111111111111111111", "A source", snapshot));
    state.restore_comparison(comparison.clone());
    state.begin();
    state.copy(snapshot);
    state.end(snapshot);
    let copied = SoundSnapshot::capture(snapshot);
    snapshot.gain = 0.75;
    let automated = SoundSnapshot::capture(snapshot);
    let command = replay(&mut state, true, &mut snapshot);
    assert_eq!(command.changes, []);
    assert_eq!(SoundSnapshot::capture(snapshot), automated);
    assert_eq!(state.comparison.slots.first(), Some(&automated));
    assert_eq!(state.comparison.slots.get(1), comparison.slots.get(1));
    assert_eq!(state.comparison.origins, comparison.origins);
    replay(&mut state, false, &mut snapshot);
    assert_eq!(state.comparison.slots.first(), Some(&automated));
    assert_eq!(state.comparison.slots.get(1), Some(&copied));
    assert_eq!(state.comparison.origins, comparison.origins);
}

#[test]
fn rejected_metadata_only_copy_keeps_redo_and_slot_contents() {
    let mut snapshot = Snapshot::default();
    let mut state = EditingState::new(snapshot);
    snapshot.gain = 0.25;
    state.begin();
    state.copy(snapshot);
    state.end(snapshot);
    let before = state.comparison(snapshot);
    state.begin_replay(true, snapshot).expect("undo copy");
    state.complete_replay(true, snapshot, false);
    assert_eq!(state.comparison, before);
    assert!(state.can_undo());
    assert!(!state.can_redo());
    replay(&mut state, true, &mut snapshot);
    assert_eq!(
        state.comparison.slots.get(1),
        Some(&SoundSnapshot::capture(Snapshot::default()))
    );
}

#[test]
fn accepted_readback_updates_only_replayed_ids_and_preserves_modified_status() {
    let initial = Snapshot::default();
    let mut snapshot = initial;
    let mut state = EditingState::new(snapshot);
    let before = state.comparison(snapshot);
    let mut target = snapshot;
    target.gain = 0.25;
    let mut after = before.clone();
    *after.slots.first_mut().expect("active sound") = SoundSnapshot::capture(target);
    *after.origins.first_mut().expect("origin") = Some(origin("11111111111111111111111111111111", "Source", target));
    state.history.push(Command {
        changes: vec![(0, initial.gain, target.gain)],
        metadata: Some((before, after)),
    });
    let requested = state.begin_replay(false, snapshot).expect("redo preset");
    apply(&requested, false, &mut snapshot);
    snapshot.gain = 0.251;
    snapshot.mono = 1.0;
    state.complete_replay(false, snapshot, true);
    let comparison = state.comparison(snapshot);
    let origin = comparison
        .origins
        .first()
        .and_then(Option::as_ref)
        .expect("source origin");
    let effective = origin.effective.expect("accepted baseline");
    target.gain = 0.251;
    assert_eq!(origin.baseline.values.first(), Some(&0.25));
    assert_eq!(effective, SoundSnapshot::capture(target));
    assert_ne!(comparison.slots.first(), Some(&effective));
    let stored = state.history.first().expect("command");
    assert_eq!(stored.changes, vec![(0, initial.gain, 0.251)]);
    assert_eq!(
        stored.metadata.as_ref().expect("metadata").1.slots.first(),
        Some(&effective)
    );
    replay(&mut state, true, &mut snapshot);
    assert_eq!(snapshot.mono, 1.0);
    let retry = state.begin_replay(false, snapshot).expect("redo accepted target");
    assert_eq!(retry.changes, vec![(0, initial.gain, 0.251)]);
    apply(&retry, false, &mut snapshot);
    state.complete_replay(false, snapshot, true);
    let comparison = state.comparison(snapshot);
    assert_ne!(
        comparison.slots.first(),
        comparison
            .origins
            .first()
            .and_then(Option::as_ref)
            .and_then(|origin| origin.effective)
            .as_ref(),
    );
}

#[test]
fn restore_invalidates_inflight_replay_and_stale_completion() {
    let mut snapshot = Snapshot::default();
    let mut state = EditingState::new(snapshot);
    snapshot.gain = 0.25;
    switch(&mut state, 1, &mut snapshot);
    state.begin_replay(true, snapshot).expect("undo switch");
    snapshot.gain = 0.125;
    state.synchronize(1, snapshot);
    let comparison = state.comparison(snapshot);
    state.complete_replay(true, snapshot, false);
    assert_eq!(state.comparison, comparison);
    assert!(!state.can_undo());
    assert!(!state.can_redo());
    assert!(state.rejected_replay.is_none());
}

#[test]
fn opposite_direction_completion_does_not_consume_the_ticket() {
    let mut snapshot = Snapshot::default();
    let mut state = EditingState::new(snapshot);
    snapshot.gain = 0.25;
    switch(&mut state, 1, &mut snapshot);
    let command = state.begin_replay(true, snapshot).expect("undo switch");
    state.complete_replay(false, snapshot, true);
    assert!(state.replay.is_some());
    apply(&command, true, &mut snapshot);
    state.complete_replay(true, snapshot, true);
    assert!(state.can_redo());
}
