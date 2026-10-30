//! The selection of the main window: the file the preview plays and the other selected files,
//! table selection events, Select All, the column choices, and the waveform of the selected file.

use std::path::{Path, PathBuf};

use audiobatch_core::AnalysisRecord;
use audiobatch_playback::Player;
use gpui_kit::component::table::{TableEvent, TableState};
use gpui_kit::{App, Context, Entity, Pixels, Window};

use super::AppView;
use super::preview::PreviewSource;
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
        let playing = self.player.as_ref().is_some_and(Player::is_playing)
            || self
                .start_after_cues
                .as_ref()
                .is_some_and(super::cue_start::CueStartIntent::should_play);
        let continue_playback = self.settings.continue_playback && playing;
        if matches!(event, TableEvent::ColumnWidthsChanged(_)) {
            cx.notify();
            return;
        }
        let state = table.read(cx);
        let request = table_selection_request(
            event,
            state.selected_row(),
            self.selected.as_deref(),
            state.delegate(),
            continue_playback,
        );
        if let Some((path, play)) = request {
            // Pointer intent is handled by the row's mouse-down callback, including clicking the preview anchor.
            // Cursor restoration emits the same event, so a same-path event must preserve the batch selection.
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
        let changed = self.selected.as_deref() != Some(path)
            || self
                .preview_source
                .needs_reload(path, self.player.as_ref().and_then(Player::loaded_path));
        if changed {
            self.preview_source.begin(path);
        }
        self.selected = Some(path.to_path_buf());
        self.table.update(cx, |state, _| {
            state.delegate_mut().set_selected_path(Some(path.to_path_buf()));
        });
        if self.is_demo() {
            cx.notify();
            return;
        }
        self.request_artwork(Some(path), cx);
        if changed {
            self.selected_tags = None;
            self.clear_tag_marks(cx);
            self.tags_task = Some(self.spawn_tag_read(path.to_path_buf(), cx));
            // Each file starts at the whole-file zoom.
            self.waveform
                .update(cx, |waveform, cx| waveform.set_zoom(MINIMUM_ZOOM, cx));
            self.zoom_reset_pending = true;
        }
        // With playing from the first cue on, a new file stays silent until its cue points are read,
        // so it starts at the cue instead of jumping there.
        let wait_for_cues = changed && play && self.settings.play_from_first_cue_point;
        if changed {
            self.start_after_cues = self.settings.play_from_first_cue_point.then(|| {
                if play {
                    super::cue_start::CueStartIntent::new(path)
                } else {
                    super::cue_start::CueStartIntent::seek(path)
                }
            });
        } else if play {
            self.start_after_cues = None;
        }
        if let Some(player) = &mut self.player {
            if changed || player.loaded_path() != Some(path) {
                player.set_insert_gain(1.0);
                player.load(path, 0.0, play && !wait_for_cues);
            } else if play {
                player.play();
            }
        }
        // Load first so the previous decoder cannot authorize presentation or gain for the newly selected file.
        self.refresh_selected_waveform(cx);
        self.apply_preview_gain(cx);
        cx.notify();
    }

    /// Clears the selection and the preview.
    pub(super) fn clear_selection(&mut self, cx: &mut Context<Self>) {
        self.start_after_cues = None;
        self.preview_source.clear();
        self.selected = None;
        self.selected_tags = None;
        self.tags_task = None;
        self.request_artwork(None, cx);
        self.table
            .update(cx, |state, _| state.delegate_mut().set_selected_path(None));
        if let Some(player) = &mut self.player {
            player.set_insert_gain(1.0);
            player.unload();
        }
        self.table.update(cx, TableState::clear_selection);
        self.clear_tag_marks(cx);
        self.waveform
            .update(cx, |waveform, cx| waveform.show_placeholder(NO_FILE_TEXT, cx));
    }

    /// Shows a waveform and source warnings only for the captured decoder version or a current static source.
    /// An absent decoder does not discard useful analysis, but an old decoder cannot borrow new analysis.
    pub(super) fn refresh_selected_waveform(&self, cx: &mut Context<Self>) {
        let Some(selected) = self.selected.clone() else {
            return;
        };
        let row = self.table.read(cx).delegate().row_for(&selected).cloned();
        let loaded_path = self.player.as_ref().and_then(Player::loaded_path);
        self.waveform.update(cx, |view, cx| match row {
            None => view.show_placeholder(NO_FILE_TEXT, cx),
            Some(row) => {
                if let Some(record) = row.ready_record() {
                    if presentation_matches_source(&self.preview_source, loaded_path, record) {
                        view.set_record(record, cx);
                    } else {
                        view.show_placeholder("Analysis does not match the preview source", cx);
                    }
                } else {
                    view.show_placeholder(placeholder_text(&row), cx);
                }
            }
        });
    }
}

