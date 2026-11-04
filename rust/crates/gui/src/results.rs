//! The results table: one row per file with its analysis status and levels.
//!
//! The titles are short, and their tooltips describe each column with its unit, so the cells hold only numbers.
//! Columns can be reordered and resized by dragging their headers.
//! Name, Folder, Path, and Status share extra window width with ten times the weight of the other columns.
//! Folder shows the parent folder of each file, and the full Path column is hidden until the user shows it.
//! Failed files have white text with reduced opacity, cells have a faint divider between them,
//! and the Status column shows a progress ring while a file is analyzed, normalized, or processed with plugins.
//! A failure or notice of a normalization or plugin processing replaces the status text of its row,
//! with the full text as the tooltip.
//! `ResultsDelegate` implements the gpui-kit `TableDelegate`, which renders only the visible rows,
//! so lists of many thousands of files stay smooth.

use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::f32::consts::{FRAC_PI_2, TAU};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use audiobatch_core::format::{
    average_bitrate_kbps, format_bitrate_compact, format_bits_per_sample, format_custom_gain_compact, format_duration,
    format_folder, format_loudness_compact, format_peak_compact, format_sample_rate_compact, format_status,
    format_true_peak_compact, format_type_label,
};
use audiobatch_core::record::same_custom_gain;
use audiobatch_core::sorting::{SortMode, compare_by_mode, compare_natural};
use audiobatch_core::{AnalysisRecord, AnalysisStatus, MeasurementProducer, WaveformOverview};
use gpui_kit::component::menu::PopupMenu;
use gpui_kit::component::table::{Column, ColumnSort, TableDelegate, TableState};
#[cfg(test)]
use gpui_kit::test::TestSupportExt as _;
use gpui_kit::{
    App, AppContext as _, Bounds, ClickEvent, Context, Div, FocusHandle, InteractiveElement as _, IntoElement,
    MouseButton, MouseDownEvent, ParentElement as _, PathBuilder, Pixels, SharedString, Stateful,
    StatefulInteractiveElement as _, Styled as _, Window, canvas, div, point, px, rgba,
};

use crate::app::gain_writer::GainWrite;
use crate::file_drag::DraggedFiles;
use crate::files::{OPEN_PARENT_FOLDER_LABEL, REVEAL_LABEL};
use crate::menus::{
    ClearCustomGain, NormalizeSelected, OpenParentFolder, ProcessSelected, ReanalyzeSelected, RemoveSelected,
    RevealSelected, TrashSelected,
};
use crate::theme::{MONO_FONT, color, palette, tooltip, white};

/// Height of a table row.
pub const ROW_HEIGHT: f32 = 24.0;

/// Height of the header row.
pub const HEADER_HEIGHT: f32 = 28.0;

/// Font size of the cells.
const CELL_FONT_SIZE: f32 = 14.0;

/// Background of the rows selected besides the app's selected file, the colour the table gives its selected row.
const SELECTION_BACKGROUND: u32 = 0x76A8_DA38;

/// Columns hidden until the user shows them.
/// The Folder column shows where a file is more compactly than Path, which stays available from the Columns menu.
pub const DEFAULT_HIDDEN_COLUMNS: [ColumnKey; 4] = [
    ColumnKey::Path,
    ColumnKey::PeakLeft,
    ColumnKey::PeakRight,
    ColumnKey::Gain,
];

/// Font size of the column titles.
const HEADER_FONT_SIZE: f32 = 14.0;

/// Horizontal text inset inside a cell.
const CELL_INSET: f32 = 4.0;

/// Diameter of the activity ring in the Status column.
const RING_SIZE: f32 = 14.0;

/// Inner radius of the activity ring as a proportion of the outer radius.
const RING_INNER_PROPORTION: f32 = 0.52;

/// Opacity of the text of rows without errors.
const NORMAL_TEXT_ALPHA: f32 = 0.95;

/// Opacity of the text of rows whose analysis failed.
const ERROR_TEXT_ALPHA: f32 = 0.72;

/// Opacity of the status text of a waiting row.
const WAITING_TEXT_ALPHA: f32 = 0.55;

/// Opacity of the one pixel divider at the right edge of each cell.
const DIVIDER_ALPHA: f32 = 0.08;

/// Narrowest width the Status column shrinks to when the other columns take up the table.
const STATUS_MINIMUM_WIDTH: f32 = 180.0;

/// Widest width the Status column stretches to, past the 1200 pixel default limit of a column.
const STATUS_MAXIMUM_WIDTH: f32 = 8192.0;

/// Column definitions in the default order.
/// The short titles leave the units to the header tooltips, so the numeric columns stay narrow.
/// The widths fit typical values, and are wider where a title needs the room next to the sort indicator.
/// These are preferred widths, which automatic sizing adjusts to fit the visible columns in the window.
const COLUMNS: [ColumnSpec; 16] = [
    column_spec(ColumnKey::Name, "Name", "File name", 140.0, false),
    column_spec(
        ColumnKey::Folder,
        "Folder",
        "Folder of the file, starting with ~ inside a macOS home folder",
        160.0,
        false,
    ),
    column_spec(ColumnKey::Path, "Path", "Full path of the file", 160.0, false),
    column_spec(ColumnKey::Type, "Type", "File type", 66.0, false),
    column_spec(
        ColumnKey::Length,
        "Length",
        "Duration in minutes and seconds",
        72.0,
        true,
    ),
    column_spec(
        ColumnKey::BitDepth,
        "Bit Depth",
        "Bit depth of the source audio",
        86.0,
        true,
    ),
    column_spec(ColumnKey::Bitrate, "Bitrate", "Average bitrate in kbps", 76.0, true),
    column_spec(ColumnKey::SampleRate, "Rate", "Sample rate in kHz", 70.0, true),
    column_spec(ColumnKey::Peak, "Peak", "Sample peak in dBFS", 76.0, true),
    column_spec(
        ColumnKey::PeakLeft,
        "Peak L",
        "Sample peak of the left channel in dBFS",
        76.0,
        true,
    ),
    column_spec(
        ColumnKey::PeakRight,
        "Peak R",
        "Sample peak of the right channel in dBFS",
        76.0,
        true,
    ),
    column_spec(ColumnKey::TruePeak, "True Peak", "True peak in dBTP", 92.0, true),
    column_spec(
        ColumnKey::ShortTerm,
        "LUFS-S",
        "Maximum short-term loudness in LUFS",
        78.0,
        true,
    ),
    column_spec(
        ColumnKey::Integrated,
        "LUFS-I",
        "Integrated loudness in LUFS",
        78.0,
        true,
    ),
    column_spec(ColumnKey::Gain, "Gain", "Per-file gain in dB", 76.0, true),
    column_spec(ColumnKey::Status, "Status", "Analysis status", 180.0, false),
];

/// Analysis progress of one row.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum RowState {
    /// Waiting for a worker.
    Pending,
    /// Being decoded, with the decoded fraction.
    Analyzing(f32),
    /// Being normalized, with the normalized fraction.
    Normalizing(f32),
    /// Being processed with the plugin chain, with the processed fraction.
    Processing(f32),
    /// Has a cached or fresh record, which may be a failure.
    Done,
}

/// Columns of the results table, in their default order.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ColumnKey {
    Name,
    Folder,
    Path,
    Type,
    Length,
    BitDepth,
    Bitrate,
    SampleRate,
    Peak,
    PeakLeft,
    PeakRight,
    TruePeak,
    ShortTerm,
    Integrated,
    Gain,
    Status,
}

/// One file in the table.
#[derive(Debug, Clone)]
pub struct ResultRow {
    /// Absolute path of the file.
    pub path: PathBuf,
    /// File name shown in the first column.
    pub file_name: SharedString,
    /// Analysis progress.
    pub state: RowState,
    /// The result, once the file has been analyzed.
    pub record: Option<AnalysisRecord>,
    /// Message of the last normalization or plugin processing of the file, shown instead of the status.
    pub message: Option<RowMessage>,
}

/// A message about the last operation on a file.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RowMessage {
    /// Text shown in the Status column and its tooltip.
    pub text: String,
    /// True for a failure, shown in red, and false for a notice.
    pub is_error: bool,
}

/// Table data source for the analysis results.
pub struct ResultsDelegate {
    rows: Vec<ResultRow>,
    index: HashMap<PathBuf, usize>,
    /// Explicit edits, including clears, scoped to the file identity they were made for.
    gain_edits: HashMap<PathBuf, CustomGainEdit>,
    /// Definitions of the shown columns in display order, which changes when the user drags a column.
    columns: Vec<Column>,
    /// Keys of the shown columns in display order, matching `columns`.
    order: Vec<ColumnKey>,
    /// Definitions of every column, shown or hidden, in the default order.
    all_columns: Vec<(ColumnKey, Column)>,
    /// Columns the user has hidden.
    hidden: HashSet<ColumnKey>,
    sort: Option<(ColumnKey, ColumnSort)>,
    /// Column that orders rows the primary sort leaves equal, and whether it is ascending,
    /// set by a Command or Control click on a header.
    secondary_sort: Option<(ColumnKey, bool)>,
    /// File the app has selected, which the preview plays.
    /// The table keeps its selection by index, so a sort from the header selects this file's row again.
    selected: Option<PathBuf>,
    /// Files selected besides `selected`, by Command or Control clicks, Shift clicks, and Select All.
    /// Actions on the selection work on them too.
    extra_selected: HashSet<PathBuf>,
    /// Completed plain pointer selection awaiting the table's selection event, identified independently of its index.
    pointer_selection: Option<PathBuf>,
    /// Last observed widths by identity, separate from the preferred widths in `all_columns`.
    column_widths: HashMap<ColumnKey, Pixels>,
    /// Window width and scale of the last automatic layout, invalidated by visibility changes.
    column_layout: Option<(f32, f32)>,
    /// One automatic fit in progress, so double clicks do not reset manually resized columns.
    column_fit: Option<(usize, Pixels)>,
    /// The app state the right-click row menu needs, which the main view keeps up to date.
    row_menu: RowMenuState,
}

/// The app state behind the right-click row menu, which the delegate cannot see itself.
#[derive(Clone, Default)]
pub struct RowMenuState {
    /// Focus handle of the main view, which handles the menu's actions.
    pub action_context: Option<FocusHandle>,
    /// True when no analysis, normalization, or plugin processing is running.
    pub idle: bool,
    /// True when a normalization could start now.
    pub can_normalize: bool,
    /// True when plugin processing could start now, with an enabled plugin in the chain.
    pub can_process: bool,
    /// Normalization target shown in the normalize item, for example "-14.0 LUFS".
    pub normalize_target: String,
}

/// An explicit gain choice belonging only to one version of a file.
#[derive(Clone, Copy)]
struct CustomGainEdit {
    file_size: u64,
    modified_time_ms: i64,
    gain: Option<f32>,
    pending: bool,
}

/// One column of the results table.
struct ColumnSpec {
    key: ColumnKey,
    /// Short title for the header.
    title: &'static str,
    /// What the column shows with its unit, for the header tooltip.
    description: &'static str,
    /// Default width.
    width: f32,
    /// True for numbers, which line up on the right.
    right_aligned: bool,
}

impl ResultRow {
    /// Creates a pending row for a file.
    #[must_use]
    pub fn pending(path: PathBuf) -> Self {
        let file_name = path
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_default();
        Self {
            path,
            file_name: file_name.into(),
            state: RowState::Pending,
            record: None,
            message: None,
        }
    }

    /// Creates a finished row from a record.
    #[must_use]
    pub fn from_record(record: AnalysisRecord) -> Self {
        let mut row = Self::pending(record.path.clone());
        row.state = RowState::Done;
        row.record = Some(record);
        row
    }

    /// Returns the record when it holds usable levels.
    #[must_use]
    pub fn ready_record(&self) -> Option<&AnalysisRecord> {
        self.record.as_ref().filter(|record| record.is_ready())
    }

    /// Returns true when the analysis of the file failed.
    #[must_use]
    pub fn has_error(&self) -> bool {
        self.state == RowState::Done && self.record.as_ref().is_some_and(AnalysisRecord::has_error)
    }

    /// Returns the status text, with an activity label while the file is busy,
    /// and the message of the last normalization or plugin processing once it is done.
    #[must_use]
    pub fn status_text(&self) -> String {
        match (&self.state, &self.record, &self.message) {
            (RowState::Pending, _, _) => String::from("Waiting"),
            (RowState::Analyzing(_), _, _) => String::from("Analyzing"),
            (RowState::Normalizing(_), _, _) => String::from("Normalizing"),
            (RowState::Processing(_), _, _) => String::from("Processing"),
            (RowState::Done, _, Some(message)) => message.text.clone(),
            (RowState::Done, Some(record), None) => gui_measurement_status(record).to_string(),
            (RowState::Done, None, None) => String::new(),
        }
    }

    /// Returns true when the Status column shows a failure:
    /// a failed analysis, or a failed normalization or plugin processing.
    #[must_use]
    pub fn shows_error(&self) -> bool {
        self.has_error()
            || (self.state == RowState::Done && self.message.as_ref().is_some_and(|message| message.is_error))
    }

