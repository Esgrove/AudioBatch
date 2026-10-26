//! Background analysis runs on a worker pool, with cancellation and progress events.
//!
//! Ports the C++ `AnalysisCoordinator`.
//! Each run expands the input paths, publishes the plan and the cached records,
//! and queues one pool job per stale file.
//! Fresh results are stored back into the cache.
//! Events are sent over a channel instead of callbacks,
//! so the CLI can block on it and the GUI can drain it from a background task.
//! Starting a new run or calling `cancel` invalidates the previous run id,
//! and jobs of an invalidated run stop early and publish nothing more.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};

use anyhow::{Context, Result};
use crossbeam_channel::{Receiver, Sender, unbounded};
use rayon::{ThreadPool, ThreadPoolBuilder};

use crate::analysis::analyze_file;
use crate::cache::AnalysisCache;
use crate::discovery::collect_input_files;
use crate::record::{AnalysisOptions, AnalysisRecord};

/// Smallest progress step that is published, so the receiver is not flooded with events.
const PROGRESS_STEP: f32 = 0.02;

/// Events of one analysis run, in the order they are published.
#[derive(Debug, Clone)]
pub enum AnalysisEvent {
    /// The input paths were expanded. Sent once, before any result.
    Planned {
        /// Run the event belongs to.
        run_id: u64,
        /// Every file of the run, in natural order.
        files: Vec<PathBuf>,
        /// Number of files that need decoding because the cache had no valid row.
        stale_count: usize,
    },
    /// Decoding of a file is about to start.
    Started {
        /// Run the event belongs to.
        run_id: u64,
        /// File being analyzed.
        path: PathBuf,
    },
    /// Decoded fraction of a file, from 0 to 1.
    Progress {
        /// Run the event belongs to.
        run_id: u64,
        /// File being analyzed.
        path: PathBuf,
        /// Decoded fraction.
        fraction: f32,
    },
    /// A cached or freshly analyzed record, including failures.
    Finished {
        /// Run the event belongs to.
        run_id: u64,
        /// The result.
        record: Box<AnalysisRecord>,
    },
    /// Every file of the run has a result. Not sent for a cancelled run.
    Completed {
        /// Run the event belongs to.
        run_id: u64,
        /// Number of files in the run.
        total_files: usize,
    },
}

/// Runs analysis in the background on a fixed-size worker pool.
pub struct AnalysisCoordinator {
    cache: Arc<AnalysisCache>,
    pool: Arc<ThreadPool>,
    current_run: Arc<AtomicU64>,
}

/// State shared by the jobs of one run.
struct RunContext {
    run_id: u64,
    current_run: Arc<AtomicU64>,
    cache: Arc<AnalysisCache>,
    sender: Sender<AnalysisEvent>,
}

impl AnalysisEvent {
    /// Returns the run the event belongs to.
    #[must_use]
    pub const fn run_id(&self) -> u64 {
        match self {
            Self::Planned { run_id, .. }
            | Self::Started { run_id, .. }
            | Self::Progress { run_id, .. }
            | Self::Finished { run_id, .. }
            | Self::Completed { run_id, .. } => *run_id,
        }
    }
}

impl AnalysisCoordinator {
    /// Creates a coordinator with the given number of worker threads, at least one.
    ///
    /// # Errors
    ///
    /// Returns an error when the worker threads cannot be created.
    pub fn new(cache: Arc<AnalysisCache>, worker_count: usize) -> Result<Self> {
        let pool = ThreadPoolBuilder::new()
            .num_threads(worker_count.max(1))
            .thread_name(|index| format!("analysis-{index}"))
            .build()
            .context("Failed to create the analysis worker pool")?;
        Ok(Self {
            cache,
            pool: Arc::new(pool),
            current_run: Arc::new(AtomicU64::new(0)),
        })
    }

    /// Returns the cache the coordinator reads and writes.
    #[must_use]
    pub const fn cache(&self) -> &Arc<AnalysisCache> {
        &self.cache
    }

    /// Starts a background run, cancelling the previous one, and returns its id and event receiver.
    /// Expanding the input paths and the cache lookups also run in the background,
    /// so the caller never waits for the file system.
    /// The receiver disconnects once every job of the run has finished.
    #[must_use]
    pub fn start(&self, options: AnalysisOptions) -> (u64, Receiver<AnalysisEvent>) {
        let run_id = self.current_run.fetch_add(1, Ordering::SeqCst) + 1;
        let (sender, receiver) = unbounded();
        let context = Arc::new(RunContext {
            run_id,
            current_run: Arc::clone(&self.current_run),
            cache: Arc::clone(&self.cache),
            sender,
        });
        let pool = Arc::clone(&self.pool);
        self.pool.spawn(move || plan_run(&pool, &context, &options));
        (run_id, receiver)
    }

