//! Background plugin processing runs on a worker pool, with cancellation and progress events.
//!
//! The caller creates one processing chain per worker on the UI thread and passes them to `start`.
//! Each file job leases a free chain from the run's pool, processes the file, and returns the chain,
//! so a chain is only ever used by one worker at a time.
//! Runs keep the cache in step with the files on disk the same way normalization runs do,
//! and a file that writes the same output as another file in the run fails without being touched.
//! Starting a new run or calling `cancel` invalidates the previous run id.
//! Jobs of an invalidated run stop early, also while waiting for a chain or for the UI thread,
//! and only report files whose output was already committed.

use std::collections::HashSet;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex, PoisonError};
use std::time::Duration;

use anyhow::{Context as _, Result};
use audiobatch_core::cache::AnalysisCache;
use audiobatch_core::normalize::{batch_output_conflict_message, find_batch_output_conflicts};
use audiobatch_core::operation::{OperationResult, update_cache};
use audiobatch_core::record::AnalysisRecord;
use crossbeam_channel::{Receiver, Sender, unbounded};
use rayon::{ThreadPool, ThreadPoolBuilder};

use crate::processing::{ProcessingOptions, process_file};
use crate::processing_chain::ProcessingChain;

/// Smallest progress step that is published, so the receiver is not flooded with events.
const PROGRESS_STEP: f32 = 0.02;

/// How often a job waiting for a free chain checks for cancellation.
const CHAIN_WAIT_INTERVAL: Duration = Duration::from_millis(50);

/// Events of one processing run, in the order they are published for each file.
#[derive(Debug, Clone)]
pub enum ProcessingEvent {
    /// A worker has a chain and started processing a file.
    Started {
        /// Run the event belongs to.
        run_id: u64,
        /// Source file being processed.
        path: PathBuf,
    },
    /// Progress of a file through any source analysis, the render, the final write, and the output analysis.
    Progress {
        /// Run the event belongs to.
        run_id: u64,
        /// Source file being processed.
        path: PathBuf,
        /// Processed fraction, from 0 to 1.
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

/// Runs plugin processing in the background on a fixed-size worker pool.
pub struct ProcessingCoordinator {
    /// The cache kept in step with processed files.
    cache: Arc<AnalysisCache>,
    /// The worker threads.
    pool: ThreadPool,
    /// Number of worker threads.
    worker_count: usize,
    /// Id of the current run. Bumping it cancels the run.
    current_run: Arc<AtomicU64>,
}

/// The chains of one run that no worker is using.
struct ChainPool {
    /// The free chains.
    free: Mutex<Vec<ProcessingChain>>,
    /// Signalled when a chain goes back to the pool.
    returned: Condvar,
}

/// A chain leased from the pool, which goes back when the lease is dropped.
struct ChainLease<'a> {
    /// The pool the chain goes back to.
    pool: &'a ChainPool,
    /// The leased chain, taken out when the lease is dropped.
    chain: Option<ProcessingChain>,
}

/// State shared by the jobs of one run.
struct RunContext {
    /// Id of this run.
    run_id: u64,
    /// Id of the current run, which differs from `run_id` once this run is cancelled.
    current_run: Arc<AtomicU64>,
    /// The cache kept in step with processed files.
    cache: Arc<AnalysisCache>,
    /// Settings of the run.
    options: ProcessingOptions,
    /// The chains of the run.
    chains: ChainPool,
    /// Receives the events.
    sender: Sender<ProcessingEvent>,
    /// Files without a result yet.
    pending: AtomicUsize,
    /// Number of files in the run.
    total_files: usize,
}

impl ProcessingEvent {
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

impl ProcessingCoordinator {
    /// Creates a coordinator with the given number of worker threads, at least one,
    /// that keeps the given analysis cache up to date.
    ///
    /// # Errors
    ///
    /// Returns an error when the worker threads cannot be created.
    pub fn new(cache: Arc<AnalysisCache>, worker_count: usize) -> Result<Self> {
        let worker_count = worker_count.max(1);
        let pool = ThreadPoolBuilder::new()
            .num_threads(worker_count)
            .thread_name(|index| format!("plugin-processing-{index}"))
            .build()
            .context("Failed to create the plugin processing worker pool")?;
        Ok(Self {
            cache,
            pool,
            worker_count,
            current_run: Arc::new(AtomicU64::new(0)),
        })
    }

