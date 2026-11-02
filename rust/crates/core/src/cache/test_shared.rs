//! Shared-contract, captured-source, concurrency, and independent-preview regression tests.

use std::sync::{Arc, Barrier};

use rusqlite::{Connection, params};
use tempfile::TempDir;

use super::{ANALYSIS_VERSION, AnalysisCache, default_database_path};
use crate::record::{AnalysisError, AnalysisStatus, MeasurementProducer, same_custom_gain};
use crate::tags::SeratoOverviewState;
use crate::test_helpers::analyze_fixture_copy;

/// Opens an explicitly located database so independent connections address the same file.
fn temporary_cache(directory: &TempDir) -> AnalysisCache {
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    cache.open().expect("cache opens");
    cache
}

/// Changes persisted columns through a second connection, representing another application.
fn update(directory: &TempDir, assignments: &str) {
    Connection::open(directory.path().join("analysis.db"))
        .expect("second connection")
        .execute_batch(&format!("UPDATE file_analysis SET {assignments}"))
        .expect("updates shared row");
}

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
        update(&directory, assignment);
        assert!(cache.get_analysis(&record.path).is_none(), "{assignment}");
    }
}

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
    update(&directory, "analysis_producer = 'cpp'");
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

#[test]
fn explicit_gain_edits_and_clears_win_over_stale_measurements_on_two_connections() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let writer = AnalysisCache::with_path(directory.path().join("analysis.db"));
    let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    record.custom_gain_db = Some(-9.0);
    cache.store_analysis(&record).expect("initial write");
    for gain in [Some(2.0), None, Some(-3.0), None] {
        assert!(
            writer
                .store_custom_gain_for_record(&record, gain)
                .expect("explicit edit")
        );
        cache.store_analysis(&record).expect("stale full write");
        let cached = cache.get_analysis(&record.path).expect("cache hit");
        assert!(same_custom_gain(cached.custom_gain_db, gain));
        assert_eq!(cached.measurement_producer, MeasurementProducer::Rust);
    }
    let mut stale_identity = record;
    stale_identity.modified_time_ms += 1;
    assert!(
        !writer
            .store_custom_gain_for_record(&stale_identity, Some(7.0))
            .expect("stale edit ignored")
    );
}

#[test]
fn independent_waveforms_preserve_gain_producer_and_cpp_bytes() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    record.measurement_producer = MeasurementProducer::Cpp;
    cache.store_analysis(&record).expect("foreign row");
    cache
        .store_custom_gain_for_record(&record, Some(-3.0))
        .expect("gain edit");
    update(&directory, "waveform_data = x'01020304', waveform_version = 77");
    let waveform = record.waveform.take().expect("native waveform");
    assert!(cache.store_waveform(&record, &waveform).expect("artifact write"));
    cache.store_analysis(&record).expect("waveform-free measurement write");
    let cached = cache.get_analysis(&record.path).expect("cache hit");
    assert_eq!(cached.measurement_producer, MeasurementProducer::Cpp);
    assert!(same_custom_gain(cached.custom_gain_db, Some(-3.0)));
    assert_eq!(cache.get_waveform(&record.path).as_deref(), Some(waveform.as_ref()));
    let bytes: Vec<u8> = Connection::open(directory.path().join("analysis.db"))
        .expect("connection")
        .query_row("SELECT waveform_data FROM file_analysis", [], |row| row.get(0))
        .expect("cpp bytes");
    assert_eq!(bytes, [1, 2, 3, 4]);
}

#[test]
fn malformed_optional_blobs_do_not_invalidate_measurements_or_negative_tag_outcomes() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("stores");
    cache
        .store_serato_overview(&record.path, &SeratoOverviewState::Invalid)
        .expect("completed invalid outcome");
    Connection::open(directory.path().join("analysis.db"))
        .expect("connection")
        .execute_batch(
            "UPDATE rust_waveform_cache SET waveform_data = zeroblob(4194304);
         UPDATE serato_tag_cache SET decoded_data = zeroblob(4194304);
         UPDATE file_analysis SET waveform_data = zeroblob(4194304);",
        )
        .expect("corrupts previews");
    assert!(cache.get_analysis(&record.path).is_some());
    assert!(cache.get_waveform(&record.path).is_none());
    assert!(
        cache.get_serato_overview(&record.path).is_none(),
        "negative outcomes cannot contain payload bytes"
    );
    Connection::open(directory.path().join("analysis.db"))
        .expect("connection")
        .execute("UPDATE serato_tag_cache SET decoded_data = NULL", [])
        .expect("restores valid negative outcome");
    assert_eq!(
        cache.get_serato_overview(&record.path),
        Some(SeratoOverviewState::Invalid)
    );
    Connection::open(directory.path().join("analysis.db"))
        .expect("connection")
        .execute(
            "UPDATE serato_tag_cache SET result_state = 1, decoded_data = zeroblob(4194304)",
            [],
        )
        .expect("corrupt present outcome");
    assert!(cache.get_serato_overview(&record.path).is_none());
    assert!(cache.get_analysis(&record.path).is_some());
}

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
        update(&directory, "custom_gain_db = 0, has_custom_gain = 0");
        cache.store_analysis(&record).expect("restores valid measurements");
        update(&directory, assignment);
        assert!(cache.get_analysis(&record.path).is_none(), "{assignment}");
    }
}

