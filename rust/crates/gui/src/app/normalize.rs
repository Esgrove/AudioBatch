//! Normalizing files from the main view.
//!
//! A run normalizes the selected file, or every analyzed file after a confirmation,
//! on the normalization coordinator, which shares the analysis cache.
//! Analysis and normalization never run at the same time, and changes to the list wait for a run to finish.
//! Each result replaces the row of its source: a converted source makes way for its `.aif` output,
//! and a failure keeps the row with the reason in its Status column.
//! The end of a run shows its summary in the status bar and lists any failures in a dialog.

use std::path::Path;

use audiobatch_core::operation::{OperationOutcome, OperationResult};
use audiobatch_core::{AnalysisRecord, NormalizeEvent};
use crossbeam_channel::Receiver;
use gpui_kit::{Context, PromptLevel, SharedString, Task, Window};

use super::{AppView, drain_events};
use crate::menus::set_menus;
use crate::normalization::{
    ALREADY_NORMALIZING, BLOCKED_BY_ANALYSIS, NORMALIZATION_IN_PROGRESS, NormalizeRun, describe_target,
    normalization_options, set_target, target_range,
};
use crate::results::RowMessage;
use crate::settings::NormalizeMode;

/// Title of the normalization dialogs.
const DIALOG_TITLE: &str = "Normalize";

impl AppView {
    /// Returns true while a normalization run is in progress.
    pub(super) fn is_normalizing(&self) -> bool {
        self.normalize_task.is_some() && self.normalize_run.as_ref().is_some_and(|run| !run.completed)
    }

    /// Returns true when the selected file can be normalized now.
    /// It runs on every render, so it leaves checking that the file still exists to `normalize_selected`.
    pub(super) fn can_normalize_selected(&self, cx: &Context<Self>) -> bool {
        !self.is_demo()
            && self.normalize_coordinator.is_some()
            && self.normalization_blocked_reason().is_none()
            && self.selected_record(cx).is_some()
    }

    /// Shows that the list cannot change while files are being normalized.
    /// Returns true when the change has to wait.
    pub(super) fn blocked_by_normalization(&mut self, cx: &mut Context<Self>) -> bool {
        if self.is_normalizing() {
            self.status = SharedString::from(NORMALIZATION_IN_PROGRESS);
            cx.notify();
            return true;
        }
        false
    }

    /// Normalizes the selected file to the target.
    pub(super) fn normalize_selected(&mut self, window: &Window, cx: &mut Context<Self>) {
        let records: Vec<AnalysisRecord> = self.selected_record(cx).cloned().into_iter().collect();
        if records.is_empty() {
            self.status = SharedString::from("Select an analyzed file to normalize");
            cx.notify();
            return;
        }
        if records.iter().any(|record| !record.path.is_file()) {
            self.status = SharedString::from("The selected file no longer exists");
            cx.notify();
            return;
        }
        self.start_normalization(records, window, cx);
    }

    /// Asks for confirmation, then normalizes every analyzed file in the list to the target.
    pub(super) fn normalize_all(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.report_blocked_normalization(cx) {
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
            self.status = SharedString::from("No analyzed files to normalize");
            cx.notify();
            return;
        }
        let files = if records.len() == 1 { "file" } else { "files" };
        let detail = format!(
            "Normalize {} {files} to {}?\n\nEach file is written as AIFF next to its source, \
             and converted sources are moved to the system trash.",
            records.len(),
            describe_target(&self.settings)
        );
        let answer = window.prompt(
            PromptLevel::Warning,
            DIALOG_TITLE,
            Some(&detail),
            &["Normalize", "Cancel"],
            cx,
        );
        cx.spawn_in(window, async move |this, cx| {
            if answer.await == Ok(0) {
                let _ = this.update_in(cx, |view, window, cx| view.start_normalization(records, window, cx));
            }
        })
        .detach();
    }

    /// Switches the normalization mode and saves it.
    pub(super) fn set_normalize_mode(&mut self, mode: NormalizeMode, cx: &mut Context<Self>) {
        if self.settings.normalize_mode != mode {
            self.settings.normalize_mode = mode;
            self.settings.save();
            self.refresh_normalize_menus(cx);
        }
        cx.notify();
    }

