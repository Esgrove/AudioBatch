//! Builders shared by the unit tests of several core modules.
//!
//! They live here rather than in the `test-fixtures` crate, because they call into the core crate,
//! which `test-fixtures` cannot depend on.

use std::fs;
use std::path::{Path, PathBuf};

use crossbeam_channel::{Sender, bounded};
use rayon::ThreadPool;
use tempfile::TempDir;

use crate::analysis::analyze_file;
use crate::cache::AnalysisCache;
use crate::record::AnalysisRecord;

/// Analyzes a file without cancellation or progress.
pub fn analyze(path: &Path) -> AnalysisRecord {
    analyze_file(path, &|| false, &mut |_| {})
}

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

/// Copies a fixture into the directory under the given name and cuts the copy to `length` bytes,
/// the way an interrupted download or copy leaves a file.
pub fn truncated_fixture_copy(directory: &TempDir, fixture: &str, copy_name: &str, length: u64) -> PathBuf {
    let path = test_fixtures::copy_fixture(directory, fixture, Some(copy_name));
    fs::OpenOptions::new()
        .write(true)
        .open(&path)
        .and_then(|file| file.set_len(length))
        .expect("truncates the copy");
    path
}

/// Copies a fixture into the directory under the given name and cuts the copy to half its length.
pub fn half_fixture_copy(directory: &TempDir, fixture: &str, copy_name: &str) -> PathBuf {
    let length = fs::metadata(test_fixtures::fixture(fixture))
        .expect("fixture metadata")
        .len();
    truncated_fixture_copy(directory, fixture, copy_name, length / 2)
}

/// Copies a fixture into the directory under the given name
/// and overwrites the middle third of the copy with a fixed pattern of bytes that is not audio data,
/// so the container and the codec meet damaged packets after a valid start.
pub fn scrambled_fixture_copy(directory: &TempDir, fixture: &str, copy_name: &str) -> PathBuf {
    let path = test_fixtures::copy_fixture(directory, fixture, Some(copy_name));
    let mut bytes = fs::read(&path).expect("reads the copy");
    let third = bytes.len() / 3;
    for (index, byte) in bytes.iter_mut().enumerate().skip(third).take(third) {
        *byte = (index * 7919 % 251) as u8;
    }
    fs::write(&path, bytes).expect("writes the copy");
    path
}

/// Opens a cache in a subfolder of the directory, which the cache has to create.
pub fn temporary_cache(directory: &TempDir) -> AnalysisCache {
    let cache = AnalysisCache::with_path(directory.path().join("cache/analysis.db"));
    cache.open().expect("cache opens");
    cache
}

/// Occupies every worker of the pool until the returned sender is dropped,
/// so jobs queued in the meantime start only once the test lets them.
/// Returns once every worker runs a blocking job.
pub fn block_pool(pool: &ThreadPool) -> Sender<()> {
    let workers = pool.current_num_threads();
    let (release, released) = bounded::<()>(0);
    let (started_sender, started) = bounded(workers);
    for _ in 0..workers {
        let released = released.clone();
        let started_sender = started_sender.clone();
        pool.spawn(move || {
            let _ = started_sender.send(());
            let _ = released.recv();
        });
    }
    for _ in 0..workers {
        started.recv().expect("a worker runs the blocking job");
    }
    release
}