    /// Returns the tooltip of the Status column: the message of the last operation, or the reason a file failed.
    #[must_use]
    pub fn status_tooltip(&self) -> Option<String> {
        if self.state != RowState::Done {
            return None;
        }
        if let Some(message) = &self.message {
            return Some(message.text.clone());
        }
        self.record.as_ref().and_then(|record| {
            if record.has_error() && !record.error_message.is_empty() {
                Some(record.error_message.clone())
            } else if gui_measurement_status(record) == "Cached (C++)" {
                Some(String::from("Measurements and source-over history were produced by AudioBatch C++. Lossy playback decoding can differ."))
            } else {
                None
            }
        })
    }
}

impl ResultsDelegate {
    /// Creates an empty table whose visible columns share the available width by weight.
    /// The table's scrollbars overlay the rows, so the columns can use its whole width.
    #[must_use]
    pub fn new(table_width: f32) -> Self {
        let all_columns = COLUMNS
            .iter()
            .map(|spec| {
                let column = Column::new(column_id(spec.key), spec.title).sortable().p_0();
                let column = match spec.key {
                    ColumnKey::Status => column
                        .width(px(spec.width))
                        .min_width(px(STATUS_MINIMUM_WIDTH))
                        .max_width(px(STATUS_MAXIMUM_WIDTH)),
                    _ => column.width(px(spec.width)),
                };
                (spec.key, column)
            })
            .collect();
        let mut delegate = Self {
            rows: Vec::new(),
            index: HashMap::new(),
            gain_edits: HashMap::new(),
            columns: Vec::new(),
            order: Vec::new(),
            all_columns,
            hidden: DEFAULT_HIDDEN_COLUMNS.into_iter().collect(),
            // The quietest files come first until the user picks another sort.
            sort: Some((ColumnKey::Peak, ColumnSort::Ascending)),
            secondary_sort: None,
            selected: None,
            extra_selected: HashSet::new(),
            pointer_selection: None,
            column_widths: HashMap::new(),
            column_layout: None,
            column_fit: None,
            row_menu: RowMenuState::default(),
        };
        delegate.apply_sort_to_columns();
        delegate.rebuild_visible_columns();
        let widths = delegate.fitted_widths(table_width, 1.0);
        for ((key, column), width) in delegate.order.iter().zip(&mut delegate.columns).zip(widths) {
            column.width = width;
            delegate.column_widths.insert(*key, width);
        }
        delegate
    }

    /// Updates the app state the right-click row menu shows.
    pub fn set_row_menu_state(&mut self, state: RowMenuState) {
        self.row_menu = state;
    }

    /// Fills the right-click menu of the selected rows.
    ///
    /// The items, labels, and order match the C++ file menu, since both applications offer the same file actions.
    /// The one difference is Normalization Format Support, because this application has no such dialog.
    /// Every item acts on the whole selection, which right-clicking an unselected row first replaces with that row.
    fn fill_row_menu(&self, menu: PopupMenu) -> PopupMenu {
        let state = &self.row_menu;
        let selected = self.selected_paths();
        let any_exists = selected.iter().any(|path| path.exists());
        let any_parent_folder = selected.iter().any(|path| path.parent().is_some_and(Path::is_dir));
        let any_normalizable = selected
            .iter()
            .any(|path| self.row_for(path).and_then(ResultRow::ready_record).is_some());
        let any_custom_gain = selected.iter().any(|path| {
            self.row_for(path)
                .and_then(|row| row.record.as_ref())
                .is_some_and(|record| record.custom_gain_db.is_some())
        });
        let menu = match &state.action_context {
            Some(focus) => menu.action_context(focus.clone()),
            None => menu,
        };
        menu.menu_with_disabled(REVEAL_LABEL, Box::new(RevealSelected), !any_exists)
            .menu_with_disabled(OPEN_PARENT_FOLDER_LABEL, Box::new(OpenParentFolder), !any_parent_folder)
            .separator()
            .menu_with_disabled("Re-analyze Selected", Box::new(ReanalyzeSelected), !state.idle)
            .menu_with_disabled(
                format!("Normalize to {}", state.normalize_target),
                Box::new(NormalizeSelected),
                !(state.can_normalize && any_normalizable),
            )
            .separator()
            .menu_with_disabled(
                "Process With Plugin Chain",
                Box::new(ProcessSelected),
                !(state.can_process && any_normalizable),
            )
            .menu_with_disabled("Clear Per-file Gain", Box::new(ClearCustomGain), !any_custom_gain)
            .separator()
            .menu_with_disabled("Remove from List", Box::new(RemoveSelected), selected.is_empty())
            .menu_with_disabled("Move to Trash", Box::new(TrashSelected), !any_exists)
    }

    /// Returns true when the column shows.
    #[must_use]
    pub fn is_column_visible(&self, key: ColumnKey) -> bool {
        !self.hidden.contains(&key)
    }

    /// Returns the hidden columns in the default order.
    #[must_use]
    pub fn hidden_columns(&self) -> Vec<ColumnKey> {
        COLUMNS
            .iter()
            .map(|spec| spec.key)
            .filter(|key| self.hidden.contains(key))
            .collect()
    }

    /// Hides exactly the given columns. The Name column always shows.
    pub fn set_hidden_columns(&mut self, hidden: impl IntoIterator<Item = ColumnKey>) {
        self.hidden = hidden.into_iter().filter(|&key| key != ColumnKey::Name).collect();
        self.rebuild_visible_columns();
    }

    /// Shows or hides a column. The table must refresh its columns afterwards.
    pub fn set_column_visible(&mut self, key: ColumnKey, visible: bool) {
        if key == ColumnKey::Name {
            return;
        }
        if visible {
            self.hidden.remove(&key);
        } else {
            self.hidden.insert(key);
        }
        self.rebuild_visible_columns();
    }

    /// Returns the row at a display index.
    #[must_use]
    pub fn row(&self, index: usize) -> Option<&ResultRow> {
        self.rows.get(index)
    }

    /// Returns the row of a file.
    #[must_use]
    pub fn row_for(&self, path: &Path) -> Option<&ResultRow> {
        self.index.get(path).and_then(|&index| self.rows.get(index))
    }

    /// Returns the display index of a file.
    #[must_use]
    pub fn index_of(&self, path: &Path) -> Option<usize> {
        self.index.get(path).copied()
    }

    /// Returns the current row of the preview anchor, independent of the table's numeric cursor.
    #[must_use]
    pub fn selected_index(&self) -> Option<usize> {
        self.selected.as_deref().and_then(|path| self.index_of(path))
    }

