//! File system actions on listed files: revealing a file in the platform file manager.

use std::io;
use std::path::Path;
use std::process::Command;

/// Menu label of the reveal action, named after the platform file manager.
pub const REVEAL_LABEL: &str = if cfg!(target_os = "macos") {
    "Reveal in Finder"
} else if cfg!(target_os = "windows") {
    "Show in Explorer"
} else {
    "Show in File Manager"
};

/// Opens the platform file manager with the file selected.
/// Linux file managers have no common selection argument, so there the containing folder opens instead.
///
/// # Errors
/// Returns an error when the file manager cannot be started.
pub fn reveal_in_file_manager(path: &Path) -> io::Result<()> {
    let mut command = if cfg!(target_os = "macos") {
        let mut command = Command::new("open");
        command.arg("-R").arg(path);
        command
    } else if cfg!(target_os = "windows") {
        let mut command = Command::new("explorer");
        command.arg(format!("/select,{}", path.display()));
        command
    } else {
        let mut command = Command::new("xdg-open");
        command.arg(path.parent().unwrap_or(path));
        command
    };
    command.spawn().map(drop)
}