/// Resolves a current table event into a source selection and its playback request.
/// The table can publish a selection after its rows or cursor have already changed.
fn table_selection_request(
    event: &TableEvent,
    cursor: Option<usize>,
    selected: Option<&Path>,
    delegate: &ResultsDelegate,
    continue_playback: bool,
) -> Option<(PathBuf, bool)> {
    let (index, play) = match event {
        TableEvent::SelectRow(index) if cursor == Some(*index) => (*index, continue_playback),
        TableEvent::DoubleClickedRow(index) => (*index, true),
        _ => return None,
    };
    let path = &delegate.row(index)?.path;
    // Owner-driven cursor restoration is not a new selection or an explicit play request.
    if matches!(event, TableEvent::SelectRow(_)) && selected == Some(path.as_path()) {
        return None;
    }
    Some((path.clone(), play))
}

/// Allows static analysis without a decoder, but binds a loaded decoder's waveform and warnings to its capture.
/// Checking disk identity also rejects a result that became stale before any replacement analysis arrived.
pub(super) fn presentation_matches_source(
    source: &PreviewSource,
    loaded_path: Option<&Path>,
    record: &AnalysisRecord,
) -> bool {
    record.is_ready()
        && loaded_path.map_or_else(
            || record.matches_file_on_disk(),
            |path| path == record.path && source.matches_record(record),
        )
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

#[cfg(test)]
mod test_table_selection_intent {
    use super::*;
    use crate::app::cue_start::CueStartIntent;

    /// Returns the production event decision for two listed sources without opening a player.
    fn request(event: &TableEvent, cursor: usize, continuation: bool) -> Option<(PathBuf, bool)> {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_files(&[PathBuf::from("/a.wav"), PathBuf::from("/b.wav")]);
        table_selection_request(event, Some(cursor), Some(Path::new("/a.wav")), &delegate, continuation)
    }

    #[test]
    fn restored_anchor_never_requests_play_or_reselection() {
        for continuation in [false, true] {
            assert_eq!(request(&TableEvent::SelectRow(0), 0, continuation), None);
        }
    }

    #[test]
    fn restored_anchor_preserves_pending_play_and_seek_cues() {
        let path = Path::new("/a.wav");
        for mut intent in [CueStartIntent::new(path), CueStartIntent::seek(path)] {
            intent.receive_cues(path, std::iter::once(3.0));
            assert_eq!(request(&TableEvent::SelectRow(0), 0, intent.should_play()), None);
            assert_eq!(intent.position(Some(path), Some(30.0)), Some(3.0));
        }
    }

    #[test]
    fn another_source_continues_only_when_requested() {
        for continuation in [false, true] {
            assert_eq!(
                request(&TableEvent::SelectRow(1), 1, continuation),
                Some((PathBuf::from("/b.wav"), continuation))
            );
        }
        assert_eq!(request(&TableEvent::SelectRow(0), 1, true), None);
    }

    #[test]
    fn explicit_activation_of_the_anchor_still_requests_play() {
        assert_eq!(
            request(&TableEvent::DoubleClickedRow(0), 0, false),
            Some((PathBuf::from("/a.wav"), true))
        );
    }
}

#[cfg(test)]
mod test_preview_presentation_authority {
    use std::io::Write as _;

    use audiobatch_core::AnalysisStatus;
    use audiobatch_core::record::file_identity;

    use super::*;

    /// Makes source metadata without a player or a GPUI context.
    fn record(path: &Path) -> AnalysisRecord {
        let (file_size, modified_time_ms) = file_identity(path).expect("source identity");
        AnalysisRecord {
            path: path.to_path_buf(),
            file_size,
            modified_time_ms,
            status: AnalysisStatus::Analyzed,
            ..AnalysisRecord::default()
        }
    }

    #[test]
    fn static_presentation_requires_current_ready_analysis_but_no_decoder() {
        let mut file = tempfile::NamedTempFile::new().expect("source");
        let mut record = record(file.path());
        let source = PreviewSource::default();
        assert!(presentation_matches_source(&source, None, &record));
        record.status = AnalysisStatus::Failed;
        assert!(!presentation_matches_source(&source, None, &record));
        record.status = AnalysisStatus::Analyzed;
        file.write_all(b"changed source").expect("replacement");
        assert!(!presentation_matches_source(&source, None, &record));
    }

    #[test]
    fn loaded_presentation_requires_the_same_capture_not_just_the_same_path() {
        let mut file = tempfile::NamedTempFile::new().expect("source");
        let old_record = record(file.path());
        let mut source = PreviewSource::default();
        source.begin(file.path());
        assert!(presentation_matches_source(&source, Some(file.path()), &old_record));
        file.write_all(b"new source").expect("replacement");
        let new_record = record(file.path());
        assert!(!presentation_matches_source(&source, Some(file.path()), &old_record));
        assert!(!presentation_matches_source(&source, Some(file.path()), &new_record));
        source.begin(file.path());
        assert!(presentation_matches_source(&source, Some(file.path()), &new_record));
        assert!(!presentation_matches_source(
            &source,
            Some(Path::new("another.wav")),
            &new_record,
        ));
    }

    #[test]
    fn clear_then_reselect_never_reauthorizes_an_old_tag_generation() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let mut source = PreviewSource::default();
        source.begin(file.path());
        let (version, generation) = source.token().expect("first load");
        source.clear();
        source.begin(file.path());
        assert!(!source.accepts(&version, generation));
        let (current_version, current_generation) = source.token().expect("second load");
        assert!(source.accepts(&current_version, current_generation));
    }
}
