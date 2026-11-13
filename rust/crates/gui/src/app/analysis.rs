//! Analysis runs of the main window: listing folders and files, dropped paths and later launches,
//! starting a run, and applying its events to the table, the status line, and the selection.
//! The events are drained from the coordinator channel by a foreground task in batches,
//! so a large run updates the table at a steady rate instead of once per event.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use audiobatch_core::discovery::{has_supported_extension, normalize_path};
use audiobatch_core::{AnalysisEvent, AnalysisOptions, AnalysisRecord, AnalysisStatus};
use crossbeam_channel::{Receiver, TryRecvError};
use gpui_kit::{Context, SharedString, Task, Window};

use super::AppView;

/// How often the analysis and normalization events are applied to the table.
pub(super) const ANALYSIS_POLL_INTERVAL: Duration = Duration::from_millis(50);

/// How often the paths of later launches are checked.
const LAUNCH_POLL_INTERVAL: Duration = Duration::from_millis(200);

/// What an analysis run covers.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum RunKind {
    /// Every listed folder and file, replacing the rows with what is found.
    Scan,
    /// Some listed files again, keeping the other rows.
    Reanalyze,
}

/// Progress of the current analysis run.
#[derive(Debug, Clone)]
pub(super) struct RunProgress {
    /// Id of the run, which its events carry.
    pub(super) run_id: u64,
    /// What the run covers.
    pub(super) kind: RunKind,
    /// True once the run has listed the files it analyzes.
    pub(super) planned: bool,
    /// Number of files the run analyzes.
    pub(super) total: usize,
    /// Number of files the run has finished.
    pub(super) finished: usize,
    /// True once the run has completed.
    pub(super) completed: bool,
    started: Instant,
    analyzed: usize,
    cached: usize,
    failed: usize,
}

/// Events already received, with the channel's terminal state kept distinct from an empty batch.
#[non_exhaustive]
pub(super) struct DrainedEvents<T> {
    pub(super) batch: Vec<T>,
    pub(super) disconnected: bool,
}

impl RunProgress {
    /// Starts tracking an analysis run and its elapsed time.
    fn new(run_id: u64, kind: RunKind) -> Self {
        Self {
            run_id,
            kind,
            planned: false,
            total: 0,
            finished: 0,
            completed: false,
            started: Instant::now(),
            analyzed: 0,
            cached: 0,
            failed: 0,
        }
    }

    /// Counts every result, including files hidden from the table during the run.
    fn record(&mut self, record: &AnalysisRecord) {
        if record.status == AnalysisStatus::Failed {
            self.failed += 1;
        } else if record.from_cache {
            self.cached += 1;
        } else {
            self.analyzed += 1;
        }
    }

    /// Returns the completed run's duration and mutually exclusive result counts.
    fn log_line(&self) -> String {
        format!(
            "Analysis complete: {:.2} s, {} files ({} analyzed, {} from cache, {} failed)",
            self.started.elapsed().as_secs_f64(),
            self.analyzed + self.cached + self.failed,
            self.analyzed,
            self.cached,
            self.failed
        )
    }
}

impl AppView {
    /// Adds folders and files to the list and scans every listed root again.
    /// Nested subfolders already inside an existing root are skipped, and parent folders absorb child entries.
    /// Cached files come back immediately, so only new or changed files are decoded.
    pub(super) fn add_roots(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        if self.blocked_by_file_operation(cx) {
            return;
        }
        let mut added = false;
        for path in paths.iter().map(|path| normalize_path(path)) {
            self.excluded.remove(&path);
            let covered = self
                .roots
                .iter()
                .any(|existing| existing == &path || (existing.is_dir() && path.starts_with(existing)));
            // Folder deduplication must not swallow an explicit file request beneath that folder.
            // A removed or newly created file still needs a scan, without adding a redundant root.
            added |= covered && path.is_file() && has_supported_extension(&path);
            if !covered {
                log_open_path(&path);
                if path.is_dir() {
                    self.roots.retain(|existing| !existing.starts_with(&path));
                }
                self.roots.push(path);
                added = true;
            }
        }
        if added {
            self.start_run(RunKind::Scan, self.roots.clone(), false, cx);
        } else {
            self.status = SharedString::from("Folder is already included");
            self.status_sticky = true;
            cx.notify();
        }
    }

