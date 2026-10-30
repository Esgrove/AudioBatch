//! Processing files with the plugin chain from the main view.
//!
//! A run processes the selected file, or every analyzed file after a confirmation,
//! on the processing coordinator, which shares the analysis cache.
//! The settings of the live instances are captured first, and each worker gets its own chain of new instances,
//! created on the UI thread before the run starts.
//! If any plugin cannot be loaded, the run does not start, so no file is processed with an incomplete chain.
//! Analysis, normalization, and plugin processing never run at the same time,
//! and changes to the list wait for a run to finish.
//! Results replace rows the same way normalization results do, and the run can be cancelled,
//! which leaves the files in progress with their originals.

use std::path::Path;

use audiobatch_core::AnalysisRecord;
use audiobatch_plugins::{ProcessingChain, ProcessingEvent, ProcessingOptions};
use crossbeam_channel::Receiver;
use gpui_kit::{Context, PromptLevel, SharedString, Task, Window};
use vst3_host::UiThread;

use super::analysis::ANALYSIS_POLL_INTERVAL;
use super::normalize::CANCELLED_RUN_FINISHING;
use super::{AppView, drain_events};
use crate::normalization::normalization_options;
use crate::operation_run::{OperationKind, OperationRun, discard_run};

/// Status text when processing is asked for while files are being analyzed.
const BLOCKED_BY_ANALYSIS: &str = "Wait for analysis to finish before processing";

/// Status text when processing is asked for while files are being normalized.
const BLOCKED_BY_NORMALIZATION: &str = "Wait for normalization to finish before processing";

/// Status text when processing is asked for while a run is already in progress.
const ALREADY_PROCESSING: &str = "Plugin processing already in progress";

/// Message when processing is asked for without an enabled plugin.
const EMPTY_CHAIN: &str =
    "The plugin chain has no enabled plugins. Add plugins from the Plugins menu or the Plugin Chain window.";

impl AppView {
    /// Returns true while a plugin processing run is in progress.
    pub(super) fn is_processing(&self) -> bool {
        self.process_task.is_some() && self.process_run.as_ref().is_some_and(|run| !run.completed)
    }

    /// Returns true when the selected file can be processed now.
    /// It runs on every render, so it leaves checking that the file still exists to `process_selected`.
    pub(super) fn can_process_selected(&self, cx: &Context<Self>) -> bool {
        !self.is_demo()
            && self.process_coordinator.is_some()
            && self.processing_blocked_reason().is_none()
            && self.plugins.chain.enabled_count() > 0
            && self.selected_record(cx).is_some()
    }

    /// Processes the selected file with the plugin chain.
    pub(super) fn process_selected(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let records = self.selected_ready_records(cx);
        if records.is_empty() {
            self.status = SharedString::from("Select an analyzed file to process");
            cx.notify();
            return;
        }
        if records.iter().any(|record| !record.path.is_file()) {
            self.status = SharedString::from("A selected file no longer exists");
            cx.notify();
            return;
        }
        self.start_processing(records, window, cx);
    }

    /// Asks for confirmation, then processes every analyzed file in the list with the plugin chain.
    pub(super) fn process_all(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.report_blocked_processing(cx) {
            return;
        }
        let records: Vec<AnalysisRecord> = {
            let delegate = self.table.read(cx).delegate();
            (0..delegate.len())
                .filter_map(|index| delegate.row(index).and_then(|row| row.ready_record()).cloned())
                .filter(|record| record.path.is_file())
                .collect()
        };
        if records.is_empty() {
            self.status = SharedString::from("No analyzed files to process");
            cx.notify();
            return;
        }
        let files = if records.len() == 1 { "file" } else { "files" };
        let detail = format!(
            "Process {} {files} with the plugin chain ({})?\n\nEach file is written as 16-bit AIFF next to its source, \
             and converted sources are moved to the system trash.",
            records.len(),
            self.chain_summary()
        );
        let answer = window.prompt(
            PromptLevel::Warning,
            OperationKind::Process.dialog_title(),
            Some(&detail),
            &["Process", "Cancel"],
            cx,
        );
        cx.spawn_in(window, async move |this, cx| {
            if answer.await == Ok(0) {
                let _ = this.update_in(cx, |view, window, cx| view.start_processing(records, window, cx));
            }
        })
        .detach();
    }

