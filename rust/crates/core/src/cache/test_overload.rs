//! Regression tests for required overload histories and non-destructive analysis cache refills.

use std::path::Path;
use std::sync::Arc;

use rusqlite::params;
use serato_tags::{BAND_COUNT, Overview};
use tempfile::TempDir;

use super::{AnalysisCache, CacheError, path_key};
use crate::record::{AnalysisError, AnalysisRecord, AnalysisStatus, same_custom_gain};
use crate::tags::SeratoOverviewState;
use crate::test_helpers::{analyze_fixture_copy, temporary_cache};

/// Reads the independent loudness, waveform, and Serato blobs without interpreting or rewriting them.
fn unchanged_blobs(cache: &AnalysisCache, path: &Path) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
    cache
        .with_connection(|connection| {
            Ok(connection.query_row(
                "SELECT analysis.short_term_loudness, analysis.waveform_data, serato.decoded_data
                 FROM file_analysis AS analysis JOIN serato_tag_cache AS serato
                 ON analysis.file_path = serato.file_path WHERE analysis.file_path = ?1",
                params![path_key(path)],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )?)
        })
        .expect("reads unchanged blobs")
}

/// Checks that an unusable analysis misses and refills without losing a gain, an explicit clear, or Serato data.
fn assert_miss_refills_preserving_user_data(assignment: &str) {
    let directory = TempDir::new().expect("temp dir");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    record.custom_gain_db = Some(-9.0);
    let serato = SeratoOverviewState::Present(Arc::new(Overview::from_slices(vec![[42; BAND_COUNT]; 2])));
    for expected_gain in [Some(3.0), None] {
        cache.store_analysis(&record).expect("stores valid analysis");
        assert!(
            cache
                .store_custom_gain(&record.path, expected_gain)
                .expect("sets or clears gain")
        );
        cache
            .store_serato_overview(&record.path, &serato)
            .expect("stores independent Serato data");
        let blobs = unchanged_blobs(&cache, &record.path);
        cache
            .with_connection(
                |connection| Ok(connection.execute(&format!("UPDATE file_analysis SET {assignment}"), [])?),
            )
            .expect("invalidates the analysis");

        assert!(cache.get_analysis(&record.path).is_none(), "{assignment}");
        assert!(same_custom_gain(
            cache.get_custom_gain(&record.path, record.file_size, record.modified_time_ms),
            expected_gain
        ));
        assert_eq!(cache.get_serato_overview(&record.path), Some(serato.clone()));
        assert_eq!(unchanged_blobs(&cache, &record.path), blobs);

        cache.store_analysis(&record).expect("refills analysis");
        let cached = cache.get_analysis(&record.path).expect("refilled cache hit");
        assert_eq!(cached.status, AnalysisStatus::Cached);
        assert_eq!(cached.overloads, record.overloads);
        assert!(same_custom_gain(cached.custom_gain_db, expected_gain));
        assert_eq!(cache.get_serato_overview(&record.path), Some(serato.clone()));
        assert_eq!(unchanged_blobs(&cache, &record.path), blobs);
    }
}

#[test]
fn all_clear_history_round_trips_as_present_data() {
    let directory = TempDir::new().expect("temp dir");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let mut history = record.overloads.as_deref().expect("analyzed overload history").clone();
    assert_ne!(history.flags, [] as [u8; 0]);
    history.flags.fill(0);
    let bytes = history.to_bytes().expect("all-clear history serializes");
    record.overloads = Some(Arc::new(history));

    cache.store_analysis(&record).expect("stores all-clear history");
    let cached = cache.get_analysis(&record.path).expect("cache hit");
    assert_eq!(cached.overloads, record.overloads);
    let stored: Vec<u8> = cache
        .with_connection(|connection| {
            Ok(connection.query_row(
                "SELECT overload_history FROM file_analysis WHERE file_path = ?1",
                params![path_key(&record.path)],
                |row| row.get(0),
            )?)
        })
        .expect("reads stored overload history");
    assert_eq!(stored, bytes);
}

#[test]
fn missing_history_misses_then_refills_preserving_user_data() {
    assert_miss_refills_preserving_user_data("overload_history = NULL");
}

#[test]
fn malformed_history_misses_then_refills_preserving_user_data() {
    for assignment in ["overload_history = x''", "overload_history = x'00'"] {
        assert_miss_refills_preserving_user_data(assignment);
    }
}

#[test]
fn mismatched_stream_metadata_misses_then_refills_preserving_user_data() {
    for assignment in [
        "sample_rate = sample_rate + 1",
        "length_in_samples = length_in_samples + 1",
        "channels = channels + 1",
        "duration_seconds = duration_seconds + 1",
    ] {
        assert_miss_refills_preserving_user_data(assignment);
    }
}