    /// Replaces the listed folders and files and scans them, deduplicating nested folders.
    pub(super) fn replace_roots(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        if self.blocked_by_file_operation(cx) {
            return;
        }
        let mut deduplicated: Vec<PathBuf> = Vec::new();
        for path in paths.iter().map(|path| normalize_path(path)) {
            let covered = deduplicated
                .iter()
                .any(|existing| existing == &path || (existing.is_dir() && path.starts_with(existing)));
            if !covered {
                if path.is_dir() {
                    deduplicated.retain(|existing| !existing.starts_with(&path));
                }
                deduplicated.push(path);
            }
        }
        self.roots = deduplicated;
        for path in &self.roots {
            log_open_path(path);
        }
        self.excluded.clear();
        self.start_run(RunKind::Scan, self.roots.clone(), false, cx);
    }

    /// Opens the paths that later launches of the app hand over, as if they were dropped onto the window,
    /// and brings the window to the front.
    pub fn listen_for_launches(&mut self, launches: Receiver<Vec<PathBuf>>, window: &Window, cx: &Context<Self>) {
        self.launch_task = Some(cx.spawn_in(window, async move |this, cx| {
            loop {
                cx.background_executor().timer(LAUNCH_POLL_INTERVAL).await;
                let received: Vec<Vec<PathBuf>> = launches.try_iter().collect();
                if received.is_empty() {
                    continue;
                }
                let alive = this.update_in(cx, |view, window, cx| {
                    window.activate_window();
                    for paths in received.iter().filter(|paths| !paths.is_empty()) {
                        view.handle_drop(paths, false, cx);
                    }
                });
                if alive.is_err() {
                    break;
                }
            }
        }));
    }

    /// Adds dropped paths, or replaces the listed folders when folders are dropped without Shift held.
    pub(super) fn handle_drop(&mut self, paths: &[PathBuf], append: bool, cx: &mut Context<Self>) {
        let supported: Vec<PathBuf> = paths
            .iter()
            .filter(|path| path.is_dir() || (path.is_file() && has_supported_extension(path)))
            .cloned()
            .collect();
        if supported.is_empty() {
            self.status = SharedString::from("No supported dropped files");
            self.status_sticky = true;
            cx.notify();
            return;
        }
        if !append && supported.iter().any(|path| path.is_dir()) {
            self.replace_roots(&supported, cx);
        } else {
            self.add_roots(&supported, cx);
        }
    }

    /// Starts an analysis run, replacing any run in progress.
    /// Rows the replaced run left waiting or analyzing keep their previous result,
    /// because the events of the replaced run are ignored from now on.
    pub(super) fn start_run(&mut self, kind: RunKind, inputs: Vec<PathBuf>, refresh: bool, cx: &mut Context<Self>) {
        let Some(coordinator) = self.coordinator.clone() else {
            return;
        };
        if inputs.is_empty() || self.is_demo() || self.blocked_by_file_operation(cx) {
            return;
        }
        if self.is_analyzing() {
            tracing::info!("Analysis cancelled: replaced by a new run");
            self.table.update(cx, |state, cx| {
                state.delegate_mut().finish_interrupted_rows();
                cx.notify();
            });
        }
        if refresh && self.selected.as_ref().is_some_and(|selected| inputs.contains(selected)) {
            self.invalidate_artwork(cx);
        }
        self.refresh_artwork(cx);
        self.enrichment.invalidate();
        if let Some(drain) = self.analysis_task.take() {
            drain.detach();
        }
        self.analysis_workers += 1;
        let audiobatch_core::coordinator::AnalysisRun { run_id, receiver } = coordinator.start(AnalysisOptions {
            input_paths: inputs,
            recursive: true,
            refresh,
        });
        self.run = Some(RunProgress::new(run_id, kind));
        self.status = SharedString::from(match kind {
            RunKind::Scan => "Scanning for audio files...",
            RunKind::Reanalyze => "Analyzing...",
        });
        self.status_sticky = false;
        self.analysis_task = Some(spawn_event_drain(receiver, cx));
        self.sync_action_menus(cx);
        cx.notify();
    }

    /// Returns true while a run is in progress.
    pub(super) fn is_analyzing(&self) -> bool {
        self.analysis_task.is_some() && self.run.as_ref().is_some_and(|run| !run.completed)
    }