    /// Stores the target of a mode while its slider moves.
    pub(super) fn change_normalize_target(&mut self, mode: NormalizeMode, value: f32, cx: &mut Context<Self>) {
        if set_target(&mut self.settings, mode, value) {
            cx.notify();
        }
    }

    /// Saves the target once the slider is released, and shows it in the menus.
    pub(super) fn store_normalize_target(&mut self, cx: &Context<Self>) {
        self.settings.save();
        self.refresh_normalize_menus(cx);
    }

    /// Resets the target of the current mode to its default.
    pub(super) fn reset_normalize_target(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let mode = self.settings.normalize_mode;
        let default = target_range(mode).default;
        self.normalize_target_slider(mode)
            .clone()
            .update(cx, |slider, cx| slider.set_value(default, window, cx));
        set_target(&mut self.settings, mode, default);
        self.store_normalize_target(cx);
        cx.notify();
    }

    /// Stops listening to the normalization run, letting files in progress finish with their originals intact.
    pub(super) fn cancel_normalization(&mut self) {
        if let Some(coordinator) = &self.normalize_coordinator {
            coordinator.cancel();
        }
        self.normalize_task = None;
        self.normalize_run = None;
    }

    /// Returns why normalization cannot start now, or none when it can.
    fn normalization_blocked_reason(&self) -> Option<&'static str> {
        if self.is_normalizing() {
            Some(ALREADY_NORMALIZING)
        } else if self.is_analyzing() {
            Some(BLOCKED_BY_ANALYSIS)
        } else {
            None
        }
    }

    /// Puts the normalization target into the labels of the normalize menu items.
    fn refresh_normalize_menus(&mut self, cx: &Context<Self>) {
        let target = describe_target(&self.settings);
        if self.menu_state.normalize_target != target {
            self.menu_state.normalize_target = target;
            set_menus(cx, &self.menu_state);
        }
    }

    /// Shows why normalization cannot start now. Returns true when it cannot.
    fn report_blocked_normalization(&mut self, cx: &mut Context<Self>) -> bool {
        if let Some(reason) = self.normalization_blocked_reason() {
            self.status = SharedString::from(reason);
            cx.notify();
            return true;
        }
        false
    }

    /// Starts normalizing the records, unloading any of them from the player first,
    /// so the files can be replaced on every platform.
    fn start_normalization(&mut self, records: Vec<AnalysisRecord>, window: &Window, cx: &mut Context<Self>) {
        if records.is_empty() || self.is_demo() || self.report_blocked_normalization(cx) {
            return;
        }
        let Some(coordinator) = self.normalize_coordinator.clone() else {
            self.status = SharedString::from("Normalization unavailable");
            cx.notify();
            return;
        };
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
        let total = records.len();
        let (run_id, receiver) =
            coordinator.start(records, normalization_options(&self.settings, self.overwrite_existing));
        let run = NormalizeRun::new(run_id, total);
        self.status = run.progress_text().into();
        self.normalize_run = Some(run);
        self.normalize_task = Some(spawn_normalize_drain(receiver, window, cx));
        cx.notify();
    }

    /// Applies a batch of normalization events to the table, the selection, and the status bar.
    fn apply_normalize_events(&mut self, events: Vec<NormalizeEvent>, window: &mut Window, cx: &mut Context<Self>) {
        let Some(run_id) = self.normalize_run.as_ref().map(|run| run.run_id) else {
            return;
        };
        let mut completed = false;
        let mut replaced = false;
        for event in events.into_iter().filter(|event| event.run_id() == run_id) {
            match event {
                NormalizeEvent::Started { path, .. } => self.mark_normalizing(&path, 0.0, cx),
                NormalizeEvent::Progress { path, fraction, .. } => self.mark_normalizing(&path, fraction, cx),
                NormalizeEvent::Finished { result, .. } => {
                    if let Some(run) = &mut self.normalize_run {
                        run.record(&result);
                    }
                    self.apply_normalization_result(*result, cx);
                    replaced = true;
                }
                NormalizeEvent::Completed { .. } => completed = true,
            }
        }
        // Sorting once per batch keeps normalizing a long list from sorting it again for every file.
        if replaced {
            self.table.update(cx, |state, cx| {
                state.delegate_mut().sort_rows();
                cx.notify();
            });
            self.restore_selection(cx);
        }
        if let Some(run) = &mut self.normalize_run {
            run.completed |= completed;
            self.status = if run.completed {
                run.summary()
            } else {
                run.progress_text()
            }
            .into();
        }
        if completed {
            self.finish_normalization(window, cx);
        }
        cx.notify();
    }

    /// Shows the normalized fraction of a file in its row.
    fn mark_normalizing(&self, path: &Path, fraction: f32, cx: &mut Context<Self>) {
        self.table.update(cx, |state, cx| {
            state.delegate_mut().mark_normalizing(path, fraction);
            cx.notify();
        });
    }

    /// Replaces the row of a normalized source with its result,
    /// and moves the selection and the listed roots over to a converted output.
    /// The caller sorts the rows and restores the selection after the batch.
    fn apply_normalization_result(&mut self, result: OperationResult, cx: &mut Context<Self>) {
        let source = result.source;
        let output_path = match &result.outcome {
            OperationOutcome::Failed { .. } => None,
            OperationOutcome::CommittedButUnanalyzed { output, .. } | OperationOutcome::Succeeded { output, .. } => {
                Some(output.path.clone())
            }
        };
        self.table.update(cx, |state, cx| {
            let delegate = state.delegate_mut();
            match result.outcome {
                OperationOutcome::Failed { error } => delegate.fail_normalization(&source, &error),
                OperationOutcome::CommittedButUnanalyzed { output, error } => {
                    let message = RowMessage {
                        text: error,
                        is_error: true,
                    };
                    delegate.replace_after_normalization(&source, output, Some(message));
                }
                OperationOutcome::Succeeded { output, notice } => {
                    let message = notice.map(|text| RowMessage { text, is_error: false });
                    delegate.replace_after_normalization(&source, output, message);
                }
            }
            cx.notify();
        });
        if let Some(output_path) = output_path {
            self.excluded.remove(&output_path);
            for root in &mut self.roots {
                if *root == source {
                    root.clone_from(&output_path);
                }
            }
            if self.selected.as_deref() == Some(source.as_path()) {
                self.selected = None;
                self.select_path(&output_path, false, cx);
            }
        } else if self.selected.as_deref() == Some(source.as_path()) {
            self.refresh_selected_waveform(cx);
        }
    }

    /// Reports the end of a run: the log line, and a dialog listing the failures.
    fn finish_normalization(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(run) = &self.normalize_run else {
            return;
        };
        if crate::debug_enabled() {
            eprintln!("{}", run.log_line());
        }
        if let Some(message) = run.failure_message() {
            let answer = window.prompt(PromptLevel::Warning, DIALOG_TITLE, Some(&message), &["OK"], cx);
            cx.spawn(async move |_, _| {
                let _ = answer.await;
            })
            .detach();
        }
        if self.selected.as_deref().is_some_and(|selected| !selected.exists()) {
            self.clear_selection(cx);
        }
    }
}

/// Drains the normalization events on a timer and applies them to the view in batches.
/// It runs in the window, because the end of a run can show a dialog.
fn spawn_normalize_drain(receiver: Receiver<NormalizeEvent>, window: &Window, cx: &Context<AppView>) -> Task<()> {
    cx.spawn_in(window, async move |this, cx| {
        loop {
            let (batch, disconnected) = drain_events(&receiver);
            if !batch.is_empty()
                && this
                    .update_in(cx, |view, window, cx| view.apply_normalize_events(batch, window, cx))
                    .is_err()
            {
                break;
            }
            if disconnected {
                break;
            }
            cx.background_executor().timer(super::ANALYSIS_POLL_INTERVAL).await;
        }
    })
}