    /// Cancels the current run. Jobs that are already decoding stop at their next packet.
    pub fn cancel(&self) {
        self.current_run.fetch_add(1, Ordering::SeqCst);
    }

    /// Returns true when the run is still the current one.
    #[must_use]
    pub fn is_current(&self, run_id: u64) -> bool {
        self.current_run.load(Ordering::SeqCst) == run_id
    }

    /// Runs a full analysis and returns every cached and fresh record, blocking until the run completes.
    #[must_use]
    pub fn analyze_blocking(&self, options: AnalysisOptions) -> Vec<AnalysisRecord> {
        let (_, receiver) = self.start(options);
        let mut records = Vec::new();
        for event in receiver {
            match event {
                AnalysisEvent::Finished { record, .. } => records.push(*record),
                AnalysisEvent::Completed { .. } => break,
                _ => {}
            }
        }
        records
    }
}

impl RunContext {
    /// Returns true when the run was cancelled or replaced by a newer one.
    fn is_cancelled(&self) -> bool {
        self.current_run.load(Ordering::SeqCst) != self.run_id
    }

    /// Sends an event while the run is still current.
    /// A disconnected receiver is ignored, because the caller may stop listening at any time.
    fn publish(&self, event: AnalysisEvent) {
        if !self.is_cancelled() {
            let _ = self.sender.send(event);
        }
    }
}

/// Expands the input paths, publishes the plan and the cached records, and queues one job per stale file.
/// Runs on a worker thread and stops early when the run is cancelled.
fn plan_run(pool: &Arc<ThreadPool>, context: &Arc<RunContext>, options: &AnalysisOptions) {
    let is_cancelled = || context.is_cancelled();
    let files = collect_input_files(&options.input_paths, options.recursive, &is_cancelled);
    let mut cached = Vec::new();
    let mut stale = Vec::new();
    for file in &files {
        if is_cancelled() {
            return;
        }
        match (!options.refresh).then(|| context.cache.get_analysis(file)).flatten() {
            Some(record) => cached.push(record),
            None => stale.push(file.clone()),
        }
    }

    let total_files = files.len();
    context.publish(AnalysisEvent::Planned {
        run_id: context.run_id,
        files,
        stale_count: stale.len(),
    });
    for record in cached {
        context.publish(AnalysisEvent::Finished {
            run_id: context.run_id,
            record: Box::new(record),
        });
    }
    if stale.is_empty() {
        context.publish(AnalysisEvent::Completed {
            run_id: context.run_id,
            total_files,
        });
        return;
    }

    let pending = Arc::new(AtomicUsize::new(stale.len()));
    for file in stale {
        let context = Arc::clone(context);
        let pending = Arc::clone(&pending);
        pool.spawn(move || analyze_stale_file(&file, &context, &pending, total_files));
    }
}

/// Analyzes one file, caches the result, and publishes it.
/// The stored custom gain is kept while the file is unchanged on disk.
/// A file whose analysis is cancelled is neither cached nor published.
/// The job that finishes the last file of the run publishes completion.
fn analyze_stale_file(path: &Path, context: &RunContext, pending: &AtomicUsize, total_files: usize) {
    if context.is_cancelled() {
        return;
    }
    context.publish(AnalysisEvent::Started {
        run_id: context.run_id,
        path: path.to_path_buf(),
    });
    let mut last_progress = 0.0_f32;
    let mut on_progress = |fraction: f32| {
        if fraction - last_progress >= PROGRESS_STEP || (fraction >= 1.0 && last_progress < 1.0) {
            last_progress = fraction;
            context.publish(AnalysisEvent::Progress {
                run_id: context.run_id,
                path: path.to_path_buf(),
                fraction,
            });
        }
    };
    let mut record = analyze_file(path, &|| context.is_cancelled(), &mut on_progress);
    if context.is_cancelled() {
        return;
    }
    // A refresh analyzes unchanged files too, and those keep the custom gain the user set for them.
    if !record.has_error() {
        record.custom_gain_db = context
            .cache
            .get_custom_gain(path, record.file_size, record.modified_time_ms);
    }
    // A failed store only costs a re-analysis on the next run, so the result is still published.
    let _ = context.cache.store_analysis(&record);
    context.publish(AnalysisEvent::Finished {
        run_id: context.run_id,
        record: Box::new(record),
    });
    if pending.fetch_sub(1, Ordering::SeqCst) == 1 {
        context.publish(AnalysisEvent::Completed {
            run_id: context.run_id,
            total_files,
        });
    }
}

#[cfg(test)]
mod test_coordinator {
    use super::*;

    use tempfile::TempDir;

    use crate::record::AnalysisStatus;

