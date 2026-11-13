//! Menu and transport actions of the main view.
//!
//! Covers the file dialogs, analyzing the selected file again, clearing its custom gain, removing it from the list,
//! moving it to the trash, revealing it in the file manager or opening its folder, clearing the list,
//! playback, the events of the controls column, the About dialog, and the synthetic stress test.
//! Removing or trashing files waits while files are being normalized or processed with plugins.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use audiobatch_core::file_selection::MAXIMUM_REVEAL_FOLDERS;
use audiobatch_playback::{Player, PlayerEvent};
use gpui_kit::{Context, Entity, PathPromptOptions, PromptLevel, SharedString, Task, Window};

use super::gain::{MAXIMUM_GAIN_DB, MINIMUM_GAIN_DB};
use super::{APP_TITLE, AppView, DEFAULT_GAIN_DB, RunKind};
use crate::build_info::about_message;
use crate::controls::{ControlsEvent, SliderControl};
use crate::demo::{DemoRandom, ROWS_PER_TICK, advance_row, demo_rows, demo_waveform};
use crate::files::{open_parent_folders, reveal_in_file_manager};
use crate::playback_status::format_playback_time;
use crate::waveform::{MAXIMUM_ZOOM, MINIMUM_ZOOM, PlayheadSource, WaveformEvent, WaveformView};

/// How often player events are checked.
const PLAYER_POLL_INTERVAL: Duration = Duration::from_millis(100);

/// How often the stress test updates rows, about thirty times per second.
const DEMO_TICK_INTERVAL: Duration = Duration::from_millis(33);

impl AppView {
    /// Scans the listed folders again, analyzing only new and changed files.
    pub(super) fn rescan(&mut self, cx: &mut Context<Self>) {
        self.start_run(RunKind::Scan, self.roots.clone(), false, cx);
    }

    /// Analyzes the selected files again, ignoring their cached results.
    /// Waits for a scan in progress, whose files would otherwise be left waiting.
    pub(super) fn reanalyze_selected(&mut self, cx: &mut Context<Self>) {
        let selected = self.selected_paths(cx);
        if selected.is_empty() {
            return;
        }
        if self
            .run
            .as_ref()
            .is_some_and(|run| run.kind == RunKind::Scan && self.is_analyzing())
        {
            self.status = SharedString::from("Wait for the scan to finish before analyzing again");
            self.status_sticky = true;
            cx.notify();
            return;
        }
        self.start_run(RunKind::Reanalyze, selected, true, cx);
    }

    /// Removes the selected file from the list and selects the file that took its place.
    pub(super) fn remove_selected(&mut self, cx: &mut Context<Self>) {
        let selected = self.selected_paths(cx);
        if selected.is_empty() || self.blocked_by_file_operation(cx) {
            return;
        }
        self.remove_paths(&selected, cx);
        self.status = describe_files("Removed", "from the list", &selected).into();
        self.status_sticky = true;
        self.sync_action_menus(cx);
        cx.notify();
    }

    /// Moves the selected file to the system trash, forgets its cached result, and removes it from the list.
    /// Asks first unless `confirm` is false, as it is for the shortcut with Shift held.
    pub(super) fn trash_selected(&mut self, confirm: bool, window: &mut Window, cx: &mut Context<Self>) {
        let selected = self.selected_paths(cx);
        if selected.is_empty() || self.blocked_by_file_operation(cx) {
            return;
        }
        if !confirm {
            self.move_to_trash(&selected, cx);
            return;
        }
        let detail = match selected.as_slice() {
            [single] => format!("Move\n\n{}\n\nto the system trash?", single.display()),
            _ => format!("Move {} selected files to the system trash?", selected.len()),
        };
        let answer = window.prompt(
            PromptLevel::Warning,
            "Move to Trash",
            Some(&detail),
            &["Move to Trash", "Cancel"],
            cx,
        );
        cx.spawn_in(window, async move |this, cx| {
            if answer.await == Ok(0) {
                let _ = this.update(cx, |view, cx| {
                    let remaining: Vec<PathBuf> = {
                        let delegate = view.table.read(cx).delegate();
                        selected
                            .into_iter()
                            .filter(|path| delegate.row_for(path).is_some())
                            .collect()
                    };
                    if !remaining.is_empty() {
                        view.move_to_trash(&remaining, cx);
                    }
                });
            }
        })
        .detach();
    }