    /// Cancels the processing run.
    /// Files being rendered stop at their next block and keep their originals, and their rows show their records again.
    /// The events of the run are still drained, since a file that was already past rendering may still commit,
    /// and its row must then show the output that replaced the source.
    pub(super) fn cancel_processing(&mut self, cx: &mut Context<Self>) {
        if !self.is_processing() {
            return;
        }
        if let Some(coordinator) = &self.process_coordinator {
            coordinator.cancel();
        }
        if let Some(run) = &mut self.process_run {
            run.cancelled = true;
            run.completed = true;
        }
        self.process_winding_down = true;
        self.table.update(cx, |state, cx| {
            state.delegate_mut().finish_interrupted_rows();
            cx.notify();
        });
        self.status = SharedString::from("Plugin processing cancelled");
        cx.notify();
    }

    /// Discards presentation of the run but retains its drain until every writer has returned.
    pub(super) fn stop_processing(&mut self) {
        if let Some(coordinator) = &self.process_coordinator {
            coordinator.cancel();
        }
        // Removing the run rejects queued results, not the receiver's terminal disconnect.
        discard_run(&mut self.process_run, &mut self.process_winding_down);
    }

    /// Switches the normalization before the plugins on or off, for this session.
    pub(super) fn set_normalize_first(&mut self, normalize_first: bool, cx: &mut Context<Self>) {
        self.normalize_first = normalize_first;
        cx.notify();
    }

