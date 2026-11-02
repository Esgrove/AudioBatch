//! Public storage and cache contracts against an ordinary library build, with no environment mutation.

use std::path::Path;

use audiobatch_core::AnalysisCache;
use audiobatch_core::storage::StoragePaths;

/// Shared measurements and application-private files stay beneath the supplied root.
#[test]
fn isolates_shared_and_private_storage_below_one_root() {
    let directory = tempfile::tempdir().expect("temporary root");
    let paths = StoragePaths::in_root(directory.path()).expect("absolute root");
    assert_eq!(paths.database(), directory.path().join("AudioBatch/analysis.db"));
    assert_eq!(paths.data(), directory.path().join("AudioBatch-rs"));
    assert_eq!(paths.logs(), directory.path().join("AudioBatch-rs/logs"));
}

/// Invalid supplied roots fail instead of falling back to user storage.
#[test]
fn rejects_relative_empty_nul_and_file_roots() {
    for root in ["", "relative", "../other", "/tmp/invalid\0root"] {
        assert!(StoragePaths::in_root(Path::new(root)).is_err());
    }
    let file = tempfile::NamedTempFile::new().expect("temporary file");
    assert!(StoragePaths::in_root(file.path()).is_err());
}

/// The real cache creates its shared database only inside the explicitly supplied scratch directory.
#[test]
fn opens_shared_database_without_initializing_private_storage() {
    let directory = tempfile::tempdir().expect("temporary root");
    let paths = StoragePaths::in_root(directory.path()).expect("absolute root");
    let cache = AnalysisCache::with_path(paths.database());
    cache.open().expect("open isolated cache");
    assert!(paths.database().is_file());
    assert!(!paths.data().exists());
    assert!(!paths.logs().exists());
}