    /// Shows the selected files in the platform file manager, one window per parent folder.
    pub(super) fn reveal_selected(&mut self, cx: &mut Context<Self>) {
        let selected = self.selected_paths(cx);
        if selected.is_empty() {
            return;
        }
        let result = reveal_in_file_manager(&selected);
        self.report_folder_action(result, "show", cx);
    }

    /// Opens the parent folders of the selected files in the platform file manager.
    pub(super) fn open_parent_folder(&mut self, cx: &mut Context<Self>) {
        let selected = self.selected_paths(cx);
        if selected.is_empty() {
            return;
        }
        let result = open_parent_folders(&selected);
        self.report_folder_action(result, "open", cx);
    }

    /// Shows in the status when the file manager could not start, or when the folder limit left folders out.
    fn report_folder_action(&mut self, result: std::io::Result<usize>, verb: &str, cx: &mut Context<Self>) {
        let status = match result {
            Err(error) => format!("Could not {verb} the selected files: {error}"),
            Ok(0) => return,
            Ok(skipped) => folder_limit_status(skipped),
        };
        self.status = status.into();
        self.status_sticky = true;
        cx.notify();
    }

    /// Removes every file and folder and stops any run and playback.
    /// Files being normalized or processed finish their current block and keep their originals.
    pub(super) fn clear_list(&mut self, cx: &mut Context<Self>) {
        self.flush_gain_store(cx);
        self.enrichment.invalidate();
        if let Some(coordinator) = &self.coordinator {
            coordinator.cancel();
        }
        self.cancel_normalization();
        self.stop_processing();
        if let Some(drain) = self.analysis_task.take() {
            drain.detach();
        }
        let was_demo = self.demo_task.take().is_some();
        self.roots.clear();
        self.excluded.clear();
        self.run = None;
        self.clear_selection(cx);
        self.artwork_rewrites.clear();
        self.table.update(cx, |state, cx| {
            state.delegate_mut().clear();
            cx.notify();
        });
        if was_demo {
            self.connect_playhead(cx);
        }
        self.status = SharedString::default();
        self.status_sticky = false;
        self.sync_action_menus(cx);
        cx.notify();
    }

    /// Cancels the running analysis, normalization, or processing, from the Cancel button or File > Cancel Run.
    /// Files a run has not committed keep their rows.
    pub(super) fn cancel_run(&mut self, cx: &mut Context<Self>) {
        if self.is_processing() {
            self.cancel_processing(cx);
        } else if self.is_normalizing() {
            self.cancel_normalization_run(cx);
        } else if self.is_analyzing() {
            self.cancel_analysis(cx);
        }
    }

    /// Returns true during an analysis, normalization, or processing run.
    pub(super) fn is_running(&self) -> bool {
        self.is_analyzing() || self.is_normalizing() || self.is_processing() || self.is_winding_down()
    }

    /// Plays the selected file, or stops playback while playing.
    pub(super) fn toggle_playback(&mut self, cx: &mut Context<Self>) {
        // An explicit play or stop wins over a start that waits for the cue points.
        self.start_after_cues = None;
        if self.player.as_ref().is_some_and(Player::is_playing) {
            if let Some(player) = &mut self.player {
                player.pause();
            }
            cx.notify();
            return;
        }
        let Some(selected) = self.selected.clone() else {
            cx.notify();
            return;
        };
        let needs_load = self.player.as_ref().is_some_and(|player| {
            player.loaded_path() != Some(selected.as_path())
                || self.preview_source.needs_reload(&selected, player.loaded_path())
        });
        if needs_load {
            self.select_path(&selected, true, cx);
            return;
        }
        if let Some(player) = &mut self.player {
            player.play();
        }
        cx.notify();
    }

    /// Stops playback and returns to the start of the file.
    pub(super) fn stop_playback(&mut self, cx: &mut Context<Self>) {
        self.start_after_cues = None;
        if let Some(player) = &mut self.player {
            player.stop();
        }
        cx.notify();
    }

