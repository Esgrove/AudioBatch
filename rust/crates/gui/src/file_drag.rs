//! Dragging selected files out of the results table into other applications, as copies.
//!
//! A row drag carries `DraggedFiles`, which draws nothing inside the window.
//! It is its own type rather than gpui's `ExternalPaths`,
//! so the main window's drop handler never takes a drag of its own rows for files dropped from outside.
//! The dragged files are resolved from the table when the drag leaves the window, not when the row renders,
//! so a mouse down that changed the selection just before the drag is included,
//! and rendering does not copy the selection for every visible row.
//! When the pointer leaves the window, gpui calls the `external_drag_payload` resolver of the row,
//! which offers the files that still exist through `payload`.
//! On macOS gpui hands them to an `NSDraggingSession` with the Copy operation outside the app.
//! On Windows gpui offers no outbound drag yet, so `windows::start_file_drag` runs an OLE drag instead.

#[cfg(windows)]
mod windows;

use std::path::PathBuf;

use audiobatch_core::file_selection::drag_out_paths;
use gpui_kit::component::table::TableState;
use gpui_kit::{App, Context, Empty, ExternalDragPayload, FileDragPaths, IntoElement, Render, WeakEntity, Window};

use crate::results::ResultsDelegate;

#[cfg(windows)]
pub use windows::start_file_drag;

/// The value of a drag of result rows: the pressed row and the table it belongs to.
#[derive(Clone)]
pub struct DraggedFiles {
    /// File of the row the drag started on.
    pressed: PathBuf,
    /// The results table, which knows the selection when the drag leaves the window.
    table: WeakEntity<TableState<ResultsDelegate>>,
}

impl DraggedFiles {
    /// Creates the drag value for a press on the row of a file.
    #[must_use]
    pub const fn new(pressed: PathBuf, table: WeakEntity<TableState<ResultsDelegate>>) -> Self {
        Self { pressed, table }
    }

    /// Returns the files the drag hands to other applications:
    /// the selection when the pressed row is selected, otherwise only the pressed row,
    /// keeping only files that still exist, without duplicates, in selection order.
    #[must_use]
    pub fn existing_paths(&self, cx: &App) -> Vec<PathBuf> {
        let paths = self.table.upgrade().map_or_else(
            || vec![self.pressed.clone()],
            |table| table.read(cx).delegate().drag_paths(&self.pressed),
        );
        drag_out_paths(paths.iter().map(PathBuf::as_path))
    }

    /// Returns the payload gpui offers the platform when the drag leaves the window,
    /// or none when no dragged file exists any more.
    #[must_use]
    pub fn payload(&self, cx: &App) -> Option<ExternalDragPayload> {
        let paths = self.existing_paths(cx);
        (!paths.is_empty())
            .then(|| ExternalDragPayload::Files(FileDragPaths::new(paths.into_iter().map(|path| (path, false)))))
    }
}

impl Render for DraggedFiles {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        // The platform draws the file icons once the drag leaves the window.
        Empty
    }
}
