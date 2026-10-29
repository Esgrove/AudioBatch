//! Background normalization runs on a worker pool, with cancellation and progress events.
//!
//! The GUI and the CLI use it. Each run normalizes a fixed list of analyzed records, one pool job per file,
//! and keeps the cache in step with the files on disk:
//! a converted source leaves the cache, and the analysis of each normalized output is stored.
//! A file that writes the same output as another file in the run fails without being touched.
//! Starting a new run or calling `cancel` invalidates the previous run id.
//! Jobs of an invalidated run stop early, and only report files whose output was already committed,
//! because the receiver must still learn that those sources were replaced.

use std::collections::HashSet;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};

use crossbeam_channel::{Receiver, Sender, unbounded};
use rayon::{ThreadPool, ThreadPoolBuilder};

use crate::cache::AnalysisCache;
use crate::normalize::{
    NormalizationOptions, batch_output_conflict_message, find_batch_output_conflicts, normalize_file,
};
use crate::operation::{OperationResult, update_cache};
use crate::record::AnalysisRecord;

/// Smallest progress step that is published, so the receiver is not flooded with events.
const PROGRESS_STEP: f32 = 0.02;

/// A failure to create the normalization worker threads.
#[derive(Debug, thiserror::Error)]
#[error("Failed to create the normalization worker pool")]
pub struct NormalizeCoordinatorError(#[source] pub rayon::ThreadPoolBuildError);

/// Events of one normalization run, in the order they are published for each file.
#[derive(Debug, Clone)]
pub enum NormalizeEvent {
    /// A worker started normalizing a file.
    Started {
        /// Run the event belongs to.
        run_id: u64,
        /// Source file being normalized.
        path: PathBuf,
    },
    /// Progress of a file through any source analysis, the render, and the output analysis, from 0 to 1.
    Progress {
        /// Run the event belongs to.
        run_id: u64,
        /// Source file being normalized.
        path: PathBuf,
        /// Normalized fraction.
        fraction: f32,
    },
    /// The result of one file, including failures.
    Finished {
        /// Run the event belongs to.
        run_id: u64,
        /// The result.
        result: Box<OperationResult>,
    },
    /// Every file of the run has a result. Not sent for a cancelled run.
    Completed {
        /// Run the event belongs to.
        run_id: u64,
        /// Number of files in the run.
        total_files: usize,
    },
}

/// Runs normalization in the background on a fixed-size worker pool.
pub struct NormalizeCoordinator {
    cache: Arc<AnalysisCache>,
    pool: ThreadPool,
    current_run: Arc<AtomicU64>,
}

/// State shared by the jobs of one run.
struct RunContext {
    run_id: u64,
    current_run: Arc<AtomicU64>,
    cache: Arc<AnalysisCache>,
    options: NormalizationOptions,
    sender: Sender<NormalizeEvent>,
    pending: AtomicUsize,
    total_files: usize,
}

impl NormalizeEvent {
    /// Returns the run the event belongs to.
    #[must_use]
    pub const fn run_id(&self) -> u64 {
        match self {
            Self::Started { run_id, .. }
            | Self::Progress { run_id, .. }
            | Self::Finished { run_id, .. }
            | Self::Completed { run_id, .. } => *run_id,
        }
    }
}

impl NormalizeCoordinator {
    /// Creates a coordinator with the given number of worker threads, at least one,
    /// that keeps the given analysis cache up to date.
    ///
    /// # Errors
    ///
    /// Returns an error when the worker threads cannot be created.
    pub fn new(cache: Arc<AnalysisCache>, worker_count: usize) -> Result<Self, NormalizeCoordinatorError> {
        let pool = ThreadPoolBuilder::new()
            .num_threads(worker_count.max(1))
            .thread_name(|index| format!("normalize-{index}"))
            .build()
            .map_err(NormalizeCoordinatorError)?;
        Ok(Self {
            cache,
            pool,
            current_run: Arc::new(AtomicU64::new(0)),
        })
    }

    /// Starts normalizing the records in the background, cancelling the previous run,
    /// and returns the run id and its event receiver.
    /// The records must be analyzed, and the receiver disconnects once every job of the run has finished.
    /// A file listed more than once is normalized once.
    #[must_use]
    pub fn start(
        &self,
        mut records: Vec<AnalysisRecord>,
        options: NormalizationOptions,
    ) -> (u64, Receiver<NormalizeEvent>) {
        let mut seen = HashSet::new();
        records.retain(|record| seen.insert(record.path.clone()));
        let run_id = self.current_run.fetch_add(1, Ordering::SeqCst) + 1;
        let (sender, receiver) = unbounded();
        let conflicts = find_batch_output_conflicts(&records);
        let context = Arc::new(RunContext {
            run_id,
            current_run: Arc::clone(&self.current_run),
            cache: Arc::clone(&self.cache),
            options,
            sender,
            pending: AtomicUsize::new(records.len()),
            total_files: records.len(),
        });
        if records.is_empty() {
            context.publish(NormalizeEvent::Completed { run_id, total_files: 0 });
        }
        for record in records {
            let context = Arc::clone(&context);
            let conflicting = conflicts.contains(&record.path);
            self.pool.spawn(move || normalize_job(&record, conflicting, &context));
        }
        (run_id, receiver)
    }

    /// Cancels the current run. Files being rendered stop at their next block and keep their originals.
    pub fn cancel(&self) {
        self.current_run.fetch_add(1, Ordering::SeqCst);
    }

    /// Returns true when the run is still the current one.
    #[must_use]
    pub fn is_current(&self, run_id: u64) -> bool {
        self.current_run.load(Ordering::SeqCst) == run_id
    }
}

impl RunContext {
    /// Returns true when the run was cancelled or replaced by a newer one.
    fn is_cancelled(&self) -> bool {
        self.current_run.load(Ordering::SeqCst) != self.run_id
    }

    /// Sends an event while the run is still current.
    /// A disconnected receiver is ignored, because the caller may stop listening at any time.
    fn publish(&self, event: NormalizeEvent) {
        if !self.is_cancelled() {
            let _ = self.sender.send(event);
        }
    }

    /// Counts a file as done, and publishes completion when it was the last file of the run.
    fn finish_file(&self) {
        if self.pending.fetch_sub(1, Ordering::SeqCst) == 1 {
            self.publish(NormalizeEvent::Completed {
                run_id: self.run_id,
                total_files: self.total_files,
            });
        }
    }
}

/// Normalizes one file, updates the cache, and publishes the result.
/// A file that lost a batch output conflict fails without being touched.
fn normalize_job(record: &AnalysisRecord, conflicting: bool, context: &RunContext) {
    if context.is_cancelled() {
        return;
    }
    if conflicting {
        let result = OperationResult::failure(&record.path, batch_output_conflict_message(&record.path));
        context.publish(NormalizeEvent::Finished {
            run_id: context.run_id,
            result: Box::new(result),
        });
        context.finish_file();
        return;
    }

    context.publish(NormalizeEvent::Started {
        run_id: context.run_id,
        path: record.path.clone(),
    });
    let mut last_progress = 0.0_f32;
    let mut on_progress = |fraction: f32| {
        if fraction - last_progress >= PROGRESS_STEP || (fraction >= 1.0 && last_progress < 1.0) {
            last_progress = fraction;
            context.publish(NormalizeEvent::Progress {
                run_id: context.run_id,
                path: record.path.clone(),
                fraction,
            });
        }
    };
    // A panic in a decoder on a malformed file fails only this file,
    // since a panic in a pool job would otherwise abort the whole process.
    let result = catch_unwind(AssertUnwindSafe(|| {
        normalize_file(record, &context.options, &|| context.is_cancelled(), &mut on_progress)
    }))
    .unwrap_or_else(|_| OperationResult::failure(&record.path, "Normalization failed unexpectedly"));
    update_cache(&context.cache, record, &result);
    let committed = result.output().is_some();
    let finished = NormalizeEvent::Finished {
        run_id: context.run_id,
        result: Box::new(result),
    };
    if committed {
        let _ = context.sender.send(finished);
    } else {
        context.publish(finished);
    }
    context.finish_file();
}

#[cfg(test)]
mod test_normalize_coordinator {
    use std::fs;
    use std::path::Path;

    use tempfile::TempDir;

    use super::*;
    use crate::test_helpers::{block_pool, ready_fixture_copy};

    /// A coordinator with its cache and options whose disposer deletes files.
    struct Setup {
        coordinator: NormalizeCoordinator,
        cache: Arc<AnalysisCache>,
        options: NormalizationOptions,
    }

    /// Returns a coordinator whose cache lives in its own subfolder.
    fn setup(directory: &TempDir) -> Setup {
        let cache_folder = directory.path().join("cache");
        fs::create_dir_all(&cache_folder).expect("creates cache folder");
        let cache = Arc::new(AnalysisCache::with_path(cache_folder.join("analysis.db")));
        let coordinator = NormalizeCoordinator::new(Arc::clone(&cache), 2).expect("pool");
        let options = NormalizationOptions {
            dispose: Arc::new(|path: &Path| fs::remove_file(path).is_ok()),
            ..NormalizationOptions::default()
        };
        Setup {
            coordinator,
            cache,
            options,
        }
    }

    /// Collects every event of a run until it completes or the receiver disconnects.
    fn collect(receiver: &Receiver<NormalizeEvent>) -> Vec<NormalizeEvent> {
        let mut events = Vec::new();
        for event in receiver {
            let completed = matches!(event, NormalizeEvent::Completed { .. });
            events.push(event);
            if completed {
                break;
            }
        }
        events
    }

    /// Returns the results of the finished events.
    fn results(events: &[NormalizeEvent]) -> Vec<&OperationResult> {
        events
            .iter()
            .filter_map(|event| match event {
                NormalizeEvent::Finished { result, .. } => Some(result.as_ref()),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn normalizes_every_file_and_updates_the_cache() {
        let directory = TempDir::new().expect("temp dir");
        let first = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "first.wav");
        let second = ready_fixture_copy(&directory, "sine_mono_44100_16.wav", "second.wav");
        let setup = setup(&directory);
        setup.cache.store_analysis(&first).expect("stores");

        let (run_id, receiver) = setup.coordinator.start(vec![first.clone(), second], setup.options);
        let events = collect(&receiver);
        assert!(events.iter().all(|event| event.run_id() == run_id));
        assert!(matches!(
            events.last(),
            Some(NormalizeEvent::Completed { total_files: 2, .. })
        ));
        let results = results(&events);
        assert_eq!(results.len(), 2);
        assert!(results.iter().all(|result| result.error().is_none()));

        let output = directory.path().join("first.aif");
        assert!(output.is_file());
        assert!(!first.path.exists());
        assert!(!setup.cache.remove_analysis(&first.path).expect("removes"));
        assert!(setup.cache.get_analysis(&output).is_some());
    }

    #[test]
    fn each_file_starts_before_it_finishes() {
        let directory = TempDir::new().expect("temp dir");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let setup = setup(&directory);
        let (_, receiver) = setup.coordinator.start(vec![record], setup.options);
        let events = collect(&receiver);
        let started = events
            .iter()
            .position(|event| matches!(event, NormalizeEvent::Started { .. }));
        let finished = events
            .iter()
            .position(|event| matches!(event, NormalizeEvent::Finished { .. }));
        assert!(started.is_some_and(|started| finished.is_some_and(|finished| started < finished)));
        assert!(
            events
                .iter()
                .any(|event| matches!(event, NormalizeEvent::Progress { fraction, .. } if *fraction >= 1.0))
        );
    }

    #[test]
    fn conflicting_file_fails_without_being_touched() {
        let directory = TempDir::new().expect("temp dir");
        let wav = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let flac = ready_fixture_copy(&directory, "sine_stereo_44100_16.flac", "song.flac");
        let setup = setup(&directory);
        let (_, receiver) = setup.coordinator.start(vec![wav, flac], setup.options);
        let events = collect(&receiver);
        let failed: Vec<&OperationResult> = results(&events)
            .into_iter()
            .filter(|result| result.error().is_some())
            .collect();
        assert_eq!(failed.len(), 1);
        let loser = &failed[0].source;
        assert!(loser.is_file());
        assert!(
            !events
                .iter()
                .any(|event| matches!(event, NormalizeEvent::Started { path, .. } if path == loser))
        );
    }

    #[test]
    fn empty_run_completes_immediately() {
        let directory = TempDir::new().expect("temp dir");
        let setup = setup(&directory);
        let (_, receiver) = setup.coordinator.start(Vec::new(), setup.options);
        let events = collect(&receiver);
        assert!(matches!(
            events.as_slice(),
            [NormalizeEvent::Completed { total_files: 0, .. }]
        ));
    }

    #[test]
    fn run_cancelled_before_it_starts_leaves_every_file() {
        let directory = TempDir::new().expect("temp dir");
        let first = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "first.wav");
        let second = ready_fixture_copy(&directory, "sine_mono_44100_16.wav", "second.wav");
        let setup = setup(&directory);
        let release = block_pool(&setup.coordinator.pool);
        let (run_id, receiver) = setup
            .coordinator
            .start(vec![first.clone(), second.clone()], setup.options);
        setup.coordinator.cancel();
        drop(release);
        assert!(!setup.coordinator.is_current(run_id));
        let events: Vec<NormalizeEvent> = receiver.iter().collect();
        assert!(events.is_empty(), "{events:?}");
        assert!(first.path.is_file() && second.path.is_file());
        assert!(!directory.path().join("first.aif").exists());
        assert!(!directory.path().join("second.aif").exists());
    }

    #[test]
    fn cancel_during_a_commit_reports_the_committed_file_and_skips_the_rest() {
        let directory = TempDir::new().expect("temp dir");
        let first = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "first.wav");
        let second = ready_fixture_copy(&directory, "sine_mono_44100_16.wav", "second.wav");
        let cache_folder = directory.path().join("cache");
        fs::create_dir_all(&cache_folder).expect("creates cache folder");
        let cache = Arc::new(AnalysisCache::with_path(cache_folder.join("analysis.db")));
        let coordinator = NormalizeCoordinator::new(cache, 1).expect("pool");
        let current_run = Arc::clone(&coordinator.current_run);
        // The disposer runs once the first output is in place, so the cancel lands between the two files.
        let options = NormalizationOptions {
            dispose: Arc::new(move |path: &Path| {
                current_run.fetch_add(1, Ordering::SeqCst);
                fs::remove_file(path).is_ok()
            }),
            ..NormalizationOptions::default()
        };

        let (run_id, receiver) = coordinator.start(vec![first.clone(), second.clone()], options);
        let events: Vec<NormalizeEvent> = receiver.iter().collect();

        assert!(!coordinator.is_current(run_id));
        assert!(
            !events
                .iter()
                .any(|event| matches!(event, NormalizeEvent::Completed { .. }))
        );
        let results = results(&events);
        assert_eq!(results.len(), 1, "{events:?}");
        let committed = results.first().expect("one result");
        assert_eq!(committed.source, first.path);
        assert_eq!(
            committed.output().map(|output| output.path.clone()),
            Some(directory.path().join("first.aif"))
        );
        assert!(!first.path.exists());
        assert!(second.path.is_file());
        assert!(!directory.path().join("second.aif").exists());
        assert!(
            !events
                .iter()
                .any(|event| matches!(event, NormalizeEvent::Started { path, .. } if *path == second.path))
        );
    }
}