    /// Returns the number of rows.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.rows.len()
    }

    /// Returns the number of rows whose analysis failed.
    #[must_use]
    pub fn failed_count(&self) -> usize {
        self.rows.iter().filter(|row| row.has_error()).count()
    }

    /// Returns the summed fraction of the rows being worked on, for the overall progress.
    #[must_use]
    pub fn active_progress(&self) -> f32 {
        self.rows
            .iter()
            .map(|row| match row.state {
                RowState::Analyzing(fraction) | RowState::Normalizing(fraction) | RowState::Processing(fraction) => {
                    fraction
                }
                RowState::Pending | RowState::Done => 0.0,
            })
            .sum()
    }

    /// Replaces the rows with the planned files, keeping the results of files that are still listed.
    pub fn set_files(&mut self, files: &[PathBuf]) {
        let mut previous: HashMap<PathBuf, ResultRow> =
            self.rows.drain(..).map(|row| (row.path.clone(), row)).collect();
        self.rows = files
            .iter()
            .map(|path| {
                previous
                    .remove(path)
                    .unwrap_or_else(|| ResultRow::pending(path.clone()))
            })
            .collect();
        self.sort_rows();
        self.extra_selected.retain(|path| self.index.contains_key(path));
        self.gain_edits.retain(|path, _| self.index.contains_key(path));
    }

    /// Replaces every row, for example with synthetic rows.
    pub fn set_rows(&mut self, rows: Vec<ResultRow>) {
        self.gain_edits.clear();
        self.rows = rows;
        self.sort_rows();
        self.extra_selected.retain(|path| self.index.contains_key(path));
    }

    /// Marks a listed file as waiting, for a run that works on only some of the files.
    pub fn mark_pending(&mut self, path: &Path) {
        if let Some(row) = self.row_mut(path) {
            row.state = RowState::Pending;
            row.message = None;
        }
    }

    /// Marks a file as being analyzed.
    pub fn mark_started(&mut self, path: &Path) {
        if let Some(row) = self.row_mut(path) {
            row.state = RowState::Analyzing(0.0);
        }
    }

    /// Updates the decoded fraction of a file.
    pub fn mark_progress(&mut self, path: &Path, fraction: f32) {
        if let Some(row) = self.row_mut(path)
            && row.state != RowState::Done
        {
            row.state = RowState::Analyzing(fraction);
        }
    }

    /// Marks a file as being normalized, with the normalized fraction.
    pub fn mark_normalizing(&mut self, path: &Path, fraction: f32) {
        if let Some(row) = self.row_mut(path) {
            row.state = RowState::Normalizing(fraction);
        }
    }

    /// Marks a file as being processed with the plugin chain, with the processed fraction.
    pub fn mark_processing(&mut self, path: &Path, fraction: f32) {
        if let Some(row) = self.row_mut(path) {
            row.state = RowState::Processing(fraction);
        }
    }

    /// Ends the normalization or plugin processing of a file that was left unchanged,
    /// keeping its record and showing the reason.
    pub fn fail_operation(&mut self, path: &Path, error: &str) {
        if let Some(row) = self.row_mut(path) {
            row.state = RowState::Done;
            row.message = Some(RowMessage {
                text: error.to_string(),
                is_error: true,
            });
        }
    }

    /// Replaces the row of a normalized or processed source with the output record and an optional message.
    /// A converted source leaves the list, since the output is a new file next to it.
    /// A file left unchanged, such as one already at the normalization target, keeps the custom gain of its row,
    /// since the gain still applies to the same version of the file.
    /// The output may belong elsewhere in the order,
    /// so the caller sorts the rows once after applying a batch of results.
    pub fn replace_after_operation(&mut self, source: &Path, mut output: AnalysisRecord, message: Option<RowMessage>) {
        let output_path = output.path.clone();
        if output_path != source {
            let was_anchor = self.selected.as_deref() == Some(source);
            let was_extra = self.extra_selected.contains(source);
            self.remove(source);
            if was_anchor {
                self.set_selected_path(Some(output_path.clone()));
            } else if was_extra && self.selected.as_ref() != Some(&output_path) {
                self.extra_selected.insert(output_path.clone());
            }
        }
        self.merge_custom_gain(&mut output);
        if let Some(row) = self.row_mut(&output_path) {
            row.state = RowState::Done;
            row.record = Some(output);
            row.message = message;
        } else {
            let mut row = ResultRow::from_record(output);
            row.message = message;
            self.index.insert(output_path, self.rows.len());
            self.rows.push(row);
        }
    }

    /// Stores the result of a file, adding a row when the file is not listed yet.
    /// Only a still-pending explicit edit overrides the incoming persisted gain.
    /// Returns a presentation override, never a request to replay an inherited database value.
    #[expect(
        clippy::option_option,
        reason = "The outer option requests persistence, the inner option is a nullable gain"
    )]
    pub fn set_record(&mut self, mut record: AnalysisRecord) -> Option<Option<f32>> {
        let carried_over = self.merge_custom_gain(&mut record);
        let Some(row) = self.row_mut(&record.path) else {
            self.index.insert(record.path.clone(), self.rows.len());
            self.rows.push(ResultRow::from_record(record));
            return None;
        };
        row.state = RowState::Done;
        row.record = Some(record);
        row.message = None;
        carried_over
    }

    /// Sets or clears the custom gain of a file with a usable result.
    /// Returns true when the gain changed.
    pub fn set_custom_gain(&mut self, path: &Path, custom_gain_db: Option<f32>) -> bool {
        let Some(record) = self
            .row_mut(path)
            .and_then(|row| row.record.as_mut())
            .filter(|record| record.is_ready())
        else {
            return false;
        };
        let edit = CustomGainEdit {
            file_size: record.file_size,
            modified_time_ms: record.modified_time_ms,
            gain: custom_gain_db,
            pending: true,
        };
        let changed = !same_custom_gain(record.custom_gain_db, custom_gain_db);
        record.custom_gain_db = custom_gain_db;
        self.gain_edits.insert(path.to_path_buf(), edit);
        changed
    }

    /// Takes a newly requested gain edit once, never replaying inherited values after a cross-process clear.
    pub fn take_gain_write(&mut self, path: &Path) -> Option<GainWrite> {
        let edit = self.gain_edits.get_mut(path)?;
        if !edit.pending {
            return None;
        }
        edit.pending = false;
        let record = self.row_for(path)?.ready_record()?;
        Some(GainWrite::new(record))
    }

    /// Attaches only the native preview field for the same captured source, preserving messages and measurements.
    pub fn attach_waveform(
        &mut self,
        path: &Path,
        file_size: u64,
        modified_time_ms: i64,
        waveform: Arc<WaveformOverview>,
    ) {
        if let Some(record) = self.row_mut(path).and_then(|row| row.record.as_mut())
            && record.file_size == file_size
            && record.modified_time_ms == modified_time_ms
        {
            record.waveform = Some(waveform);
        }
    }

    /// Keeps at most the selected row's waveform in memory, since unselected artifacts live in their native cache.
    pub fn release_waveforms_except(&mut self, selected: Option<&Path>) {
        for row in &mut self.rows {
            if selected != Some(row.path.as_path())
                && let Some(record) = &mut row.record
            {
                record.waveform = None;
            }
        }
    }

    /// Removes the row of a file and returns its former index.
    pub fn remove(&mut self, path: &Path) -> Option<usize> {
        let index = self.index_of(path)?;
        self.rows.remove(index);
        self.gain_edits.remove(path);
        self.extra_selected.remove(path);
        self.rebuild_index();
        Some(index)
    }

    /// Removes the rows of the files, in one pass however many there are, and returns how many it removed.
    pub fn remove_many(&mut self, paths: &HashSet<PathBuf>) -> usize {
        let count = self.rows.len();
        self.rows.retain(|row| !paths.contains(&row.path));
        self.extra_selected.retain(|path| !paths.contains(path));
        self.gain_edits.retain(|path, _| !paths.contains(path));
        self.rebuild_index();
        count - self.rows.len()
    }

    /// Updates a row in place by display index, for the synthetic stress test.
    pub fn update_row(&mut self, index: usize, update: impl FnOnce(&mut ResultRow)) {
        if let Some(row) = self.rows.get_mut(index) {
            update(row);
        }
    }

    /// Ends the waiting, analyzing, normalizing, and processing states of rows with a result,
    /// after their run was replaced or cancelled.
    /// Rows without a result stay waiting for the scan that lists them again.
    pub fn finish_interrupted_rows(&mut self) {
        for row in &mut self.rows {
            if matches!(
                row.state,
                RowState::Pending | RowState::Analyzing(_) | RowState::Normalizing(_) | RowState::Processing(_)
            ) && row.record.is_some()
            {
                row.state = RowState::Done;
            }
        }
    }

    /// Settles the rows a cancelled analysis left waiting or analyzing, so no row stays busy:
    /// with the cached result when `cached` has one, the previous result when the row has one,
    /// and otherwise with a message to analyze the file again, or that it no longer exists.
    pub fn finish_cancelled_analysis(&mut self, cached: impl Fn(&Path) -> Option<AnalysisRecord>) {
        for row in &mut self.rows {
            if !matches!(row.state, RowState::Pending | RowState::Analyzing(_)) {
                continue;
            }
            row.state = RowState::Done;
            if let Some(mut record) = cached(&row.path) {
                if let Some(edit) = self.gain_edits.get(&row.path).copied() {
                    if edit.pending
                        && edit.file_size == record.file_size
                        && edit.modified_time_ms == record.modified_time_ms
                    {
                        record.custom_gain_db = edit.gain;
                    } else {
                        self.gain_edits.remove(&row.path);
                    }
                }
                row.record = Some(record);
            } else if row.record.is_none() {
                let text = if row.path.exists() {
                    "Analysis did not finish. Re-analyze the file to try again."
                } else {
                    "File does not exist"
                };
                row.message = Some(RowMessage {
                    text: text.to_string(),
                    is_error: true,
                });
            }
        }
    }

    /// Remembers the file the app has selected, so a sort from the header can select its row again.
    /// The other selected files stay selected.
    pub fn set_selected_path(&mut self, path: Option<PathBuf>) {
        if let Some(path) = &path {
            self.extra_selected.remove(path);
        }
        if path.is_none() {
            self.extra_selected.clear();
        }
        self.selected = path;
    }

    /// Makes the file the only selected one, for a plain click on its row.
    pub fn select_only(&mut self, path: Option<PathBuf>) {
        self.extra_selected.clear();
        self.selected = path;
    }

    /// Consumes completed plain pointer intent once, before resolving the table's selection event.
    pub const fn take_pointer_selection(&mut self) -> Option<PathBuf> {
        self.pointer_selection.take()
    }

    /// Returns the selected files, the app's selected file first and the others in row order.
    #[must_use]
    pub fn selected_paths(&self) -> Vec<PathBuf> {
        let extra = self
            .rows
            .iter()
            .filter(|row| self.extra_selected.contains(&row.path))
            .map(|row| row.path.clone());
        self.selected.iter().cloned().chain(extra).collect()
    }

    /// Returns true when the file is selected, as the app's selected file or one of the others.
    #[must_use]
    pub fn is_path_selected(&self, path: &Path) -> bool {
        self.selected.as_deref() == Some(path) || self.extra_selected.contains(path)
    }

    /// Returns the files a drag that started on the row of `pressed` carries:
    /// the whole selection when that row is selected, otherwise only its file.
    #[must_use]
    pub fn drag_paths(&self, pressed: &Path) -> Vec<PathBuf> {
        if self.is_path_selected(pressed) {
            self.selected_paths()
        } else {
            vec![pressed.to_path_buf()]
        }
    }

    /// Selects every row besides the app's selected file.
    pub fn select_all(&mut self) {
        self.extra_selected = self
            .rows
            .iter()
            .map(|row| row.path.clone())
            .filter(|path| self.selected.as_ref() != Some(path))
            .collect();
    }

    /// Adds the row to the selection, or takes it out, for a Command or Control click.
    /// The app's selected file stays selected, since the preview plays it.
    pub fn toggle_row(&mut self, index: usize) {
        let Some(path) = self.rows.get(index).map(|row| row.path.clone()) else {
            return;
        };
        if self.selected.as_ref() == Some(&path) {
            return;
        }
        if !self.extra_selected.remove(&path) {
            self.extra_selected.insert(path);
        }
    }

    /// Selects the rows from the app's selected file to the row, for a Shift click.
    pub fn select_range_to(&mut self, index: usize) {
        let Some(anchor) = self.selected.as_deref().and_then(|path| self.index_of(path)) else {
            return;
        };
        let (first, last) = (anchor.min(index), anchor.max(index));
        self.extra_selected = self
            .rows
            .get(first..=last)
            .unwrap_or_default()
            .iter()
            .map(|row| row.path.clone())
            .filter(|path| self.selected.as_ref() != Some(path))
            .collect();
    }

    /// Returns true when the app has selected a file, which a Command, Control, or Shift click extends.
    #[must_use]
    pub const fn has_selection(&self) -> bool {
        self.selected.is_some()
    }

    /// Removes every row.
    pub fn clear(&mut self) {
        self.rows.clear();
        self.index.clear();
        self.gain_edits.clear();
        self.extra_selected.clear();
    }

    /// Sorts the rows by the current sort column, or by path when none is chosen.
    pub fn sort_rows(&mut self) {
        let ordering = self
            .sort
            .and_then(|(key, sort)| (sort != ColumnSort::Default).then_some((key, sort == ColumnSort::Ascending)));
        let directed = |ordering: Ordering, ascending: bool| if ascending { ordering } else { ordering.reverse() };
        let secondary = self.secondary_sort;
        match ordering {
            Some((key, ascending)) => self.rows.sort_by(|first, second| {
                // Failed and unfinished rows always go last.
                let first_ready = first.ready_record().is_some();
                let second_ready = second.ready_record().is_some();
                second_ready.cmp(&first_ready).then_with(|| {
                    let by_secondary = || {
                        secondary.map_or(Ordering::Equal, |(secondary_key, secondary_ascending)| {
                            directed(compare_column(first, second, secondary_key), secondary_ascending)
                        })
                    };
                    directed(compare_column(first, second, key), ascending)
                        .then_with(by_secondary)
                        .then_with(|| directed(compare_rows(first, second, key), ascending))
                })
            }),
            None => self.rows.sort_by(|first, second| {
                compare_natural(&first.path.to_string_lossy(), &second.path.to_string_lossy())
            }),
        }
        self.rebuild_index();
    }

    /// Applies manual width deltas to preferred widths without remembering automatic expansion as a preference.
    /// Call with the live table widths, not an earlier event's snapshot.
    pub fn remember_column_widths(&mut self, widths: &[Pixels]) {
        for (key, &width) in self.order.iter().zip(widths) {
            if let Some(previous) = self.column_widths.insert(*key, width)
                && let Some((_, column)) = self.all_columns.iter_mut().find(|(column_key, _)| column_key == key)
            {
                column.width = (column.width + (width - previous)).clamp(column.min_width, column.max_width);
            }
        }
    }

    /// Allocates widths from preferred sizes and bounds in physical pixels.
    fn fitted_widths(&self, table_width: f32, scale: f32) -> Vec<Pixels> {
        let columns: Vec<_> = self
            .order
            .iter()
            .filter_map(|key| self.all_columns.iter().find(|(column_key, _)| column_key == key))
            .collect();
        let limits: Vec<_> = columns
            .iter()
            .map(|(_, column)| {
                (
                    (f64::from(f32::from(column.min_width)) * f64::from(scale)).ceil(),
                    (f64::from(f32::from(column.max_width)) * f64::from(scale)).floor(),
                )
            })
            .collect();
        let mut widths: Vec<_> = columns
            .iter()
            .zip(&limits)
            .map(|((_, column), &(minimum, maximum))| {
                (f64::from(f32::from(column.width)) * f64::from(scale)).clamp(minimum, maximum)
            })
            .collect();
        let target = (f64::from(table_width) * f64::from(scale)).floor().clamp(
            limits.iter().map(|(minimum, _)| minimum).sum(),
            limits.iter().map(|(_, maximum)| maximum).sum(),
        );
        for _ in 0..=columns.len() {
            let remaining = target - widths.iter().sum::<f64>();
            if remaining.abs() < 0.000_001 {
                break;
            }
            let weights: Vec<_> = columns
                .iter()
                .zip(&widths)
                .zip(&limits)
                .map(|(((key, _), &width), &(minimum, maximum))| {
                    if (remaining > 0.0 && width < maximum) || (remaining < 0.0 && width > minimum) {
                        column_weight(*key)
                    } else {
                        0.0
                    }
                })
                .collect();
            let weight_sum: f64 = weights.iter().sum();
            if weight_sum <= 0.0 {
                break;
            }
            for ((width, weight), &(minimum, maximum)) in widths.iter_mut().zip(weights).zip(&limits) {
                *width = (*width + remaining * weight / weight_sum).clamp(minimum, maximum);
            }
        }
        let mut previous = 0.0;
        let mut accumulated = 0.0;
        widths
            .into_iter()
            .map(|width| {
                accumulated += width;
                let rounded = accumulated.round();
                let result = px((rounded - previous) as f32 / scale);
                previous = rounded;
                result
            })
            .collect()
    }

    /// Sets, flips, or clears the secondary sort for a Command or Control click on a header.
    /// The primary column clears it, the current secondary column flips its direction,
    /// and any other column becomes the ascending secondary sort.
    pub fn toggle_secondary_sort(&mut self, column_index: usize) {
        let Some(&key) = self.order.get(column_index) else {
            return;
        };
        let primary = self
            .sort
            .filter(|(_, sort)| *sort != ColumnSort::Default)
            .map(|(key, _)| key);
        self.secondary_sort = match self.secondary_sort {
            _ if primary == Some(key) => None,
            Some((current, ascending)) if current == key => Some((current, !ascending)),
            _ => Some((key, true)),
        };
        self.sort_rows();
    }

    /// Returns the columns the user can show or hide, with their titles, in the default order.
    /// The Name column always shows.
    pub fn column_choices() -> impl Iterator<Item = (ColumnKey, &'static str)> {
        COLUMNS
            .iter()
            .filter(|spec| spec.key != ColumnKey::Name)
            .map(|spec| (spec.key, spec.title))
    }

    /// Returns the column with the identifier the settings store, if there is one.
    #[must_use]
    pub fn column_from_id(id: &str) -> Option<ColumnKey> {
        COLUMNS.iter().map(|spec| spec.key).find(|&key| column_id(key) == id)
    }

    /// Returns the identifier of a column, which the settings store.
    #[must_use]
    pub const fn column_identifier(key: ColumnKey) -> &'static str {
        column_id(key)
    }

    /// Applies a plain pointer press, even when the pressed row is already the preview anchor.
    /// Programmatic cursor restoration does not call this, so it keeps the batch selection.
    fn plain_click_row(&mut self, row_index: usize) {
        if let Some(path) = self.row(row_index).map(|row| row.path.clone()) {
            self.select_only(Some(path));
        }
    }

    /// Records a completed plain pointer click before the table emits its positional selection event.
    fn pointer_click_row(&mut self, path: &Path) {
        self.pointer_selection = Some(path.to_path_buf());
    }

    /// Returns the display index of the Status column.
    #[cfg(test)]
    fn status_index(&self) -> Option<usize> {
        self.order.iter().position(|&key| key == ColumnKey::Status)
    }

    /// Returns the definition of the column at a display index.
    fn spec_at(&self, column_index: usize) -> Option<&'static ColumnSpec> {
        let key = *self.order.get(column_index)?;
        COLUMNS.iter().find(|spec| spec.key == key)
    }

    /// Rebuilds the shown columns: the ones still shown keep their display order,
    /// and a column shown again goes back after the shown column that precedes it in the default order.
    fn rebuild_visible_columns(&mut self) {
        let mut order: Vec<ColumnKey> = self
            .order
            .iter()
            .copied()
            .filter(|key| !self.hidden.contains(key))
            .collect();
        let defaults: Vec<ColumnKey> = COLUMNS.iter().map(|spec| spec.key).collect();
        for (position, &key) in defaults.iter().enumerate() {
            if self.hidden.contains(&key) || order.contains(&key) {
                continue;
            }
            let after = defaults
                .get(..position)
                .unwrap_or_default()
                .iter()
                .rev()
                .find_map(|previous| order.iter().position(|shown| shown == previous));
            order.insert(after.map_or(0, |index| index + 1), key);
        }
        self.columns = order
            .iter()
            .filter_map(|key| {
                self.all_columns
                    .iter()
                    .find(|(column_key, _)| column_key == key)
                    .map(|(_, column)| column.clone())
            })
            .collect();
        self.order = order;
        for (key, column) in self.order.iter().zip(&self.columns) {
            self.column_widths.insert(*key, column.width);
        }
        self.column_layout = None;
    }

    /// Marks the primary sort column with its direction and every other column as unsorted,
    /// so the table shows the right indicators after it refreshes its columns.
    fn apply_sort_to_columns(&mut self) {
        let sort = self.sort;
        for (key, column) in &mut self.all_columns {
            let direction = match sort {
                Some((sorted, direction)) if sorted == *key => direction,
                _ => ColumnSort::Default,
            };
            *column = column.clone().sort(direction);
        }
        for (key, column) in self.order.iter().zip(&mut self.columns) {
            if let Some((_, definition)) = self.all_columns.iter().find(|(column_key, _)| column_key == key) {
                column.sort = definition.sort;
            }
        }
    }

    /// Moves a column to another display index when the user drags its header.
    /// The table moves its own header state.
    /// The delegate keeps its display order and column definitions in step, so the cells and titles follow the header.
    fn reorder_column(&mut self, column_index: usize, target_index: usize) {
        if column_index < self.order.len() && target_index < self.order.len() {
            let key = self.order.remove(column_index);
            self.order.insert(target_index, key);
            let column = self.columns.remove(column_index);
            self.columns.insert(target_index, column);
        }
    }

    /// Applies an unsubmitted explicit edit only to the same file identity.
    /// An identity change retires the old edit rather than letting it affect a replacement file.
    #[expect(
        clippy::option_option,
        reason = "The outer option requests persistence, the inner option is a nullable gain"
    )]
    fn merge_custom_gain(&mut self, incoming: &mut AnalysisRecord) -> Option<Option<f32>> {
        let incoming_gain = incoming.custom_gain_db;
        if let Some(edit) = self.gain_edits.get(&incoming.path).copied() {
            if edit.pending
                && edit.file_size == incoming.file_size
                && edit.modified_time_ms == incoming.modified_time_ms
            {
                incoming.custom_gain_db = edit.gain;
            } else {
                self.gain_edits.remove(&incoming.path);
            }
        }
        (!same_custom_gain(incoming_gain, incoming.custom_gain_db)).then_some(incoming.custom_gain_db)
    }

    /// Returns a mutable row by path.
    fn row_mut(&mut self, path: &Path) -> Option<&mut ResultRow> {
        let index = *self.index.get(path)?;
        self.rows.get_mut(index)
    }

    /// Rebuilds the path to index map after the order changed.
    fn rebuild_index(&mut self) {
        self.index = self
            .rows
            .iter()
            .enumerate()
            .map(|(index, row)| (row.path.clone(), index))
            .collect();
    }

    /// Returns the display text of a cell.
    fn cell_string(&self, row_index: usize, column_index: usize) -> String {
        let (Some(row), Some(&key)) = (self.rows.get(row_index), self.order.get(column_index)) else {
            return String::new();
        };
        match key {
            ColumnKey::Name => row.file_name.to_string(),
            ColumnKey::Folder => format_folder(&row.path),
            ColumnKey::Path => row.path.to_string_lossy().into_owned(),
            ColumnKey::Status => row.status_text(),
            ColumnKey::Type => row
                .record
                .as_ref()
                .map_or_else(|| file_type(&row.path), format_type_label),
            _ => row
                .ready_record()
                .map_or_else(String::new, |record| level_text(record, key)),
        }
    }
}

