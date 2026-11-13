//! File system actions on listed files: revealing files in the platform file manager and opening their folders.
//! `open_folder` also opens single folders, such as the chain presets folder.
//!
//! Both act on the selected files grouped by parent folder through `group_by_parent_folder`,
//! one file manager window per folder, up to `MAXIMUM_REVEAL_FOLDERS` folders,
//! so a large selection does not open hundreds of windows.

use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

use audiobatch_core::file_selection::{FolderGroups, MAXIMUM_REVEAL_FOLDERS, group_by_parent_folder};

/// Menu label of the reveal action, named after the platform file manager.
/// It matches the C++ label, since both applications show the same file menu items.
pub const REVEAL_LABEL: &str = if cfg!(target_os = "macos") {
    "Reveal in Finder"
} else if cfg!(target_os = "windows") {
    "Reveal in Explorer"
} else {
    "Reveal in File Manager"
};

/// Menu label of the action that opens the parent folders of the selected files.
pub const OPEN_PARENT_FOLDER_LABEL: &str = "Open Parent Folder";

/// Reveals the files in the platform file manager, one window per parent folder, up to `MAXIMUM_REVEAL_FOLDERS`.
///
/// Finder selects every file of a folder in its window.
/// Explorer's `/select` takes only one file per window, so it selects the first of the folder.
/// Returns the number of folders left out because of the limit.
///
/// # Errors
/// Returns an error when the file manager cannot be started.
pub fn reveal_in_file_manager(paths: &[PathBuf]) -> io::Result<usize> {
    let FolderGroups {
        groups,
        skipped_folders,
    } = group_by_parent_folder(paths, MAXIMUM_REVEAL_FOLDERS);
    for (folder, files) in &groups {
        reveal_folder_files(folder, files)?;
    }
    Ok(skipped_folders)
}

/// Opens each distinct parent folder of the files in the platform file manager, up to `MAXIMUM_REVEAL_FOLDERS`.
/// Returns the number of folders left out because of the limit.
///
/// # Errors
/// Returns an error when the file manager cannot be started.
pub fn open_parent_folders(paths: &[PathBuf]) -> io::Result<usize> {
    let FolderGroups {
        groups,
        skipped_folders,
    } = group_by_parent_folder(paths, MAXIMUM_REVEAL_FOLDERS);
    for (folder, _) in &groups {
        open_folder(folder)?;
    }
    Ok(skipped_folders)
}

/// Shows the files of one folder selected in the platform file manager.
/// Files that no longer exist are left out, and when none exists the folder opens instead.
fn reveal_folder_files(folder: &Path, files: &[PathBuf]) -> io::Result<()> {
    let existing: Vec<&PathBuf> = files.iter().filter(|file| file.exists()).collect();
    if existing.is_empty() {
        return open_folder(folder);
    }
    if cfg!(target_os = "macos") {
        return Command::new("open").arg("-R").args(existing).spawn().map(drop);
    }
    match existing.first() {
        Some(first) if cfg!(target_os = "windows") => explorer_reveal_command(first).spawn().map(drop),
        _ => open_folder(folder),
    }
}

/// Keeps the Explorer switch outside the quotes Windows adds around paths containing spaces.
fn explorer_reveal_command(file: &Path) -> Command {
    let mut command = Command::new("explorer");
    command.arg("/select,").arg(file);
    command
}

/// Opens a folder in the platform file manager.
///
/// # Errors
/// Returns an error when the file manager cannot be started.
pub fn open_folder(folder: &Path) -> io::Result<()> {
    let program = if cfg!(target_os = "macos") {
        "open"
    } else if cfg!(target_os = "windows") {
        "explorer"
    } else {
        "xdg-open"
    };
    Command::new(program).arg(folder).spawn().map(drop)
}

#[cfg(test)]
mod test_explorer_reveal {
    use std::ffi::OsStr;
    use std::path::Path;

    use super::explorer_reveal_command;

    #[test]
    fn explorer_select_switch_is_separate_from_the_unmodified_path() {
        for file in [
            r"C:\Music\track.aif",
            r"C:\My Music\Artist - Track, Extended Mix.aif",
            "C:\\Music\\Bj\u{f6}rk - J\u{f3}ga.aif",
            r"\\?\C:\My Music\track.aif",
        ] {
            let file = Path::new(file);
            let command = explorer_reveal_command(file);
            assert_eq!(command.get_program(), OsStr::new("explorer"));
            assert_eq!(
                command.get_args().collect::<Vec<_>>(),
                [OsStr::new("/select,"), file.as_os_str()]
            );
        }
    }
}
