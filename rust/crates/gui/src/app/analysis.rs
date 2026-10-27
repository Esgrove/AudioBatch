//! Analysis runs of the main window: listing folders and files, dropped paths and later launches,
//! starting a run, and applying its events to the table, the status line, and the selection.
//! The events are drained from the coordinator channel by a foreground task in batches,
//! so a large run updates the table at a steady rate instead of once per event.

use std::path::PathBuf;
use std::time::Duration;

use audiobatch_core::discovery::{has_supported_extension, normalize_path};
use audiobatch_core::{AnalysisEvent, AnalysisOptions};
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
}

impl AppView {
    /// Adds folders and files to the list and scans every listed root again.
    /// Cached files come back immediately, so only new or changed files are decoded.
    pub(super) fn add_roots(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        if self.blocked_by_file_operation(cx) {
            return;
        }
        let mut added = false;
        for path in paths.iter().map(|path| normalize_path(path)) {
            self.excluded.remove(&path);
            if !self.roots.contains(&path) {
                self.roots.push(path);
                added = true;
            }
        }
        if added {
            self.start_run(RunKind::Scan, self.roots.clone(), false, cx);
        } else {
            self.status = SharedString::from("Folder is already included");
            cx.notify();
        }
    }

    /// Replaces the listed folders and files and scans them.
    pub(super) fn replace_roots(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        if self.blocked_by_file_operation(cx) {
            return;
        }
        self.roots = paths.iter().map(|path| normalize_path(path)).collect();
        self.excluded.clear();
        self.start_run(RunKind::Scan, self.roots.clone(), false, cx);
    }

    /// Opens the paths that later launches of the app hand over, as if they were dropped onto the window,
    /// and brings the window to the front, like the C++ application.
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
            .filter(|path| path.is_dir() || has_supported_extension(path))
            .cloned()
            .collect();
        if supported.is_empty() {
            self.status = SharedString::from("No supported dropped files");
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
            self.table.update(cx, |state, cx| {
                state.delegate_mut().finish_interrupted_rows();
                cx.notify();
            });
        }
        let (run_id, receiver) = coordinator.start(AnalysisOptions {
            input_paths: inputs,
            recursive: true,
            refresh,
        });
        self.run = Some(RunProgress {
            run_id,
            kind,
            planned: false,
            total: 0,
            finished: 0,
            completed: false,
        });
        self.status = SharedString::from(match kind {
            RunKind::Scan => "Scanning for audio files...",
            RunKind::Reanalyze => "Analyzing...",
        });
        self.analysis_task = Some(spawn_event_drain(receiver, cx));
        cx.notify();
    }

    /// Returns true while a run is in progress.
    pub(super) fn is_analyzing(&self) -> bool {
        self.analysis_task.is_some() && self.run.as_ref().is_some_and(|run| !run.completed)
    }

    /// Applies a batch of analysis events to the table and the selection.
    fn apply_analysis_events(&mut self, events: Vec<AnalysisEvent>, cx: &mut Context<Self>) {
        let Some((run_id, kind)) = self.run.as_ref().map(|run| (run.run_id, run.kind)) else {
            return;
        };
        let mut planned_total = None;
        let mut finished = 0;
        let mut selected_finished = false;
        let mut completed = false;
        let mut carried_gains = Vec::new();
        let excluded = &self.excluded;
        let selected = self.selected.as_deref();
        self.table.update(cx, |state, cx| {
            let delegate = state.delegate_mut();
            for event in events.into_iter().filter(|event| event.run_id() == run_id) {
                match event {
                    AnalysisEvent::Planned { files, .. } => {
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
                        if excluded.contains(&record.path) {
                            continue;
                        }
                        finished += 1;
                        selected_finished |= selected == Some(record.path.as_path());
                        let path = record.path.clone();
                        if let Some(gain) = delegate.set_record(*record) {
                            carried_gains.push((path, gain));
                        }
                    }
                    AnalysisEvent::Completed { .. } => {
                        delegate.sort_rows();
                        completed = true;
                    }
                }
            }
            cx.notify();
        });
        // The coordinator stored each row before publishing it, so a carried over gain can be stored now.
        for (path, gain) in carried_gains {
            self.store_custom_gain(&path, Some(gain));
        }

        if let Some(run) = &mut self.run {
            if let Some(total) = planned_total {
                run.planned = true;
                run.total = total;
            }
            run.finished += finished;
            run.completed |= completed;
        }
        self.update_run_status(cx);
        if planned_total.is_some() || completed {
            self.restore_selection(cx);
        }
        if completed {
            if crate::debug_enabled() {
                tracing::info!("analysis: {}", self.status);
            }
            if std::mem::take(&mut self.autoplay_pending) {
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
        }
        if selected_finished {
            self.refresh_selected_waveform(cx);
            self.apply_preview_gain(cx);
        }
        cx.notify();
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
pub(super) fn drain_events<T>(receiver: &Receiver<T>) -> (Vec<T>, bool) {
    let mut batch = Vec::new();
    loop {
        match receiver.try_recv() {
            Ok(event) => batch.push(event),
            Err(TryRecvError::Empty) => return (batch, false),
            Err(TryRecvError::Disconnected) => return (batch, true),
        }
    }
}

/// Drains the analysis events on a timer and applies them to the view in batches.
fn spawn_event_drain(receiver: Receiver<AnalysisEvent>, cx: &Context<AppView>) -> Task<()> {
    cx.spawn(async move |this, cx| {
        loop {
            let (batch, disconnected) = drain_events(&receiver);
            if !batch.is_empty()
                && this
                    .update(cx, |view, cx| view.apply_analysis_events(batch, cx))
                    .is_err()
            {
                break;
            }
            if disconnected {
                break;
            }
            cx.background_executor().timer(ANALYSIS_POLL_INTERVAL).await;
        }
    })
}