#[test]
fn unknown_history_version_misses_then_refills_preserving_user_data() {
    assert_miss_refills_preserving_user_data(
        "overload_history = CAST(substr(overload_history, 1, 4) || x'ffffffff' || substr(overload_history, 9) AS BLOB)",
    );
}

#[test]
fn older_analysis_version_misses_then_refills_preserving_user_data() {
    assert_miss_refills_preserving_user_data("analysis_version = 3");
}

#[test]
fn missing_or_invalid_history_cannot_replace_a_valid_row() {
    let directory = TempDir::new().expect("temp dir");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("stores valid analysis");
    cache.store_custom_gain(&record.path, Some(3.0)).expect("stores gain");
    let mut missing = record.clone();
    missing.overloads = None;
    let mut malformed = record.clone();
    let mut history = malformed.overloads.as_deref().expect("overload history").clone();
    history.flags.clear();
    malformed.overloads = Some(Arc::new(history));
    let mut wrong_rate = record.clone();
    wrong_rate.sample_rate += 1;
    let mut wrong_length = record.clone();
    wrong_length.length_in_samples += 1;
    let mut wrong_channels = record.clone();
    wrong_channels.channels += 1;
    let mut wrong_duration = record.clone();
    wrong_duration.duration_seconds += 1.0;
    let mut non_finite_duration = record.clone();
    non_finite_duration.duration_seconds = f64::NAN;
    for invalid in [
        missing,
        malformed,
        wrong_rate,
        wrong_length,
        wrong_channels,
        wrong_duration,
        non_finite_duration,
    ] {
        assert!(matches!(
            cache.store_analysis(&invalid),
            Err(CacheError::InvalidOverloadHistory)
        ));
        let cached = cache.get_analysis(&record.path).expect("original row still hits");
        assert_eq!(cached.overloads, record.overloads);
        assert!(same_custom_gain(cached.custom_gain_db, Some(3.0)));
    }
}

#[test]
fn repeatable_failure_requires_no_overload_history() {
    let directory = TempDir::new().expect("temp dir");
    let cache = temporary_cache(&directory);
    let path = directory.path().join("broken.wav");
    std::fs::write(&path, b"not an audio stream").expect("writes broken file");
    let mut record = AnalysisRecord::from_path(&path);
    record.status = AnalysisStatus::Failed;
    record.error = Some(AnalysisError::Unsupported);
    record.error_message = "Unsupported audio stream".to_owned();
    assert!(record.overloads.is_none());
    cache.store_analysis(&record).expect("stores repeatable failure");
    let cached = cache.get_analysis(&path).expect("failed cache hit");
    assert_eq!(cached.status, AnalysisStatus::Failed);
    assert_eq!(cached.error, Some(AnalysisError::Unsupported));
    assert!(cached.overloads.is_none());
}

#[test]
fn migration_adds_history_without_removing_gains_or_serato() {
    let directory = TempDir::new().expect("temp dir");
    let database_path = directory.path().join("analysis.db");
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let serato = SeratoOverviewState::Present(Arc::new(Overview::from_slices(vec![[42; BAND_COUNT]; 2])));
    let blobs = {
        let cache = AnalysisCache::with_path(&database_path);
        cache.store_analysis(&record).expect("stores valid analysis");
        cache.store_custom_gain(&record.path, Some(3.0)).expect("stores gain");
        cache
            .store_serato_overview(&record.path, &serato)
            .expect("stores independent Serato data");
        let blobs = unchanged_blobs(&cache, &record.path);
        cache
            .with_connection(|connection| {
                connection.execute_batch(
                    "ALTER TABLE file_analysis DROP COLUMN overload_history;
                     UPDATE file_analysis SET analysis_version = 3;",
                )?;
                Ok(())
            })
            .expect("simulates earlier schema");
        blobs
    };

    let cache = AnalysisCache::with_path(&database_path);
    cache.open().expect("migrates earlier schema");
    assert!(cache.get_analysis(&record.path).is_none());
    assert!(same_custom_gain(
        cache.get_custom_gain(&record.path, record.file_size, record.modified_time_ms),
        Some(3.0)
    ));
    assert_eq!(cache.get_serato_overview(&record.path), Some(serato.clone()));
    assert_eq!(unchanged_blobs(&cache, &record.path), blobs);
    cache.store_analysis(&record).expect("refills migrated cache");
    let cached = cache.get_analysis(&record.path).expect("refilled cache hit");
    assert_eq!(cached.overloads, record.overloads);
    assert!(same_custom_gain(cached.custom_gain_db, Some(3.0)));
    assert_eq!(cache.get_serato_overview(&record.path), Some(serato));
    assert_eq!(unchanged_blobs(&cache, &record.path), blobs);
}
