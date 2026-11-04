//! Background artifact progress as unique completed and total file counts.

use super::*;

/// Captures real identities so completion tests exercise ordinary source validation.
fn identity(path: &Path) -> EnrichmentIdentity {
    let (file_size, modified_time_ms) = file_identity(path).expect("source identity");
    EnrichmentIdentity {
        path: path.to_path_buf(),
        file_size,
        modified_time_ms,
    }
}

/// Candidate counts stay unique across queued, active, completed, and selected retry states.
#[test]
fn progress_counts_unique_candidates_not_selected_retries() {
    let first = tempfile::NamedTempFile::new().expect("first source");
    let second = tempfile::NamedTempFile::new().expect("second source");
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.enqueue(identity(first.path()), false);
    queue.enqueue(identity(first.path()), true);
    queue.enqueue(identity(second.path()), false);
    assert_eq!(queue.progress(), Some((0, 2)));
    let active = queue.take_next(Some(first.path()), None, true).expect("first repair");
    assert_eq!(queue.progress(), Some((0, 2)));
    queue.complete(&active);
    assert_eq!(queue.progress(), Some((1, 2)));
    queue.enqueue(identity(first.path()), true);
    assert_eq!(queue.progress(), Some((1, 2)));
    let retry = queue
        .take_next(Some(first.path()), None, true)
        .expect("selected lookup");
    assert!(retry.artifacts_attempted);
    queue.complete(&retry);
    assert_eq!(queue.progress(), Some((1, 2)));
    let remaining = queue.take_next(None, None, true).expect("remaining repair");
    queue.complete(&remaining);
    assert_eq!(queue.progress(), None);
    queue.enqueue(identity(first.path()), false);
    assert!(queue.pending.is_empty());
}

/// Cache-only foreground work never advances completed repair, and cancellation never advances it early.
#[test]
fn foreground_and_cancelled_readers_do_not_report_completed_repairs() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let mut queue = EnrichmentQueue::default();
    queue.enqueue(identity(file.path()), true);
    let cached = queue
        .take_next(Some(file.path()), None, false)
        .expect("foreground cache");
    assert_eq!(queue.progress(), None);
    queue.complete(&cached);
    queue.gate_open = true;
    assert_eq!(queue.progress(), Some((0, 1)));
    let repair = queue.take_next(Some(file.path()), None, true).expect("repair");
    queue.pause();
    assert_eq!(queue.progress(), None);
    queue.gate_open = true;
    assert_eq!(queue.progress(), Some((0, 1)));
    assert!(queue.take_next(Some(file.path()), None, true).is_none());
    queue.complete(&repair);
    assert_eq!(queue.progress(), Some((0, 1)));
    assert!(
        !queue
            .take_next(Some(file.path()), None, true)
            .expect("retry")
            .artifacts_attempted
    );
}

/// Invalidating a load clears totals but does not free an older still-running reader's slot.
#[test]
fn invalidated_completion_cannot_change_new_generation_progress() {
    let first = tempfile::NamedTempFile::new().expect("first source");
    let second = tempfile::NamedTempFile::new().expect("second source");
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.enqueue(identity(first.path()), false);
    let old = queue.take_next(None, None, true).expect("old repair");
    queue.invalidate();
    assert_eq!(queue.progress(), None);
    queue.gate_open = true;
    queue.enqueue(identity(second.path()), false);
    assert_eq!(queue.progress(), Some((0, 1)));
    assert!(queue.take_next(None, None, true).is_none());
    queue.complete(&old);
    assert_eq!(queue.progress(), Some((0, 1)));
    let current = queue.take_next(None, None, true).expect("new repair");
    queue.complete(&current);
    assert_eq!(queue.progress(), None);
}

/// Replaced sources leave candidate accounting rather than keeping an unreachable repair pending forever.
#[test]
fn stale_source_is_removed_from_progress_when_its_reader_returns() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let source = identity(file.path());
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.enqueue(source, false);
    let active = queue.take_next(None, None, true).expect("repair");
    std::fs::write(file.path(), b"replacement source").expect("replace source");
    queue.complete(&active);
    assert!(queue.candidates.is_empty());
    assert!(queue.pending.is_empty());
    assert_eq!(queue.progress(), None);
}