    /// Returns why processing cannot start now, or none when it can.
    fn processing_blocked_reason(&self) -> Option<&'static str> {
        if self.is_processing() {
            Some(ALREADY_PROCESSING)
        } else if self.is_normalizing() {
            Some(BLOCKED_BY_NORMALIZATION)
        } else if self.is_winding_down() {
            Some(CANCELLED_RUN_FINISHING)
        } else if self.is_analyzing() {
            Some(BLOCKED_BY_ANALYSIS)
        } else {
            None
        }
    }

    /// Shows why processing cannot start now. Returns true when it cannot.
    fn report_blocked_processing(&mut self, cx: &mut Context<Self>) -> bool {
        if let Some(reason) = self.processing_blocked_reason() {
            self.status = SharedString::from(reason);
            cx.notify();
            return true;
        }
        false
    }

    /// Starts processing the records: captures the live settings, creates one chain per worker,
    /// and unloads any of the records from the player, so the files can be replaced on every platform.
    fn start_processing(&mut self, records: Vec<AnalysisRecord>, window: &mut Window, cx: &mut Context<Self>) {
        if records.is_empty() || self.is_demo() || self.report_blocked_processing(cx) {
            return;
        }
        let Some(coordinator) = self.process_coordinator.clone() else {
            self.status = SharedString::from("Plugin processing unavailable");
            cx.notify();
            return;
        };
        if self.plugins.chain.enabled_count() == 0 {
            show_warning(EMPTY_CHAIN, window, cx);
            return;
        }
        let Some(ui) = UiThread::current() else {
            return;
        };
        if let Err(error) = self.capture_plugin_states(cx) {
            self.report_status(format!("Could not capture plugin settings: {error:#}"), cx);
            return;
        }
        if let Err(error) = self.plugins.chain.save() {
            tracing::error!("{error:#}");
        }
        let chain = self.plugins.chain.chain();
        let mut chains = Vec::new();
        for _ in 0..coordinator.chains_needed(records.len()) {
            match ProcessingChain::instantiate(&ui, &self.plugins.dispatcher, &chain) {
                Ok(instances) => chains.push(instances),
                Err(error) => {
                    self.status = SharedString::from("Plugin load failed");
                    show_warning(&error.to_string(), window, cx);
                    cx.notify();
                    return;
                }
            }
        }
        self.artwork_rewrites
            .extend(records.iter().map(|record| record.path.clone()));
        if self
            .selected
            .as_ref()
            .is_some_and(|selected| records.iter().any(|record| &record.path == selected))
        {
            self.invalidate_artwork(cx);
        }
        if let Some(player) = &mut self.player
            && player
                .loaded_path()
                .is_some_and(|loaded| records.iter().any(|record| record.path == loaded))
        {
            player.unload();
        }
        self.table.update(cx, |state, cx| {
            let delegate = state.delegate_mut();
            for record in &records {
                delegate.mark_pending(&record.path);
            }
            cx.notify();
        });
        let options = ProcessingOptions {
            normalize_before_plugins: self
                .normalize_first
                .then(|| normalization_options(&self.settings, self.overwrite_existing)),
            overwrite_existing: self.overwrite_existing,
            ..ProcessingOptions::default()
        };
        let total = records.len();
        let (run_id, receiver) = coordinator.start(records, options, chains);
        let run = OperationRun::new(OperationKind::Process, run_id, total);
        self.status = run.progress_text().into();
        self.process_run = Some(run);
        self.process_task = Some(spawn_process_drain(receiver, window, cx));
        cx.notify();
    }

    /// Applies a batch of processing events to the table, the selection, and the status bar.
    fn apply_process_events(&mut self, events: Vec<ProcessingEvent>, window: &mut Window, cx: &mut Context<Self>) {
        let Some((run_id, cancelled)) = self.process_run.as_ref().map(|run| (run.run_id, run.cancelled)) else {
            return;
        };
        let mut completed = false;
        let mut replaced = false;
        for event in events.into_iter().filter(|event| event.run_id() == run_id) {
            // After a cancel, only outputs that replaced their source are applied.
            if cancelled && !matches!(&event, ProcessingEvent::Finished { result, .. } if result.output().is_some()) {
                continue;
            }
            match event {
                ProcessingEvent::Started { path, .. } => self.mark_processing(&path, 0.0, cx),
                ProcessingEvent::Progress { path, fraction, .. } => self.mark_processing(&path, fraction, cx),
                ProcessingEvent::Finished { result, .. } => {
                    if let Some(run) = &mut self.process_run {
                        run.record(&result);
                    }
                    self.apply_operation_result(*result, cx);
                    replaced = true;
                }
                ProcessingEvent::Completed { .. } => completed = true,
            }
        }
        if replaced {
            self.table.update(cx, |state, cx| {
                state.delegate_mut().sort_rows();
                cx.notify();
            });
            self.restore_selection(cx);
        }
        if let Some(run) = self.process_run.as_mut().filter(|run| !run.cancelled) {
            run.completed |= completed;
            self.status = if run.completed {
                run.summary()
            } else {
                run.progress_text()
            }
            .into();
        }
        if completed && let Some(run) = self.process_run.clone() {
            self.finish_operation_run(&run, window, cx);
        }
        cx.notify();
    }

    /// Shows the processed fraction of a file in its row.
    fn mark_processing(&self, path: &Path, fraction: f32, cx: &mut Context<Self>) {
        self.table.update(cx, |state, cx| {
            state.delegate_mut().mark_processing(path, fraction);
            cx.notify();
        });
    }
}

/// Shows a warning about plugin processing in a dialog.
fn show_warning(message: &str, window: &mut Window, cx: &mut Context<AppView>) {
    let answer = window.prompt(
        PromptLevel::Warning,
        OperationKind::Process.dialog_title(),
        Some(message),
        &["OK"],
        cx,
    );
    cx.spawn(async move |_, _| {
        let _ = answer.await;
    })
    .detach();
}

/// Drains the processing events on a timer and applies them to the view in batches.
/// It runs in the window, because the end of a run can show a dialog.
fn spawn_process_drain(receiver: Receiver<ProcessingEvent>, window: &Window, cx: &Context<AppView>) -> Task<()> {
    cx.spawn_in(window, async move |this, cx| {
        loop {
            let (batch, disconnected) = drain_events(&receiver);
            if !batch.is_empty()
                && this
                    .update_in(cx, |view, window, cx| view.apply_process_events(batch, window, cx))
                    .is_err()
            {
                break;
            }
            if disconnected {
                let _ = this.update(cx, |view, cx| {
                    view.process_winding_down = false;
                    if let Some(run) = &mut view.process_run {
                        run.completed = true;
                    }
                    view.artwork_rewrites.clear();
                    view.refresh_artwork(cx);
                    cx.notify();
                });
                break;
            }
            cx.background_executor().timer(ANALYSIS_POLL_INTERVAL).await;
        }
    })
}