#[test]
fn source_replacement_rejects_delayed_measurements_artifacts_gain_and_removal() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let old = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&old).expect("old measurements");
    let replacement = test_fixtures::copy_fixture(&directory, "sine_stereo_44100_16.wav", Some("replacement.wav"));
    std::fs::copy(replacement, &old.path).expect("replaces source");
    let new = crate::analyze_file(&old.path, &|| false, &mut |_| {});
    cache.store_analysis(&new).expect("new measurements");
    cache.store_analysis(&old).expect("ignores delayed old analysis");
    assert!(
        !cache
            .store_waveform(&old, old.waveform.as_deref().expect("waveform"))
            .expect("stale preview")
    );
    assert!(!cache.store_custom_gain_for_record(&old, Some(9.0)).expect("stale gain"));
    assert!(
        !cache
            .store_serato_overview_for_record(&old, &SeratoOverviewState::Absent)
            .expect("stale tags")
    );
    assert!(!cache.remove_analysis_for_record(&old).expect("stale deletion"));
    let cached = cache.get_analysis(&new.path).expect("new source still hits");
    assert_eq!(cached.file_size, new.file_size);
    assert_eq!(cached.channels, new.channels);
    assert_eq!(cache.get_waveform(&new.path), new.waveform);
}

#[test]
fn current_cpp_schema_migrates_only_known_successes_and_preserves_gain_and_tags() {
    let directory = TempDir::new().expect("temporary directory");
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let database = directory.path().join("analysis.db");
    {
        let cache = AnalysisCache::with_path(&database);
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
    let cached = cache
        .get_analysis(&record.path)
        .expect("adopted known C++ measurements");
    assert_eq!(cached.measurement_producer, MeasurementProducer::Cpp);
    assert!(same_custom_gain(cached.custom_gain_db, Some(3.0)));
    assert_eq!(
        cache.get_serato_overview(&record.path),
        Some(SeratoOverviewState::Absent)
    );
    assert_eq!(ANALYSIS_VERSION, 13);
    cache.open().expect("idempotent migration");
}

#[test]
fn unproven_schema_cannot_adopt_forged_current_or_v12_rows() {
    for version in [12, 13] {
        let directory = TempDir::new().expect("temporary directory");
        let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
        let database = directory.path().join("analysis.db");
        AnalysisCache::with_path(&database)
            .store_analysis(&record)
            .expect("seed");
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

#[test]
fn concurrent_initialization_is_atomic_and_retryable() {
    let directory = TempDir::new().expect("temporary directory");
    let database = directory.path().join("analysis.db");
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

#[test]
fn explicit_zero_gain_and_invalid_edit_preserve_the_optional_gain_contract() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    record.custom_gain_db = Some(0.0);
    cache.store_analysis(&record).expect("explicit unity gain");
    let unity = cache.get_analysis(&record.path).expect("cache hit");
    assert!(same_custom_gain(unity.custom_gain_db, Some(0.0)));
    assert!(
        !cache
            .store_custom_gain_for_record(&record, Some(f32::NAN))
            .expect("reject invalid edit")
    );
    record.custom_gain_db = Some(-3.0);
    cache.store_analysis(&record).expect("stale gain write");
    assert!(same_custom_gain(
        cache.get_analysis(&record.path).expect("cache hit").custom_gain_db,
        Some(0.0)
    ));
    cache
        .store_custom_gain_for_record(&record, None)
        .expect("explicit clear");
    cache.store_analysis(&record).expect("stale gain write");
    assert!(
        cache
            .get_analysis(&record.path)
            .expect("cache hit")
            .custom_gain_db
            .is_none()
    );
}

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
        update(&directory, assignment);
        cache.store_analysis(&failure).expect("native refill");
        assert!(
            cache
                .get_analysis(&record.path)
                .expect("native failure hit")
                .has_error()
        );
    }
}

#[test]
fn foreign_failure_cannot_be_relabelled_as_a_current_contract_measurement() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "broken.wav");
    record.measurement_producer = MeasurementProducer::Cpp;
    cache.store_analysis(&record).expect("foreign failure ignored");
    let rows: i64 = Connection::open(directory.path().join("analysis.db"))
        .expect("connection")
        .query_row("SELECT count(*) FROM file_analysis", [], |row| row.get(0))
        .expect("row count");
    assert_eq!(rows, 0);
}

#[test]
fn explicit_cpp_origin_v12_migration_agrees_with_cpp_initialization() {
    let directory = TempDir::new().expect("temporary directory");
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    {
        let cache = temporary_cache(&directory);
        cache.store_analysis(&record).expect("seed");
    }
    update(&directory, "analysis_version = 12, analysis_producer = 'cpp'");
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    assert_eq!(
        cache
            .get_analysis(&record.path)
            .expect("adopted C++ v12")
            .measurement_producer,
        MeasurementProducer::Cpp
    );
}

#[test]
fn fractional_history_sample_rate_is_not_a_valid_measurement() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let history = Arc::make_mut(record.short_term_loudness.as_mut().expect("history"));
    history.sample_rate += 0.25;
    assert!(cache.store_analysis(&record).is_err());
}

#[test]
fn default_cache_location_does_not_move_settings() {
    assert!(crate::cache::data_directory().ends_with("AudioBatch-rs"));
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
