//! Expands input paths into the sorted list of supported audio files to analyze.
//!
//! Directories are walked one level deep or recursively, hidden and dot-prefixed entries are skipped,
//! and the result is de-duplicated and sorted in natural order by full path.

use std::collections::HashSet;
use std::path::{Component, Path, PathBuf};

use walkdir::WalkDir;

use crate::sorting::compare_natural;

/// File extensions the decoder handles, in lowercase.
pub(crate) const SUPPORTED_EXTENSIONS: [&str; 13] = [
    "aac", "aif", "aifc", "aiff", "bwf", "caf", "flac", "m4a", "mp3", "oga", "ogg", "wav", "wave",
];

/// Returns true when the path has a supported audio file extension, without touching the file system.
#[must_use]
pub fn has_supported_extension(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| {
            SUPPORTED_EXTENSIONS
                .iter()
                .any(|supported| supported.eq_ignore_ascii_case(extension))
        })
}

/// Returns true when the path is an existing file with a supported audio extension.
#[must_use]
pub(crate) fn is_supported_audio_file(path: &Path) -> bool {
    path.is_file() && has_supported_extension(path)
}

/// Expands the input paths into a de-duplicated, naturally sorted list of supported files.
///
/// The directory walk checks `is_cancelled` between entries and returns the files found so far when it is set,
/// so a scan of a large or slow folder can be abandoned quickly.
pub(crate) fn collect_input_files(
    input_paths: &[PathBuf],
    recursive: bool,
    is_cancelled: &dyn Fn() -> bool,
) -> Vec<PathBuf> {
    let mut files = Vec::new();
    let mut seen = HashSet::new();
    let mut add_file = |path: &Path| {
        // Dot-prefixed files are skipped on every platform.
        // This includes AppleDouble "._" files,
        // which hold macOS metadata on non-Apple file systems and are never audio.
        if is_hidden(path) || !is_supported_audio_file(path) {
            return;
        }
        let absolute = normalize_path(path);
        if seen.insert(absolute.clone()) {
            files.push(absolute);
        }
    };

    for input_path in input_paths {
        if is_cancelled() {
            break;
        }
        if input_path.is_dir() {
            // Links are followed, and walkdir reports a loop as an error entry instead of walking it again.
            let walker = WalkDir::new(input_path)
                .follow_links(true)
                .min_depth(1)
                .max_depth(if recursive { usize::MAX } else { 1 })
                .into_iter()
                .filter_entry(|entry| !is_hidden(entry.path()));
            for entry in walker {
                if is_cancelled() {
                    break;
                }
                if let Ok(entry) = entry
                    && entry.file_type().is_file()
                {
                    add_file(entry.path());
                }
            }
        } else {
            add_file(input_path);
        }
    }

    files.sort_by(|first, second| compare_natural(&first.to_string_lossy(), &second.to_string_lossy()));
    files
}

/// Returns the absolute form of a path with `.` and `..` components resolved lexically.
///
/// Symbolic links are kept as they are, so the path stays the one the user gave.
#[must_use]
pub fn normalize_path(path: &Path) -> PathBuf {
    let absolute = std::path::absolute(path).unwrap_or_else(|_| path.to_path_buf());
    let mut normalized = PathBuf::new();
    for component in absolute.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                normalized.pop();
            }
            other => normalized.push(other),
        }
    }
    normalized
}

/// Returns true when the final path component starts with a dot.
fn is_hidden(path: &Path) -> bool {
    path.file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| name.starts_with('.'))
}

#[cfg(test)]
mod test_collect_input_files {
    use super::*;

    use tempfile::TempDir;

