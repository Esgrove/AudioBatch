//! Cache-first admission: delivering existing parts before repair without immediate selected retries.

use super::*;
use crate::app::preview::PreviewSource;

/// Captures a source identity without decoding or opening a window.
fn identity(path: &Path) -> EnrichmentIdentity {
    let audiobatch_core::record::FileIdentity {
        file_size,
        modified_time_ms,
    } = file_identity(path).expect("source identity");
    EnrichmentIdentity {
        path: path.to_path_buf(),
        file_size,
        modified_time_ms,
    }
}

/// Restricts foreground admission to the selected cache lookup even when a prior repair gate remains open.
/// Background work and additional selected readers must wait for the occupied slot.
#[test]
fn foreground_admits_only_the_selected_cache_load_even_with_an_open_gate() {
    let first = tempfile::NamedTempFile::new().expect("background source");
    let selected = tempfile::NamedTempFile::new().expect("selected source");
    for gate_open in [false, true] {
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = gate_open;
        queue.enqueue(identity(first.path()), false);
        queue.enqueue(identity(selected.path()), true);
        let request = queue
            .take_next(Some(selected.path()), None, false)
            .expect("selected cache load");
        assert_eq!(request.mode, EnrichmentMode::CacheOnly);
        assert_eq!(request.identity, identity(selected.path()));
        assert!(request.selected.load(Ordering::Acquire));
        assert_eq!(queue.pending, [identity(first.path())]);
        assert!(queue.take_next(Some(selected.path()), None, false).is_none());
    }
}

/// Retains one deferred repair after a cache-only attempt without spinning selected retries during foreground work.
#[test]
fn cache_completion_defers_repair_without_an_immediate_selected_retry() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let identity = identity(file.path());
    let mut queue = EnrichmentQueue::default();
    queue.select(identity.clone(), None);
    let request = queue.take_next(Some(file.path()), None, true).expect("cache load");
    assert_eq!(request.mode, EnrichmentMode::CacheOnly);
    queue.complete(&request);
    assert_eq!(queue.selected_cache_attempt, Some(identity.clone()));
    assert!(queue.selected_attempt.is_none());
    assert!(queue.completed.is_empty());
    for _ in 0..3 {
        queue.select(identity.clone(), None);
        assert!(queue.take_next(Some(file.path()), None, true).is_none());
        assert_eq!(queue.pending.len(), 1);
        assert_eq!(queue.pending.front(), Some(&identity));
    }
    queue.gate_open = true;
    assert!(queue.take_next(Some(file.path()), None, false).is_none());
    let repair = queue.take_next(Some(file.path()), None, true).expect("deferred repair");
    assert_eq!(repair.mode, EnrichmentMode::Repair);
    assert_eq!(repair.identity, identity);
}

/// Keeps an admitted reader's cache-only mode fixed so opening repair admission cannot change its I/O contract.
#[test]
fn completion_reconciliation_does_not_upgrade_an_active_cache_only_reader() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let mut queue = EnrichmentQueue::default();
    queue.select(identity(file.path()), None);
    let request = queue.take_next(Some(file.path()), None, false).expect("cache load");
    queue.gate_open = true;
    queue.select(identity(file.path()), None);
    assert!(queue.pending.is_empty());
    assert!(queue.take_next(Some(file.path()), None, true).is_none());
    assert_eq!(request.mode, EnrichmentMode::CacheOnly);
    queue.complete(&request);
    assert_eq!(
        queue.take_next(Some(file.path()), None, true).expect("repair").mode,
        EnrichmentMode::Repair
    );
}

/// Holds admission through cancellation so changing selection cannot overlap two blocking cache readers.
#[test]
fn cancelled_cache_load_retains_the_slot_until_its_worker_returns() {
    let first = tempfile::NamedTempFile::new().expect("first source");
    let second = tempfile::NamedTempFile::new().expect("second source");
    let mut source = PreviewSource::default();
    source.begin(first.path());
    let mut queue = EnrichmentQueue::default();
    queue.select(identity(first.path()), source.token().as_ref());
    let request = queue
        .take_next(Some(first.path()), source.token(), false)
        .expect("first cache load");
    queue.deselect();
    source.begin(second.path());
    queue.select(identity(second.path()), source.token().as_ref());
    assert!(!request.is_running());
    assert!(queue.take_next(Some(second.path()), source.token(), false).is_none());
    queue.complete(&request);
    let next = queue
        .take_next(Some(second.path()), source.token(), false)
        .expect("second cache load");
    assert_eq!(next.identity, identity(second.path()));
    assert_eq!(next.mode, EnrichmentMode::CacheOnly);
}

/// Rejects a cache delivery from an earlier visit to the same path so a selection round trip cannot revive it.
#[test]
fn cache_load_round_trip_accepts_only_the_latest_preview_token() {
    let first = tempfile::NamedTempFile::new().expect("first source");
    let second = tempfile::NamedTempFile::new().expect("second source");
    let mut source = PreviewSource::default();
    source.begin(first.path());
    let mut queue = EnrichmentQueue::default();
    queue.select(identity(first.path()), source.token().as_ref());
    let old = queue
        .take_next(Some(first.path()), source.token(), false)
        .expect("first cache load");
    queue.deselect();
    source.begin(second.path());
    queue.select(identity(second.path()), source.token().as_ref());
    queue.deselect();
    source.begin(first.path());
    queue.select(identity(first.path()), source.token().as_ref());
    assert!(!queue.accepts_preview(&old, &source));
    assert!(queue.take_next(Some(first.path()), source.token(), false).is_none());
    queue.complete(&old);
    let current = queue
        .take_next(Some(first.path()), source.token(), false)
        .expect("latest cache load");
    assert!(queue.accepts_preview(&current, &source));
    assert!(!queue.accepts_preview(&old, &source));
    queue.select(identity(first.path()), source.token().as_ref());
    assert!(current.is_running());
}

/// Forgets old cache attempts on load invalidation while retaining admission until the obsolete reader drains.
#[test]
fn new_load_generation_discards_cache_attempts_but_not_active_admission() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let mut queue = EnrichmentQueue::default();
    queue.select(identity(file.path()), None);
    let old = queue.take_next(Some(file.path()), None, false).expect("cache load");
    queue.invalidate();
    queue.select(identity(file.path()), None);
    assert!(queue.take_next(Some(file.path()), None, false).is_none());
    queue.complete(&old);
    assert!(queue.selected_cache_attempt.is_none());
    assert!(queue.completed.is_empty());
    assert!(queue.take_next(Some(file.path()), None, false).is_some());
}
