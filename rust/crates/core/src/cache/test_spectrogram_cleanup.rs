//! Spectral cache cleanup: captured-identity removal, post-commit pruning, and deleted-file pruning.

use std::sync::Arc;

use rusqlite::Connection;
use tempfile::TempDir;

use super::test_spectrogram::{artifact, artifact_count};
use crate::record::MeasurementProducer;
use crate::test_helpers::{analyze_fixture_copy, temporary_cache, temporary_cache_path};

/// Discards stale source work and preserves newer artifacts during delayed captured-identity removal.
#[test]
fn changed_sources_reject_stale_publication_and_remove_only_the_captured_identity() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let old = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let old_data = artifact(&old);
    cache.store_analysis(&old).expect("old measurements");
    assert!(cache.store_spectrogram(&old, &old_data).expect("old artifact"));
    let replacement = test_fixtures::copy_fixture(&directory, "sine_stereo_44100_16.wav", Some("replacement.wav"));
    std::fs::copy(replacement, &old.path).expect("replace source");
    assert!(cache.get_spectrogram(&old.path).is_none());
    assert!(!cache.store_spectrogram(&old, &old_data).expect("reject stale artifact"));
    let new = crate::analyze_file(&old.path, &|| false, &mut |_| {});
    cache.store_analysis(&new).expect("new measurements");
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    assert_eq!(artifact_count(&connection), 0);
    assert!(cache.store_spectrogram(&new, &artifact(&new)).expect("new artifact"));
    cache.remove_analysis_for_record(&old).expect("delayed removal");
    assert!(cache.get_spectrogram(&new.path).is_some());
    assert!(cache.remove_analysis_for_record(&new).expect("current removal"));
    assert_eq!(artifact_count(&connection), 0);
}

/// Failed optional cleanup cannot roll back committed required results, gains, or measurement provenance.
#[test]
fn optional_cleanup_failure_preserves_committed_measurements() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let prior = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
    cache.store_analysis(&prior).expect("prior measurements");
    cache
        .store_custom_gain_for_record(&prior, Some(-2.5))
        .expect("prior gain");
    assert!(
        cache
            .store_spectrogram(&prior, &artifact(&prior))
            .expect("prior artifact")
    );
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    connection
        .execute_batch(
            "CREATE TRIGGER abort_spectrum_cleanup BEFORE DELETE ON spectrogram_cache
     BEGIN SELECT RAISE(ABORT, 'optional spectrum cleanup failed'); END;",
        )
        .expect("aborting optional cleanup");
    let mut fresh = prior;
    fresh.measurement_producer = MeasurementProducer::Cpp;
    fresh.channels = 1;
    fresh.waveform = None;
    let overloads = Arc::make_mut(fresh.overloads.as_mut().expect("overload history"));
    overloads.channels = 1;
    overloads.flags.truncate(overloads.flags.len() / 2);
    cache.store_analysis(&fresh).expect("required measurements commit");
    let cached = cache.get_analysis(&fresh.path).expect("committed measurement hit");
    assert_eq!(cached.measurement_producer, MeasurementProducer::Cpp);
    assert_eq!(cached.channels, 1);
    assert_eq!(cached.custom_gain_db, Some(-2.5));
    assert_eq!(cached.short_term_loudness, fresh.short_term_loudness);
    assert_eq!(cached.overloads, fresh.overloads);
    assert_eq!(artifact_count(&connection), 1);
    assert!(cache.get_spectrogram(&fresh.path).is_none());
    connection
        .execute_batch("DROP TRIGGER abort_spectrum_cleanup")
        .expect("allow cleanup");
    cache.store_analysis(&fresh).expect("retry optional cleanup");
    assert_eq!(artifact_count(&connection), 0);
}

/// Retains newer work when a delayed cleanup request no longer names the winning measurements.
#[test]
fn stale_cleanup_cannot_delete_a_newer_source_artifact() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("measurements");
    assert!(cache.store_spectrogram(&record, &artifact(&record)).expect("artifact"));
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    let mut stale = record;
    stale.modified_time_ms -= 1;
    crate::cache::spectrogram::prune_incompatible(&connection, &stale).expect("stale cleanup ignored");
    assert_eq!(artifact_count(&connection), 1);
}

/// Removes orphaned spectra even when neither a source file nor a measurement row remains.
#[test]
fn remove_cleans_orphaned_spectral_revisions_of_missing_files() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("measurements");
    assert!(cache.store_spectrogram(&record, &artifact(&record)).expect("artifact"));
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    connection
        .execute("DELETE FROM file_analysis", [])
        .expect("orphan spectrum");
    connection
        .execute("DELETE FROM rust_waveform_cache", [])
        .expect("remove native preview");
    std::fs::remove_file(&record.path).expect("remove source");
    assert!(cache.remove_analysis(&record.path).expect("orphan cleanup"));
    assert_eq!(artifact_count(&connection), 0);
}

/// Pruning deleted files removes their measurements and every artifact, while existing files keep theirs.
#[test]
fn prune_removes_every_table_row_of_deleted_files_only() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let deleted = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let kept = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
    for record in [&deleted, &kept] {
        cache.store_analysis(record).expect("measurements");
        assert!(cache.store_spectrogram(record, &artifact(record)).expect("artifact"));
    }
    std::fs::remove_file(&deleted.path).expect("delete source");
    let removed = cache.prune_missing_files(&|| false).expect("prune");
    assert!(removed >= 2, "measurement and spectral rows of the deleted file");
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    let remaining = |table: &str| -> i64 {
        connection
            .query_row(
                &format!("SELECT count(*) FROM {table} WHERE file_path = ?1"),
                [deleted.path.to_string_lossy()],
                |row| row.get(0),
            )
            .expect("row count")
    };
    for table in [
        "file_analysis",
        "rust_waveform_cache",
        "spectrogram_cache",
        "serato_tag_cache",
    ] {
        assert_eq!(remaining(table), 0, "{table} of the deleted file");
    }
    assert!(cache.get_analysis(&kept.path).is_some());
    assert!(cache.get_spectrogram(&kept.path).is_some());
}

/// Files in a missing folder may be on an unmounted drive, so their rows and gains survive pruning.
#[test]
fn prune_keeps_rows_whose_folder_is_missing_and_honours_stop() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("measurements");
    assert!(cache.store_spectrogram(&record, &artifact(&record)).expect("artifact"));
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    let unmounted = directory.path().join("unmounted").join("track.wav");
    for table in ["file_analysis", "spectrogram_cache"] {
        connection
            .execute(
                &format!("UPDATE {table} SET file_path = ?1"),
                [unmounted.to_string_lossy()],
            )
            .expect("move rows to a missing folder");
    }
    assert_eq!(cache.prune_missing_files(&|| false).expect("prune"), 0);
    assert_eq!(artifact_count(&connection), 1);
    connection
        .execute(
            "UPDATE spectrogram_cache SET file_path = ?1",
            [directory.path().join("gone.wav").to_string_lossy()],
        )
        .expect("orphan in an existing folder");
    assert_eq!(cache.prune_missing_files(&|| true).expect("stopped prune"), 0);
    assert_eq!(artifact_count(&connection), 1);
    assert_eq!(cache.prune_missing_files(&|| false).expect("prune"), 1);
    assert_eq!(artifact_count(&connection), 0);
}