    fn create(directory: &Path, name: &str) -> PathBuf {
        let path = directory.join(name);
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).expect("create parent");
        }
        std::fs::write(&path, b"data").expect("write file");
        path
    }

    #[test]
    fn skips_hidden_and_unsupported_files() {
        let directory = TempDir::new().expect("temp dir");
        create(directory.path(), "visible.wav");
        create(directory.path(), ".hidden.wav");
        create(directory.path(), "._visible.wav");
        create(directory.path(), "notes.txt");
        let files = collect_input_files(&[directory.path().to_path_buf()], false, &|| false);
        assert_eq!(files.len(), 1);
        assert!(files[0].ends_with("visible.wav"));
    }

    #[test]
    fn extensions_match_without_case() {
        let directory = TempDir::new().expect("temp dir");
        for name in ["upper.WAV", "mixed.Flac", "song.AiFf", "track.Mp3", "notes.TXT"] {
            create(directory.path(), name);
        }
        let files = collect_input_files(&[directory.path().to_path_buf()], false, &|| false);
        let names: Vec<String> = files
            .iter()
            .filter_map(|file| file.file_name().map(|name| name.to_string_lossy().into_owned()))
            .collect();
        assert_eq!(names, ["mixed.Flac", "song.AiFf", "track.Mp3", "upper.WAV"]);
        let explicit = create(directory.path(), "explicit.OGG");
        assert_eq!(
            collect_input_files(std::slice::from_ref(&explicit), false, &|| false),
            vec![normalize_path(&explicit)]
        );
        assert!(has_supported_extension(Path::new("a.M4A")));
        assert!(!has_supported_extension(Path::new("a.WAVX")));
        assert!(!has_supported_extension(Path::new("WAV")));
    }

    #[test]
    fn explicit_hidden_file_is_skipped() {
        let directory = TempDir::new().expect("temp dir");
        let hidden = create(directory.path(), ".hidden.wav");
        assert_eq!(collect_input_files(&[hidden], false, &|| false), Vec::<PathBuf>::new());
    }

    #[test]
    fn recursion_is_optional() {
        let directory = TempDir::new().expect("temp dir");
        create(directory.path(), "top.wav");
        create(directory.path(), "nested/inner.flac");
        create(directory.path(), ".git/ignored.wav");
        let flat = collect_input_files(&[directory.path().to_path_buf()], false, &|| false);
        assert_eq!(flat.len(), 1);
        let recursive = collect_input_files(&[directory.path().to_path_buf()], true, &|| false);
        assert_eq!(recursive.len(), 2);
    }

    #[test]
    fn duplicates_are_removed_and_order_is_natural() {
        let directory = TempDir::new().expect("temp dir");
        let second = create(directory.path(), "track 10.wav");
        let first = create(directory.path(), "track 2.wav");
        let files = collect_input_files(&[second.clone(), first, second], false, &|| false);
        assert_eq!(files.len(), 2);
        assert!(files[0].ends_with("track 2.wav"));
        assert!(files[1].ends_with("track 10.wav"));
    }

    #[test]
    fn parent_components_are_resolved() {
        let directory = TempDir::new().expect("temp dir");
        let file = create(directory.path(), "nested/tone.wav");
        let indirect = directory.path().join("nested/../nested/./tone.wav");
        let files = collect_input_files(&[indirect], false, &|| false);
        assert_eq!(files, vec![normalize_path(&file)]);
        assert!(!files[0].to_string_lossy().contains(".."));
    }

    #[cfg(unix)]
    #[test]
    fn symlinked_files_and_folders_are_followed_without_looping() {
        let directory = TempDir::new().expect("temp dir");
        let target = create(directory.path(), "library/target.wav");
        let scanned = directory.path().join("scanned");
        std::fs::create_dir(&scanned).expect("create folder");
        std::os::unix::fs::symlink(&target, scanned.join("linked.wav")).expect("link file");
        std::os::unix::fs::symlink(directory.path().join("library"), scanned.join("folder")).expect("link folder");
        std::os::unix::fs::symlink(&scanned, scanned.join("loop")).expect("link loop");
        let files = collect_input_files(std::slice::from_ref(&scanned), true, &|| false);
        assert_eq!(
            files,
            vec![
                normalize_path(&scanned.join("folder/target.wav")),
                normalize_path(&scanned.join("linked.wav"))
            ]
        );
    }

    #[test]
    fn cancellation_stops_the_scan() {
        let directory = TempDir::new().expect("temp dir");
        create(directory.path(), "a.wav");
        assert_eq!(
            collect_input_files(&[directory.path().to_path_buf()], false, &|| true),
            Vec::<PathBuf>::new()
        );
    }
}
