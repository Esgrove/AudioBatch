//! Background analysis runs on a worker pool, with cancellation and progress events.
//!
//! Each run expands the input paths, publishes the plan and the cached records,
//! and queues one pool job per stale file.
//! Fresh results are stored back into the cache.
//! Events are sent over a channel instead of callbacks,
//! so the CLI can block on it and the GUI can drain it from a background task.
//! Starting a new run or calling `cancel` invalidates the previous run id,
//! and jobs of an invalidated run stop early and publish nothing more.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender, unbounded};
use rayon::{ThreadPool, ThreadPoolBuilder};

use crate::analysis::analyze_file_with_observer_catching_panics;
use crate::cache::AnalysisCache;
use crate::decode::StreamInfo;
use crate::discovery::{collect_input_files, normalize_path};
use crate::format::format_error_chain;
use crate::meter::BlockMeter;
use crate::output_file::SourceIdentity;
use crate::record::{AnalysisOptions, AnalysisRecord, file_identity};
use crate::waveform::WaveformOverview;

/// Smallest progress step that is published, so the receiver is not flooded with events.
const PROGRESS_STEP: f32 = 0.02;

/// Minimum interval between selected-file waveform snapshots on a decoding worker.
const PREVIEW_INTERVAL: Duration = Duration::from_millis(250);

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
    /// A cached or freshly analyzed measurement record, including failures.
    /// Drawing payloads are persisted before publication and loaded independently for the selected file.
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