    /// Applies a batch of analysis events to the table and the selection.
    fn apply_analysis_events(&mut self, events: Vec<AnalysisEvent>, cx: &mut Context<Self>) {
        let Some((run_id, kind)) = self
            .run
            .as_ref()
            .filter(|run| !run.completed)
            .map(|run| (run.run_id, run.kind))
        else {
            return;
        };
        if matches!(kind, RunKind::Scan)
            && let Some(files) = events.iter().find_map(|event| match event {
                AnalysisEvent::Planned { files, .. } if event.run_id() == run_id => Some(files),
                _ => None,
            })
        {
            self.store_unlisted_gains(files, cx);
        }
        let mut planned_total = None;
        let mut finished = 0;
        let mut selected_finished = false;
        let mut completed = false;
        let excluded = &self.excluded;
        let selected = self.selected.as_deref();
        let run = &mut self.run;
        self.table.update(cx, |state, cx| {
            let delegate = state.delegate_mut();
            for event in events.into_iter().filter(|event| event.run_id() == run_id) {
                match event {
                    AnalysisEvent::Planned { files, .. } => {
                        tracing::info!("Found {} audio files", files.len());
                        let files: Vec<PathBuf> = files.into_iter().filter(|path| !excluded.contains(path)).collect();
                        planned_total = Some(files.len());
                        match kind {
                            RunKind::Scan => delegate.set_files(&files),
                            RunKind::Reanalyze => files.iter().for_each(|path| delegate.mark_pending(path)),
                        }
                    }
                    AnalysisEvent::Started { path, .. } => delegate.mark_started(&path),
                    AnalysisEvent::Progress { path, fraction, .. } => delegate.mark_progress(&path, fraction),
                    AnalysisEvent::Finished { record, .. } => {
                        if let Some(run) = run {
                            run.record(&record);
                        }
                        if record.status == AnalysisStatus::Failed {
                            tracing::warn!(
                                "Analysis failed for \"{}\": {}",
                                record.path.display(),
                                record.error_message
                            );
                        }
                        if excluded.contains(&record.path) {
                            continue;
                        }
                        finished += 1;
                        selected_finished |= selected == Some(record.path.as_path());
                        let mut record = *record;
                        record.spectrogram = None;
                        if selected != Some(record.path.as_path()) {
                            record.waveform = None;
                        }
                        delegate.set_record(record);
                    }
                    AnalysisEvent::Completed { .. } => {
                        delegate.sort_rows();
                        completed = true;
                    }
                }
            }
            cx.notify();
        });
        if let Some(run) = &mut self.run {
            if let Some(total) = planned_total {
                run.planned = true;
                run.total = total;
            }
            run.finished += finished;
            run.completed |= completed;
            if completed {
                tracing::info!("{}", run.log_line());
            }
        }
        self.update_run_status(cx);
        if planned_total.is_some() || completed {
            self.restore_selection(cx);
        }
        if completed && std::mem::take(&mut self.autoplay_pending) {
            self.autoplay_first_ready(cx);
        }
        if completed {
            self.status_sticky = false;
            self.resume_enrichment(cx);
        }
        self.sync_action_menus(cx);
        if selected_finished {
            self.reconcile_selected_preview(cx);
            self.refresh_selected_waveform(cx);
            self.refresh_artwork(cx);
            self.apply_preview_gain(cx);
            self.request_selected_enrichment(cx);
        }
        cx.notify();
    }

    /// Starts the first playable row after a completed load requested autoplay.
    fn autoplay_first_ready(&mut self, cx: &mut Context<Self>) {
        let first_playable = {
            let delegate = self.table.read(cx).delegate();
            (0..delegate.len()).find_map(|index| {
                delegate
                    .row(index)
                    .filter(|row| row.ready_record().is_some())
                    .map(|row| (index, row.path.clone()))
            })
        };
        if let Some((index, path)) = first_playable {
            self.table.update(cx, |state, cx| state.set_selected_row(index, cx));
            self.select_path(&path, true, cx);
        }
    }

    /// Sets the status text from the progress of the current run.
    fn update_run_status(&mut self, cx: &Context<Self>) {
        let Some(run) = &self.run else {
            return;
        };
        let (listed, failed) = {
            let delegate = self.table.read(cx).delegate();
            (delegate.len(), delegate.failed_count())
        };
        if run.completed {
            let failures = if failed > 0 {
                format!(", {failed} failed")
            } else {
                String::new()
            };
            self.status = match run.kind {
                RunKind::Scan if listed == 0 => String::from("No supported audio files found"),
                RunKind::Scan => format!("Loaded {listed} files{failures}"),
                RunKind::Reanalyze => format!("Analyzed {} files{failures}", run.total),
            }
            .into();
        } else if run.planned {
            self.status = format!("Analyzing {}/{}", run.finished, run.total).into();
        }
    }
}