impl TableDelegate for ResultsDelegate {
    fn columns_count(&self, _cx: &App) -> usize {
        self.columns.len()
    }

    fn rows_count(&self, _cx: &App) -> usize {
        self.rows.len()
    }

    fn context_menu(
        &mut self,
        _row_index: usize,
        menu: PopupMenu,
        _window: &mut Window,
        _cx: &mut Context<TableState<Self>>,
    ) -> PopupMenu {
        // The main view has already made the right-clicked row part of the selection, see `on_table_event`.
        self.fill_row_menu(menu)
    }

    fn column(&self, column_index: usize, _cx: &App) -> Column {
        self.columns
            .get(column_index)
            .cloned()
            .unwrap_or_else(|| Column::new("", ""))
    }

    fn perform_sort(
        &mut self,
        column_index: usize,
        sort: ColumnSort,
        window: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) {
        self.sort = self.order.get(column_index).map(|&key| (key, sort));
        // Sorting twice by one column adds nothing.
        if self.sort.map(|(key, _)| key) == self.secondary_sort.map(|(key, _)| key) {
            self.secondary_sort = None;
        }
        self.apply_sort_to_columns();
        self.sort_rows();
        // The table calls this while it is being updated, so its selection can only move afterwards.
        cx.defer_in(window, restore_table_cursor);
        cx.notify();
    }

    fn move_column(
        &mut self,
        column_index: usize,
        target_index: usize,
        _window: &mut Window,
        _cx: &mut Context<TableState<Self>>,
    ) {
        self.reorder_column(column_index, target_index);
    }

    /// Renders a header title, with the secondary sort marker "[2^]" or "[2v]",
    /// and sets the secondary sort on a Command or Control click.
    fn render_th(
        &mut self,
        column_index: usize,
        _window: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        let name = self.column(column_index, cx).name;
        let name = match self.secondary_sort {
            Some((key, ascending)) if self.order.get(column_index) == Some(&key) => {
                SharedString::from(format!("{name} {}", if ascending { "[2^]" } else { "[2v]" }))
            }
            _ => name,
        };
        let title = div()
            .id(("column-title", column_index))
            .size_full()
            .flex()
            .items_center()
            .px(px(CELL_INSET))
            .text_size(px(HEADER_FONT_SIZE))
            .truncate()
            .child(name)
            .on_click(cx.listener(move |state, event: &ClickEvent, window, cx| {
                if event.modifiers().secondary(cx) {
                    cx.stop_propagation();
                    state.delegate_mut().toggle_secondary_sort(column_index);
                    restore_table_cursor(state, window, cx);
                    cx.notify();
                }
            }));
        match self.spec_at(column_index) {
            Some(spec) => title.tooltip(tooltip(spec.description)),
            None => title,
        }
    }

    /// Highlights the rows selected besides the app's selected file, which the table highlights itself,
    /// and extends the selection on a Command or Control click and on a Shift click.
    /// Without a selected file, the click selects its row like a plain one.
    fn render_tr(
        &mut self,
        row_index: usize,
        _window: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) -> Stateful<Div> {
        let selected = self
            .rows
            .get(row_index)
            .is_some_and(|row| self.extra_selected.contains(&row.path));
        let path = self.rows.get(row_index).map(|row| row.path.clone());
        let row = path.as_ref().map_or_else(
            || div().id(("empty-row", row_index)),
            |path| div().id(Arc::<Path>::from(path.as_path())),
        );
        let row = if selected {
            row.bg(rgba(SELECTION_BACKGROUND))
        } else {
            row
        };
        // A drag of the row hands its files to other applications as copies, see `file_drag`.
        let row = match &path {
            Some(path) => row
                .on_drag(DraggedFiles::new(path.clone(), cx.weak_entity()), |files, _, _, cx| {
                    cx.new(|_| files.clone())
                })
                .external_drag_payload(|files: &DraggedFiles, _, cx| files.payload(cx)),
            None => row,
        };
        let click_path = path.clone();
        row.on_click(cx.listener(move |state, event: &ClickEvent, _window, cx| {
            let modifiers = event.modifiers();
            if matches!(event, ClickEvent::Mouse(_))
                && !modifiers.shift
                && !modifiers.secondary(cx)
                && let Some(path) = &click_path
            {
                // The table appends its click listener after this one and emits SelectRow afterwards.
                state.delegate_mut().pointer_click_row(path);
            }
        }))
        .on_mouse_down(
            MouseButton::Left,
            cx.listener(move |state, event: &MouseDownEvent, _window, cx| {
                let Some(row_index) = path.as_deref().and_then(|path| state.delegate().index_of(path)) else {
                    return;
                };
                let modifiers = event.modifiers;
                let extends = modifiers.shift || modifiers.secondary(cx);
                if !extends {
                    state.delegate_mut().plain_click_row(row_index);
                    cx.notify();
                    return;
                }
                if !state.delegate().has_selection() {
                    state.set_selected_row(row_index, cx);
                    return;
                }
                if modifiers.shift {
                    state.delegate_mut().select_range_to(row_index);
                } else {
                    state.delegate_mut().toggle_row(row_index);
                }
                cx.notify();
            }),
        )
    }

    fn render_td(
        &mut self,
        row_index: usize,
        column_index: usize,
        _window: &mut Window,
        _cx: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        let text = self.cell_string(row_index, column_index);
        let row = self.rows.get(row_index);
        let (key, right_aligned) = self
            .spec_at(column_index)
            .map_or((ColumnKey::Name, false), |spec| (spec.key, spec.right_aligned));
        let state = row.map_or(RowState::Done, |row| row.state);
        let failed = row.is_some_and(ResultRow::has_error);
        let status_error = row.is_some_and(ResultRow::shows_error);
        let text_color = match (key, state) {
            (ColumnKey::Status, RowState::Pending) => white(WAITING_TEXT_ALPHA),
            (ColumnKey::Status, RowState::Done) if status_error => color(palette::RED),
            _ if failed => white(ERROR_TEXT_ALPHA),
            _ => white(NORMAL_TEXT_ALPHA),
        };

        let cell = div()
            .size_full()
            .flex()
            .flex_row()
            .items_center()
            .gap(px(8.0))
            .px(px(CELL_INSET))
            .border_r_1()
            .border_color(white(DIVIDER_ALPHA))
            .text_size(px(CELL_FONT_SIZE))
            .text_color(text_color);
        let cell = if right_aligned { cell.justify_end() } else { cell };
        let cell = if key == ColumnKey::Type {
            cell.font_family(MONO_FONT)
        } else {
            cell
        };
        let cell = match (key, state) {
            (ColumnKey::Status, RowState::Pending) => cell.child(activity_ring(0.0)),
            (
                ColumnKey::Status,
                RowState::Analyzing(fraction) | RowState::Normalizing(fraction) | RowState::Processing(fraction),
            ) => cell.child(activity_ring(fraction)),
            _ => cell,
        };
        let cell = cell.id(("cell", column_index));
        // The Status cell shows a failure reason or a notice as its tooltip, because the cell is too narrow for it.
        let status_tooltip = row
            .filter(|_| key == ColumnKey::Status)
            .and_then(ResultRow::status_tooltip);
        let cell = match status_tooltip {
            Some(message) => cell.tooltip(tooltip(message)),
            None => cell,
        };
        #[cfg(test)]
        let cell = cell.test_support();
        cell.child(div().min_w_0().truncate().child(text))
    }

    fn render_empty(&mut self, _window: &mut Window, _cx: &mut Context<TableState<Self>>) -> impl IntoElement {
        div()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .text_size(px(14.0))
            .text_color(color(palette::GREY_MIDDLE_LIGHT))
            .child("Drop audio files or folders here, or use File > Choose Folder")
    }

    fn fit_column(
        &mut self,
        column_index: usize,
        _window: &mut Window,
        _cx: &mut Context<TableState<Self>>,
    ) -> Option<Pixels> {
        self.column_fit
            .filter(|(index, _)| *index == column_index)
            // fit_col rounds content upward to device pixels.
            // The predecessor avoids rounding an already quantized quotient up by another pixel.
            .map(|(_, width)| px(f32::from(width).next_down()))
    }

    fn cell_label(&self, row_index: usize, column_index: usize, _cx: &App) -> Option<SharedString> {
        Some(self.cell_string(row_index, column_index).into())
    }

    fn cell_text(&self, row_index: usize, column_index: usize, _cx: &App) -> String {
        self.cell_string(row_index, column_index)
    }
}

/// Fits visible columns after a window or visibility change, leaving manual drags alone at unchanged geometry.
/// Individual fits preserve the table's selection, scrolling, and header drag state.
pub fn fit_results_columns(
    state: &mut TableState<ResultsDelegate>,
    table_width: f32,
    window: &mut Window,
    cx: &mut Context<TableState<ResultsDelegate>>,
) {
    let current: Vec<_> = (0..).map_while(|index| state.col_width(index)).collect();
    state.delegate_mut().remember_column_widths(&current);
    let scale = window.scale_factor();
    if state.delegate().column_layout.is_some_and(|(width, previous_scale)| {
        (width - table_width).abs() < 0.01 && (previous_scale - scale).abs() < 0.001
    }) {
        return;
    }
    let targets = state.delegate().fitted_widths(table_width, scale);
    state.delegate_mut().column_layout = Some((table_width, scale));
    for (index, target) in targets.into_iter().enumerate() {
        if state
            .col_width(index)
            .is_some_and(|width| (f32::from(width - target)).abs() < 0.001)
        {
            continue;
        }
        state.delegate_mut().column_fit = Some((index, target));
        state.fit_col(index, window, cx);
        if let Some(width) = state.col_width(index)
            && let Some(&key) = state.delegate().order.get(index)
        {
            state.delegate_mut().column_widths.insert(key, width);
        }
    }
    state.delegate_mut().column_fit = None;
}

