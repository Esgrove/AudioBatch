//! Builders shared by the unit tests of several core modules.
//!
//! They live here rather than in the `test-fixtures` crate, because they call into the core crate,
//! which `test-fixtures` cannot depend on.

use tempfile::TempDir;

use crate::analysis::analyze_file;
use crate::cache::AnalysisCache;
use crate::record::AnalysisRecord;

/// Copies a fixture into the directory under its own name and analyzes the copy, failed or not.
pub fn analyze_fixture_copy(directory: &TempDir, fixture: &str) -> AnalysisRecord {
    let path = test_fixtures::copy_fixture(directory, fixture, None);
    analyze_file(&path, &|| false, &mut |_| {})
}

/// Copies a fixture into the directory under the given name and analyzes the copy,
/// failing the test when the analysis fails.
pub fn ready_fixture_copy(directory: &TempDir, fixture: &str, copy_name: &str) -> AnalysisRecord {
    let path = test_fixtures::copy_fixture(directory, fixture, Some(copy_name));
    let record = analyze_file(&path, &|| false, &mut |_| {});
    assert!(record.is_ready(), "{}", record.error_message);
    record
}

/// Opens a cache in a subfolder of the directory, which the cache has to create.
pub fn temporary_cache(directory: &TempDir) -> AnalysisCache {
    let cache = AnalysisCache::with_path(directory.path().join("cache/analysis.db"));
    cache.open().expect("cache opens");
    cache
}
