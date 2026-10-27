//! Finding VST3 bundles on disk.
//!
//! The default folders are the standard VST3 locations of each platform,
//! after the folders in the `VST3_PATH` environment variable.
//! `VST3_PATH` is split with the platform's path list separator, so Windows drive letters stay intact.
//! The search is recursive but does not descend into `.vst3` entries,
//! because a bundle can contain other files with that extension, such as the library inside a Windows bundle.

use std::collections::HashSet;
use std::env;
use std::path::{Path, PathBuf};

/// Extension of VST3 bundles and single-file plugins.
const VST3_EXTENSION: &str = "vst3";

/// Environment variable with extra plugin folders.
const VST3_PATH_VARIABLE: &str = "VST3_PATH";

/// Returns the folders searched for plugins by default: the `VST3_PATH` folders, then the platform folders.
#[must_use]
pub fn default_search_folders() -> Vec<PathBuf> {
    let mut folders: Vec<PathBuf> = env::var_os(VST3_PATH_VARIABLE)
        .map(|value| {
            env::split_paths(&value)
                .filter(|path| !path.as_os_str().is_empty())
                .collect()
        })
        .unwrap_or_default();
    folders.extend(platform_folders());
    let mut seen = HashSet::new();
    folders.retain(|folder| seen.insert(folder.clone()));
    folders
}

/// Returns every VST3 bundle inside the folders, sorted by path.
///
/// Missing folders are skipped. A folder that is itself a bundle is returned as is.
#[must_use]
pub fn find_bundles(folders: &[PathBuf]) -> Vec<PathBuf> {
    let mut bundles = Vec::new();
    for folder in folders {
        if is_bundle_path(folder) {
            if folder.exists() {
                bundles.push(folder.clone());
            }
        } else {
            collect_bundles(folder, &mut bundles, &mut HashSet::new());
        }
    }
    bundles.sort();
    bundles.dedup();
    bundles
}

/// Returns true when the path has the `.vst3` extension.
#[must_use]
pub fn is_bundle_path(path: &Path) -> bool {
    path.extension()
        .is_some_and(|extension| extension.eq_ignore_ascii_case(VST3_EXTENSION))
}

/// Adds the bundles below a folder.
/// Symlinked folders are followed, but each real folder is visited once,
/// so a link that points back up the tree cannot recurse without end.
fn collect_bundles(folder: &Path, bundles: &mut Vec<PathBuf>, visited: &mut HashSet<PathBuf>) {
    let Ok(real_folder) = std::fs::canonicalize(folder) else {
        return;
    };
    if !visited.insert(real_folder) {
        return;
    }
    let Ok(entries) = std::fs::read_dir(folder) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let hidden = path
            .file_name()
            .is_some_and(|name| name.to_string_lossy().starts_with('.'));
        if hidden {
            continue;
        }
        if is_bundle_path(&path) {
            bundles.push(path);
        } else if path.is_dir() {
            collect_bundles(&path, bundles, visited);
        }
    }
}

/// Returns the standard plugin folders of macOS.
#[cfg(target_os = "macos")]
fn platform_folders() -> Vec<PathBuf> {
    let mut folders = Vec::new();
    if let Some(home) = env::var_os("HOME") {
        folders.push(PathBuf::from(home).join("Library/Audio/Plug-Ins/VST3"));
    }
    folders.push(PathBuf::from("/Library/Audio/Plug-Ins/VST3"));
    folders
}

/// Returns the standard plugin folders of Windows.
#[cfg(windows)]
fn platform_folders() -> Vec<PathBuf> {
    let mut folders = Vec::new();
    if let Some(local) = env::var_os("LOCALAPPDATA") {
        folders.push(PathBuf::from(local).join("Programs").join("Common").join("VST3"));
    }
    if let Some(common) = env::var_os("CommonProgramFiles") {
        folders.push(PathBuf::from(common).join("VST3"));
    }
    folders
}

/// Returns no folders on platforms without plugin support.
#[cfg(not(any(target_os = "macos", windows)))]
const fn platform_folders() -> Vec<PathBuf> {
    Vec::new()
}

#[cfg(test)]
mod test_discovery {
    use super::*;

    #[test]
    fn finds_bundles_without_descending_into_them() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let nested = folder.path().join("Vendor");
        let bundle = nested.join("Effect.vst3");
        std::fs::create_dir_all(bundle.join("Contents").join("x86_64-win")).expect("bundle folders");
        std::fs::write(bundle.join("Contents").join("x86_64-win").join("Effect.vst3"), b"").expect("library");
        std::fs::write(folder.path().join("Single.VST3"), b"").expect("single file plugin");
        std::fs::write(folder.path().join("readme.txt"), b"").expect("other file");
        let found = find_bundles(&[folder.path().to_path_buf()]);
        assert_eq!(found, vec![folder.path().join("Single.VST3"), bundle]);
    }

    #[test]
    fn skips_hidden_entries_and_missing_folders() {
        let folder = tempfile::tempdir().expect("temporary folder");
        std::fs::create_dir_all(folder.path().join(".hidden").join("Hidden.vst3")).expect("hidden bundle");
        std::fs::create_dir_all(folder.path().join(".Trashed.vst3")).expect("hidden bundle");
        let missing = folder.path().join("missing");
        assert_eq!(
            find_bundles(&[folder.path().to_path_buf(), missing]),
            Vec::<PathBuf>::new()
        );
    }

    #[test]
    fn a_bundle_given_as_a_folder_is_returned_as_is() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let bundle = folder.path().join("Effect.vst3");
        std::fs::create_dir_all(&bundle).expect("bundle");
        assert_eq!(find_bundles(std::slice::from_ref(&bundle)), vec![bundle]);
    }

    #[cfg(unix)]
    #[test]
    fn a_symlink_back_up_the_tree_is_not_followed_forever() {
        let folder = tempfile::tempdir().expect("temporary folder");
        let vendor = folder.path().join("Vendor");
        std::fs::create_dir_all(vendor.join("Effect.vst3")).expect("bundle");
        std::os::unix::fs::symlink(folder.path(), vendor.join("Loop")).expect("symlink");
        assert_eq!(
            find_bundles(&[folder.path().to_path_buf()]),
            vec![vendor.join("Effect.vst3")]
        );
    }

    #[test]
    fn default_folders_have_no_duplicates() {
        let folders = default_search_folders();
        let unique: HashSet<&PathBuf> = folders.iter().collect();
        assert_eq!(unique.len(), folders.len());
    }
}
