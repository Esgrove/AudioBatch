//! Shared-schema adoption, simultaneous initialization, and platform cache-location contracts.

use std::sync::{Arc, Barrier};

use rusqlite::{Connection, params};
use tempfile::TempDir;

use super::{ANALYSIS_VERSION, AnalysisCache, default_database_path};
use crate::record::{MeasurementProducer, same_custom_gain};
use crate::tags::SeratoOverviewState;
use crate::test_helpers::{analyze_fixture_copy, temporary_cache, temporary_cache_path, update_cached_measurements};

/// Refills legacy measurements for source statistics without losing persisted gain or independent tag outcomes.
#[test]
fn current_cpp_schema_migrates_only_known_successes_and_preserves_gain_and_tags() {
    let directory = TempDir::new().expect("temporary directory");
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let database = temporary_cache_path(&directory);
    {
        let cache = temporary_cache(&directory);
        cache.store_analysis(&record).expect("seed");
        cache.store_custom_gain_for_record(&record, Some(3.0)).expect("gain");
        cache
            .store_serato_overview(&record.path, &SeratoOverviewState::Absent)
            .expect("tags");
    }
    Connection::open(&database)
        .expect("connection")
        .execute_batch(
            "ALTER TABLE file_analysis DROP COLUMN analysis_producer; UPDATE file_analysis SET analysis_version = 12;",
        )
        .expect("known C++ v12 schema");
    let cache = AnalysisCache::with_path(&database);
    assert!(cache.get_analysis(&record.path).is_none());
    cache.store_analysis(&record).expect("refill source statistics");
    let cached = cache.get_analysis(&record.path).expect("refilled measurements");
    assert_eq!(cached.measurement_producer, MeasurementProducer::Rust);
    assert!(same_custom_gain(cached.custom_gain_db, Some(3.0)));
    assert_eq!(
        cache.get_serato_overview(&record.path),
        Some(SeratoOverviewState::Absent)
    );
    assert_eq!(ANALYSIS_VERSION, 15);
    cache.open().expect("idempotent migration");
}

/// Invalidates rows whose schema cannot prove their origin even when their version claims compatibility.
#[test]
fn unproven_schema_cannot_adopt_forged_current_or_v12_rows() {
    for version in [12, 13] {
        let directory = TempDir::new().expect("temporary directory");
        let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
        let database = temporary_cache_path(&directory);
        temporary_cache(&directory).store_analysis(&record).expect("seed");
        let connection = Connection::open(&database).expect("connection");
        connection
            .execute_batch(
                "ALTER TABLE file_analysis DROP COLUMN analysis_producer;
             ALTER TABLE file_analysis DROP COLUMN has_custom_gain;",
            )
            .expect("unproven schema");
        connection
            .execute("UPDATE file_analysis SET analysis_version = ?1", [version])
            .expect("unknown origin");
        let cache = AnalysisCache::with_path(&database);
        assert!(cache.get_analysis(&record.path).is_none());
        cache.store_analysis(&record).expect("native refill");
        assert_eq!(
            cache
                .get_analysis(&record.path)
                .expect("native hit")
                .measurement_producer,
            MeasurementProducer::Rust
        );
    }
}

/// Serializes simultaneous schema creation so a failed initial attempt remains safely retryable.
#[test]
fn concurrent_initialization_is_atomic_and_retryable() {
    let directory = TempDir::new().expect("temporary directory");
    let database = temporary_cache_path(&directory);
    let barrier = Arc::new(Barrier::new(2));
    std::thread::scope(|scope| {
        for _ in 0..2 {
            let barrier = Arc::clone(&barrier);
            let database = &database;
            scope.spawn(move || {
                let cache = AnalysisCache::with_path(database);
                barrier.wait();
                if cache.open().is_err() {
                    cache.open().expect("retry succeeds without partial migration");
                }
            });
        }
    });
    let connection = Connection::open(&database).expect("connection");
    let producers: i64 = connection
        .query_row(
            "SELECT count(*) FROM pragma_table_info('file_analysis') WHERE name = ?1",
            params!["analysis_producer"],
            |row| row.get(0),
        )
        .expect("schema ready");
    assert_eq!(producers, 1);
}

/// Invalidates explicitly identified legacy rows consistently with the other application's migration.
#[test]
fn explicit_cpp_origin_v12_migration_agrees_with_cpp_initialization() {
    let directory = TempDir::new().expect("temporary directory");
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    {
        let cache = temporary_cache(&directory);
        cache.store_analysis(&record).expect("seed");
    }
    update_cached_measurements(&directory, "analysis_version = 12, analysis_producer = 'cpp'");
    let cache = AnalysisCache::with_path(temporary_cache_path(&directory));
    assert!(cache.get_analysis(&record.path).is_none());
    cache.store_analysis(&record).expect("native refill");
    assert_eq!(
        cache
            .get_analysis(&record.path)
            .expect("current contract")
            .measurement_producer,
        MeasurementProducer::Rust
    );
}

/// Shares measurements at the platform cache path while keeping Rust settings in their own directory.
#[test]
fn default_cache_location_does_not_move_settings() {
    assert!(crate::cache::data_directory().ends_with("AudioBatch-rs"));
    assert!(default_database_path().ends_with("AudioBatch/analysis.db"));
    #[cfg(target_os = "macos")]
    assert_eq!(
        default_database_path(),
        dirs::home_dir().expect("home").join("Library/AudioBatch/analysis.db")
    );
    #[cfg(target_os = "windows")]
    assert_eq!(
        default_database_path(),
        dirs::config_dir()
            .expect("roaming app data")
            .join("AudioBatch/analysis.db")
    );
}