    /// Returns the number of worker threads, which is the most chains a run can use at once.
    #[must_use]
    pub const fn worker_count(&self) -> usize {
        self.worker_count
    }

    /// Returns how many chains a run of `file_count` files needs: one per worker, but no more than the files.
    #[must_use]
    pub fn chains_needed(&self, file_count: usize) -> usize {
        self.worker_count.min(file_count)
    }

    /// Starts processing the records in the background through the chains, cancelling the previous run,
    /// and returns the run id and its event receiver.
    /// The records must be analyzed, and the receiver disconnects once every job of the run has finished.
    /// A file listed more than once is processed once.
    /// Without any chain, every file fails.
    #[must_use]
    pub fn start(
        &self,
        mut records: Vec<AnalysisRecord>,
        options: ProcessingOptions,
        chains: Vec<ProcessingChain>,
    ) -> (u64, Receiver<ProcessingEvent>) {
        let mut seen = HashSet::new();
        records.retain(|record| seen.insert(record.path.clone()));
        let run_id = self.current_run.fetch_add(1, Ordering::SeqCst) + 1;
        let (sender, receiver) = unbounded();
        let conflicts = find_batch_output_conflicts(&records);
        let has_chains = !chains.is_empty();
        let context = Arc::new(RunContext {
            run_id,
            current_run: Arc::clone(&self.current_run),
            cache: Arc::clone(&self.cache),
            options,
            chains: ChainPool {
                free: Mutex::new(chains),
                returned: Condvar::new(),
            },
            sender,
            pending: AtomicUsize::new(records.len()),
            total_files: records.len(),
        });
        if records.is_empty() {
            context.publish(ProcessingEvent::Completed { run_id, total_files: 0 });
        }
        for record in records {
            let context = Arc::clone(&context);
            let refusal = if !has_chains {
                Some("Plugin chain instances are not available".to_string())
            } else if conflicts.contains(&record.path) {
                Some(batch_output_conflict_message(&record.path))
            } else {
                None
            };
            self.pool.spawn(move || processing_job(&record, refusal, &context));
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

impl ChainPool {
    /// Takes a free chain, waiting for one to come back while `keep_waiting` returns true.
    fn acquire(&self, keep_waiting: &dyn Fn() -> bool) -> Option<ChainLease<'_>> {
        let mut free = self.free.lock().unwrap_or_else(PoisonError::into_inner);
        loop {
            if !keep_waiting() {
                return None;
            }
            if let Some(chain) = free.pop() {
                return Some(ChainLease {
                    pool: self,
                    chain: Some(chain),
                });
            }
            free = self
                .returned
                .wait_timeout(free, CHAIN_WAIT_INTERVAL)
                .unwrap_or_else(PoisonError::into_inner)
                .0;
        }
    }
}

impl ChainLease<'_> {
    /// Returns the leased chain.
    const fn chain(&self) -> Option<&ProcessingChain> {
        self.chain.as_ref()
    }
}

impl Drop for ChainLease<'_> {
    fn drop(&mut self) {
        if let Some(chain) = self.chain.take() {
            self.pool
                .free
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .push(chain);
            self.pool.returned.notify_one();
        }
    }
}

impl RunContext {
    /// Returns true when the run was cancelled or replaced by a newer one.
    fn is_cancelled(&self) -> bool {
        self.current_run.load(Ordering::SeqCst) != self.run_id
    }

    /// Sends an event while the run is still current.
    /// A disconnected receiver is ignored, because the caller may stop listening at any time.
    fn publish(&self, event: ProcessingEvent) {
        if !self.is_cancelled() {
            let _ = self.sender.send(event);
        }
    }

    /// Publishes a result. A committed output is reported even after a cancel,
    /// because the receiver must still learn that the source was replaced.
    fn publish_result(&self, result: OperationResult) {
        let committed = result.output().is_some();
        let finished = ProcessingEvent::Finished {
            run_id: self.run_id,
            result: Box::new(result),
        };
        if committed {
            let _ = self.sender.send(finished);
        } else {
            self.publish(finished);
        }
    }

