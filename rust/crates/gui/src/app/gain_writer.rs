//! Ordered explicit gain persistence, isolated from GUI updates and audio callbacks.
//!
//! One thread owns SQLite writes, and pending gestures coalesce to one edit per source path.
//! The queue is bounded by explicitly edited loaded sources rather than the number of input events.
//! Shutdown drains pending edits without requiring any message-thread delivery.

use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

use audiobatch_core::{AnalysisCache, AnalysisRecord};

/// Captured source identity and nullable explicit gain, without histories or waveform ownership.
#[derive(Clone)]
#[non_exhaustive]
pub struct GainWrite {
    record: AnalysisRecord,
}

/// Coalesced FIFO paths and their latest explicit edit.
#[derive(Default)]
struct PendingWrites {
    order: VecDeque<PathBuf>,
    edits: HashMap<PathBuf, GainWrite>,
    closing: bool,
}

/// GUI-owned writer whose thread never waits for the GUI to adopt a completion.
pub(super) struct GainWriter {
    pending: Arc<(Mutex<PendingWrites>, Condvar)>,
    thread: Option<JoinHandle<()>>,
}

impl GainWrite {
    /// Copies only fields needed by identity-conditional persistence.
    pub fn new(record: &AnalysisRecord) -> Self {
        Self {
            record: AnalysisRecord {
                path: record.path.clone(),
                file_size: record.file_size,
                modified_time_ms: record.modified_time_ms,
                custom_gain_db: record.custom_gain_db,
                ..AnalysisRecord::default()
            },
        }
    }
}

impl GainWriter {
    /// Starts one serial writer, preserving explicit clears as real edits.
    pub(super) fn new(cache: Arc<AnalysisCache>) -> Self {
        Self::with_write(move |edit| {
            let record = &edit.record;
            if let Err(error) = cache.store_custom_gain_for_record(record, record.custom_gain_db) {
                tracing::warn!("Could not store custom gain of {}: {error}", record.path.display());
            }
        })
    }

    /// Runs a serial persistence sink without holding the admission mutex during blocking writes.
    fn with_write(mut write: impl FnMut(GainWrite) + Send + 'static) -> Self {
        let pending = Arc::new((Mutex::new(PendingWrites::default()), Condvar::new()));
        let worker_pending = Arc::clone(&pending);
        let thread = thread::spawn(move || {
            let (mutex, wake) = &*worker_pending;
            loop {
                let edit = {
                    let Ok(mut pending) = mutex.lock() else { return };
                    while pending.order.is_empty() && !pending.closing {
                        let Ok(next) = wake.wait(pending) else { return };
                        pending = next;
                    }
                    let Some(path) = pending.order.pop_front() else { return };
                    pending.edits.remove(&path)
                };
                if let Some(edit) = edit {
                    write(edit);
                }
            }
        });
        Self {
            pending,
            thread: Some(thread),
        }
    }

    /// Enqueues immediately without waiting for SQLite or a full work channel.
    pub(super) fn enqueue(&self, edit: GainWrite) {
        let (mutex, wake) = &*self.pending;
        if let Ok(mut pending) = mutex.lock() {
            if pending.closing {
                return;
            }
            let path = edit.record.path.clone();
            if !pending.edits.contains_key(&path) {
                pending.order.push_back(path.clone());
            }
            pending.edits.insert(path, edit);
            wake.notify_one();
        }
    }

    /// Stops admission and returns the draining thread for an asynchronous quit wait.
    pub(super) fn finish(&mut self) -> Option<JoinHandle<()>> {
        let (mutex, wake) = &*self.pending;
        if let Ok(mut pending) = mutex.lock() {
            pending.closing = true;
            wake.notify_one();
        }
        self.thread.take()
    }
}

impl Drop for GainWriter {
    fn drop(&mut self) {
        // Normal application quit awaits this thread off the message thread.
        // Abnormal entity release still wakes it, without a GUI-thread database wait.
        drop(self.finish());
    }
}

#[cfg(test)]
mod test_gain_coalescing {
    use std::sync::mpsc;
    use std::time::Duration;

    use audiobatch_core::waveform::WaveformBuilder;

    use super::*;

    #[test]
    fn blocked_persistence_does_not_block_admission_and_shutdown_drains_in_order() {
        let (entered_sender, entered) = mpsc::channel();
        let (release, released) = mpsc::channel();
        let (stored, results) = mpsc::channel();
        let mut first = true;
        let mut writer = GainWriter::with_write(move |edit| {
            if first {
                first = false;
                entered_sender.send(()).expect("worker entered");
                released
                    .recv_timeout(Duration::from_secs(1))
                    .expect("release blocked writer");
            }
            stored
                .send((edit.record.path, edit.record.custom_gain_db))
                .expect("stored edit");
        });
        let mut record = AnalysisRecord {
            path: "active".into(),
            custom_gain_db: Some(1.0),
            ..AnalysisRecord::default()
        };
        writer.enqueue(GainWrite::new(&record));
        entered
            .recv_timeout(Duration::from_secs(1))
            .expect("worker blocks in sink");
        // Admission succeeds while the persistence sink still waits, with no timer-based assertion.
        record.path = "pending".into();
        writer.enqueue(GainWrite::new(&record));
        record.custom_gain_db = None;
        writer.enqueue(GainWrite::new(&record));
        let worker = writer.finish().expect("draining worker");
        record.path = "after-finish".into();
        writer.enqueue(GainWrite::new(&record));
        release.send(()).expect("release worker");
        worker.join().expect("writer exits without UI delivery");
        let results: Vec<_> = results.try_iter().collect();
        assert_eq!(
            results,
            [(PathBuf::from("active"), Some(1.0)), (PathBuf::from("pending"), None)]
        );
    }

    #[test]
    fn queued_gain_capture_does_not_retain_native_artifacts() {
        let mut builder = WaveformBuilder::new(1, Some(64));
        builder.push(&[&[0.25; 64]]);
        let waveform = builder.finish(48_000, 64);
        let weak = Arc::downgrade(&waveform);
        let record = AnalysisRecord {
            path: "source".into(),
            waveform: Some(waveform),
            ..AnalysisRecord::default()
        };
        let write = GainWrite::new(&record);
        drop(record);
        assert!(weak.upgrade().is_none());
        assert!(write.record.waveform.is_none());
        assert!(write.record.short_term_loudness.is_none());
        assert!(write.record.overloads.is_none());
    }

    #[test]
    fn latest_nullable_edit_keeps_fifo_order() {
        let writer = GainWriter {
            pending: Arc::new((Mutex::new(PendingWrites::default()), Condvar::new())),
            thread: None,
        };
        let mut record = AnalysisRecord {
            path: "a".into(),
            custom_gain_db: Some(2.0),
            ..AnalysisRecord::default()
        };
        writer.enqueue(GainWrite::new(&record));
        record.path = "b".into();
        writer.enqueue(GainWrite::new(&record));
        record.path = "a".into();
        record.custom_gain_db = None;
        writer.enqueue(GainWrite::new(&record));
        let pending = writer.pending.0.lock().expect("queue");
        assert_eq!(pending.order, [PathBuf::from("a"), PathBuf::from("b")]);
        assert_eq!(pending.edits.len(), 2);
        assert!(pending.edits[&PathBuf::from("a")].record.custom_gain_db.is_none());
        drop(pending);
    }
}
