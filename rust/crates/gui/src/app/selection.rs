//! The selection of the main window: the file the preview plays and the other selected files,
//! table selection events, Select All, the column choices, and the waveform of the selected file.

use std::path::{Path, PathBuf};

use audiobatch_core::AnalysisRecord;
use audiobatch_playback::Player;
use gpui_kit::component::table::{TableEvent, TableState};
use gpui_kit::{App, Context, Entity, Pixels, Window};

use super::AppView;
use crate::menus::{ColumnChoice, set_menus};
use crate::results::{ResultRow, ResultsDelegate, RowState};
use crate::waveform::{MINIMUM_ZOOM, NO_FILE_TEXT};

impl AppView {
    /// Returns the usable result of the selected file.
    pub(super) fn selected_record<'app>(&self, cx: &'app App) -> Option<&'app AnalysisRecord> {
        let selected = self.selected.as_deref()?;
        self.table.read(cx).delegate().row_for(selected)?.ready_record()
    }

    /// Selects the row of the selected file again after the rows were sorted or replaced,
    /// because the table keeps its selection by index.
    /// Clears the selection when the file is no longer listed.
    pub(super) fn restore_selection(&mut self, cx: &mut Context<Self>) {
        let Some(selected) = self.selected.clone() else {
            return;
        };
        let index = self.table.read(cx).delegate().index_of(&selected);
        match index {
            Some(index) => self.table.update(cx, |state, cx| {
                if state.selected_row() != Some(index) {
                    state.set_selected_row(index, cx);
                }
            }),
            None if !self.is_demo() => self.clear_selection(cx),
            None => {}
        }
    }

    /// Handles row selection and double clicks in the table,
    /// and renders again after a column resize so the Status column fills the width again.
    /// With playback continuation on, selecting another file while one plays starts the new one.
    pub(super) fn on_table_event(
        &mut self,
        table: &Entity<TableState<ResultsDelegate>>,
        event: &TableEvent,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        // A file waiting for its cue points before it starts counts as playing, so moving on keeps playing.
        let playing = self.player.as_ref().is_some_and(Player::is_playing) || self.start_after_cues.is_some();
        let continue_playback = self.settings.continue_playback && playing;
        let (index, play) = match event {
            // Events arrive after the update that sent them.
            // A selection the rows have since moved away from would pick another file.
            TableEvent::SelectRow(index) if table.read(cx).selected_row() != Some(*index) => return,
            TableEvent::SelectRow(index) => (*index, continue_playback),
            TableEvent::DoubleClickedRow(index) => (*index, true),
            TableEvent::ColumnWidthsChanged(_) => {
                cx.notify();
                return;
            }
            _ => return,
        };
        let path = table.read(cx).delegate().row(index).map(|row| row.path.clone());
        if let Some(path) = path {
            // A plain click on another row selects only that file.
            // The table also selects the row of the same file again after a sort, which keeps the other selected files.
            if self.selected.as_deref() != Some(path.as_path()) {
                table.update(cx, |state, _| state.delegate_mut().select_only(Some(path.clone())));
            }
            self.select_path(&path, play, cx);
        }
    }

    /// Returns the selected files, the file the preview plays first.
    pub(super) fn selected_paths(&self, cx: &App) -> Vec<PathBuf> {
        self.table.read(cx).delegate().selected_paths()
    }

    /// Returns the analyzed records of the selected files, the file the preview plays first.
    pub(super) fn selected_ready_records(&self, cx: &App) -> Vec<AnalysisRecord> {
        let delegate = self.table.read(cx).delegate();
        delegate
            .selected_paths()
            .iter()
            .filter_map(|path| delegate.row_for(path).and_then(|row| row.ready_record()).cloned())
            .collect()
    }

    /// Shows or hides a results table column from Options > Columns, and saves the choice.
    pub(super) fn toggle_column(&mut self, id: &str, cx: &mut Context<Self>) {
        let Some(key) = ResultsDelegate::column_from_id(id) else {
            return;
        };
        let hidden = self.table.update(cx, |state, cx| {
            let widths: Vec<Pixels> = (0..).map_while(|index| state.col_width(index)).collect();
            let delegate = state.delegate_mut();
            delegate.remember_column_widths(&widths);
            let visible = delegate.is_column_visible(key);
            delegate.set_column_visible(key, !visible);
            let hidden = delegate.hidden_columns();
            state.refresh(cx);
            cx.notify();
            hidden
        });
        self.settings.hidden_columns = Some(
            hidden
                .into_iter()
                .map(|key| ResultsDelegate::column_identifier(key).to_string())
                .collect(),
        );
        self.settings.save();
        self.menu_state.columns = column_choices(self.table.read(cx).delegate());
        set_menus(cx, &self.menu_state);
        cx.notify();
    }

    /// Selects every file in the list, keeping the file the preview plays, or the first file when none is selected.
    pub(super) fn select_all(&mut self, cx: &mut Context<Self>) {
        if self.selected.is_none() {
            let first = self.table.read(cx).delegate().row(0).map(|row| row.path.clone());
            let Some(first) = first else {
                return;
            };
            self.table.update(cx, |state, cx| state.set_selected_row(0, cx));
            self.select_path(&first, false, cx);
        }
        self.table.update(cx, |state, cx| {
            state.delegate_mut().select_all();
            cx.notify();
        });
        cx.notify();
    }

    /// Selects a file: shows its waveform and loads it into the player, playing it when asked.
    pub(super) fn select_path(&mut self, path: &Path, play: bool, cx: &mut Context<Self>) {
        let changed = self.selected.as_deref() != Some(path);
        self.selected = Some(path.to_path_buf());
        self.table.update(cx, |state, _| {
            state.delegate_mut().set_selected_path(Some(path.to_path_buf()));
        });
        if self.is_demo() {
            cx.notify();
            return;
        }
        self.refresh_selected_waveform(cx);
        self.apply_preview_gain(cx);
        if changed {
            self.selected_tags = None;
            self.clear_tag_marks(cx);
            self.tags_task = Some(self.spawn_tag_read(path.to_path_buf(), cx));
            self.request_artwork(Some(path), cx);
            // Each file starts at the whole-file zoom.
            self.waveform
                .update(cx, |waveform, cx| waveform.set_zoom(MINIMUM_ZOOM, cx));
            self.zoom_reset_pending = true;
        }
        // With playing from the first cue on, a new file stays silent until its cue points are read,
        // so it starts at the cue instead of jumping there.
        let wait_for_cues = changed && play && self.settings.play_from_first_cue_point;
        self.start_after_cues = wait_for_cues.then(|| path.to_path_buf());
        if let Some(player) = &mut self.player {
            if changed || player.loaded_path() != Some(path) {
                player.load(path, 0.0, play && !wait_for_cues);
            } else if play {
                player.play();
            }
        }
        cx.notify();
    }

    /// Clears the selection and the preview.
    pub(super) fn clear_selection(&mut self, cx: &mut Context<Self>) {
        self.selected = None;
        self.selected_tags = None;
        self.tags_task = None;
        self.request_artwork(None, cx);
        self.table
            .update(cx, |state, _| state.delegate_mut().set_selected_path(None));
        if let Some(player) = &mut self.player {
            player.unload();
        }
        self.table.update(cx, TableState::clear_selection);
        self.clear_tag_marks(cx);
        self.waveform
            .update(cx, |waveform, cx| waveform.show_placeholder(NO_FILE_TEXT, cx));
    }

    /// Shows the waveform of the selected file, or a placeholder while it has no result.
    pub(super) fn refresh_selected_waveform(&self, cx: &mut Context<Self>) {
        let Some(selected) = self.selected.clone() else {
            return;
        };
        let row = self.table.read(cx).delegate().row_for(&selected).cloned();
        self.waveform.update(cx, |view, cx| match row {
            None => view.show_placeholder(NO_FILE_TEXT, cx),
            Some(row) => {
                if let Some(record) = row.ready_record() {
                    view.show(record.waveform.clone(), record.short_term_loudness.clone(), cx);
                } else {
                    view.show_placeholder(placeholder_text(&row), cx);
                }
            }
        });
    }
}

/// Returns the columns of the Columns submenu, checked while they show.
pub(super) fn column_choices(delegate: &ResultsDelegate) -> Vec<ColumnChoice> {
    ResultsDelegate::column_choices()
        .map(|(key, title)| ColumnChoice {
            id: ResultsDelegate::column_identifier(key).into(),
            title,
            visible: delegate.is_column_visible(key),
        })
        .collect()
}

/// Returns the waveform placeholder for a file without a usable result.
fn placeholder_text(row: &ResultRow) -> String {
    match row.state {
        RowState::Pending => String::from("Waiting for analysis"),
        RowState::Analyzing(fraction) => format!("Analyzing {:.0}%", fraction * 100.0),
        RowState::Normalizing(fraction) => format!("Normalizing {:.0}%", fraction * 100.0),
        RowState::Processing(fraction) => format!("Processing {:.0}%", fraction * 100.0),
        RowState::Done if row.has_error() => String::from("Could not read this file"),
        RowState::Done => String::from("No waveform for this file"),
    }
}