    /// Counts a file as done, and publishes completion when it was the last file of the run.
    fn finish_file(&self) {
        if self.pending.fetch_sub(1, Ordering::SeqCst) == 1 {
            self.publish(ProcessingEvent::Completed {
                run_id: self.run_id,
                total_files: self.total_files,
            });
        }
    }
}

/// Returns the default number of processing workers: one less than the physical cores, but at least one,
/// so the UI thread and audio playback keep a core while plugins render.
#[must_use]
pub fn default_worker_count() -> usize {
    num_cpus::get_physical().saturating_sub(1).max(1)
}

/// Processes one file with a leased chain, updates the cache, and publishes the result.
/// A file with a refusal, such as a batch output conflict, fails without being touched.
fn processing_job(record: &AnalysisRecord, refusal: Option<String>, context: &RunContext) {
    if context.is_cancelled() {
        return;
    }
    if let Some(error) = refusal {
        context.publish_result(OperationResult::failure(&record.path, error));
        context.finish_file();
        return;
    }
    let Some(lease) = context.chains.acquire(&|| !context.is_cancelled()) else {
        return;
    };
    let Some(chain) = lease.chain() else {
        return;
    };
    context.publish(ProcessingEvent::Started {
        run_id: context.run_id,
        path: record.path.clone(),
    });
    let mut last_progress = 0.0_f32;
    let mut on_progress = |fraction: f32| {
        if fraction - last_progress >= PROGRESS_STEP || (fraction >= 1.0 && last_progress < 1.0) {
            last_progress = fraction;
            context.publish(ProcessingEvent::Progress {
                run_id: context.run_id,
                path: record.path.clone(),
                fraction,
            });
        }
    };
    // Plugins are third party code, so a panic while processing fails only this file,
    // and its chain still goes back to the pool.
    let result = catch_unwind(AssertUnwindSafe(|| {
        process_file(
            record,
            &context.options,
            chain,
            &|| context.is_cancelled(),
            &mut on_progress,
        )
    }))
    .unwrap_or_else(|_| OperationResult::failure(&record.path, "Plugin processing failed unexpectedly"));
    // The chain goes back before the result is published, so a waiting worker can start right away.
    drop(lease);
    update_cache(&context.cache, record, &result);
    context.publish_result(result);
    context.finish_file();
}

#[cfg(test)]
mod test_processing_coordinator {
    use std::fs;

    use audiobatch_test_plugin::Kind;
    use tempfile::TempDir;

    use super::*;
    use crate::test_helpers::{deleting_options, gain_state, ready_fixture_copy, test_chain};

    /// Returns a coordinator with `workers` workers whose cache lives in its own subfolder.
    fn coordinator(directory: &TempDir, workers: usize) -> (ProcessingCoordinator, Arc<AnalysisCache>) {
        let cache_folder = directory.path().join("cache");
        fs::create_dir_all(&cache_folder).expect("creates cache folder");
        let cache = Arc::new(AnalysisCache::with_path(cache_folder.join("analysis.db")));
        let coordinator = ProcessingCoordinator::new(Arc::clone(&cache), workers).expect("pool");
        (coordinator, cache)
    }

    /// Returns `count` chains of one unity gain plugin.
    fn gain_chains(count: usize) -> Vec<ProcessingChain> {
        (0..count)
            .map(|_| test_chain(&[(Kind::Gain, gain_state(1.0))]))
            .collect()
    }

    /// Collects every event of a run until it completes or the receiver disconnects.
    fn collect(receiver: &Receiver<ProcessingEvent>) -> Vec<ProcessingEvent> {
        let mut events = Vec::new();
        for event in receiver {
            let completed = matches!(event, ProcessingEvent::Completed { .. });
            events.push(event);
            if completed {
                break;
            }
        }
        events
    }

