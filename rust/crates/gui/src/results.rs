//! The results table: one row per file with its analysis status and levels.
//!
//! The columns, their order, their default widths,
//! and the cell styling follow `AudioFileTableModel.cpp` of the C++ application,
//! with an added Length column after the file type.
//! The titles are short, and their tooltips describe each column with its unit, so the cells hold only numbers.
//! Columns can be reordered by dragging their headers, and the Status column fills the width left over.
//! Failed files have white text with reduced opacity, cells have a faint divider between them,
//! and the Status column shows a progress ring while a file is busy.
//! `ResultsDelegate` implements the gpui-kit `TableDelegate`, which renders only the visible rows,
//! so lists of many thousands of files stay smooth.

use std::cmp::Ordering;
use std::collections::HashMap;
use std::f32::consts::{PI, TAU};
use std::path::{Path, PathBuf};

use audiobatch_core::AnalysisRecord;
use audiobatch_core::format::{
    average_bitrate_kbps, format_bitrate_compact, format_bits_per_sample, format_duration, format_loudness_compact,
    format_peak_compact, format_sample_rate_compact, format_status, format_true_peak_compact, format_type_label,
};
use audiobatch_core::sorting::{SortMode, compare_by_mode, compare_natural};
use gpui_kit::component::table::{Column, ColumnSort, TableDelegate, TableState};
use gpui_kit::{
    App, Bounds, Context, InteractiveElement as _, IntoElement, ParentElement as _, PathBuilder, Pixels, SharedString,
    StatefulInteractiveElement as _, Styled as _, Window, canvas, div, point, px,
};

use crate::theme::{MONO_FONT, color, palette, tooltip, white};

/// Height of a table row, as in the C++ results table.
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
    /// Has a cached or fresh record, which may be a failure.
    Done,
}

/// Columns of the results table, in the order of the C++ application with Length added after Type.
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
/// The widths are the C++ defaults, wider where a title needs the room next to the sort indicator.
/// The Name and Path widths are a starting point that `ResultsDelegate::new` stretches,
/// and the Status width is the least it gets, because it fills whatever the table has left.
const COLUMNS: [ColumnSpec; 12] = [
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

    /// Returns the status text, with the activity labels of the C++ application while the file is busy.
    #[must_use]
    pub fn status_text(&self) -> String {
        match (&self.state, &self.record) {
            (RowState::Pending, _) => String::from("Waiting"),
            (RowState::Analyzing(_), _) => String::from("Analyzing"),
            (RowState::Done, Some(record)) => format_status(record.status).to_string(),
            (RowState::Done, None) => String::new(),
        }
    }
}