/// Gives long text columns ten times the resize share of a compact value column.
const fn column_weight(key: ColumnKey) -> f64 {
    match key {
        ColumnKey::Name | ColumnKey::Folder | ColumnKey::Path | ColumnKey::Status => 10.0,
        _ => 1.0,
    }
}

/// Formats GUI measurement provenance without changing core statuses or CLI output.
fn gui_measurement_status(record: &AnalysisRecord) -> &'static str {
    if record.status == AnalysisStatus::Cached && record.measurement_producer == MeasurementProducer::Cpp {
        "Cached (C++)"
    } else {
        format_status(record.status)
    }
}

/// Restores the table cursor from the current path index after either kind of header sort.
/// Reading the current index here also handles another reorder before a deferred primary-sort callback.
fn restore_table_cursor(
    state: &mut TableState<ResultsDelegate>,
    _window: &mut Window,
    cx: &mut Context<TableState<ResultsDelegate>>,
) {
    if let Some(index) = state.delegate().selected_index()
        && state.selected_row() != Some(index)
    {
        state.set_selected_row(index, cx);
    }
}

/// Builds a column definition, so the table of definitions stays readable.
const fn column_spec(
    key: ColumnKey,
    title: &'static str,
    description: &'static str,
    width: f32,
    right_aligned: bool,
) -> ColumnSpec {
    ColumnSpec {
        key,
        title,
        description,
        width,
        right_aligned,
    }
}

/// Draws the small activity ring of the Status column:
/// a dim disc with a blue ring that fills clockwise from the top as the file is decoded, empty while the file waits.
fn activity_ring(progress: f32) -> impl IntoElement {
    canvas(
        |_, _, _| (),
        move |bounds, (), window, _| paint_activity_ring(bounds, progress, window),
    )
    .size(px(RING_SIZE))
    .flex_none()
}

/// Paints the activity ring into its bounds.
fn paint_activity_ring(bounds: Bounds<Pixels>, progress: f32, window: &mut Window) {
    let half_diameter = f32::from(bounds.size.width.min(bounds.size.height)) * 0.5;
    let radius = half_diameter - 1.0;
    if radius <= 0.0 {
        return;
    }
    let half_width = f32::from(bounds.size.width) * 0.5;
    let half_height = f32::from(bounds.size.height) * 0.5;
    let center_x = f32::from(bounds.origin.x) + half_width;
    let center_y = f32::from(bounds.origin.y) + half_height;
    if let Some(disc) = circle_path(center_x, center_y, radius, PathBuilder::fill()) {
        window.paint_path(disc, color(palette::GREY_MIDDLE).opacity(0.3));
    }
    let progress = progress.clamp(0.0, 0.999);
    if progress >= 0.005 {
        let inner = radius * RING_INNER_PROPORTION;
        let start = -FRAC_PI_2;
        let end = TAU.mul_add(progress, start);
        let at = |angle: f32, length: f32| {
            point(
                px(length.mul_add(angle.cos(), center_x)),
                px(length.mul_add(angle.sin(), center_y)),
            )
        };
        let large_arc = progress > 0.5;
        let mut builder = PathBuilder::fill();
        builder.move_to(at(start, radius));
        builder.arc_to(point(px(radius), px(radius)), px(0.0), large_arc, true, at(end, radius));
        builder.line_to(at(end, inner));
        builder.arc_to(point(px(inner), px(inner)), px(0.0), large_arc, false, at(start, inner));
        builder.close();
        if let Ok(path) = builder.build() {
            window.paint_path(path, color(palette::BLUE).opacity(0.9));
        }
    }
    if let Some(outline) = circle_path(center_x, center_y, radius, PathBuilder::stroke(px(1.0))) {
        window.paint_path(outline, white(0.12));
    }
}

/// Builds a full circle with the given builder, filled or stroked.
fn circle_path(center_x: f32, center_y: f32, radius: f32, mut builder: PathBuilder) -> Option<gpui_kit::Path<Pixels>> {
    let radii = point(px(radius), px(radius));
    builder.move_to(point(px(center_x + radius), px(center_y)));
    builder.arc_to(radii, px(0.0), false, true, point(px(center_x - radius), px(center_y)));
    builder.arc_to(radii, px(0.0), false, true, point(px(center_x + radius), px(center_y)));
    builder.close();
    builder.build().ok()
}

/// Returns the identifier of a column.
const fn column_id(key: ColumnKey) -> &'static str {
    match key {
        ColumnKey::Name => "name",
        ColumnKey::Folder => "folder",
        ColumnKey::Path => "path",
        ColumnKey::Type => "type",
        ColumnKey::Length => "length",
        ColumnKey::BitDepth => "bit_depth",
        ColumnKey::Bitrate => "bitrate",
        ColumnKey::SampleRate => "sample_rate",
        ColumnKey::Peak => "peak",
        ColumnKey::PeakLeft => "peak_left",
        ColumnKey::PeakRight => "peak_right",
        ColumnKey::TruePeak => "true_peak",
        ColumnKey::ShortTerm => "short_term",
        ColumnKey::Integrated => "integrated",
        ColumnKey::Gain => "custom_gain",
        ColumnKey::Status => "status",
    }
}

/// Returns the text of a level or property column for a ready record, without the unit its column title names.
fn level_text(record: &AnalysisRecord, key: ColumnKey) -> String {
    match key {
        ColumnKey::Length => format_duration(record.duration_seconds),
        ColumnKey::BitDepth => format_bits_per_sample(record),
        ColumnKey::Bitrate => format_bitrate_compact(record),
        ColumnKey::SampleRate => format_sample_rate_compact(record),
        ColumnKey::Peak => format_peak_compact(f64::from(record.overall_peak)),
        ColumnKey::PeakLeft => format_peak_compact(f64::from(record.peak_left)),
        ColumnKey::PeakRight => format_peak_compact(f64::from(record.peak_right)),
        ColumnKey::TruePeak => format_true_peak_compact(record.overall_true_peak),
        ColumnKey::ShortTerm => format_loudness_compact(record.max_short_term_lufs),
        ColumnKey::Integrated => format_loudness_compact(record.integrated_lufs),
        ColumnKey::Gain => format_custom_gain_compact(record),
        ColumnKey::Name | ColumnKey::Folder | ColumnKey::Path | ColumnKey::Type | ColumnKey::Status => String::new(),
    }
}

/// Returns the full parent folder of a file as text, for sorting the Folder column.
fn parent_text(path: &Path) -> String {
    path.parent()
        .map_or_else(String::new, |parent| parent.to_string_lossy().into_owned())
}

/// Returns the uppercase extension of a path.
fn file_type(path: &Path) -> String {
    path.extension()
        .map(|extension| extension.to_string_lossy().to_ascii_uppercase())
        .unwrap_or_default()
}

/// Orders two rows ascending by a column, with the name breaking ties.
fn compare_rows(first: &ResultRow, second: &ResultRow, key: ColumnKey) -> Ordering {
    let by_name = || compare_natural(&first.file_name, &second.file_name);
    let (Some(first_record), Some(second_record)) = (first.ready_record(), second.ready_record()) else {
        return by_name();
    };
    match key {
        ColumnKey::Name => compare_by_mode(first_record, second_record, SortMode::Name),
        ColumnKey::Path => compare_by_mode(first_record, second_record, SortMode::Path),
        ColumnKey::Peak => compare_by_mode(first_record, second_record, SortMode::Peak),
        ColumnKey::Integrated => compare_by_mode(first_record, second_record, SortMode::Loudness),
        _ => compare_column(first, second, key).then_with(by_name),
    }
}

/// Orders two rows ascending by a column alone, leaving ties equal, so a secondary sort can break them.
/// Rows without a usable record compare equal.
fn compare_column(first: &ResultRow, second: &ResultRow, key: ColumnKey) -> Ordering {
    let (Some(first_record), Some(second_record)) = (first.ready_record(), second.ready_record()) else {
        return Ordering::Equal;
    };
    match key {
        ColumnKey::Name => compare_natural(&first.file_name, &second.file_name),
        // The full parent path orders the folders, so home folders of different users do not interleave.
        ColumnKey::Folder => compare_natural(&parent_text(&first.path), &parent_text(&second.path)),
        ColumnKey::Path => compare_natural(&first.path.to_string_lossy(), &second.path.to_string_lossy()),
        ColumnKey::Peak => first_record
            .overall_peak
            .abs()
            .total_cmp(&second_record.overall_peak.abs()),
        ColumnKey::Integrated => first_record.integrated_lufs.total_cmp(&second_record.integrated_lufs),
        ColumnKey::Status => first.status_text().cmp(&second.status_text()),
        ColumnKey::PeakLeft => first_record.peak_left.abs().total_cmp(&second_record.peak_left.abs()),
        ColumnKey::PeakRight => first_record.peak_right.abs().total_cmp(&second_record.peak_right.abs()),
        ColumnKey::TruePeak => first_record
            .overall_true_peak
            .abs()
            .total_cmp(&second_record.overall_true_peak.abs()),
        ColumnKey::ShortTerm => first_record
            .max_short_term_lufs
            .total_cmp(&second_record.max_short_term_lufs),
        ColumnKey::Length => first_record.duration_seconds.total_cmp(&second_record.duration_seconds),
        ColumnKey::SampleRate => first_record.sample_rate.cmp(&second_record.sample_rate),
        ColumnKey::BitDepth => first_record.bits_per_sample.cmp(&second_record.bits_per_sample),
        ColumnKey::Bitrate => average_bitrate_kbps(first_record).total_cmp(&average_bitrate_kbps(second_record)),
        ColumnKey::Type => format_type_label(first_record).cmp(&format_type_label(second_record)),
        // Files without a gain come first in ascending order.
        ColumnKey::Gain => match (first_record.custom_gain_db, second_record.custom_gain_db) {
            (Some(first_gain), Some(second_gain)) => first_gain.total_cmp(&second_gain),
            (first_gain, second_gain) => first_gain.is_some().cmp(&second_gain.is_some()),
        },
    }
}

#[cfg(test)]
mod test_preview_artifacts {
    use audiobatch_core::waveform::WaveformBuilder;

    use super::*;

    /// Creates a bounded waveform without decoding audio or measuring levels.
    fn waveform() -> Arc<WaveformOverview> {
        let mut builder = WaveformBuilder::new(1, Some(64));
        builder.push(&[&[0.25; 64]]);
        builder.finish(48_000, 64)
    }

    /// Creates a source-scoped foreign cached measurement with an operation notice.
    fn row(path: &str) -> ResultRow {
        let mut row = ResultRow::from_record(AnalysisRecord {
            path: path.into(),
            file_size: 100,
            modified_time_ms: 200,
            custom_gain_db: Some(-3.0),
            measurement_producer: MeasurementProducer::Cpp,
            status: AnalysisStatus::Cached,
            ..AnalysisRecord::default()
        });
        row.message = Some(RowMessage {
            text: "Output exists".into(),
            is_error: true,
        });
        row
    }

    /// Limits artifact attachment to the waveform field so optional enrichment cannot replace authoritative row state.
    #[test]
    fn attachment_preserves_measurements_origin_messages_selection_and_row_order() {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(vec![row("a"), row("b")]);
        delegate.select_only(Some("a".into()));
        delegate.select_all();
        let selection = delegate.selected_paths();
        let order: Vec<_> = delegate.rows.iter().map(|row| row.path.clone()).collect();
        delegate.attach_waveform(Path::new("a"), 100, 200, waveform());
        let row = delegate.row_for(Path::new("a")).expect("same row");
        let record = row.ready_record().expect("same measurements");
        assert_eq!(record.measurement_producer, MeasurementProducer::Cpp);
        assert_eq!(record.status, AnalysisStatus::Cached);
        assert!(same_custom_gain(record.custom_gain_db, Some(-3.0)));
        assert_eq!(row.status_text(), "Output exists");
        assert!(record.waveform.is_some());
        assert_eq!(delegate.selected_paths(), selection);
        assert_eq!(
            delegate.rows.iter().map(|row| row.path.clone()).collect::<Vec<_>>(),
            order
        );
    }

    /// Requires both captured identity fields to match so reusing a path cannot attach an obsolete waveform.
    #[test]
    fn stale_same_path_artifact_never_attaches_to_replacement_measurements() {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(vec![row("a")]);
        delegate.attach_waveform(Path::new("a"), 99, 200, waveform());
        delegate.attach_waveform(Path::new("a"), 100, 199, waveform());
        assert!(
            delegate
                .row_for(Path::new("a"))
                .expect("row")
                .ready_record()
                .expect("record")
                .waveform
                .is_none()
        );
    }