    /// Handles what the user did in the controls column.
    pub(super) fn handle_controls_event(&mut self, event: ControlsEvent, window: &mut Window, cx: &mut Context<Self>) {
        match event {
            ControlsEvent::TogglePlayback => self.toggle_playback(cx),
            ControlsEvent::TogglePreviewSection => self.toggle_preview_section(cx),
            ControlsEvent::ResetSlider(control) => self.reset_slider(control, window, cx),
            ControlsEvent::OpenSettings => self.open_settings_window(window, cx),
            ControlsEvent::SetNormalizeMode(mode) => self.set_normalize_mode(mode, cx),
            ControlsEvent::SetOverwrite(overwrite) => {
                self.overwrite_existing = overwrite;
                cx.notify();
            }
            ControlsEvent::NormalizeSelected => self.normalize_selected(window, cx),
            ControlsEvent::SetNormalizeFirst(normalize_first) => self.set_normalize_first(normalize_first, cx),
            ControlsEvent::ProcessSelected => self.process_selected(window, cx),
            ControlsEvent::CancelRun => self.cancel_run(cx),
            ControlsEvent::OpenPluginChain => self.open_chain_window(cx),
            ControlsEvent::SetPreviewChain(enabled) => {
                if enabled != self.settings.preview_plugin_chain {
                    self.toggle_plugin_preview(cx);
                }
            }
        }
    }

    /// Starts the synthetic stress test with the given number of rows.
    pub(super) fn start_demo(&mut self, rows: usize, cx: &mut Context<Self>) {
        let mut random = DemoRandom::new(0x00a0_d10b);
        let generated = demo_rows(rows, &mut random);
        self.table.update(cx, |state, cx| {
            state.delegate_mut().set_rows(generated);
            cx.notify();
        });
        let crate::demo::DemoWaveform { overview, loudness } = demo_waveform();
        let playhead = Some(PlayheadSource::Simulated {
            started: Instant::now(),
            duration: overview.duration_seconds(),
        });
        self.waveform.update(cx, |view, cx| {
            view.show(Some(overview), Some(loudness), cx);
            view.set_playhead(playhead, cx);
        });
        self.roots = vec![PathBuf::from("/demo")];
        self.status = format!("Stress test: {rows} rows, {ROWS_PER_TICK} updates every {DEMO_TICK_INTERVAL:?}").into();
        let table = self.table.downgrade();
        self.demo_task = Some(cx.spawn(async move |_, cx| {
            loop {
                cx.background_executor().timer(DEMO_TICK_INTERVAL).await;
                let updated = table.update(cx, |state, cx| {
                    let delegate = state.delegate_mut();
                    let count = delegate.len();
                    for _ in 0..ROWS_PER_TICK {
                        let index = random.next_index(count);
                        delegate.update_row(index, |row| advance_row(row, &mut random));
                    }
                    cx.notify();
                });
                if updated.is_err() {
                    break;
                }
            }
        }));
        cx.notify();
    }

    /// Handles seeks, wheel zoom, and wheel gain from the waveform.
    pub(super) fn on_waveform_event(
        &mut self,
        _waveform: &Entity<WaveformView>,
        event: &WaveformEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match event {
            WaveformEvent::SeekAndPlayRequested(seconds) => self.seek_and_play_selected(*seconds, cx),
            WaveformEvent::RestartRequested => self.seek_and_play_selected(0.0, cx),
            WaveformEvent::ZoomBy(factor) => {
                let current = f64::from(self.zoom_slider.read(cx).value().start());
                let zoom = (current * factor).clamp(MINIMUM_ZOOM, MAXIMUM_ZOOM);
                self.zoom_slider
                    .update(cx, |slider, cx| slider.set_value(zoom as f32, window, cx));
                self.waveform.update(cx, |waveform, cx| waveform.set_zoom(zoom, cx));
            }
            WaveformEvent::GainBy(decibels) if self.can_adjust_gain(cx) => {
                // Rounding to the slider step keeps repeated steps from accumulating float error.
                let current = self.gain_slider.read(cx).value().start();
                let gain = ((current + decibels) * 10.0).round() / 10.0;
                let gain = gain.clamp(MINIMUM_GAIN_DB, MAXIMUM_GAIN_DB);
                self.gain_slider
                    .update(cx, |slider, cx| slider.set_value(gain, window, cx));
                self.change_selected_gain(gain, cx);
                self.schedule_gain_store(cx);
            }
            WaveformEvent::GainBy(_) => {}
        }
        cx.notify();
    }

