//! Shared measurement provenance, required-value validation, and producer-scoped failure tests.

use std::sync::Arc;

use rusqlite::Connection;
use tempfile::TempDir;

use crate::record::{AnalysisError, AnalysisStatus, MeasurementProducer};
use crate::test_helpers::{analyze_fixture_copy, temporary_cache, temporary_cache_path, update_cached_measurements};

/// Reuses successful measurements from either producer without requiring or hydrating a native waveform.
#[test]
fn successful_measurements_reuse_both_producers_without_waveforms() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_stereo_44100_320k.mp3");
    record.waveform = None;
    for producer in [MeasurementProducer::Cpp, MeasurementProducer::Rust] {
        record.measurement_producer = producer;
        cache
            .store_analysis(&record)
            .expect("stores waveform-free measurements");
        let cached = cache.get_analysis(&record.path).expect("shared hit");
        assert_eq!(cached.measurement_producer, producer);
        assert!(cached.waveform.is_none());
        assert!(cache.get_waveform(&record.path).is_none());
        assert_eq!(cached.short_term_loudness, record.short_term_loudness);
        assert_eq!(cached.overloads, record.overloads);
    }
}

/// Rejects unrecognized provenance, contract versions, and statuses instead of presenting them as measured results.
#[test]
fn unknown_producers_versions_and_statuses_never_become_successes() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    for assignment in [
        "analysis_producer = 'unknown'",
        "analysis_producer = ''",
        "analysis_version = 12",
        "analysis_version = 6",
        "status = 0",
        "status = -1",
        "status = 99",
    ] {
        cache.store_analysis(&record).expect("seeds valid row");
        update_cached_measurements(&directory, assignment);
        assert!(cache.get_analysis(&record.path).is_none(), "{assignment}");
    }
}

/// Keeps failures decoder-specific and prevents a native failure from replacing compatible foreign success.
#[test]
fn failures_are_producer_scoped_and_cannot_replace_same_source_success() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "broken.wav");
    cache.store_analysis(&record).expect("native failure");
    assert!(
        cache
            .get_analysis(&record.path)
            .expect("native negative hit")
            .has_error()
    );
    update_cached_measurements(&directory, "analysis_producer = 'cpp'");
    assert!(cache.get_analysis(&record.path).is_none());
    record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    record.measurement_producer = MeasurementProducer::Cpp;
    cache.store_analysis(&record).expect("foreign success");
    let mut failure = record.clone();
    failure.status = AnalysisStatus::Failed;
    failure.error = Some(AnalysisError::Unsupported);
    failure.measurement_producer = MeasurementProducer::Rust;
    failure.waveform = None;
    cache.store_analysis(&failure).expect("ignores failure");
    assert_eq!(
        cache
            .get_analysis(&record.path)
            .expect("success retained")
            .measurement_producer,
        MeasurementProducer::Cpp
    );
    assert_eq!(cache.get_waveform(&record.path), record.waveform);
}

/// Rejects malformed required fields and oversized histories before adopting untrusted database contents.
#[test]
fn malformed_required_values_and_bounded_histories_miss_without_panicking() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    for assignment in [
        "length_in_samples = -1",
        "sample_rate = 44100.5",
        "analysis_version = 13.5",
        "has_custom_gain = 2",
        "custom_gain_db = 1e999",
        "format_name = zeroblob(1)",
        "error_message = zeroblob(1)",
        "file_name = printf('%.*c', 4097, 'a')",
        "error_message = printf('%.*c', 65537, 'a')",
        "sample_rate = 0",
        "channels = 65",
        "duration_seconds = -1",
        "overall_peak = 1e100",
        "overall_true_peak = -1",
        "integrated_lufs = 1e100",
        "short_term_loudness = zeroblob(16777217)",
        "short_term_loudness = x'00'",
        "short_term_loudness = 'not a blob'",
        "overload_history = zeroblob(1048576)",
        "overload_history = x'00'",
        "file_name = zeroblob(1)",
    ] {
        update_cached_measurements(&directory, "custom_gain_db = 0, has_custom_gain = 0");
        cache.store_analysis(&record).expect("restores valid measurements");
        update_cached_measurements(&directory, assignment);
        assert!(cache.get_analysis(&record.path).is_none(), "{assignment}");
    }
}

/// Leaves a successful preview untouched when a same-source failure loses measurement publication.
#[test]
fn ignored_failure_does_not_prune_or_replace_successful_native_preview() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("success");
    let mut failure = record.clone();
    failure.status = AnalysisStatus::Failed;
    failure.error = Some(AnalysisError::Unsupported);
    failure.sample_rate = 0;
    failure.channels = 0;
    failure.duration_seconds = 0.0;
    failure.length_in_samples = 0;
    failure.waveform = None;
    cache.store_analysis(&failure).expect("ignored failure");
    assert!(cache.get_analysis(&record.path).expect("success retained").is_ready());
    assert_eq!(cache.get_waveform(&record.path), record.waveform);
}

/// Allows a repeatable native failure to replace successes whose contract or producer cannot be trusted.
#[test]
fn obsolete_and_unknown_successes_do_not_block_native_negative_refill() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let mut failure = record.clone();
    failure.status = AnalysisStatus::Failed;
    failure.error = Some(AnalysisError::Unsupported);
    failure.waveform = None;
    for assignment in ["analysis_version = 0", "analysis_producer = 'unknown'"] {
        cache.store_analysis(&record).expect("success");
        update_cached_measurements(&directory, assignment);
        cache.store_analysis(&failure).expect("native refill");
        assert!(
            cache
                .get_analysis(&record.path)
                .expect("native failure hit")
                .has_error()
        );
    }
}

/// Refuses to republish another decoder's failure as a current native negative result.
#[test]
fn foreign_failure_cannot_be_relabelled_as_a_current_contract_measurement() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "broken.wav");
    record.measurement_producer = MeasurementProducer::Cpp;
    cache.store_analysis(&record).expect("foreign failure ignored");
    let rows: i64 = Connection::open(temporary_cache_path(&directory))
        .expect("connection")
        .query_row("SELECT count(*) FROM file_analysis", [], |row| row.get(0))
        .expect("row count");
    assert_eq!(rows, 0);
}

/// Requires history timing to retain the exact integer sample rate of the measured stream.
#[test]
fn fractional_history_sample_rate_is_not_a_valid_measurement() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let history = Arc::make_mut(record.short_term_loudness.as_mut().expect("history"));
    history.sample_rate += 0.25;
    assert!(cache.store_analysis(&record).is_err());
}