    /// Bounds retained native artifacts to the selected row and releases the final waveform when selection clears.
    #[test]
    fn unselected_artifacts_are_released_but_the_selected_artifact_survives() {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(vec![row("a"), row("b")]);
        let selected = waveform();
        let unselected = waveform();
        let selected_weak = Arc::downgrade(&selected);
        let unselected_weak = Arc::downgrade(&unselected);
        delegate.attach_waveform(Path::new("a"), 100, 200, selected);
        delegate.attach_waveform(Path::new("b"), 100, 200, unselected);
        delegate.release_waveforms_except(Some(Path::new("a")));
        assert!(unselected_weak.upgrade().is_none());
        assert!(selected_weak.upgrade().is_some());
        delegate.release_waveforms_except(None);
        assert!(selected_weak.upgrade().is_none());
    }
}

#[cfg(test)]
mod test_measurement_provenance {
    use super::*;

    /// Makes a settled row without needing a filesystem source or GUI context.
    fn row(status: AnalysisStatus, producer: MeasurementProducer) -> ResultRow {
        ResultRow::from_record(AnalysisRecord {
            path: "track.wav".into(),
            status,
            measurement_producer: producer,
            ..AnalysisRecord::default()
        })
    }

    /// Exposes foreign cached provenance only in GUI presentation
    /// so users see decoder origin without changing CLI status.
    #[test]
    fn only_foreign_cache_hits_have_a_gui_origin_suffix() {
        let foreign = row(AnalysisStatus::Cached, MeasurementProducer::Cpp);
        let native = row(AnalysisStatus::Cached, MeasurementProducer::Rust);
        assert_eq!(foreign.status_text(), "Cached (C++)");
        assert_eq!(native.status_text(), "Cached");
        assert_eq!(format_status(AnalysisStatus::Cached), "Cached");
        assert!(foreign.status_tooltip().is_some());
        assert!(native.status_tooltip().is_none());
        for status in [
            AnalysisStatus::Pending,
            AnalysisStatus::Analyzed,
            AnalysisStatus::Failed,
        ] {
            assert_eq!(
                row(status, MeasurementProducer::Cpp).status_text(),
                format_status(status)
            );
        }
    }

    /// Keeps actionable operation notices and current activity ahead of provenance so origin cannot obscure row status.
    #[test]
    fn operation_messages_and_activity_take_precedence_over_origin() {
        let mut foreign = row(AnalysisStatus::Cached, MeasurementProducer::Cpp);
        foreign.message = Some(RowMessage {
            text: "Output exists".into(),
            is_error: true,
        });
        assert_eq!(foreign.status_text(), "Output exists");
        assert_eq!(foreign.status_tooltip().as_deref(), Some("Output exists"));
        foreign.state = RowState::Processing(0.5);
        assert_eq!(foreign.status_text(), "Processing");
        assert!(foreign.status_tooltip().is_none());
    }

    /// Sorts cached rows by the status text users see so provenance labels and table ordering remain consistent.
    #[test]
    fn status_sorting_uses_the_presented_origin() {
        let native = row(AnalysisStatus::Cached, MeasurementProducer::Rust);
        let foreign = row(AnalysisStatus::Cached, MeasurementProducer::Cpp);
        assert!(compare_rows(&native, &foreign, ColumnKey::Status).is_lt());
    }
}

#[cfg(test)]
mod test_custom_gain {
    use super::*;

    use audiobatch_core::AnalysisStatus;

    /// Returns an analyzed record of the path with the custom gain.
    fn record(path: &str, custom_gain_db: Option<f32>) -> AnalysisRecord {
        AnalysisRecord {
            path: PathBuf::from(path),
            file_size: 100,
            modified_time_ms: 1000,
            status: AnalysisStatus::Analyzed,
            custom_gain_db,
            ..Default::default()
        }
    }

    /// Returns a delegate whose rows are the records.
    fn delegate_with(records: Vec<AnalysisRecord>) -> ResultsDelegate {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(records.into_iter().map(ResultRow::from_record).collect());
        delegate
    }

    /// Returns the custom gain of the row of the path.
    fn gain_of(delegate: &ResultsDelegate, path: &str) -> Option<f32> {
        delegate
            .row_for(Path::new(path))
            .and_then(|row| row.record.as_ref())
            .and_then(|record| record.custom_gain_db)
    }

    #[test]
    fn gain_changes_only_for_ready_rows() {
        let mut delegate = delegate_with(vec![record("/a.wav", None)]);
        assert!(delegate.set_custom_gain(Path::new("/a.wav"), Some(2.0)));
        assert!(!delegate.set_custom_gain(Path::new("/a.wav"), Some(2.0)));
        assert_eq!(gain_of(&delegate, "/a.wav"), Some(2.0));
        delegate.set_files(&[PathBuf::from("/a.wav"), PathBuf::from("/b.wav")]);
        assert!(!delegate.set_custom_gain(Path::new("/b.wav"), Some(1.0)));
    }

    /// Treats an incoming persisted clear as authoritative so a stale displayed gain cannot become a new write.
    #[test]
    fn new_persisted_clear_does_not_inherit_or_replay_an_old_gain() {
        let mut delegate = delegate_with(vec![record("/a.wav", Some(-3.0))]);
        assert_eq!(delegate.set_record(record("/a.wav", None)), None);
        assert_eq!(gain_of(&delegate, "/a.wav"), None);
        assert!(delegate.take_gain_write(Path::new("/a.wav")).is_none());
    }

    /// Consumes each explicit edit once so a later cross-process clear is not overwritten by replayed GUI state.
    #[test]
    fn submitted_explicit_edit_is_not_replayed_over_a_later_external_clear() {
        let mut delegate = delegate_with(vec![record("/a.wav", None)]);
        delegate.set_custom_gain(Path::new("/a.wav"), Some(-3.0));
        assert!(delegate.take_gain_write(Path::new("/a.wav")).is_some());
        assert!(delegate.take_gain_write(Path::new("/a.wav")).is_none());
        delegate.set_record(record("/a.wav", None));
        assert_eq!(gain_of(&delegate, "/a.wav"), None);
        assert!(delegate.take_gain_write(Path::new("/a.wav")).is_none());
    }

    #[test]
    fn new_record_of_a_changed_file_starts_without_a_gain() {
        let mut delegate = delegate_with(vec![record("/a.wav", Some(-3.0))]);
        let mut changed = record("/a.wav", None);
        changed.modified_time_ms += 1;
        assert_eq!(delegate.set_record(changed), None);
        assert_eq!(gain_of(&delegate, "/a.wav"), None);
    }

    #[test]
    fn explicit_clear_wins_stale_analysis_and_requests_cache_clear() {
        let mut delegate = delegate_with(vec![record("/a.wav", Some(-3.0))]);
        assert!(delegate.set_custom_gain(Path::new("/a.wav"), None));
        assert_eq!(delegate.set_record(record("/a.wav", Some(-3.0))), Some(None));
        assert_eq!(gain_of(&delegate, "/a.wav"), None);
        assert_eq!(delegate.set_record(record("/a.wav", None)), None);
        assert!(!delegate.set_custom_gain(Path::new("/a.wav"), None));
        assert_eq!(delegate.set_record(record("/a.wav", Some(2.0))), Some(None));
    }

    #[test]
    fn changed_identity_does_not_inherit_an_explicit_edit() {
        let mut delegate = delegate_with(vec![record("/a.wav", Some(-3.0))]);
        delegate.set_custom_gain(Path::new("/a.wav"), None);
        let mut changed = record("/a.wav", Some(4.0));
        changed.modified_time_ms += 1;
        assert_eq!(delegate.set_record(changed), None);
        assert!(same_custom_gain(gain_of(&delegate, "/a.wav"), Some(4.0)));
    }

    #[test]
    fn operation_and_cancelled_analysis_keep_explicit_clear() {
        let mut delegate = delegate_with(vec![record("/a.wav", Some(-3.0))]);
        delegate.set_custom_gain(Path::new("/a.wav"), None);
        delegate.replace_after_operation(Path::new("/a.wav"), record("/a.wav", Some(-3.0)), None);
        assert_eq!(gain_of(&delegate, "/a.wav"), None);
        delegate.mark_started(Path::new("/a.wav"));
        delegate.finish_cancelled_analysis(|_| Some(record("/a.wav", Some(-3.0))));
        assert_eq!(gain_of(&delegate, "/a.wav"), None);
        delegate.remove(Path::new("/a.wav"));
        delegate.set_record(record("/a.wav", Some(-3.0)));
        assert!(same_custom_gain(gain_of(&delegate, "/a.wav"), Some(-3.0)));
    }

    #[test]
    fn files_without_a_gain_sort_first() {
        let without = ResultRow::from_record(record("/a.wav", None));
        let lower = ResultRow::from_record(record("/b.wav", Some(-1.0)));
        let higher = ResultRow::from_record(record("/c.wav", Some(2.0)));
        assert_eq!(compare_rows(&without, &lower, ColumnKey::Gain), Ordering::Less);
        assert_eq!(compare_rows(&lower, &higher, ColumnKey::Gain), Ordering::Less);
        assert_eq!(compare_rows(&higher, &without, ColumnKey::Gain), Ordering::Greater);
    }
}

#[cfg(test)]
mod test_selection {
    use super::*;

    /// Returns a delegate with pending rows for the paths, sorted by path.
    fn delegate_with(paths: &[&str]) -> ResultsDelegate {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_files(&paths.iter().map(PathBuf::from).collect::<Vec<_>>());
        delegate
    }

    /// Returns the selected paths as text, the file the preview plays first.
    fn selected(delegate: &ResultsDelegate) -> Vec<String> {
        delegate
            .selected_paths()
            .iter()
            .map(|path| path.display().to_string())
            .collect()
    }

    #[test]
    fn toggling_adds_and_removes_rows_but_keeps_the_selected_file() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav", "/c.wav"]);
        delegate.select_only(Some(PathBuf::from("/b.wav")));
        delegate.toggle_row(2);
        assert_eq!(selected(&delegate), ["/b.wav", "/c.wav"]);
        delegate.toggle_row(1);
        assert_eq!(selected(&delegate), ["/b.wav", "/c.wav"]);
        delegate.toggle_row(2);
        assert_eq!(selected(&delegate), ["/b.wav"]);
    }

    #[test]
    fn a_drag_carries_the_selection_only_from_a_selected_row() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav", "/c.wav"]);
        delegate.select_only(Some(PathBuf::from("/b.wav")));
        delegate.toggle_row(0);
        let from_selected = delegate.drag_paths(Path::new("/a.wav"));
        assert_eq!(from_selected, [PathBuf::from("/b.wav"), PathBuf::from("/a.wav")]);
        assert!(delegate.is_path_selected(Path::new("/a.wav")));
        assert!(!delegate.is_path_selected(Path::new("/c.wav")));
        assert_eq!(delegate.drag_paths(Path::new("/c.wav")), [PathBuf::from("/c.wav")]);
    }

    #[test]
    fn a_shift_click_selects_the_range_from_the_selected_file() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav", "/c.wav", "/d.wav"]);
        delegate.select_only(Some(PathBuf::from("/c.wav")));
        delegate.select_range_to(0);
        assert_eq!(selected(&delegate), ["/c.wav", "/a.wav", "/b.wav"]);
        delegate.select_range_to(3);
        assert_eq!(selected(&delegate), ["/c.wav", "/d.wav"]);
    }

    #[test]
    fn a_cancelled_analysis_leaves_no_row_waiting() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        let cached = AnalysisRecord {
            path: PathBuf::from("/a.wav"),
            ..AnalysisRecord::default()
        };
        delegate.finish_cancelled_analysis(|path| (path == Path::new("/a.wav")).then(|| cached.clone()));
        let first = delegate.row_for(Path::new("/a.wav")).expect("first row");
        assert_eq!(first.state, RowState::Done);
        assert!(first.record.is_some());
        let second = delegate.row_for(Path::new("/b.wav")).expect("second row");
        assert_eq!(second.state, RowState::Done);
        assert_eq!(second.status_text(), "File does not exist");
        assert!(second.shows_error());
    }

    #[test]
    fn removing_many_rows_drops_them_from_the_rows_the_index_and_the_selection() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav", "/c.wav", "/d.wav"]);
        delegate.select_only(Some(PathBuf::from("/a.wav")));
        delegate.toggle_row(2);
        let removed: HashSet<PathBuf> = ["/b.wav", "/c.wav", "/missing.wav"].iter().map(PathBuf::from).collect();
        assert_eq!(delegate.remove_many(&removed), 2);
        assert_eq!(delegate.len(), 2);
        assert_eq!(delegate.index_of(Path::new("/d.wav")), Some(1));
        assert_eq!(selected(&delegate), ["/a.wav"]);
    }

    #[test]
    fn select_all_and_a_plain_click() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav", "/c.wav"]);
        delegate.select_only(Some(PathBuf::from("/a.wav")));
        delegate.select_all();
        assert_eq!(selected(&delegate), ["/a.wav", "/b.wav", "/c.wav"]);
        delegate.set_selected_path(Some(PathBuf::from("/a.wav")));
        assert_eq!(selected(&delegate).len(), 3);
        delegate.plain_click_row(delegate.selected_index().expect("preview anchor row"));
        assert_eq!(selected(&delegate), ["/a.wav"]);
        delegate.select_all();
        delegate.plain_click_row(1);
        assert_eq!(selected(&delegate), ["/b.wav"]);
    }

    #[test]
    fn completed_pointer_click_records_path_once_without_changing_batch_selection() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        delegate.select_only(Some(PathBuf::from("/a.wav")));
        delegate.select_all();
        delegate.pointer_click_row(Path::new("/a.wav"));
        assert_eq!(delegate.take_pointer_selection(), Some(PathBuf::from("/a.wav")));
        assert_eq!(delegate.take_pointer_selection(), None);
        assert_eq!(selected(&delegate), ["/a.wav", "/b.wav"]);
    }

    #[test]
    fn pointer_intent_keeps_its_path_when_rows_change_before_publication() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        delegate.pointer_click_row(Path::new("/a.wav"));
        delegate.remove(Path::new("/a.wav"));
        assert_eq!(delegate.row(0).expect("replacement row").path, Path::new("/b.wav"));
        assert_eq!(delegate.take_pointer_selection(), Some(PathBuf::from("/a.wav")));
        assert_eq!(delegate.take_pointer_selection(), None);
    }

    #[test]
    fn owner_cursor_restoration_has_no_pointer_intent() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        delegate.select_only(Some(PathBuf::from("/a.wav")));
        delegate.select_all();
        delegate.set_selected_path(Some(PathBuf::from("/a.wav")));
        delegate.sort_rows();
        assert_eq!(delegate.take_pointer_selection(), None);
        assert_eq!(selected(&delegate), ["/a.wav", "/b.wav"]);
    }

    #[test]
    fn removed_and_vanished_rows_leave_the_selection() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav", "/c.wav"]);
        delegate.select_only(Some(PathBuf::from("/a.wav")));
        delegate.select_all();
        delegate.remove(Path::new("/b.wav"));
        assert_eq!(selected(&delegate), ["/a.wav", "/c.wav"]);
        delegate.set_files(&[PathBuf::from("/a.wav")]);
        assert_eq!(selected(&delegate), ["/a.wav"]);
        delegate.set_selected_path(None);
        assert_eq!(selected(&delegate), Vec::<String>::new());
    }
}

