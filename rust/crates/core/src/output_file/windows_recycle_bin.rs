//! Tells whether files in a folder go to the Windows recycle bin when they are trashed.
//!
//! The shell deletes a file on a drive without a recycle bin, such as a network share, permanently,
//! and the `trash` crate cannot tell beforehand.
//! A hidden probe file is trashed first and looked up in the recycle bin,
//! and the answer is remembered for the folder for the rest of the process.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{LazyLock, Mutex};

/// Answers already found, by folder.
static RECYCLE_BIN_FOLDERS: LazyLock<Mutex<HashMap<PathBuf, bool>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

/// Returns true when a file trashed from the folder lands in the recycle bin.
/// A probe that cannot be created or trashed counts as no recycle bin, so no original is ever deleted permanently.
pub(super) fn folder_has_recycle_bin(folder: &Path) -> bool {
    let known = RECYCLE_BIN_FOLDERS
        .lock()
        .ok()
        .and_then(|folders| folders.get(folder).copied());
    if let Some(known) = known {
        return known;
    }
    let recycled = probe_recycle_bin(folder);
    if let Ok(mut folders) = RECYCLE_BIN_FOLDERS.lock() {
        folders.insert(folder.to_path_buf(), recycled);
    }
    recycled
}

/// Trashes a hidden probe file from the folder and reports whether it reached the recycle bin,
/// removing it from there again when it did.
fn probe_recycle_bin(folder: &Path) -> bool {
    let Ok(probe) = tempfile::Builder::new()
        .prefix(".audiobatch-recycle-probe-")
        .tempfile_in(folder)
        .and_then(|file| file.into_temp_path().keep().map_err(|error| error.error))
    else {
        return false;
    };
    let Some(probe_name) = probe.file_name().map(std::ffi::OsStr::to_os_string) else {
        return false;
    };
    if trash::delete(&probe).is_err() {
        let _ = std::fs::remove_file(&probe);
        return false;
    }
    let Ok(items) = trash::os_limited::list() else {
        return false;
    };
    // The probe name is unique, while the recycle bin may spell the original folder differently,
    // such as in another case or with a verbatim prefix.
    let probes: Vec<_> = items.into_iter().filter(|item| item.name == probe_name).collect();
    if probes.is_empty() {
        return false;
    }
    let _ = trash::os_limited::purge_all(&probes);
    true
}
