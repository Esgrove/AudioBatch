//! The results table: one row per file with its analysis status and levels.
//!
//! The titles are short, and their tooltips describe each column with its unit, so the cells hold only numbers.
//! Columns can be reordered by dragging their headers, and the Status column fills the width left over.
//! Failed files have white text with reduced opacity, cells have a faint divider between them,
//! and the Status column shows a progress ring while a file is analyzed, normalized, or processed with plugins.
//! A failure or notice of a normalization or plugin processing replaces the status text of its row,
//! with the full text as the tooltip.
//! `ResultsDelegate` implements the gpui-kit `TableDelegate`, which renders only the visible rows,
//! so lists of many thousands of files stay smooth.

use std::cmp::Ordering;
use std::collections::HashMap;
use std::f32::consts::{PI, TAU};
use std::path::{Path, PathBuf};

use audiobatch_core::AnalysisRecord;
use audiobatch_core::format::{
    average_bitrate_kbps, format_bitrate_compact, format_bits_per_sample, format_custom_gain_compact, format_duration,
    format_loudness_compact, format_peak_compact, format_sample_rate_compact, format_status, format_true_peak_compact,
    format_type_label,
};
use audiobatch_core::record::same_custom_gain;
use audiobatch_core::sorting::{SortMode, compare_by_mode, compare_natural};
use gpui_kit::component::table::{Column, ColumnSort, TableDelegate, TableState};
use gpui_kit::{
    App, Bounds, Context, InteractiveElement as _, IntoElement, ParentElement as _, PathBuilder, Pixels, SharedString,
    StatefulInteractiveElement as _, Styled as _, Window, canvas, div, point, px,
};

use crate::theme::{MONO_FONT, color, palette, tooltip, white};

/// Height of a table row.
pub const ROW_HEIGHT: f32 = 24.0;

/// Height of the header row.
pub const HEADER_HEIGHT: f32 = 28.0;

/// Font size of the cells.
const CELL_FONT_SIZE: f32 = 14.0;

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
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ColumnKey {
    Name,
    Path,
    Type,
    Length,
    BitDepth,
    Bitrate,
    SampleRate,
    Peak,
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
    /// Column definitions for the table in display order, which changes when the user drags a column.
    columns: Vec<Column>,
    /// Keys of the columns in display order, matching `columns`.
    order: Vec<ColumnKey>,
    sort: Option<(ColumnKey, ColumnSort)>,
    /// File the app has selected.
    /// The table keeps its selection by index, so a sort from the header selects this file's row again.
    selected: Option<PathBuf>,
    /// Width the Status column fills, which `fit_column` hands to the table.
    status_fill_width: Pixels,
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

/// Column definitions in the default order.
/// The short titles leave the units to the header tooltips, so the numeric columns stay narrow.
/// The widths fit typical values, and are wider where a title needs the room next to the sort indicator.
/// The Name and Path widths are a starting point that `ResultsDelegate::new` stretches,
/// and the Status width is the least it gets, because it fills whatever the table has left.
const COLUMNS: [ColumnSpec; 13] = [
    column_spec(ColumnKey::Name, "Name", "File name", 140.0, false),
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
            (RowState::Done, Some(record), None) => format_status(record.status).to_string(),
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
        self.record
            .as_ref()
            .filter(|record| record.has_error() && !record.error_message.is_empty())
            .map(|record| record.error_message.clone())
    }
}

impl ResultsDelegate {
    /// Creates an empty table whose Name and Path columns share the width the other columns leave free,
    /// in the 140 to 160 ratio of their default widths.
    /// The table's scrollbars overlay the rows, so the columns can use its whole width.
    #[must_use]
    pub fn new(table_width: f32) -> Self {
        let fixed: f32 = COLUMNS
            .iter()
            .filter(|spec| !matches!(spec.key, ColumnKey::Name | ColumnKey::Path))
            .map(|spec| spec.width)
            .sum();
        let free = (table_width - fixed).max(300.0);
        let name_width = (free * 140.0 / 300.0).max(140.0).floor();
        let path_width = (free - name_width).max(160.0).floor();
        let columns = COLUMNS
            .iter()
            .map(|spec| {
                let column = Column::new(column_id(spec.key), spec.title).sortable().p_0();
                match spec.key {
                    ColumnKey::Name => column.width(px(name_width)),
                    ColumnKey::Path => column.width(px(path_width)),
                    ColumnKey::Status => column
                        .width(px(spec.width))
                        .min_width(px(STATUS_MINIMUM_WIDTH))
                        .max_width(px(STATUS_MAXIMUM_WIDTH)),
                    _ => column.width(px(spec.width)),
                }
            })
            .collect();
        Self {
            rows: Vec::new(),
            index: HashMap::new(),
            columns,
            order: COLUMNS.iter().map(|spec| spec.key).collect(),
            sort: None,
            selected: None,
            status_fill_width: px(STATUS_MINIMUM_WIDTH),
        }
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
    }