#[cfg(test)]
mod test_operation_rows {
    use super::*;

    use audiobatch_core::AnalysisStatus;

    /// Returns an analyzed record of the path.
    fn record(path: &str) -> AnalysisRecord {
        AnalysisRecord {
            path: PathBuf::from(path),
            status: AnalysisStatus::Analyzed,
            ..Default::default()
        }
    }

    /// Returns a delegate with ready rows for the paths, ordered by path instead of the default Peak sort.
    fn delegate_with(paths: &[&str]) -> ResultsDelegate {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.sort = None;
        delegate.set_rows(paths.iter().map(|path| ResultRow::from_record(record(path))).collect());
        delegate
    }

    #[test]
    fn conversion_preserves_the_full_selection_in_either_completion_order() {
        for anchor_first in [false, true] {
            let mut delegate = delegate_with(&["/a.wav", "/b.wav", "/unselected.wav"]);
            delegate.select_only(Some(PathBuf::from("/a.wav")));
            delegate.toggle_row(delegate.index_of(Path::new("/b.wav")).expect("extra row"));
            let sources = if anchor_first { ["a", "b"] } else { ["b", "a"] };
            for source in sources {
                delegate.replace_after_operation(
                    Path::new(&format!("/{source}.wav")),
                    record(&format!("/{source}.aif")),
                    None,
                );
            }
            delegate.sort_rows();
            assert_eq!(delegate.selected_paths(), ["/a.aif", "/b.aif"].map(PathBuf::from));
            assert_eq!(delegate.selected_index(), delegate.index_of(Path::new("/a.aif")));
        }
    }

    #[test]
    fn listed_output_and_mixed_failures_preserve_selection_without_duplicates() {
        let mut delegate = delegate_with(&["/a.aif", "/a.wav", "/b.wav", "/c.wav"]);
        delegate.select_only(Some(PathBuf::from("/b.wav")));
        delegate.select_all();
        delegate.fail_operation(Path::new("/b.wav"), "Output exists");
        delegate.replace_after_operation(Path::new("/a.wav"), record("/a.aif"), None);
        delegate.replace_after_operation(Path::new("/c.wav"), record("/c.aif"), None);
        delegate.sort_rows();
        assert_eq!(
            delegate.selected_paths(),
            ["/b.wav", "/a.aif", "/c.aif"].map(PathBuf::from)
        );
        assert_eq!(delegate.len(), 3);
        assert!(
            delegate
                .row_for(Path::new("/b.wav"))
                .expect("failed source")
                .shows_error()
        );
    }

    #[test]
    fn unselected_conversion_does_not_extend_the_selection() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        delegate.select_only(Some(PathBuf::from("/a.wav")));
        delegate.replace_after_operation(Path::new("/b.wav"), record("/b.aif"), None);
        assert_eq!(delegate.selected_paths(), [PathBuf::from("/a.wav")]);
    }

    #[test]
    fn converted_source_is_replaced_by_its_output() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        delegate.mark_normalizing(Path::new("/a.wav"), 0.5);
        assert!((delegate.active_progress() - 0.5).abs() < f32::EPSILON);
        delegate.replace_after_operation(Path::new("/a.wav"), record("/a.aif"), None);
        assert_eq!(delegate.len(), 2);
        assert!(delegate.row_for(Path::new("/a.wav")).is_none());
        let output = delegate.row_for(Path::new("/a.aif")).expect("output row");
        assert_eq!(output.state, RowState::Done);
        assert_eq!(delegate.index_of(Path::new("/a.aif")), Some(1));
        delegate.sort_rows();
        assert_eq!(delegate.index_of(Path::new("/a.aif")), Some(0));
    }

    /// Keeps an unsubmitted edit only for the unchanged source identity so rewritten audio cannot inherit stale gain.
    #[test]
    fn an_unchanged_file_keeps_its_custom_gain_and_a_rewritten_one_loses_it() {
        let mut source = record("/a.aif");
        source.custom_gain_db = Some(-3.0);
        source.file_size = 100;
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(vec![ResultRow::from_record(source)]);
        delegate.set_custom_gain(Path::new("/a.aif"), Some(-3.0));

        let mut unchanged = record("/a.aif");
        unchanged.file_size = 100;
        delegate.replace_after_operation(Path::new("/a.aif"), unchanged, None);
        let gain = |delegate: &ResultsDelegate| {
            delegate
                .row_for(Path::new("/a.aif"))
                .and_then(|row| row.record.as_ref())
                .and_then(|record| record.custom_gain_db)
        };
        assert_eq!(gain(&delegate), Some(-3.0));

        let mut rewritten = record("/a.aif");
        rewritten.file_size = 200;
        delegate.replace_after_operation(Path::new("/a.aif"), rewritten, None);
        assert_eq!(gain(&delegate), None);
    }

    #[test]
    fn processing_rows_are_done_again_after_a_cancel() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        delegate.mark_processing(Path::new("/a.wav"), 0.5);
        delegate.mark_normalizing(Path::new("/b.wav"), 0.25);
        delegate.finish_interrupted_rows();
        for path in ["/a.wav", "/b.wav"] {
            assert_eq!(delegate.row_for(Path::new(path)).expect("row").state, RowState::Done);
        }
    }

    #[test]
    fn interrupted_rows_with_a_result_are_done_again() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        delegate.mark_pending(Path::new("/a.wav"));
        delegate.mark_started(Path::new("/b.wav"));
        delegate.set_files(&[
            PathBuf::from("/a.wav"),
            PathBuf::from("/b.wav"),
            PathBuf::from("/c.wav"),
        ]);
        delegate.finish_interrupted_rows();
        let state = |path: &str| delegate.row_for(Path::new(path)).map(|row| row.state);
        assert_eq!(state("/a.wav"), Some(RowState::Done));
        assert_eq!(state("/b.wav"), Some(RowState::Done));
        assert_eq!(state("/c.wav"), Some(RowState::Pending));
    }

    #[test]
    fn listed_output_is_updated_in_place() {
        let mut delegate = delegate_with(&["/a.aif", "/a.wav"]);
        let notice = RowMessage {
            text: String::from("Some packets were silent"),
            is_error: false,
        };
        delegate.replace_after_operation(Path::new("/a.wav"), record("/a.aif"), Some(notice));
        assert_eq!(delegate.len(), 1);
        let row = delegate.row_for(Path::new("/a.aif")).expect("output row");
        assert_eq!(row.status_text(), "Some packets were silent");
        assert!(!row.shows_error());
        assert_eq!(row.status_tooltip().as_deref(), Some("Some packets were silent"));
    }

    #[test]
    fn failed_normalization_keeps_the_record_and_shows_the_reason() {
        let mut delegate = delegate_with(&["/a.wav"]);
        delegate.mark_normalizing(Path::new("/a.wav"), 0.2);
        assert_eq!(
            delegate
                .row_for(Path::new("/a.wav"))
                .map(ResultRow::status_text)
                .as_deref(),
            Some("Normalizing")
        );
        delegate.fail_operation(Path::new("/a.wav"), "Output exists");
        let row = delegate.row_for(Path::new("/a.wav")).expect("row");
        assert!(row.ready_record().is_some());
        assert!(row.shows_error());
        assert!(!row.has_error());
        assert_eq!(row.status_text(), "Output exists");
    }

    #[test]
    fn processing_rows_show_their_activity_and_progress() {
        let mut delegate = delegate_with(&["/a.wav", "/b.wav"]);
        delegate.mark_processing(Path::new("/a.wav"), 0.25);
        delegate.mark_normalizing(Path::new("/b.wav"), 0.5);
        assert!((delegate.active_progress() - 0.75).abs() < f32::EPSILON);
        let row = delegate.row_for(Path::new("/a.wav")).expect("row");
        assert_eq!(row.status_text(), "Processing");
        assert_eq!(row.status_tooltip(), None);
    }

    #[test]
    fn new_analysis_clears_the_message() {
        let mut delegate = delegate_with(&["/a.wav"]);
        delegate.fail_operation(Path::new("/a.wav"), "Output exists");
        delegate.set_record(record("/a.wav"));
        let row = delegate.row_for(Path::new("/a.wav")).expect("row");
        assert_eq!(row.message, None);
        assert_eq!(row.status_tooltip(), None);
    }
}

#[cfg(test)]
mod test_column_reorder {
    use super::*;

    /// Returns the column identifiers of the delegate in display order.
    fn column_keys(delegate: &ResultsDelegate) -> Vec<String> {
        delegate.columns.iter().map(|column| column.key.to_string()).collect()
    }

    #[test]
    fn moved_column_keeps_its_definition_and_cells() {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.reorder_column(0, 3);
        assert_eq!(delegate.order[3], ColumnKey::Name);
        assert_eq!(delegate.spec_at(3).map(|spec| spec.title), Some("Name"));
        assert_eq!(delegate.spec_at(0).map(|spec| spec.key), Some(ColumnKey::Folder));
        assert_eq!(column_keys(&delegate)[3], "name");
        assert_eq!(column_keys(&delegate)[0], "folder");
    }

    #[test]
    fn status_index_follows_the_moved_status_column() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let last = delegate.order.len() - 1;
        assert_eq!(delegate.status_index(), Some(last));
        delegate.reorder_column(last, 1);
        assert_eq!(delegate.status_index(), Some(1));
        assert_eq!(column_keys(&delegate)[1], "status");
    }

    #[test]
    fn out_of_range_move_changes_nothing() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let before = delegate.order.clone();
        delegate.reorder_column(0, delegate.order.len());
        assert_eq!(delegate.order, before);
    }
}

#[cfg(test)]
mod test_column_visibility_and_sort {
    use super::*;

    use audiobatch_core::AnalysisStatus;

    /// Returns the column identifiers of the delegate in display order.
    fn column_keys(delegate: &ResultsDelegate) -> Vec<String> {
        delegate.columns.iter().map(|column| column.key.to_string()).collect()
    }

    /// Returns an analyzed record with the name, sample peak, and integrated loudness.
    fn record(name: &str, peak: f32, integrated_lufs: f64) -> AnalysisRecord {
        AnalysisRecord {
            path: PathBuf::from(format!("/{name}")),
            file_name: name.to_string(),
            status: AnalysisStatus::Analyzed,
            overall_peak: peak,
            integrated_lufs,
            ..Default::default()
        }
    }

    /// Returns the file names of the rows, in order.
    fn names(delegate: &ResultsDelegate) -> Vec<String> {
        (0..delegate.len())
            .filter_map(|index| delegate.row(index))
            .map(|row| row.file_name.to_string())
            .collect()
    }