    /// Opens a dialog for choosing folders or files, then replaces or adds to the list.
    pub(super) fn prompt_for_paths(directories: bool, replace: bool, cx: &Context<Self>) {
        let receiver = cx.prompt_for_paths(PathPromptOptions {
            files: !directories,
            directories,
            multiple: !replace,
            prompt: None,
        });
        cx.spawn(async move |this, cx| {
            if let Ok(Ok(Some(paths))) = receiver.await {
                let _ = this.update(cx, |view, cx| {
                    if replace {
                        view.replace_roots(&paths, cx);
                    } else {
                        view.add_roots(&paths, cx);
                    }
                });
            }
        })
        .detach();
    }

    /// Shows the version, build, and system information in a native dialog.
    pub(super) fn show_about(window: &mut Window, cx: &mut Context<Self>) {
        let detail = about_message();
        let answer = window.prompt(PromptLevel::Info, APP_TITLE, Some(&detail), &["OK"], cx);
        cx.spawn(async move |_, _| {
            let _ = answer.await;
        })
        .detach();
    }

    /// Polls player events, reporting errors and refreshing the Play/Stop button when playback ends.
    /// It also closes the output device once playback has stopped,
    /// and renders again when the output text of the status bar changes,
    /// for example when the first audio callback reports the buffer size.
    pub(super) fn spawn_player_poll(cx: &Context<Self>) -> Task<()> {
        cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(PLAYER_POLL_INTERVAL).await;
                let alive = this.update(cx, |view, cx| {
                    if let Some(player) = view.player.as_mut() {
                        player.close_idle_stream();
                    }
                    let events = view.player.as_ref().map_or_default(Player::poll_events);
                    for event in events {
                        if crate::debug_enabled() {
                            tracing::info!("player: {event:?}");
                        }
                        match event {
                            PlayerEvent::Error { message, .. } => {
                                view.start_after_cues = None;
                                view.status = message.into();
                                view.status_sticky = true;
                            }
                            PlayerEvent::Ended => view.continue_after_end(cx),
                            PlayerEvent::Loaded { .. } => {}
                        }
                        cx.notify();
                    }
                    view.resolve_cue_start();
                    view.update_gain_prediction(cx);
                    if let Some(preview) = view
                        .coordinator
                        .as_ref()
                        .and_then(|coordinator| coordinator.take_preview())
                    {
                        let token = view.preview_source.token();
                        if view.selected.as_deref() == Some(preview.path())
                            && token.is_some_and(|token| token.generation == preview.generation())
                            && preview.matches_file_on_disk()
                            && view
                                .table
                                .read(cx)
                                .delegate()
                                .row_for(preview.path())
                                .is_some_and(|row| row.ready_record().is_none())
                        {
                            view.waveform.update(cx, |waveform, cx| {
                                waveform.update_progressive_overview(
                                    Arc::clone(preview.waveform()),
                                    preview.expected_frames(),
                                    cx,
                                );
                            });
                        }
                    }
                    let playback_time = view.player.as_ref().and_then(|player| {
                        let duration = player.duration_seconds()?;
                        (duration > 0.0).then(|| format_playback_time(player.position_seconds(), duration))
                    });
                    if playback_time != view.shown_playback_time {
                        view.shown_playback_time = playback_time;
                        cx.notify();
                    }
                    let output = view.output_label();
                    if output != view.shown_output {
                        view.shown_output = output;
                        cx.notify();
                    }
                });
                if alive.is_err() {
                    break;
                }
            }
        })
    }

    /// Removes the rows of files from the list, keeps them out of later scans,
    /// and selects the file that took the place of the first removed row.
    fn remove_paths(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        self.flush_gain_store(cx);
        let first_index = {
            let delegate = self.table.read(cx).delegate();
            paths.iter().filter_map(|path| delegate.index_of(path)).min()
        };
        let removed: HashSet<PathBuf> = paths.iter().cloned().collect();
        self.enrichment.remove_paths(&removed);
        self.roots.retain(|root| !removed.contains(root));
        self.excluded.extend(removed.iter().cloned());
        self.table.update(cx, |state, cx| {
            state.delegate_mut().remove_many(&removed);
            cx.notify();
        });
        self.clear_selection(cx);
        let next = first_index.and_then(|index| {
            let delegate = self.table.read(cx).delegate();
            let index = index.min(delegate.len().checked_sub(1)?);
            delegate.row(index).map(|row| (index, row.path.clone()))
        });
        if let Some((index, path)) = next {
            self.table.update(cx, |state, cx| state.set_selected_row(index, cx));
            self.select_path(&path, false, cx);
        }
        self.resume_enrichment(cx);
    }

    /// Moves files to the system trash and removes their rows.
    /// A file that cannot be moved keeps its row, and the status names the first failure.
    fn move_to_trash(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        if self.blocked_by_file_operation(cx) {
            return;
        }
        let mut trashed = Vec::new();
        let mut first_failure = None;
        for path in paths {
            if let Some(player) = &mut self.player
                && player.loaded_path() == Some(path.as_path())
            {
                player.unload();
            }
            match trash::delete(path) {
                Ok(()) => trashed.push(path.clone()),
                Err(error) => {
                    tracing::warn!("Could not move {} to the trash: {error}", path.display());
                    first_failure
                        .get_or_insert_with(|| format!("Could not move {} to the trash: {error}", display_name(path)));
                }
            }
        }
        if let Some(coordinator) = &self.coordinator {
            let records = trashed
                .iter()
                .filter_map(|path| self.table.read(cx).delegate().row_for(path)?.record.clone())
                .collect::<Vec<_>>();
            let cache = std::sync::Arc::clone(coordinator.cache());
            cx.background_executor()
                .spawn(async move {
                    for record in records {
                        if let Err(error) = cache.remove_analysis_for_record(&record) {
                            tracing::warn!(
                                "Failed to remove the cached result of {}: {error}",
                                record.path.display()
                            );
                        }
                    }
                })
                .detach();
        }
        if !trashed.is_empty() {
            self.remove_paths(&trashed, cx);
        }
        self.status_sticky = first_failure.is_some() || !trashed.is_empty();
        self.status = first_failure
            .unwrap_or_else(|| describe_files("Moved", "to the trash", &trashed))
            .into();
        self.sync_action_menus(cx);
        cx.notify();
    }

    /// Cancels the analysis run, settling the rows it left waiting or analyzing.
    fn cancel_analysis(&mut self, cx: &mut Context<Self>) {
        tracing::info!("Analysis cancelled");
        if let Some(coordinator) = &self.coordinator {
            coordinator.cancel();
        }
        if let Some(drain) = self.analysis_task.take() {
            drain.detach();
        }
        if let Some(run) = &mut self.run {
            run.completed = true;
        }
        self.table.update(cx, |state, cx| {
            // Results already applied stay usable, without a SQLite busy wait on cancellation.
            state.delegate_mut().finish_cancelled_analysis(|_| None);
            cx.notify();
        });
        self.status = SharedString::from("Analysis cancelled");
        self.status_sticky = true;
        self.sync_action_menus(cx);
        cx.notify();
    }

    /// Resets a slider of the controls column to its default:
    /// the whole file for the zoom, no custom gain for the selected file, and the default normalization target.
    fn reset_slider(&mut self, control: SliderControl, window: &mut Window, cx: &mut Context<Self>) {
        match control {
            SliderControl::Zoom => {
                self.zoom_slider
                    .update(cx, |slider, cx| slider.set_value(MINIMUM_ZOOM as f32, window, cx));
                self.waveform
                    .update(cx, |waveform, cx| waveform.set_zoom(MINIMUM_ZOOM, cx));
            }
            SliderControl::Gain => {
                self.gain_slider
                    .update(cx, |slider, cx| slider.set_value(DEFAULT_GAIN_DB, window, cx));
                self.clear_selected_gain(cx);
            }
            SliderControl::NormalizeTarget => self.reset_normalize_target(window, cx),
        }
        cx.notify();
    }

    /// Seeks the selected file to `seconds` and starts playback, reloading the player and custom gain if unloaded.
    fn seek_and_play_selected(&mut self, seconds: f64, cx: &mut Context<Self>) {
        self.start_after_cues = None;
        let Some(selected) = self.selected.clone() else {
            return;
        };
        let needs_load = self
            .preview_source
            .needs_reload(&selected, self.player.as_ref().and_then(Player::loaded_path));
        if needs_load {
            self.prepare_preview_reload(&selected, cx);
            if let Some(player) = &mut self.player {
                player.set_insert_gain(1.0);
                player.load(&selected, seconds, true);
            }
            self.refresh_selected_waveform(cx);
            self.apply_preview_gain(cx);
            self.request_selected_enrichment(cx);
            self.update_gain_prediction(cx);
        } else if let Some(player) = &mut self.player {
            let playing = player.is_playing();
            player.seek(seconds);
            if !playing {
                player.play();
            }
        }
    }
}