    /// Replaces every row, for example with synthetic rows.
    pub fn set_rows(&mut self, rows: Vec<ResultRow>) {
        self.rows = rows;
        self.sort_rows();
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
            self.remove(source);
        }
        if let Some(row) = self.row_mut(&output_path) {
            if let Some(previous) = &row.record
                && output.custom_gain_db.is_none()
                && previous.file_size == output.file_size
                && previous.modified_time_ms == output.modified_time_ms
            {
                output.custom_gain_db = previous.custom_gain_db;
            }
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
    /// A replaced record passes its custom gain on while the file is unchanged on disk,
    /// because the user may have set it before the cache had a row to keep it in.
    /// Returns the gain when it was carried over, so the caller can store it in the cache.
    pub fn set_record(&mut self, mut record: AnalysisRecord) -> Option<f32> {
        let Some(row) = self.row_mut(&record.path) else {
            self.index.insert(record.path.clone(), self.rows.len());
            self.rows.push(ResultRow::from_record(record));
            return None;
        };
        let carried_over = row
            .record
            .as_ref()
            .filter(|existing| {
                existing.file_size == record.file_size && existing.modified_time_ms == record.modified_time_ms
            })
            .and_then(|existing| existing.custom_gain_db)
            .filter(|&gain| !same_custom_gain(Some(gain), record.custom_gain_db));
        if let Some(gain) = carried_over {
            record.custom_gain_db = Some(gain);
        }
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
        if same_custom_gain(record.custom_gain_db, custom_gain_db) {
            return false;
        }
        record.custom_gain_db = custom_gain_db;
        true
    }

    /// Removes the row of a file and returns its former index.
    pub fn remove(&mut self, path: &Path) -> Option<usize> {
        let index = self.index_of(path)?;
        self.rows.remove(index);
        self.rebuild_index();
        Some(index)
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

    /// Remembers the file the app has selected, so a sort from the header can select its row again.
    pub fn set_selected_path(&mut self, path: Option<PathBuf>) {
        self.selected = path;
    }

    /// Removes every row.
    pub fn clear(&mut self) {
        self.rows.clear();
        self.index.clear();
    }

    /// Sorts the rows by the current sort column, or by path when none is chosen.
    pub fn sort_rows(&mut self) {
        let ordering = self
            .sort
            .and_then(|(key, sort)| (sort != ColumnSort::Default).then_some((key, sort == ColumnSort::Ascending)));
        match ordering {
            Some((key, ascending)) => self.rows.sort_by(|first, second| {
                // Failed and unfinished rows always go last.
                let first_ready = first.ready_record().is_some();
                let second_ready = second.ready_record().is_some();
                second_ready.cmp(&first_ready).then_with(|| {
                    let ordering = compare_rows(first, second, key);
                    if ascending { ordering } else { ordering.reverse() }
                })
            }),
            None => self.rows.sort_by(|first, second| {
                compare_natural(&first.path.to_string_lossy(), &second.path.to_string_lossy())
            }),
        }
        self.rebuild_index();
    }

    /// Returns the display index of the Status column, which stretches to fill the table width.
    fn status_index(&self) -> Option<usize> {
        self.order.iter().position(|&key| key == ColumnKey::Status)
    }

    /// Returns the definition of the column at a display index.
    fn spec_at(&self, column_index: usize) -> Option<&'static ColumnSpec> {
        let key = *self.order.get(column_index)?;
        COLUMNS.iter().find(|spec| spec.key == key)
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
        self.sort_rows();
        // The table calls this while it is being updated, so its selection can only move afterwards.
        if let Some(index) = self.selected.as_deref().and_then(|path| self.index_of(path)) {
            cx.defer_in(window, move |state, _window, cx| {
                if state.selected_row() != Some(index) {
                    state.set_selected_row(index, cx);
                }
            });
        }
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

    fn render_th(
        &mut self,
        column_index: usize,
        _window: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        let title = div()
            .id(("column-title", column_index))
            .size_full()
            .flex()
            .items_center()
            .px(px(CELL_INSET))
            .text_size(px(HEADER_FONT_SIZE))
            .truncate()
            .child(self.column(column_index, cx).name);
        match self.spec_at(column_index) {
            Some(spec) => title.tooltip(tooltip(spec.description)),
            None => title,
        }
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
        (self.order.get(column_index) == Some(&ColumnKey::Status)).then_some(self.status_fill_width)
    }

    fn cell_label(&self, row_index: usize, column_index: usize, _cx: &App) -> Option<SharedString> {
        Some(self.cell_string(row_index, column_index).into())
    }

    fn cell_text(&self, row_index: usize, column_index: usize, _cx: &App) -> String {
        self.cell_string(row_index, column_index)
    }
}

/// Widens or narrows the Status column so the columns fill the table width.
/// Status takes what the other columns leave, wherever the user has moved it, down to its minimum width,
/// below which the table scrolls horizontally instead.
/// It goes through `fit_col`, which resizes only that column, so a column drag in progress keeps working.
pub fn fill_status_column(
    state: &mut TableState<ResultsDelegate>,
    table_width: f32,
    window: &mut Window,
    cx: &mut Context<TableState<ResultsDelegate>>,
) {
    let Some(status) = state.delegate().status_index() else {
        return;
    };
    let others: f32 = (0..state.delegate().columns.len())
        .filter(|&column_index| column_index != status)
        .filter_map(|column_index| state.col_width(column_index))
        .map(f32::from)
        .sum();
    let target = (table_width - others)
        .clamp(STATUS_MINIMUM_WIDTH, STATUS_MAXIMUM_WIDTH)
        .floor();
    let current = state.col_width(status).map_or(0.0, f32::from);
    if (target - current).abs() >= 0.5 {
        state.delegate_mut().status_fill_width = px(target);
        state.fit_col(status, window, cx);
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
    let radius = f32::from(bounds.size.width.min(bounds.size.height)) / 2.0 - 1.0;
    if radius <= 0.0 {
        return;
    }
    let center_x = f32::from(bounds.origin.x) + f32::from(bounds.size.width) / 2.0;
    let center_y = f32::from(bounds.origin.y) + f32::from(bounds.size.height) / 2.0;
    if let Some(disc) = circle_path(center_x, center_y, radius, PathBuilder::fill()) {
        window.paint_path(disc, color(palette::GREY_MIDDLE).opacity(0.3));
    }
    let progress = progress.clamp(0.0, 0.999);
    if progress >= 0.005 {
        let inner = radius * RING_INNER_PROPORTION;
        let start = -PI / 2.0;
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
        ColumnKey::Path => "path",
        ColumnKey::Type => "type",
        ColumnKey::Length => "length",
        ColumnKey::BitDepth => "bit_depth",
        ColumnKey::Bitrate => "bitrate",
        ColumnKey::SampleRate => "sample_rate",
        ColumnKey::Peak => "peak",
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
        ColumnKey::TruePeak => format_true_peak_compact(record.overall_true_peak),
        ColumnKey::ShortTerm => format_loudness_compact(record.max_short_term_lufs),
        ColumnKey::Integrated => format_loudness_compact(record.integrated_lufs),
        ColumnKey::Gain => format_custom_gain_compact(record),
        ColumnKey::Name | ColumnKey::Path | ColumnKey::Type | ColumnKey::Status => String::new(),
    }
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
    let ordering = match key {
        ColumnKey::Name => return compare_by_mode(first_record, second_record, SortMode::Name),
        ColumnKey::Path => return compare_by_mode(first_record, second_record, SortMode::Path),
        ColumnKey::Peak => return compare_by_mode(first_record, second_record, SortMode::Peak),
        ColumnKey::Integrated => return compare_by_mode(first_record, second_record, SortMode::Loudness),
        ColumnKey::Status => first.status_text().cmp(&second.status_text()),
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
    };
    ordering.then_with(by_name)
}

#[cfg(test)]
mod test_custom_gain {
    use super::*;

    use audiobatch_core::AnalysisStatus;

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

    fn delegate_with(records: Vec<AnalysisRecord>) -> ResultsDelegate {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(records.into_iter().map(ResultRow::from_record).collect());
        delegate
    }

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

    #[test]
    fn new_record_of_an_unchanged_file_takes_over_the_gain() {
        let mut delegate = delegate_with(vec![record("/a.wav", Some(-3.0))]);
        assert_eq!(delegate.set_record(record("/a.wav", None)), Some(-3.0));
        assert_eq!(gain_of(&delegate, "/a.wav"), Some(-3.0));
        assert_eq!(delegate.set_record(record("/a.wav", Some(-3.0))), None);
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
mod test_operation_rows {
    use super::*;

    use audiobatch_core::AnalysisStatus;

    fn record(path: &str) -> AnalysisRecord {
        AnalysisRecord {
            path: PathBuf::from(path),
            status: AnalysisStatus::Analyzed,
            ..Default::default()
        }
    }

    fn delegate_with(paths: &[&str]) -> ResultsDelegate {
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(paths.iter().map(|path| ResultRow::from_record(record(path))).collect());
        delegate
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

    #[test]
    fn an_unchanged_file_keeps_its_custom_gain_and_a_rewritten_one_loses_it() {
        let mut source = record("/a.aif");
        source.custom_gain_db = Some(-3.0);
        source.file_size = 100;
        let mut delegate = ResultsDelegate::new(1600.0);
        delegate.set_rows(vec![ResultRow::from_record(source)]);

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
        assert_eq!(delegate.spec_at(0).map(|spec| spec.key), Some(ColumnKey::Path));
        assert_eq!(column_keys(&delegate)[3], "name");
        assert_eq!(column_keys(&delegate)[0], "path");
    }

    #[test]
    fn status_index_follows_the_moved_status_column() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let last = COLUMNS.len() - 1;
        assert_eq!(delegate.status_index(), Some(last));
        delegate.reorder_column(last, 1);
        assert_eq!(delegate.status_index(), Some(1));
        assert_eq!(column_keys(&delegate)[1], "status");
    }

    #[test]
    fn out_of_range_move_changes_nothing() {
        let mut delegate = ResultsDelegate::new(1600.0);
        let before = delegate.order.clone();
        delegate.reorder_column(0, COLUMNS.len());
        assert_eq!(delegate.order, before);
    }
}
