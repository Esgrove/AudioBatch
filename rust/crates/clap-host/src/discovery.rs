//! Filesystem-only CLAP discovery, with native loading left to isolated scanner workers.

use std::path::{Path, PathBuf};

use walkdir::WalkDir;

/// Returns the `CLAP_PATH` entries followed by the platform's user and system plugin folders.
#[must_use]
pub fn default_search_paths() -> Vec<PathBuf> {
    let mut paths: Vec<_> = std::env::var_os("CLAP_PATH").map_or_default(|value| {
        std::env::split_paths(&value)
            .filter(|path| !path.as_os_str().is_empty())
            .collect()
    });
    #[cfg(target_os = "macos")]
    {
        if let Some(home) = dirs::home_dir() {
            paths.push(home.join("Library/Audio/Plug-Ins/CLAP"));
        }
        paths.push(PathBuf::from("/Library/Audio/Plug-Ins/CLAP"));
    }
    #[cfg(windows)]
    {
        if let Some(local) = std::env::var_os("LOCALAPPDATA") {
            paths.push(PathBuf::from(local).join("Programs/Common/CLAP"));
        }
        if let Some(common) = std::env::var_os("COMMONPROGRAMFILES") {
            paths.push(PathBuf::from(common).join("CLAP"));
        } else if let Some(programs) = std::env::var_os("PROGRAMFILES") {
            paths.push(PathBuf::from(programs).join("Common Files/CLAP"));
        }
    }
    let mut unique = Vec::new();
    for path in paths {
        if !unique.contains(&path) {
            unique.push(path);
        }
    }
    unique
}

/// Returns whether the path has the CLAP package extension, regardless of capitalization.
#[must_use]
pub fn is_clap_bundle(path: &Path) -> bool {
    path.extension()
        .is_some_and(|extension| extension.eq_ignore_ascii_case("clap"))
}

/// Finds CLAP bundles beneath search roots, following symlinked folders
/// while skipping hidden entries and bundle interiors.
#[must_use]
pub fn find_bundles(paths: &[PathBuf]) -> Vec<PathBuf> {
    let mut bundles = Vec::new();
    for root in paths {
        let mut entries = WalkDir::new(root)
            .follow_links(true)
            .into_iter()
            .filter_entry(|entry| entry.depth() == 0 || !entry.file_name().as_encoded_bytes().starts_with(b"."));
        while let Some(entry) = entries.next() {
            let Ok(entry) = entry else { continue };
            if is_clap_bundle(entry.path()) {
                bundles.push(entry.path().to_path_buf());
                if entry.file_type().is_dir() {
                    entries.skip_current_dir();
                }
            }
        }
    }
    bundles.sort();
    bundles.dedup();
    bundles
}

#[cfg(test)]
mod test_discovery {
    use super::*;

    #[test]
    fn extensions_are_case_insensitive_and_exact() {
        assert!(is_clap_bundle(Path::new("Effect.CLAP")));
        assert!(!is_clap_bundle(Path::new("Effect.clap.backup")));
        assert!(!is_clap_bundle(Path::new("clap")));
    }

    #[test]
    fn scanning_prunes_bundles_and_deduplicates_roots() {
        let root = tempfile::tempdir().expect("temporary root");
        let bundle = root.path().join("Effect.clap");
        std::fs::create_dir_all(bundle.join("Contents/Nested.clap")).expect("bundle");
        let roots = vec![root.path().to_path_buf(), bundle.clone()];
        assert_eq!(find_bundles(&roots), vec![bundle]);
    }
}