impl ResultsDelegate {
    /// Creates an empty table whose Name and Path columns share the width the other columns leave free,
    /// in the 140 to 160 ratio of their C++ defaults.
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
            status_fill_width: px(STATUS_MINIMUM_WIDTH),
        }
    }

    /// Returns the display index of the Status column, which stretches to fill the table width.
    fn status_index(&self) -> Option<usize> {
        self.order.iter().position(|&key| key == ColumnKey::Status)
    }

    /// Returns the definition of the column at a display index.
    fn spec_at(&self, col_ix: usize) -> Option<&'static ColumnSpec> {
        let key = *self.order.get(col_ix)?;
        COLUMNS.iter().find(|spec| spec.key == key)
    }

    /// Moves a column to another display index when the user drags its header.
    /// The table moves its own header state.
    /// The delegate keeps its display order and column definitions in step, so the cells and titles follow the header.
    fn reorder_column(&mut self, col_ix: usize, to_ix: usize) {
        if col_ix < self.order.len() && to_ix < self.order.len() {
            let key = self.order.remove(col_ix);
            self.order.insert(to_ix, key);
            let column = self.columns.remove(col_ix);
            self.columns.insert(to_ix, column);
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

    /// Returns the summed decoded fraction of the rows being analyzed, for the overall progress.
    #[must_use]
    pub fn active_progress(&self) -> f32 {
        self.rows
            .iter()
            .map(|row| match row.state {
                RowState::Analyzing(fraction) => fraction,
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

    /// Marks a listed file as waiting, for a run that analyzes only some of the files again.
    pub fn mark_pending(&mut self, path: &Path) {
        if let Some(row) = self.row_mut(path) {
            row.state = RowState::Pending;
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

    /// Stores the result of a file, adding a row when the file is not listed yet.
    pub fn set_record(&mut self, record: AnalysisRecord) {
        if let Some(row) = self.row_mut(&record.path) {
            row.state = RowState::Done;
            row.record = Some(record);
        } else {
            self.index.insert(record.path.clone(), self.rows.len());
            self.rows.push(ResultRow::from_record(record));
        }
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

    /// Removes every row.
    pub fn clear(&mut self) {
        self.rows.clear();
        self.index.clear();
    }

    /// Sorts the rows by the current sort column, or by path when none is chosen, like the C++ default.
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

    fn column(&self, col_ix: usize, _cx: &App) -> Column {
        self.columns.get(col_ix).cloned().unwrap_or_else(|| Column::new("", ""))
    }

    fn perform_sort(
        &mut self,
        col_ix: usize,
        sort: ColumnSort,
        _window: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) {
        self.sort = self.order.get(col_ix).map(|&key| (key, sort));
        self.sort_rows();
        cx.notify();
    }

    fn move_column(&mut self, col_ix: usize, to_ix: usize, _window: &mut Window, _cx: &mut Context<TableState<Self>>) {
        self.reorder_column(col_ix, to_ix);
    }

    fn render_th(
        &mut self,
        col_ix: usize,
        _window: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        let title = div()
            .id(("column-title", col_ix))
            .size_full()
            .flex()
            .items_center()
            .px(px(CELL_INSET))
            .text_size(px(HEADER_FONT_SIZE))
            .truncate()
            .child(self.column(col_ix, cx).name);
        match self.spec_at(col_ix) {
            Some(spec) => title.tooltip(tooltip(spec.description)),
            None => title,
        }
    }

    fn render_td(
        &mut self,
        row_ix: usize,
        col_ix: usize,
        _window: &mut Window,
        _cx: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        let text = self.cell_string(row_ix, col_ix);
        let row = self.rows.get(row_ix);
        let (key, right_aligned) = self
            .spec_at(col_ix)
            .map_or((ColumnKey::Name, false), |spec| (spec.key, spec.right_aligned));
        let state = row.map_or(RowState::Done, |row| row.state);
        let failed = row.is_some_and(ResultRow::has_error);
        let text_color = match (key, state) {
            (ColumnKey::Status, RowState::Pending) => white(WAITING_TEXT_ALPHA),
            (ColumnKey::Status, RowState::Done) if failed => color(palette::RED),
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
            (ColumnKey::Status, RowState::Analyzing(fraction)) => cell.child(activity_ring(fraction)),
            _ => cell,
        };
        let cell = cell.id(("cell", col_ix));
        // Like the C++ table, the Status cell of a failed file shows the reason as its tooltip.
        let error = row
            .and_then(|row| row.record.as_ref())
            .filter(|record| key == ColumnKey::Status && failed && !record.error_message.is_empty())
            .map(|record| record.error_message.clone());
        let cell = match error {
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
        col_ix: usize,
        _window: &mut Window,
        _cx: &mut Context<TableState<Self>>,
    ) -> Option<Pixels> {
        (self.order.get(col_ix) == Some(&ColumnKey::Status)).then_some(self.status_fill_width)
    }

    fn cell_label(&self, row_ix: usize, col_ix: usize, _cx: &App) -> Option<SharedString> {
        Some(self.cell_string(row_ix, col_ix).into())
    }

    fn cell_text(&self, row_ix: usize, col_ix: usize, _cx: &App) -> String {
        self.cell_string(row_ix, col_ix)
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
        .filter(|&col_ix| col_ix != status)
        .filter_map(|col_ix| state.col_width(col_ix))
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
    };
    ordering.then_with(by_name)
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
