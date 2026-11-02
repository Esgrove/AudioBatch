//! Path lists for actions on several selected files at once.
//!
//! `drag_out_paths` chooses the files a drag hands to other applications,
//! and `group_by_parent_folder` groups files by folder for revealing them in the file manager,
//! with a limit on the number of folders so a large selection does not open hundreds of windows.

use std::collections::HashSet;
use std::path::{Path, PathBuf};

/// Most folders that revealing the selected files or opening their parent folders opens at once.
/// The C++ application uses the same limit, so both open the same windows for the same selection.
pub const MAXIMUM_REVEAL_FOLDERS: usize = 10;

/// Selected files grouped by their parent folder, in the order the folders first appear.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct FolderGroups {
    /// Each folder with its files, in selection order, at most the requested number of folders.
    pub groups: Vec<(PathBuf, Vec<PathBuf>)>,
    /// Number of further folders left out because of the limit.
    pub skipped_folders: usize,
}

/// Returns the paths a drag out of the file list offers other applications:
/// only existing regular files, without duplicates, in the given order.
///
/// Missing files and folders are left out, so the receiving application never gets a broken path.
#[must_use]
pub fn drag_out_paths<'a>(paths: impl IntoIterator<Item = &'a Path>) -> Vec<PathBuf> {
    let mut seen = HashSet::new();
    paths
        .into_iter()
        .filter(|path| !path.as_os_str().is_empty() && path.is_file())
        .filter(|path| seen.insert(path.to_path_buf()))
        .map(Path::to_path_buf)
        .collect()
}

/// Groups files by their parent folder, keeping the order in which folders and files first appear.
///
/// Duplicate files are listed once.
/// Folders beyond `maximum_folders` are counted in `skipped_folders` instead of returned.
/// Files without a parent folder are left out.
#[must_use]
pub fn group_by_parent_folder(paths: &[PathBuf], maximum_folders: usize) -> FolderGroups {
    let mut groups: Vec<(PathBuf, Vec<PathBuf>)> = Vec::new();
    let mut skipped: HashSet<&Path> = HashSet::new();
    let mut seen_files: HashSet<&Path> = HashSet::new();
    for path in paths {
        let Some(parent) = path.parent().filter(|parent| !parent.as_os_str().is_empty()) else {
            continue;
        };
        if !seen_files.insert(path.as_path()) {
            continue;
        }
        if let Some((_, files)) = groups.iter_mut().find(|(folder, _)| folder == parent) {
            files.push(path.clone());
        } else if groups.len() < maximum_folders {
            groups.push((parent.to_path_buf(), vec![path.clone()]));
        } else {
            skipped.insert(parent);
        }
    }
    FolderGroups {
        groups,
        skipped_folders: skipped.len(),
    }
}

#[cfg(test)]
mod test_drag_out_paths {
    use super::*;

    use tempfile::TempDir;

    #[test]
    fn keeps_only_existing_files_in_order_without_duplicates() {
        let directory = TempDir::new().expect("temp dir");
        let first = directory.path().join("b.wav");
        let second = directory.path().join("a.wav");
        std::fs::write(&first, b"data").expect("write file");
        std::fs::write(&second, b"data").expect("write file");
        let missing = directory.path().join("missing.wav");
        let folder = directory.path().join("folder");
        std::fs::create_dir(&folder).expect("create folder");
        let input = [
            first.as_path(),
            missing.as_path(),
            folder.as_path(),
            second.as_path(),
            first.as_path(),
            Path::new(""),
        ];
        assert_eq!(drag_out_paths(input), vec![first, second]);
    }

    #[test]
    fn nothing_left_gives_an_empty_list() {
        assert_eq!(drag_out_paths([Path::new("/no/such/file.wav")]), Vec::<PathBuf>::new());
    }
}

#[cfg(test)]
mod test_group_by_parent_folder {
    use super::*;

    #[test]
    fn groups_files_by_folder_in_first_appearance_order() {
        let paths = ["/b/1.wav", "/a/2.wav", "/b/3.wav", "/a/2.wav"].map(PathBuf::from);
        let groups = group_by_parent_folder(&paths, MAXIMUM_REVEAL_FOLDERS);
        assert_eq!(
            groups.groups,
            vec![
                (
                    PathBuf::from("/b"),
                    ["/b/1.wav", "/b/3.wav"].map(PathBuf::from).to_vec()
                ),
                (PathBuf::from("/a"), vec![PathBuf::from("/a/2.wav")]),
            ]
        );
        assert_eq!(groups.skipped_folders, 0);
    }

    #[test]
    fn limit_counts_the_folders_left_out() {
        let paths = ["/a/1.wav", "/b/1.wav", "/c/1.wav", "/a/2.wav", "/c/2.wav", "/d/1.wav"].map(PathBuf::from);
        let groups = group_by_parent_folder(&paths, 2);
        assert_eq!(
            groups
                .groups
                .iter()
                .map(|(folder, _)| folder.clone())
                .collect::<Vec<_>>(),
            ["/a", "/b"].map(PathBuf::from)
        );
        assert_eq!(groups.groups[0].1.len(), 2);
        assert_eq!(groups.skipped_folders, 2);
    }

    #[test]
    fn empty_and_parentless_paths_give_no_groups() {
        assert_eq!(group_by_parent_folder(&[], 3), FolderGroups::default());
        assert_eq!(
            group_by_parent_folder(&[PathBuf::from("file.wav")], 3),
            FolderGroups::default()
        );
    }
}
