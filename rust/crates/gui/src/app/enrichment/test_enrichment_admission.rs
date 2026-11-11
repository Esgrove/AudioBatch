//! Single-reader admission, selected-file priority, foreground yielding, and cancellation.

use super::*;

/// Creates a candidate without filesystem or GUI dependencies.
fn identity(path: &str) -> EnrichmentIdentity {
    EnrichmentIdentity {
        path: path.into(),
        file_size: 1,
        modified_time_ms: 2,
    }
}

/// Requires completed-load admission and an empty active slot so background repair stays deferred and serial.
#[test]
fn completion_gate_and_one_active_reader_are_required() {
    let mut queue = EnrichmentQueue::default();
    queue.enqueue(identity("a"), false);
    assert!(queue.take_next(None, None, true).is_none());
    queue.gate_open = true;
    assert!(queue.take_next(None, None, true).is_some());
    queue.enqueue(identity("b"), false);
    assert!(queue.take_next(None, None, true).is_none());
}

/// Allows the selected preview to reuse its cache during a foreground run while reserving repair for completion.
#[test]
fn foreground_run_allows_selected_cache_reads_but_defers_repairs() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let audiobatch_core::record::FileIdentity {
        file_size,
        modified_time_ms,
    } = file_identity(file.path()).expect("source identity");
    let selected = EnrichmentIdentity {
        path: file.path().to_path_buf(),
        file_size,
        modified_time_ms,
    };
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.enqueue(identity("background"), false);
    queue.enqueue(selected.clone(), true);
    let active = queue
        .take_next(Some(file.path()), None, false)
        .expect("selected cached artifacts remain available");
    assert_eq!(active.identity, selected);
    assert_eq!(active.mode, EnrichmentMode::CacheOnly);
    queue.complete(&active);
    assert!(queue.take_next(Some(file.path()), None, false).is_none());
    let repair = queue
        .take_next(Some(file.path()), None, true)
        .expect("foreground completion resumes repairs");
    assert_eq!(repair.mode, EnrichmentMode::Repair);
}

/// Promotes selected work ahead of background candidates without duplicating source identities in the queue.
#[test]
fn selection_promotes_and_deduplicates_candidates() {
    let mut queue = EnrichmentQueue::default();
    queue.enqueue(identity("a"), false);
    queue.enqueue(identity("b"), false);
    queue.enqueue(identity("b"), true);
    queue.enqueue(identity("a"), false);
    assert_eq!(queue.pending.len(), 2);
    queue.gate_open = true;
    assert_eq!(
        queue
            .take_next(Some(Path::new("b")), None, true)
            .expect("selected first")
            .identity,
        identity("b")
    );
}

/// Retains the cancelled reader's slot until completion so reopening the gate cannot create concurrent repairs.
#[test]
fn cancellation_does_not_release_a_blocking_slot() {
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.enqueue(identity("a"), false);
    let active = queue.take_next(None, None, true).expect("active");
    queue.pause();
    assert!(!active.is_running());
    queue.gate_open = true;
    queue.enqueue(identity("b"), true);
    assert!(queue.take_next(None, None, true).is_none());
    queue.complete(&active);
    assert_eq!(
        queue.take_next(None, None, true).expect("drained").identity,
        identity("b")
    );
}

/// Avoids background reads for terminal Serato outcomes while still reading selected display tags for cue data.
#[test]
fn valid_negative_serato_outcomes_skip_background_metadata_but_not_selected_cues() {
    for state in [
        SeratoOverviewState::Absent,
        SeratoOverviewState::Unsupported,
        SeratoOverviewState::Invalid,
    ] {
        assert!(!needs_display_tags(Some(&state), false));
        assert!(needs_display_tags(Some(&state), true));
    }
    assert!(needs_display_tags(None, false));
}

/// Prevents an invalidated load from marking new work complete or releasing its reader slot before the drain.
#[test]
fn load_invalidation_keeps_the_active_slot_and_rejects_the_old_generation() {
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.enqueue(identity("a"), false);
    let active = queue.take_next(None, None, true).expect("active");
    queue.invalidate();
    queue.gate_open = true;
    queue.enqueue(identity("b"), true);
    assert!(queue.take_next(None, None, true).is_none());
    queue.complete(&active);
    assert!(queue.completed.is_empty());
    assert_eq!(
        queue.take_next(None, None, true).expect("new generation").identity,
        identity("b")
    );
}

/// Invalidates an earlier preview generation even for the same path and waits for its reader before restarting.
#[test]
fn selection_round_trip_rejects_old_preview_without_freeing_the_reader() {
    let first = tempfile::NamedTempFile::new().expect("first source");
    let second = tempfile::NamedTempFile::new().expect("second source");
    let first_identity = EnrichmentIdentity::from_record(&super::test_enrichment_parts::record(first.path()));
    let second_identity = EnrichmentIdentity::from_record(&super::test_enrichment_parts::record(second.path()));
    let mut source = super::super::preview::PreviewSource::default();
    source.begin(first.path());
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.select(first_identity.clone(), source.token().as_ref());
    let active = queue
        .take_next(Some(first.path()), source.token(), true)
        .expect("first selected reader");
    assert!(queue.accepts_preview(&active, &source));
    queue.deselect();
    source.begin(second.path());
    queue.select(second_identity, source.token().as_ref());
    queue.deselect();
    source.begin(first.path());
    queue.select(first_identity.clone(), source.token().as_ref());
    assert!(!active.is_running());
    assert!(!queue.accepts_preview(&active, &source));
    assert!(queue.take_next(Some(first.path()), source.token(), true).is_none());
    queue.complete(&active);
    assert!(queue.completed.is_empty());
    let current = queue
        .take_next(Some(first.path()), source.token(), true)
        .expect("unchanged source restarts only after the old reader drains");
    assert_eq!(current.identity, first_identity);
    assert!(!current.artifacts_attempted);
    assert!(queue.accepts_preview(&current, &source));
    assert!(!queue.accepts_preview(&active, &source));
}

/// Interrupts background work for selection but joins an already matching preview
/// so repeated requests do not restart it.
#[test]
fn selection_cancels_unselected_work_but_joins_the_current_preview() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let selected_identity = EnrichmentIdentity::from_record(&super::test_enrichment_parts::record(file.path()));
    let mut source = super::super::preview::PreviewSource::default();
    source.begin(file.path());
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.enqueue(identity("background"), false);
    let background = queue.take_next(None, None, true).expect("background reader");
    queue.select(selected_identity.clone(), source.token().as_ref());
    assert!(!background.is_running());
    assert!(queue.take_next(Some(file.path()), source.token(), true).is_none());
    queue.complete(&background);
    let selected = queue
        .take_next(Some(file.path()), source.token(), true)
        .expect("selected reader");
    queue.select(selected_identity, source.token().as_ref());
    assert!(selected.is_running());
    assert!(queue.pending.is_empty());
    assert!(queue.accepts_preview(&selected, &source));
}

/// Retains negative display-tag results within the fixed limit to avoid repeated reads without unbounded memory.
/// Load invalidation clears those results and closes admission so they cannot leak into a new load.
#[test]
fn terminal_negative_tags_are_retained_but_bounded() {
    let mut queue = EnrichmentQueue::default();
    for index in 0..20 {
        queue.remember_tags(identity(&index.to_string()), None);
    }
    assert_eq!(queue.tags.len(), TAG_RESULT_LIMIT);
    assert!(queue.tags.front().expect("newest").1.is_none());
    queue.invalidate();
    assert!(queue.tags.is_empty());
    assert!(!queue.gate_open);
}