    #[test]
    fn showing_a_column_keeps_the_widths_of_the_others() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let mut widths: Vec<Pixels> = delegate.columns.iter().map(|column| column.width).collect();
        widths[0] += px(25.0);
        delegate.remember_column_widths(&widths);
        delegate.set_column_visible(ColumnKey::Gain, true);
        let first = delegate.columns.first().expect("a shown column");
        assert_eq!(first.width, px(165.0));
    }

    #[test]
    fn folder_shows_next_to_name_and_path_starts_hidden_in_its_place() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let keys = column_keys(&delegate);
        assert_eq!(keys.get(..2), Some(&[String::from("name"), String::from("folder")][..]));
        assert!(!keys.contains(&String::from("path")));
        assert_eq!(
            delegate.hidden_columns(),
            [
                ColumnKey::Path,
                ColumnKey::PeakLeft,
                ColumnKey::PeakRight,
                ColumnKey::Gain
            ]
        );
        delegate.set_column_visible(ColumnKey::Path, true);
        assert_eq!(column_keys(&delegate).get(2).map(String::as_str), Some("path"));
        assert!(ResultsDelegate::column_choices().any(|(key, title)| key == ColumnKey::Folder && title == "Folder"));
        assert_eq!(ResultsDelegate::column_from_id("folder"), Some(ColumnKey::Folder));
    }

    #[test]
    fn folder_cells_leave_out_the_file_name_and_sort_by_the_full_parent_path() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let record = |path: &str| AnalysisRecord {
            path: PathBuf::from(path),
            file_name: Path::new(path)
                .file_name()
                .map(|name| name.to_string_lossy().into_owned())
                .unwrap_or_default(),
            status: AnalysisStatus::Analyzed,
            ..AnalysisRecord::default()
        };
        let rows = [
            ResultRow::from_record(record("/Volumes/Music/b/track.wav")),
            ResultRow::from_record(record("/Users/zed/Music/a.wav")),
            ResultRow::from_record(record("/Users/amy/Music 10/a.wav")),
            ResultRow::from_record(record("/Users/amy/Music 9/b.wav")),
        ];
        let folder = delegate
            .order
            .iter()
            .position(|&key| key == ColumnKey::Folder)
            .expect("folder");
        delegate.rows = vec![rows[1].clone()];
        delegate.rebuild_index();
        let expected = if cfg!(target_os = "macos") {
            "~/Music"
        } else {
            "/Users/zed/Music"
        };
        assert_eq!(delegate.cell_string(0, folder), expected);
        let mut sorted = rows.to_vec();
        sorted.sort_by(|first, second| compare_rows(first, second, ColumnKey::Folder));
        let order: Vec<_> = sorted
            .iter()
            .map(|row| row.path.to_string_lossy().into_owned())
            .collect();
        assert_eq!(
            order,
            [
                "/Users/amy/Music 9/b.wav",
                "/Users/amy/Music 10/a.wav",
                "/Users/zed/Music/a.wav",
                "/Volumes/Music/b/track.wav",
            ]
        );
    }

    #[test]
    fn peak_l_peak_r_and_gain_start_hidden_and_come_back_in_their_place() {
        let mut delegate = ResultsDelegate::new(1600.0);
        assert!(!column_keys(&delegate).contains(&String::from("peak_left")));
        assert!(!delegate.is_column_visible(ColumnKey::Gain));
        delegate.set_column_visible(ColumnKey::PeakLeft, true);
        let keys = column_keys(&delegate);
        let peak = keys.iter().position(|key| key == "peak");
        let peak_left = keys.iter().position(|key| key == "peak_left");
        assert_eq!(peak_left, peak.map(|index| index + 1));
        delegate.set_column_visible(ColumnKey::Path, false);
        assert!(!column_keys(&delegate).contains(&String::from("path")));
        assert_eq!(
            delegate.hidden_columns(),
            [ColumnKey::Path, ColumnKey::PeakRight, ColumnKey::Gain]
        );
        delegate.set_column_visible(ColumnKey::Folder, false);
        assert!(!column_keys(&delegate).contains(&String::from("folder")));
        delegate.set_column_visible(ColumnKey::Name, false);
        assert!(delegate.is_column_visible(ColumnKey::Name));
    }

    #[test]
    fn rows_sort_by_peak_ascending_by_default_and_a_secondary_column_breaks_ties() {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(
            [
                record("a.wav", 0.5, -10.0),
                record("b.wav", 0.25, -20.0),
                record("c.wav", 0.5, -30.0),
            ]
            .into_iter()
            .map(ResultRow::from_record)
            .collect(),
        );
        // The Peak sort breaks its own ties by loudness, quietest first.
        assert_eq!(names(&delegate), ["b.wav", "c.wav", "a.wav"]);
        delegate.select_only(Some(PathBuf::from("/a.wav")));
        delegate.select_all();
        let previous_cursor = delegate.selected_index();
        let integrated = delegate.order.iter().position(|&key| key == ColumnKey::Integrated);
        delegate.toggle_secondary_sort(integrated.expect("the LUFS-I column shows"));
        assert_eq!(delegate.secondary_sort, Some((ColumnKey::Integrated, true)));
        assert_eq!(names(&delegate), ["b.wav", "c.wav", "a.wav"]);
        delegate.toggle_secondary_sort(integrated.expect("the LUFS-I column shows"));
        assert_eq!(names(&delegate), ["b.wav", "a.wav", "c.wav"]);
        assert_ne!(delegate.selected_index(), previous_cursor);
        assert_eq!(delegate.selected_index(), Some(1));
        assert_eq!(
            delegate.selected_paths(),
            ["/a.wav", "/b.wav", "/c.wav"].map(PathBuf::from)
        );
        let peak = delegate.order.iter().position(|&key| key == ColumnKey::Peak);
        delegate.toggle_secondary_sort(peak.expect("the Peak column shows"));
        assert_eq!(delegate.secondary_sort, None);
    }
}

#[cfg(test)]
mod test_column_sizing {
    use super::*;

    /// Returns visible preferred sizes before window expansion or contraction.
    fn preferred(delegate: &ResultsDelegate) -> Vec<f32> {
        delegate
            .order
            .iter()
            .map(|key| {
                f32::from(
                    delegate
                        .all_columns
                        .iter()
                        .find(|(column_key, _)| column_key == key)
                        .expect("visible column definition")
                        .1
                        .width,
                )
            })
            .collect()
    }

    /// Checks the physical pixel budget without requiring a window or renderer.
    fn assert_budget(delegate: &ResultsDelegate, width: f32, scale: f32) {
        let widths = delegate.fitted_widths(width, scale);
        let total: f32 = widths.iter().copied().map(f32::from).sum();
        assert!(
            (total - width).abs() < 1.0 / scale + 0.001,
            "{total} versus {width} at {scale}"
        );
        for (key, width) in delegate.order.iter().zip(widths) {
            let column = &delegate
                .all_columns
                .iter()
                .find(|(column_key, _)| column_key == key)
                .expect("column")
                .1;
            assert!(width >= column.min_width);
            assert!(width <= column.max_width);
            // Mirror fit_col's upward rounding, including its quotient guard.
            let content = f32::from(width).next_down();
            let mut pixels = (content * scale).ceil();
            if pixels / scale < content {
                pixels += 1.0;
            }
            assert!((pixels / scale - f32::from(width)).abs() < 0.001);
        }
    }

    #[test]
    fn text_columns_receive_ten_times_the_extra_width() {
        let delegate = ResultsDelegate::new(1600.0);
        let preferred = preferred(&delegate);
        let weights: f64 = delegate.order.iter().map(|&key| column_weight(key)).sum();
        let target = (weights as f32).mul_add(10.0, preferred.iter().sum::<f32>());
        let widths = delegate.fitted_widths(target, 1.0);
        for ((key, width), preferred) in delegate.order.iter().zip(widths).zip(preferred) {
            let growth = f32::from(width) - preferred;
            assert!((column_weight(*key) as f32).mul_add(-10.0, growth).abs() < 0.001);
        }
        assert_budget(&delegate, target, 1.0);
    }

    #[test]
    fn folder_display_and_sort_use_parent_directory_then_name() {
        let row = |path: &str| {
            ResultRow::from_record(AnalysisRecord {
                path: PathBuf::from(path),
                file_name: Path::new(path)
                    .file_name()
                    .expect("file name")
                    .to_string_lossy()
                    .into_owned(),
                status: AnalysisStatus::Analyzed,
                ..AnalysisRecord::default()
            })
        };
        let first = row("/Music/Album 2/track 2.wav");
        let second = row("/Music/Album 2/track 10.wav");
        let third = row("/Music/Album 10/track 1.wav");
        assert_eq!(compare_rows(&first, &second, ColumnKey::Folder), Ordering::Less);
        assert_eq!(compare_rows(&second, &third, ColumnKey::Folder), Ordering::Less);
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(vec![first]);
        let folder = delegate
            .order
            .iter()
            .position(|key| *key == ColumnKey::Folder)
            .expect("visible Folder");
        assert_eq!(delegate.cell_string(0, folder), "/Music/Album 2");
    }

    #[test]
    fn initial_layout_allocates_only_visible_columns() {
        let delegate = ResultsDelegate::new(1600.0);
        let total: f32 = delegate.columns.iter().map(|column| f32::from(column.width)).sum();
        assert!((total - 1600.0).abs() < 0.001);
        assert_eq!(delegate.columns.len(), COLUMNS.len() - DEFAULT_HIDDEN_COLUMNS.len());
    }

    #[test]
    fn shrink_below_preferred_sizes_then_overflow_at_minimums() {
        let delegate = ResultsDelegate::new(1600.0);
        let widths = delegate.fitted_widths(700.0, 1.0);
        assert!(
            widths
                .iter()
                .zip(preferred(&delegate))
                .any(|(&width, preferred)| f32::from(width) < preferred)
        );
        assert_budget(&delegate, 700.0, 1.0);
        let minimums = delegate.fitted_widths(100.0, 1.0);
        for (key, width) in delegate.order.iter().zip(&minimums) {
            let minimum = if *key == ColumnKey::Status { 180.0 } else { 20.0 };
            assert!((f32::from(*width) - minimum).abs() < 0.001);
        }
        assert!(minimums.into_iter().map(f32::from).sum::<f32>() > 100.0);
    }

    #[test]
    fn limits_redistribute_space_and_cap_excess() {
        let delegate = ResultsDelegate::new(1600.0);
        assert_budget(&delegate, 8000.0, 1.0);
        let widths = delegate.fitted_widths(100_000.0, 1.0);
        for (key, width) in delegate.order.iter().zip(widths) {
            let maximum = if *key == ColumnKey::Status {
                STATUS_MAXIMUM_WIDTH
            } else {
                1200.0
            };
            assert!((f32::from(width) - maximum).abs() < 0.001);
        }
    }

    #[test]
    fn fractional_scale_layout_is_stable_and_reversible() {
        let delegate = ResultsDelegate::new(1600.0);
        for scale in [1.0, 1.25, 1.5, 1.75, 2.0, 1.3] {
            let original = delegate.fitted_widths(1600.0, scale);
            for width in [1400.3, 2400.0, 800.2, 3840.0, 1600.0] {
                assert_budget(&delegate, width, scale);
            }
            assert_eq!(original, delegate.fitted_widths(1600.0, scale));
        }
    }

    #[test]
    fn manual_width_deltas_are_retained_without_automatic_feedback() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let baseline = preferred(&delegate);
        let mut widths: Vec<_> = delegate.columns.iter().map(|column| column.width).collect();
        delegate.remember_column_widths(&widths);
        assert_eq!(baseline, preferred(&delegate));
        widths[0] += px(40.0);
        delegate.remember_column_widths(&widths);
        for _ in 0..10 {
            delegate.remember_column_widths(&widths);
        }
        let preferences = preferred(&delegate);
        assert!((preferences[0] - baseline[0] - 40.0).abs() < 0.001);
        assert_eq!(&preferences[1..], &baseline[1..]);
        let targets = delegate.fitted_widths(2400.0, 1.0);
        for (key, &width) in delegate.order.iter().zip(&targets) {
            delegate.column_widths.insert(*key, width);
        }
        delegate.remember_column_widths(&targets);
        assert_eq!(preferences, preferred(&delegate));
    }

    #[test]
    fn sorting_does_not_reclassify_automatic_widths_as_manual_edits() {
        let mut delegate = ResultsDelegate::new(2400.0);
        let baseline = preferred(&delegate);
        let widths: Vec<_> = delegate.columns.iter().map(|column| column.width).collect();
        delegate.column_layout = Some((2400.0, 1.0));
        delegate.sort = Some((ColumnKey::Name, ColumnSort::Ascending));
        delegate.apply_sort_to_columns();
        delegate.remember_column_widths(&widths);
        assert_eq!(baseline, preferred(&delegate));
        assert!(delegate.column_layout.is_some());
        assert_eq!(
            delegate.columns.first().expect("name").sort,
            Some(ColumnSort::Ascending)
        );
    }

    #[test]
    fn visibility_and_reorder_keep_preferences_and_weights_by_identity() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let mut widths: Vec<_> = delegate.columns.iter().map(|column| column.width).collect();
        widths[0] += px(40.0);
        delegate.remember_column_widths(&widths);
        delegate.reorder_column(0, 3);
        delegate.set_column_visible(ColumnKey::Path, false);
        delegate.set_column_visible(ColumnKey::Status, false);
        assert_budget(&delegate, 2400.0, 1.5);
        let name = delegate
            .order
            .iter()
            .position(|key| *key == ColumnKey::Name)
            .expect("name");
        assert!((preferred(&delegate)[name] - 180.0).abs() < 0.001);
        assert!((column_weight(delegate.order[name]) - 10.0).abs() < f64::EPSILON);
        delegate.set_column_visible(ColumnKey::Path, true);
        delegate.set_column_visible(ColumnKey::Status, true);
        assert_budget(&delegate, 1600.0, 1.25);
        let name = delegate
            .order
            .iter()
            .position(|key| *key == ColumnKey::Name)
            .expect("name");
        assert!((preferred(&delegate)[name] - 180.0).abs() < 0.001);
    }
}
