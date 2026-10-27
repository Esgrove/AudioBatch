//! Menu and transport actions of the main view.
//!
//! Covers the file dialogs, analyzing the selected file again, clearing its custom gain, removing it from the list,
//! moving it to the trash, revealing it in the file manager, clearing the list,
//! playback, the events of the controls column, the About dialog, and the synthetic stress test.
//! Removing or trashing files waits while files are being normalized or processed with plugins.

use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use audiobatch_playback::{Player, PlayerEvent};
use gpui_kit::{Context, PathPromptOptions, PromptLevel, SharedString, Task, Window};

use super::{APP_TITLE, AppView, DEFAULT_GAIN_DB, RunKind};
use crate::controls::{ControlsEvent, SliderControl};
use crate::demo::{DemoRandom, ROWS_PER_TICK, advance_row, demo_rows, demo_waveform};
use crate::files::reveal_in_file_manager;
use crate::waveform::{MINIMUM_ZOOM, PlayheadSource};

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
        cx.notify();
    }

    /// Removes the rows of files from the list, keeps them out of later scans,
    /// and selects the file that took the place of the first removed row.
    fn remove_paths(&mut self, paths: &[PathBuf], cx: &mut Context<Self>) {
        let first_index = {
            let delegate = self.table.read(cx).delegate();
            paths.iter().filter_map(|path| delegate.index_of(path)).min()
        };
        for path in paths {
            self.excluded.insert(path.clone());
            self.roots.retain(|root| root != path);
        }
        self.table.update(cx, |state, cx| {
            for path in paths {
                state.delegate_mut().remove(path);
            }
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
    }

    /// Moves the selected file to the system trash, forgets its cached result, and removes it from the list.
    /// Asks first unless `confirm` is false, like the C++ application, where Shift with the shortcut skips the prompt.
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
                let _ = this.update(cx, |view, cx| view.move_to_trash(&selected, cx));
            }
        })
        .detach();
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
                Ok(()) => {
                    if let Some(coordinator) = &self.coordinator
                        && let Err(error) = coordinator.cache().remove_analysis(path)
                    {
                        tracing::warn!("Failed to remove the cached result of {}: {error:#}", path.display());
                    }
                    trashed.push(path.clone());
                }
                Err(error) => {
                    tracing::warn!("Could not move {} to the trash: {error}", path.display());
                    first_failure
                        .get_or_insert_with(|| format!("Could not move {} to the trash: {error}", display_name(path)));
                }
            }
        }
        if !trashed.is_empty() {
            self.remove_paths(&trashed, cx);
        }
        self.status = first_failure
            .unwrap_or_else(|| describe_files("Moved", "to the trash", &trashed))
            .into();
        cx.notify();
    }

    /// Shows the selected file in the platform file manager.
    pub(super) fn reveal_selected(&mut self, cx: &mut Context<Self>) {
        let Some(selected) = self.selected.clone() else {
            return;
        };
        if let Err(error) = reveal_in_file_manager(&selected) {
            self.status = format!("Could not show {}: {error}", display_name(&selected)).into();
            cx.notify();
        }
    }

    /// Removes every file and folder and stops any run and playback.
    /// Files being normalized or processed finish their current block and keep their originals.
    pub(super) fn clear_list(&mut self, cx: &mut Context<Self>) {
        if let Some(coordinator) = &self.coordinator {
            coordinator.cancel();
        }
        self.cancel_normalization();
        self.stop_processing();
        self.analysis_task = None;
        let was_demo = self.demo_task.take().is_some();
        self.roots.clear();
        self.excluded.clear();
        self.run = None;
        self.clear_selection(cx);
        self.table.update(cx, |state, cx| {
            state.delegate_mut().clear();
            cx.notify();
        });
        if was_demo {
            self.connect_playhead(cx);
        }
        self.status = SharedString::default();
        cx.notify();
    }

    /// Cancels the running analysis, normalization, or processing, from the Cancel button or File > Cancel Run.
    /// Files a run has not committed keep their rows, like in the C++ application.
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
        self.is_analyzing() || self.is_normalizing() || self.is_processing()
    }

    /// Cancels the analysis run, settling the rows it left waiting or analyzing.
    fn cancel_analysis(&mut self, cx: &mut Context<Self>) {
        if let Some(coordinator) = &self.coordinator {
            coordinator.cancel();
        }
        self.analysis_task = None;
        if let Some(run) = &mut self.run {
            run.completed = true;
        }
        self.table.update(cx, |state, cx| {
            state.delegate_mut().finish_interrupted_rows();
            cx.notify();
        });
        self.status = SharedString::from("Analysis cancelled");
        cx.notify();
    }

    /// Plays the selected file, or stops playback while playing.
    pub(super) fn toggle_playback(&mut self, cx: &mut Context<Self>) {
        let selected = self.selected.clone();
        if let Some(player) = &mut self.player {
            if player.is_playing() {
                player.pause();
            } else {
                let loaded = player.loaded_path().map(Path::to_path_buf);
                match (loaded, selected) {
                    (Some(loaded), Some(selected)) if loaded != selected => player.load(&selected, 0.0, true),
                    (Some(_), _) => player.play(),
                    (None, Some(selected)) => player.load(&selected, 0.0, true),
                    (None, None) => {}
                }
            }
        }
        cx.notify();
    }

    /// Stops playback and returns to the start of the file.
    pub(super) fn stop_playback(&mut self, cx: &mut Context<Self>) {
        if let Some(player) = &mut self.player {
            player.stop();
        }
        cx.notify();
    }

    /// Handles what the user did in the controls column.
    pub(super) fn handle_controls_event(&mut self, event: ControlsEvent, window: &mut Window, cx: &mut Context<Self>) {
        match event {
            ControlsEvent::TogglePlayback => self.toggle_playback(cx),
            ControlsEvent::ResetSlider(control) => self.reset_slider(control, window, cx),
            ControlsEvent::OpenSettings => self.open_settings_window(cx),
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
        let (overview, loudness) = demo_waveform();
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

    /// Shows the version in a native dialog.
    pub(super) fn show_about(window: &mut Window, cx: &mut Context<Self>) {
        let detail = format!(
            "Version {}\n\nAnalyzes the peak, true peak, and EBU R128 loudness of audio files.\nRust version of AudioBatch.",
            env!("CARGO_PKG_VERSION")
        );
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
                    let events = view.player.as_ref().map(Player::poll_events).unwrap_or_default();
                    for event in events {
                        if crate::debug_enabled() {
                            tracing::info!("player: {event:?}");
                        }
                        if let PlayerEvent::Error { message, .. } = event {
                            view.status = message.into();
                        }
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