/// Takes every event waiting in a coordinator channel.
/// Returns the events and whether the channel is disconnected, which means the run has no more events.
pub(super) fn drain_events<T>(receiver: &Receiver<T>) -> DrainedEvents<T> {
    let mut batch = Vec::new();
    loop {
        match receiver.try_recv() {
            Ok(event) => batch.push(event),
            Err(TryRecvError::Empty) => {
                return DrainedEvents {
                    batch,
                    disconnected: false,
                };
            }
            Err(TryRecvError::Disconnected) => {
                return DrainedEvents {
                    batch,
                    disconnected: true,
                };
            }
        }
    }
}

/// Logs an opened input without listing every file discovered inside a directory.
fn log_open_path(path: &std::path::Path) {
    let kind = if path.is_dir() { "directory" } else { "file" };
    tracing::info!("Opening {kind}: \"{}\"", path.display());
}

/// Drains the analysis events on a timer and applies them to the view in batches.
fn spawn_event_drain(receiver: Receiver<AnalysisEvent>, cx: &Context<AppView>) -> Task<()> {
    cx.spawn(async move |this, cx| {
        loop {
            let DrainedEvents { batch, disconnected } = drain_events(&receiver);
            if !batch.is_empty()
                && this
                    .update(cx, |view, cx| view.apply_analysis_events(batch, cx))
                    .is_err()
            {
                break;
            }
            if disconnected {
                let _ = this.update(cx, |view, cx| {
                    view.analysis_workers = view.analysis_workers.saturating_sub(1);
                    view.resume_enrichment(cx);
                });
                break;
            }
            cx.background_executor().timer(ANALYSIS_POLL_INTERVAL).await;
        }
    })
}

#[cfg(test)]
mod test_run_progress {
    use super::*;

    #[test]
    fn event_drain_distinguishes_empty_connected_and_terminal_batches() {
        let (sender, receiver) = crossbeam_channel::unbounded();
        let empty = drain_events(&receiver);
        assert_eq!(empty.batch, [] as [i32; 0]);
        assert!(!empty.disconnected);

        sender.send(1).expect("first event");
        sender.send(2).expect("second event");
        let pending = drain_events(&receiver);
        assert_eq!(pending.batch, [1, 2]);
        assert!(!pending.disconnected);

        sender.send(3).expect("final event");
        drop(sender);
        let terminal = drain_events(&receiver);
        assert_eq!(terminal.batch, [3]);
        assert!(terminal.disconnected);
        let exhausted = drain_events(&receiver);
        assert_eq!(exhausted.batch, [] as [i32; 0]);
        assert!(exhausted.disconnected);
    }

    #[test]
    fn summary_counts_fresh_cached_and_failed_results() {
        let mut run = RunProgress::new(1, RunKind::Scan);
        for (status, from_cache) in [
            (AnalysisStatus::Analyzed, false),
            (AnalysisStatus::Cached, true),
            (AnalysisStatus::Failed, false),
            (AnalysisStatus::Failed, true),
        ] {
            run.record(&AnalysisRecord {
                status,
                from_cache,
                ..AnalysisRecord::default()
            });
        }
        assert!(run.log_line().ends_with("4 files (1 analyzed, 1 from cache, 2 failed)"));
    }

    #[test]
    fn empty_run_has_a_complete_summary() {
        let run = RunProgress::new(1, RunKind::Scan);
        assert!(run.log_line().ends_with("0 files (0 analyzed, 0 from cache, 0 failed)"));
    }

    #[test]
    fn summary_includes_elapsed_seconds() {
        let mut run = RunProgress::new(1, RunKind::Reanalyze);
        run.started = Instant::now()
            .checked_sub(Duration::from_secs(2))
            .expect("earlier instant");
        let line = run.log_line();
        let seconds = line
            .strip_prefix("Analysis complete: ")
            .expect("summary prefix")
            .split_once(" s,")
            .expect("duration suffix")
            .0
            .parse::<f64>()
            .expect("numeric duration");
        assert!(seconds >= 2.0);
    }
}