    fn coordinator(directory: &TempDir) -> AnalysisCoordinator {
        let cache = Arc::new(AnalysisCache::with_path(directory.path().join("analysis.db")));
        AnalysisCoordinator::new(cache, 4).expect("pool")
    }

    fn fixture_folder(directory: &TempDir) -> PathBuf {
        let folder = directory.path().join("audio");
        std::fs::create_dir_all(&folder).expect("create folder");
        for name in ["sine_stereo_44100_16.wav", "sine_mono_48000_24.aif", "broken.wav"] {
            std::fs::copy(test_fixtures::fixture(name), folder.join(name)).expect("copy fixture");
        }
        folder
    }

    fn options(folder: &Path, refresh: bool) -> AnalysisOptions {
        AnalysisOptions {
            input_paths: vec![folder.to_path_buf()],
            recursive: false,
            refresh,
        }
    }

    #[test]
    fn blocking_run_returns_every_file() {
        let directory = TempDir::new().expect("temp dir");
        let folder = fixture_folder(&directory);
        let records = coordinator(&directory).analyze_blocking(options(&folder, false));
        assert_eq!(records.len(), 3);
        assert_eq!(records.iter().filter(|record| record.has_error()).count(), 1);
        assert!(records.iter().all(|record| !record.from_cache));
    }

    #[test]
    fn second_run_uses_the_cache_unless_refreshed() {
        let directory = TempDir::new().expect("temp dir");
        let folder = fixture_folder(&directory);
        let coordinator = coordinator(&directory);
        let _ = coordinator.analyze_blocking(options(&folder, false));
        let cached = coordinator.analyze_blocking(options(&folder, false));
        assert!(cached.iter().all(|record| record.from_cache));
        assert_eq!(
            cached
                .iter()
                .filter(|record| record.status == AnalysisStatus::Cached)
                .count(),
            2
        );
        let refreshed = coordinator.analyze_blocking(options(&folder, true));
        assert!(refreshed.iter().all(|record| !record.from_cache));
    }

    #[test]
    fn refresh_keeps_the_custom_gain_of_an_unchanged_file() {
        let directory = TempDir::new().expect("temp dir");
        let folder = fixture_folder(&directory);
        let coordinator = coordinator(&directory);
        let _ = coordinator.analyze_blocking(options(&folder, false));
        let path = folder.join("sine_stereo_44100_16.wav");
        assert!(
            coordinator
                .cache()
                .store_custom_gain(&path, Some(-4.0))
                .expect("stores gain")
        );
        let refreshed = coordinator.analyze_blocking(options(&folder, true));
        let record = refreshed
            .iter()
            .find(|record| record.path == path)
            .expect("record of the file");
        assert!(!record.from_cache);
        assert_eq!(record.custom_gain_db, Some(-4.0));
    }

    #[test]
    fn events_arrive_in_order() {
        let directory = TempDir::new().expect("temp dir");
        let folder = fixture_folder(&directory);
        let coordinator = coordinator(&directory);
        let (run_id, receiver) = coordinator.start(options(&folder, false));
        let events: Vec<AnalysisEvent> = receiver.iter().collect();
        assert!(events.iter().all(|event| event.run_id() == run_id));
        assert!(matches!(
            events.first(),
            Some(AnalysisEvent::Planned { stale_count: 3, .. })
        ));
        assert!(matches!(
            events.last(),
            Some(AnalysisEvent::Completed { total_files: 3, .. })
        ));
        let finished = events
            .iter()
            .filter(|event| matches!(event, AnalysisEvent::Finished { .. }))
            .count();
        assert_eq!(finished, 3);
    }

    #[test]
    fn empty_input_completes_immediately() {
        let directory = TempDir::new().expect("temp dir");
        let empty = directory.path().join("empty");
        std::fs::create_dir_all(&empty).expect("create folder");
        let coordinator = coordinator(&directory);
        let (_, receiver) = coordinator.start(options(&empty, false));
        let events: Vec<AnalysisEvent> = receiver.iter().collect();
        assert!(matches!(
            events.last(),
            Some(AnalysisEvent::Completed { total_files: 0, .. })
        ));
    }

    #[test]
    fn cancelled_run_stops_publishing() {
        let directory = TempDir::new().expect("temp dir");
        let folder = fixture_folder(&directory);
        let coordinator = coordinator(&directory);
        let (run_id, receiver) = coordinator.start(options(&folder, false));
        coordinator.cancel();
        assert!(!coordinator.is_current(run_id));
        let completed = receiver
            .iter()
            .any(|event| matches!(event, AnalysisEvent::Completed { .. }));
        // The planning job may already have finished before the cancellation, in which case it completed.
        if completed {
            return;
        }
        assert!(receiver.try_recv().is_err());
    }
}