    /// Returns the results of the finished events.
    fn results(events: &[ProcessingEvent]) -> Vec<&OperationResult> {
        events
            .iter()
            .filter_map(|event| match event {
                ProcessingEvent::Finished { result, .. } => Some(result.as_ref()),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn processes_every_file_with_fewer_chains_than_files_and_updates_the_cache() {
        let directory = TempDir::new().expect("temp dir");
        let records: Vec<AnalysisRecord> = (0..3)
            .map(|index| ready_fixture_copy(&directory, "sine_mono_44100_16.wav", &format!("song{index}.wav")))
            .collect();
        let (coordinator, cache) = coordinator(&directory, 2);
        cache.store_analysis(&records[0]).expect("stores");
        assert_eq!(coordinator.chains_needed(records.len()), 2);

        let (run_id, receiver) = coordinator.start(records.clone(), deleting_options(), gain_chains(1));
        let events = collect(&receiver);
        assert!(events.iter().all(|event| event.run_id() == run_id));
        assert!(matches!(
            events.last(),
            Some(ProcessingEvent::Completed { total_files: 3, .. })
        ));
        let results = results(&events);
        assert_eq!(results.len(), 3);
        assert!(results.iter().all(|result| result.error().is_none()));
        let output = directory.path().join("song0.aif");
        assert!(output.is_file());
        assert!(!records[0].path.exists());
        assert!(!cache.remove_analysis(&records[0].path).expect("removes"));
        assert!(cache.get_analysis(&output).is_some());
    }

    #[test]
    fn each_file_starts_before_it_finishes_and_reaches_full_progress() {
        let directory = TempDir::new().expect("temp dir");
        let record = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let (coordinator, _cache) = coordinator(&directory, 1);
        let (_, receiver) = coordinator.start(vec![record], deleting_options(), gain_chains(1));
        let events = collect(&receiver);
        let started = events
            .iter()
            .position(|event| matches!(event, ProcessingEvent::Started { .. }));
        let finished = events
            .iter()
            .position(|event| matches!(event, ProcessingEvent::Finished { .. }));
        assert!(started.is_some_and(|started| finished.is_some_and(|finished| started < finished)));
        assert!(
            events
                .iter()
                .any(|event| matches!(event, ProcessingEvent::Progress { fraction, .. } if *fraction >= 1.0))
        );
    }

    #[test]
    fn conflicting_files_and_runs_without_chains_fail_without_touching_files() {
        let directory = TempDir::new().expect("temp dir");
        let wav = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "song.wav");
        let flac = ready_fixture_copy(&directory, "sine_stereo_44100_16.flac", "song.flac");
        let (coordinator, _cache) = coordinator(&directory, 2);

        let (_, receiver) = coordinator.start(vec![wav.clone(), flac.clone()], deleting_options(), Vec::new());
        let events = collect(&receiver);
        let refused = results(&events);
        assert_eq!(refused.len(), 2);
        assert!(
            refused
                .iter()
                .all(|result| result.error() == Some("Plugin chain instances are not available"))
        );

        let (_, receiver) = coordinator.start(vec![wav, flac], deleting_options(), gain_chains(2));
        let events = collect(&receiver);
        let failed: Vec<&OperationResult> = results(&events)
            .into_iter()
            .filter(|result| result.error().is_some())
            .collect();
        assert_eq!(failed.len(), 1);
        assert!(failed[0].source.is_file());
    }

    #[test]
    fn a_cancelled_run_does_not_complete() {
        let directory = TempDir::new().expect("temp dir");
        let records: Vec<AnalysisRecord> = (0..4)
            .map(|index| ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", &format!("song{index}.wav")))
            .collect();
        let (coordinator, _cache) = coordinator(&directory, 2);
        let (run_id, receiver) = coordinator.start(records, deleting_options(), gain_chains(1));
        coordinator.cancel();
        assert!(!coordinator.is_current(run_id));
        let events: Vec<ProcessingEvent> = receiver.iter().collect();
        assert!(
            !events
                .iter()
                .any(|event| matches!(event, ProcessingEvent::Completed { .. }))
        );
        for result in results(&events) {
            assert!(
                result.output().is_some(),
                "only committed outputs are reported after a cancel"
            );
        }
    }

    #[test]
    fn the_default_worker_count_leaves_a_core_free() {
        let workers = default_worker_count();
        assert!(workers >= 1);
        assert!(workers <= num_cpus::get_physical().max(1));
    }
}