/// A failure to create the analysis worker threads.
#[derive(Debug, thiserror::Error)]
#[error("Failed to create the analysis worker pool")]
pub struct AnalysisCoordinatorError(#[source] pub rayon::ThreadPoolBuildError);

/// A provisional selected-file waveform, never a completed measurement or a cache artifact.
/// The waveform covers decoded frames only, while consumers retain the full source timeline independently.
#[derive(Debug, Clone)]
pub struct AnalysisPreview {
    path: PathBuf,
    source_identity: SourceIdentity,
    file_size: u64,
    modified_time_ms: i64,
    generation: u64,
    run_id: u64,
    decoded_frames: u64,
    expected_frames: Option<u64>,
    waveform: Arc<WaveformOverview>,
}

/// Runs analysis in the background on a fixed-size worker pool.
pub struct AnalysisCoordinator {
    cache: Arc<AnalysisCache>,
    pool: Arc<ThreadPool>,
    current_run: Arc<AtomicU64>,
    preview: Arc<Mutex<PreviewSlot>>,
}

/// Current selection and its only pending preview, shared with all analysis workers.
#[derive(Default)]
struct PreviewSlot {
    selected: Option<PreviewSelection>,
    latest: Option<AnalysisPreview>,
}

/// Selection admission token, with source identity captured before a worker copies any waveform buckets.
#[derive(Clone, PartialEq, Eq)]
struct PreviewSelection {
    path: PathBuf,
    generation: u64,
    file_size: u64,
    modified_time_ms: i64,
    source_identity: SourceIdentity,
}

/// Identity and throttling retained by one decoding worker without keeping preview payloads.
struct PreviewProgress {
    source_identity: Option<SourceIdentity>,
    last_preview: Option<(u64, Instant)>,
}

/// State shared by the jobs of one run.
struct RunContext {
    run_id: u64,
    current_run: Arc<AtomicU64>,
    cache: Arc<AnalysisCache>,
    sender: Sender<AnalysisEvent>,
    preview: Arc<Mutex<PreviewSlot>>,
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

impl AnalysisPreview {
    /// Returns the selected source path captured for this snapshot.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Returns the source size captured before decoding.
    #[must_use]
    pub const fn file_size(&self) -> u64 {
        self.file_size
    }

    /// Returns the source modification time captured before decoding.
    #[must_use]
    pub const fn modified_time_ms(&self) -> i64 {
        self.modified_time_ms
    }

    /// Returns the selection generation that admitted this snapshot.
    #[must_use]
    pub const fn generation(&self) -> u64 {
        self.generation
    }

    /// Returns the analysis run that decoded the provisional waveform.
    #[must_use]
    pub const fn run_id(&self) -> u64 {
        self.run_id
    }

    /// Returns how many source frames were decoded when the snapshot was made.
    #[must_use]
    pub const fn decoded_frames(&self) -> u64 {
        self.decoded_frames
    }

    /// Returns the container's expected length, if known, independently of decoded progress.
    #[must_use]
    pub const fn expected_frames(&self) -> Option<u64> {
        self.expected_frames
    }

    /// Returns the bounded waveform for the decoded prefix.
    #[must_use]
    pub const fn waveform(&self) -> &Arc<WaveformOverview> {
        &self.waveform
    }

    /// Revalidates the stronger captured source identity before foreground adoption.
    #[must_use]
    pub fn matches_file_on_disk(&self) -> bool {
        self.source_identity.validate(&self.path).is_ok()
    }
}

impl AnalysisCoordinator {
    /// Creates a coordinator with the given number of worker threads, at least one.
    ///
    /// # Errors
    ///
    /// Returns an error when the worker threads cannot be created.
    pub fn new(cache: Arc<AnalysisCache>, worker_count: usize) -> Result<Self, AnalysisCoordinatorError> {
        let pool = ThreadPoolBuilder::new()
            .num_threads(worker_count.max(1))
            .thread_name(|index| format!("analysis-{index}"))
            .build()
            .map_err(AnalysisCoordinatorError)?;
        Ok(Self {
            cache,
            pool: Arc::new(pool),
            current_run: Arc::new(AtomicU64::new(0)),
            preview: Arc::new(Mutex::new(PreviewSlot::default())),
        })
    }

    /// Returns the cache the coordinator reads and writes.
    #[must_use]
    pub const fn cache(&self) -> &Arc<AnalysisCache> {
        &self.cache
    }

    /// Selects the sole source allowed to publish provisional waveform snapshots.
    /// Changing or clearing selection immediately drops the previous pending snapshot.
    /// Paths use the same lexical normalization as input discovery, preserving symbolic links.
    /// Source identity is captured here, so replacing the file invalidates snapshots even at equal size and mtime.
    pub fn select_preview(&self, path: Option<&Path>, generation: u64) {
        let selected = path.map(normalize_path).and_then(|path| {
            let source_identity = SourceIdentity::capture(&path).ok()?;
            let (file_size, modified_time_ms) = file_identity(&path)?;
            Some(PreviewSelection {
                path,
                generation,
                file_size,
                modified_time_ms,
                source_identity,
            })
        });
        if let Ok(mut slot) = self.preview.lock() {
            slot.selected = selected;
            slot.latest = None;
        }
    }

    /// Takes the newest provisional waveform, dropping stale runs, selections, and changed sources.
    /// At most one snapshot can wait here, independently of the progress-event channel.
    #[must_use]
    pub fn take_preview(&self) -> Option<AnalysisPreview> {
        let preview = {
            let mut slot = self.preview.lock().ok()?;
            let preview = slot.latest.take()?;
            let selected = slot.selected.as_ref()?;
            if preview.path != selected.path
                || preview.generation != selected.generation
                || preview.source_identity != selected.source_identity
                || !self.is_current(preview.run_id)
            {
                return None;
            }
            drop(slot);
            preview
        };
        preview.matches_file_on_disk().then_some(preview)
    }

    /// Starts a background run, cancelling the previous one, and returns its id and event receiver.
    /// Expanding the input paths and the cache lookups also run in the background,
    /// so the caller never waits for the file system.
    /// The receiver disconnects once every job of the run has finished.
    #[must_use]
    pub fn start(&self, options: AnalysisOptions) -> (u64, Receiver<AnalysisEvent>) {
        let run_id = self.current_run.fetch_add(1, Ordering::SeqCst) + 1;
        if let Ok(mut slot) = self.preview.lock() {
            slot.latest = None;
        }
        let (sender, receiver) = unbounded();
        let context = Arc::new(RunContext {
            run_id,
            current_run: Arc::clone(&self.current_run),
            cache: Arc::clone(&self.cache),
            sender,
            preview: Arc::clone(&self.preview),
        });
        let pool = Arc::clone(&self.pool);
        self.pool.spawn(move || plan_run(&pool, &context, &options));
        (run_id, receiver)
    }

    /// Cancels the current run. Jobs that are already decoding stop at their next packet.
    pub fn cancel(&self) {
        self.current_run.fetch_add(1, Ordering::SeqCst);
        if let Ok(mut slot) = self.preview.lock() {
            slot.latest = None;
        }
    }

    /// Returns true when the run is still the current one.
    #[must_use]
    pub fn is_current(&self, run_id: u64) -> bool {
        self.current_run.load(Ordering::SeqCst) == run_id
    }

    /// Runs a full analysis and returns every cached and fresh record, blocking until the run completes.
    #[cfg(test)]
    #[must_use]
    pub(crate) fn analyze_blocking(&self, options: AnalysisOptions) -> Vec<AnalysisRecord> {
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

    /// Copies selected-file buckets only after path, source, run, generation, and time admission.
    /// The second selection check prevents an obsolete worker from overwriting a newer selection's slot.
    fn publish_preview(
        &self,
        path: &Path,
        meter: &BlockMeter,
        decoded_frames: u64,
        info: &StreamInfo,
        progress: &mut PreviewProgress,
    ) {
        if self.is_cancelled() || decoded_frames == 0 {
            return;
        }
        let selected = {
            let Ok(slot) = self.preview.lock() else {
                return;
            };
            let Some(selected) = slot.selected.as_ref().filter(|selected| selected.path == path) else {
                return;
            };
            if progress.source_identity.as_ref() != Some(&selected.source_identity)
                || progress.last_preview.as_ref().is_some_and(|(generation, time)| {
                    *generation == selected.generation && time.elapsed() < PREVIEW_INTERVAL
                })
            {
                return;
            }
            selected.clone()
        };
        if selected.source_identity.validate(path).is_err() {
            return;
        }
        let preview = AnalysisPreview {
            path: path.to_path_buf(),
            source_identity: selected.source_identity.clone(),
            file_size: selected.file_size,
            modified_time_ms: selected.modified_time_ms,
            generation: selected.generation,
            run_id: self.run_id,
            decoded_frames,
            expected_frames: info.expected_frames,
            waveform: meter.waveform_snapshot(decoded_frames),
        };
        if !preview.matches_file_on_disk() {
            return;
        }
        if let Ok(mut slot) = self.preview.lock()
            && !self.is_cancelled()
            && slot.selected.as_ref() == Some(&selected)
        {
            slot.latest = Some(preview);
            progress.last_preview = Some((selected.generation, Instant::now()));
        }
    }

    /// Removes any provisional payload before publishing the authoritative final record or failure.
    fn clear_preview(&self, path: &Path) {
        if let Ok(mut slot) = self.preview.lock()
            && slot
                .latest
                .as_ref()
                .is_some_and(|preview| preview.run_id == self.run_id && preview.path == path)
        {
            slot.latest = None;
        }
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
    let mut preview_progress = PreviewProgress {
        source_identity: SourceIdentity::capture(path).ok(),
        last_preview: None,
    };
    let mut on_decoded = |meter: &BlockMeter, decoded_frames, info: &StreamInfo| {
        context.publish_preview(path, meter, decoded_frames, info, &mut preview_progress);
    };
    let mut record =
        analyze_file_with_observer_catching_panics(path, &|| context.is_cancelled(), &mut on_progress, &mut on_decoded);
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
    if let Some(spectrogram) = &record.spectrogram
        && let Err(error) = context.cache.store_spectrogram(&record, spectrogram)
    {
        tracing::warn!("Could not cache the spectrogram: {}", format_error_chain(&error));
    }
    context.clear_preview(path);
    // Table events carry measurement metadata only, selected artifacts reload independently from the cache.
    record.spectrogram = None;
    record.waveform = None;
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
mod test_previews {
    use super::*;
    use tempfile::TempDir;

    /// A deterministic worker and selection slot without asynchronous decoding or timing sleeps.
    struct PreviewHarness {
        directory: TempDir,
        coordinator: AnalysisCoordinator,
        context: RunContext,
        path: PathBuf,
        progress: PreviewProgress,
        meter: BlockMeter,
        info: StreamInfo,
    }

    impl PreviewHarness {
        /// Creates a source copy and a manually admitted current worker run.
        fn new() -> Self {
            let directory = TempDir::new().expect("temporary directory");
            let path = directory.path().join("source.wav");
            std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.wav"), &path).expect("source copy");
            let cache = Arc::new(AnalysisCache::with_path(directory.path().join("analysis.db")));
            let coordinator = AnalysisCoordinator::new(Arc::clone(&cache), 1).expect("coordinator");
            coordinator.current_run.store(1, Ordering::SeqCst);
            let (sender, _) = unbounded();
            let context = RunContext {
                run_id: 1,
                current_run: Arc::clone(&coordinator.current_run),
                cache,
                sender,
                preview: Arc::clone(&coordinator.preview),
            };
            let progress = PreviewProgress {
                source_identity: SourceIdentity::capture(&path).ok(),
                last_preview: None,
            };
            let mut meter = BlockMeter::new(2, 44_100, Some(44_100)).expect("meter");
            meter.push(&[vec![0.25; 128], vec![-0.5; 128]]);
            Self {
                directory,
                coordinator,
                context,
                path,
                progress,
                meter,
                info: StreamInfo {
                    sample_rate: 44_100,
                    channels: 2,
                    bits_per_sample: 16,
                    expected_frames: Some(44_100),
                    format_name: "WAV file".to_string(),
                },
            }
        }

        /// Attempts publication through the same admission and throttling path as a decoder packet.
        fn publish(&mut self) {
            self.context
                .publish_preview(&self.path, &self.meter, 128, &self.info, &mut self.progress);
        }
    }

    #[test]
    fn only_selected_sources_publish_and_taking_consumes_the_only_slot() {
        let mut harness = PreviewHarness::new();
        harness.publish();
        assert!(harness.coordinator.take_preview().is_none());
        harness.coordinator.select_preview(Some(&harness.path), 7);
        harness.publish();
        let preview = harness.coordinator.take_preview().expect("selected prefix");
        assert_eq!(preview.path(), harness.path);
        assert_eq!(preview.generation(), 7);
        assert_eq!(preview.run_id(), 1);
        assert_eq!(preview.decoded_frames(), 128);
        assert_eq!(preview.expected_frames(), Some(44_100));
        assert_eq!(preview.waveform().length_in_frames(), 128);
        assert!(preview.matches_file_on_disk());
        assert!(harness.coordinator.take_preview().is_none());
        assert!(harness.coordinator.cache().get_analysis(&harness.path).is_none());
        assert!(harness.coordinator.cache().get_waveform(&harness.path).is_none());
    }

    #[test]
    fn lexical_selection_paths_share_the_discovered_worker_identity() {
        let mut harness = PreviewHarness::new();
        let nested = harness.directory.path().join("nested");
        std::fs::create_dir(&nested).expect("selection path parent");
        let selected = nested.join("../source.wav");
        assert_ne!(selected, harness.path);
        harness.coordinator.select_preview(Some(&selected), 8);
        harness.publish();
        let preview = harness
            .coordinator
            .take_preview()
            .expect("normalized selection admits the worker");
        assert_eq!(preview.path(), harness.path);
        assert_eq!(preview.generation(), 8);
        assert!(preview.matches_file_on_disk());
    }

    #[test]
    fn snapshots_are_throttled_and_replace_rather_than_queue_payloads() {
        let mut harness = PreviewHarness::new();
        harness.coordinator.select_preview(Some(&harness.path), 8);
        harness.publish();
        let first = harness.coordinator.take_preview().expect("first prefix");
        harness.publish();
        assert!(harness.coordinator.take_preview().is_none());
        harness.progress.last_preview = Some((8, Instant::now().checked_sub(PREVIEW_INTERVAL).unwrap()));
        harness.publish();
        let second = harness.coordinator.take_preview().expect("next admitted prefix");
        assert!(!Arc::ptr_eq(first.waveform(), second.waveform()));
        harness.progress.last_preview = None;
        harness.publish();
        harness.progress.last_preview = None;
        harness.publish();
        assert!(harness.coordinator.take_preview().is_some());
        assert!(harness.coordinator.take_preview().is_none());
    }

    #[test]
    fn selection_generations_cancellation_and_new_runs_reject_old_prefixes() {
        let mut harness = PreviewHarness::new();
        harness.coordinator.select_preview(Some(&harness.path), 9);
        harness.publish();
        harness.coordinator.select_preview(Some(&harness.path), 10);
        assert!(harness.coordinator.take_preview().is_none());
        harness.publish();
        assert_eq!(
            harness.coordinator.take_preview().expect("new generation").generation(),
            10
        );
        harness.progress.last_preview = None;
        harness.publish();
        harness.coordinator.cancel();
        assert!(harness.coordinator.take_preview().is_none());
        harness.progress.last_preview = None;
        harness.publish();
        assert!(harness.coordinator.take_preview().is_none());
        let (_, receiver) = harness.coordinator.start(AnalysisOptions::default());
        harness.publish();
        assert!(harness.coordinator.take_preview().is_none());
        loop {
            let event = receiver
                .recv_timeout(Duration::from_secs(5))
                .expect("bounded empty run");
            if matches!(event, AnalysisEvent::Completed { .. }) {
                break;
            }
        }
    }

    #[test]
    fn replaced_sources_invalidate_queued_and_future_worker_snapshots() {
        let mut harness = PreviewHarness::new();
        harness.coordinator.select_preview(Some(&harness.path), 11);
        harness.publish();
        let old = harness.directory.path().join("old.wav");
        std::fs::rename(&harness.path, &old).expect("moves decoded source");
        std::fs::copy(&old, &harness.path).expect("replacement source");
        assert!(harness.coordinator.take_preview().is_none());
        harness.progress.last_preview = None;
        harness.publish();
        assert!(harness.coordinator.take_preview().is_none());
        harness.coordinator.select_preview(Some(&harness.path), 12);
        harness.publish();
        assert!(harness.coordinator.take_preview().is_none());
        harness.coordinator.select_preview(None, 13);
        assert!(harness.coordinator.take_preview().is_none());
    }
}

#[cfg(test)]
mod test_coordinator {
    use super::*;

    #[cfg(unix)]
    use std::os::unix::fs::PermissionsExt;

    use tempfile::TempDir;

    #[cfg(unix)]
    use crate::record::AnalysisError;
    use crate::record::{AnalysisStatus, same_custom_gain};
    use crate::test_helpers::block_pool;

    /// Creates a coordinator with four workers and a cache in the directory.
    fn coordinator(directory: &TempDir) -> AnalysisCoordinator {
        let cache = Arc::new(AnalysisCache::with_path(directory.path().join("analysis.db")));
        AnalysisCoordinator::new(cache, 4).expect("pool")
    }

    /// Copies two tones and the broken file into a folder of the directory and returns it.
    fn fixture_folder(directory: &TempDir) -> PathBuf {
        let folder = directory.path().join("audio");
        std::fs::create_dir_all(&folder).expect("create folder");
        for name in ["sine_stereo_44100_16.wav", "sine_mono_48000_24.aif", "broken.wav"] {
            std::fs::copy(test_fixtures::fixture(name), folder.join(name)).expect("copy fixture");
        }
        folder
    }

    /// Returns the options of a recursive run over the folder.
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
        assert!(
            same_custom_gain(record.custom_gain_db, Some(-4.0)),
            "{:?}",
            record.custom_gain_db
        );
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
    fn cancelled_run_publishes_and_caches_nothing() {
        let directory = TempDir::new().expect("temp dir");
        let folder = fixture_folder(&directory);
        let coordinator = coordinator(&directory);
        let release = block_pool(&coordinator.pool);
        let (run_id, receiver) = coordinator.start(options(&folder, false));
        coordinator.cancel();
        drop(release);
        assert!(!coordinator.is_current(run_id));
        let events: Vec<AnalysisEvent> = receiver.iter().collect();
        assert!(events.is_empty(), "{events:?}");
        for name in ["sine_stereo_44100_16.wav", "sine_mono_48000_24.aif", "broken.wav"] {
            assert!(coordinator.cache().get_analysis(&folder.join(name)).is_none(), "{name}");
        }
    }

    #[test]
    fn a_new_run_replaces_the_previous_one() {
        let directory = TempDir::new().expect("temp dir");
        let folder = fixture_folder(&directory);
        let coordinator = coordinator(&directory);
        let release = block_pool(&coordinator.pool);
        let (first_id, first) = coordinator.start(options(&folder, false));
        let (second_id, second) = coordinator.start(options(&folder, false));
        drop(release);
        assert!(!coordinator.is_current(first_id));
        assert!(coordinator.is_current(second_id));
        let replaced: Vec<AnalysisEvent> = first.iter().collect();
        assert!(replaced.is_empty(), "{replaced:?}");
        let events: Vec<AnalysisEvent> = second.iter().collect();
        assert!(events.iter().all(|event| event.run_id() == second_id));
        assert!(matches!(
            events.last(),
            Some(AnalysisEvent::Completed { total_files: 3, .. })
        ));
    }

    #[cfg(unix)]
    #[test]
    fn a_file_that_could_not_be_read_is_analyzed_again_on_the_next_run() {
        let directory = TempDir::new().expect("temp dir");
        let folder = fixture_folder(&directory);
        let path = folder.join("sine_stereo_44100_16.wav");
        let coordinator = coordinator(&directory);
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o000)).expect("removes permissions");
        let unreadable = std::fs::File::open(&path).is_err();
        let records = coordinator.analyze_blocking(options(&folder, false));
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644)).expect("restores permissions");
        if !unreadable {
            // Running as root ignores the permissions, so there is nothing to check.
            return;
        }
        let failed = records
            .iter()
            .find(|record| record.path == path)
            .expect("record of the file");
        assert_eq!(failed.error, Some(AnalysisError::Unreadable));
        assert!(coordinator.cache().get_analysis(&path).is_none());

        let records = coordinator.analyze_blocking(options(&folder, false));
        let record = records
            .iter()
            .find(|record| record.path == path)
            .expect("record of the file");
        assert!(!record.from_cache);
        assert!(record.is_ready(), "{}", record.error_message);
        assert!(coordinator.cache().get_analysis(&path).is_some());
    }
}