/// Returns the file name of a path for status messages.
fn display_name(path: &Path) -> String {
    path.file_name().map_or_else(
        || path.display().to_string(),
        |name| name.to_string_lossy().into_owned(),
    )
}

/// Describes what an action did to files, naming a single file and counting several,
/// such as "Removed song.wav from the list" or "Removed 3 files from the list".
fn describe_files(action: &str, rest: &str, paths: &[PathBuf]) -> String {
    match paths {
        [single] => format!("{action} {} {rest}", display_name(single)),
        _ => format!("{action} {} files {rest}", paths.len()),
    }
}

/// Describes that revealing or opening the folders of the selection stopped at `MAXIMUM_REVEAL_FOLDERS`.
fn folder_limit_status(skipped: usize) -> String {
    let folders = if skipped == 1 { "folder" } else { "folders" };
    format!("Opened the first {MAXIMUM_REVEAL_FOLDERS} folders, {skipped} more {folders} left out")
}

#[cfg(test)]
mod test_pending_gain_removal {
    use audiobatch_core::record::{file_identity, same_custom_gain};
    use audiobatch_core::{AnalysisRecord, AnalysisStatus};

    use super::*;
    use crate::results::ResultsDelegate;

    /// Lists a ready source with metadata bound to its actual temporary file.
    fn delegate_for(path: &Path) -> ResultsDelegate {
        let audiobatch_core::record::FileIdentity {
            file_size,
            modified_time_ms,
        } = file_identity(path).expect("source identity");
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_record(AnalysisRecord {
            path: path.to_path_buf(),
            file_size,
            modified_time_ms,
            status: AnalysisStatus::Analyzed,
            ..AnalysisRecord::default()
        });
        delegate
    }

    #[test]
    fn remove_preserves_a_pending_gain_before_its_row_disappears() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let mut delegate = delegate_for(file.path());
        delegate.set_custom_gain(file.path(), Some(-3.0));
        let write = delegate.take_gain_write(file.path()).expect("pending gain write");
        let removed: HashSet<PathBuf> = std::iter::once(file.path().to_path_buf()).collect();
        delegate.remove_many(&removed);
        assert_eq!(write.path(), file.path());
        assert!(same_custom_gain(write.custom_gain_db(), Some(-3.0)));
        assert!(delegate.row_for(file.path()).is_none());
    }

    #[test]
    fn clear_preserves_latest_gains_including_an_explicit_clear() {
        let first = tempfile::NamedTempFile::new().expect("first source");
        let second = tempfile::NamedTempFile::new().expect("second source");
        let mut delegate = delegate_for(first.path());
        let second_record = delegate_for(second.path())
            .row_for(second.path())
            .expect("row")
            .record
            .clone()
            .expect("record");
        delegate.set_record(second_record);
        delegate.set_custom_gain(first.path(), Some(-3.0));
        delegate.set_custom_gain(second.path(), Some(2.0));
        delegate.set_custom_gain(first.path(), None);
        delegate.set_custom_gain(second.path(), Some(4.0));
        let first_write = delegate.take_gain_write(first.path()).expect("explicit clear");
        let second_write = delegate.take_gain_write(second.path()).expect("latest gain");
        assert!(delegate.take_gain_write(first.path()).is_none());
        delegate.clear();
        assert_eq!(first_write.path(), first.path());
        assert_eq!(first_write.custom_gain_db(), None);
        assert_eq!(second_write.path(), second.path());
        assert!(same_custom_gain(second_write.custom_gain_db(), Some(4.0)));
        assert_eq!(delegate.len(), 0);
    }

    #[test]
    fn removing_another_row_still_keeps_pending_clear_consumable() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let mut delegate = delegate_for(file.path());
        delegate.set_custom_gain(file.path(), None);
        let write = delegate.take_gain_write(file.path()).expect("explicit clear");
        delegate.remove_many(&HashSet::new());
        assert_eq!(write.path(), file.path());
        assert_eq!(write.custom_gain_db(), None);
        assert!(delegate.row_for(file.path()).is_some());
    }
}
