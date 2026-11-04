//! Optional spectral cache contracts, independent revisions, and captured-source publication tests.

use std::sync::Arc;
use std::time::{Duration, UNIX_EPOCH};

use rusqlite::types::Value;
use rusqlite::{Connection, params};
use tempfile::TempDir;

use super::AnalysisCache;
use crate::record::{AnalysisRecord, MeasurementProducer};
use crate::spectrogram::SpectrogramData;
use crate::test_helpers::{analyze_fixture_copy, temporary_cache, temporary_cache_path, update_cached_measurements};

/// Constructs a deterministic valid shared envelope without depending on FFT output or builder behavior.
fn artifact(record: &AnalysisRecord) -> SpectrogramData {
    let mut stride = 1024_u64;
    while record.length_in_samples.div_ceil(stride) > 4096 {
        stride *= 2;
    }
    let columns = u32::try_from(record.length_in_samples.div_ceil(stride)).expect("bounded fixture columns");
    let mut bytes = Vec::new();
    for value in [
        0x4345_5053_u32,
        1,
        1,
        4096,
        1024,
        512,
        4096,
        record.sample_rate,
        record.channels,
        columns,
    ] {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    for value in [
        record.length_in_samples,
        stride,
        (-120.0_f64).to_bits(),
        24.0_f64.to_bits(),
    ] {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes.extend((0..columns * 512).map(|index| u8::try_from(index % 251).expect("bounded intensity")));
    SpectrogramData::from_bytes(&bytes).expect("valid independent shared envelope")
}

/// Reads every measurement column so optional writes cannot silently change gain, provenance, or histories.
fn measurement_values(connection: &Connection) -> Vec<Value> {
    connection
        .query_row("SELECT * FROM file_analysis", [], |row| {
            (0..row.as_ref().column_count()).map(|column| row.get(column)).collect()
        })
        .expect("stored measurement columns")
}

/// Counts spectral revisions without requiring successful measurement adoption.
fn artifact_count(connection: &Connection) -> i64 {
    connection
        .query_row("SELECT count(*) FROM spectrogram_cache", [], |row| row.get(0))
        .expect("spectral row count")
}

/// Publishes only after compatible successful measurements exist, with optional absence remaining a measurement hit.
#[test]
fn missing_artifacts_and_absent_measurements_are_independent() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let data = artifact(&record);
    assert!(cache.get_spectrogram(&record.path).is_none());
    assert!(!cache.store_spectrogram(&record, &data).expect("no measurement row"));
    cache.store_analysis(&record).expect("measurements");
    assert!(cache.get_analysis(&record.path).is_some());
    assert!(cache.get_spectrogram(&record.path).is_none());
    assert!(cache.store_spectrogram(&record, &data).expect("independent artifact"));
    let reader = AnalysisCache::with_path(temporary_cache_path(&directory));
    let loaded = reader.get_spectrogram(&record.path).expect("spectral hit");
    assert_eq!(loaded.to_bytes(), data.to_bytes());
    assert!(loaded.matches_stream(record.sample_rate, record.length_in_samples, record.channels));
}

/// Never alters any measurement column while publishing optional spectra or adopting a foreign artifact producer.
#[test]
fn spectra_preserve_measurements_gain_clears_and_foreign_provenance() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
    record.measurement_producer = MeasurementProducer::Cpp;
    cache.store_analysis(&record).expect("foreign measurements");
    let data = artifact(&record);
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    for gain in [Some(-2.5), None, Some(0.0), None] {
        cache
            .store_custom_gain_for_record(&record, gain)
            .expect("explicit gain edit");
        update_cached_measurements(&directory, "updated_at_ms = 123");
        let before = measurement_values(&connection);
        assert!(cache.store_spectrogram(&record, &data).expect("artifact publication"));
        assert_eq!(measurement_values(&connection), before);
        for producer in ["cpp", "rust"] {
            connection
                .execute("UPDATE spectrogram_cache SET producer = ?1", [producer])
                .expect("foreign artifact producer");
            assert_eq!(
                cache.get_spectrogram(&record.path).expect("shared hit").to_bytes(),
                data.to_bytes()
            );
            assert_eq!(measurement_values(&connection), before);
        }
    }
}

/// Failed optional publication does not modify any previously committed required measurement column.
#[test]
fn optional_publication_failure_leaves_required_measurements_committed() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("measurements commit first");
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    let before = measurement_values(&connection);
    connection
        .execute_batch(
            "CREATE TRIGGER abort_spectrum_publication BEFORE INSERT ON spectrogram_cache
             BEGIN SELECT RAISE(ABORT, 'optional spectrum publication failed'); END;",
        )
        .expect("aborting optional publication");
    assert!(cache.store_spectrogram(&record, &artifact(&record)).is_err());
    assert!(cache.get_analysis(&record.path).is_some());
    assert_eq!(measurement_values(&connection), before);
    assert_eq!(artifact_count(&connection), 0);
}

/// Uses measurement geometry rather than loudness history validity to adopt a separately valid artifact.
#[test]
fn spectral_lookup_does_not_hydrate_required_level_histories() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("measurements");
    assert!(cache.store_spectrogram(&record, &artifact(&record)).expect("artifact"));
    update_cached_measurements(&directory, "short_term_loudness = x'00', overload_history = x'00'");
    assert!(cache.get_analysis(&record.path).is_none());
    assert!(cache.get_spectrogram(&record.path).is_some());
    assert!(
        cache
            .store_spectrogram(&record, &artifact(&record))
            .expect("geometry-only publication")
    );
}

/// Treats malformed optional payloads and metadata as artifact misses without invalidating measured levels.
#[test]
fn corrupt_and_oversized_spectra_do_not_invalidate_measurements() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
    cache.store_analysis(&record).expect("measurements");
    let data = artifact(&record);
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    for assignment in [
        "spectrogram_data = x'00'",
        "spectrogram_data = zeroblob(2097409)",
        "spectrogram_data = CAST(spectrogram_data AS TEXT)",
        "spectrogram_data = substr(spectrogram_data, 1, length(spectrogram_data) - 1)",
        "spectrogram_data = CAST(spectrogram_data || x'00' AS BLOB)",
        "sample_rate = sample_rate + 1",
        "sample_rate = 44100.5",
        "num_channels = num_channels + 1",
        "length_in_samples = length_in_samples + 1",
        "producer = 'unknown'",
        "file_size = file_size + 1",
        "modified_time_ms = modified_time_ms + 1",
    ] {
        assert!(cache.store_spectrogram(&record, &data).expect("restore valid artifact"));
        connection
            .execute_batch(&format!("UPDATE spectrogram_cache SET {assignment}"))
            .expect("corrupt optional artifact");
        assert!(cache.get_spectrogram(&record.path).is_none(), "{assignment}");
        assert!(cache.get_analysis(&record.path).is_some(), "{assignment}");
    }
}

/// Rejects incompatible measurement status, revision, producer, identity, or geometry on reads and writes.
#[test]
fn current_successful_measurement_geometry_gates_lookup_and_publication() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
    let data = artifact(&record);
    for assignment in [
        "status = 0",
        "status = 3",
        "analysis_version = 12",
        "analysis_version = 99",
        "analysis_producer = 'unknown'",
        "channels = 1",
        "sample_rate = 44100.5",
        "length_in_samples = length_in_samples + 1",
        "file_size = file_size + 1",
        "modified_time_ms = modified_time_ms + 1",
    ] {
        cache.store_analysis(&record).expect("restore measurements");
        assert!(cache.store_spectrogram(&record, &data).expect("restore artifact"));
        update_cached_measurements(&directory, assignment);
        assert!(cache.get_spectrogram(&record.path).is_none(), "{assignment}");
        assert!(
            !cache.store_spectrogram(&record, &data).expect("gated publication"),
            "{assignment}"
        );
    }
}

/// Keeps future algorithm and serialization revisions intact during current writes and same-source refills.
#[test]
fn future_revisions_coexist_and_survive_compatible_measurement_refills() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    let data = artifact(&record);
    cache.store_analysis(&record).expect("measurements");
    assert!(cache.store_spectrogram(&record, &data).expect("current artifact"));
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    connection
        .execute_batch(
            "INSERT INTO spectrogram_cache
             SELECT file_path, algorithm_version + 1, serialization_version + 1, file_size, modified_time_ms,
                sample_rate, num_channels, length_in_samples, producer, x'010203' FROM spectrogram_cache;",
        )
        .expect("future revision");
    assert!(cache.store_spectrogram(&record, &data).expect("current refill"));
    cache.store_analysis(&record).expect("compatible measurement refill");
    assert_eq!(artifact_count(&connection), 2);
    let future: Vec<u8> = connection
        .query_row(
            "SELECT spectrogram_data FROM spectrogram_cache WHERE algorithm_version = 2",
            [],
            |row| row.get(0),
        )
        .expect("untouched future bytes");
    assert_eq!(future, [1, 2, 3]);
    connection
        .execute("DELETE FROM spectrogram_cache WHERE algorithm_version = 1", [])
        .expect("future only");
    assert!(cache.get_spectrogram(&record.path).is_none());
    assert!(cache.get_analysis(&record.path).is_some());
}

/// Rejects valid spectral bytes from a different stream without touching the previously published artifact.
#[test]
fn mismatched_artifact_geometry_does_not_overwrite_existing_spectra() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_stereo_44100_16.wav");
    let data = artifact(&record);
    cache.store_analysis(&record).expect("measurements");
    assert!(cache.store_spectrogram(&record, &data).expect("original artifact"));
    for (rate, channels, length) in [
        (record.sample_rate + 1, record.channels, record.length_in_samples),
        (record.sample_rate, 1, record.length_in_samples),
        (record.sample_rate, record.channels, record.length_in_samples + 1),
    ] {
        let mut other = record.clone();
        other.sample_rate = rate;
        other.channels = channels;
        other.length_in_samples = length;
        assert!(
            !cache
                .store_spectrogram(&record, &artifact(&other))
                .expect("reject geometry")
        );
        assert_eq!(
            cache
                .get_spectrogram(&record.path)
                .expect("original retained")
                .to_bytes(),
            data.to_bytes()
        );
    }
}

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
    super::spectrogram::prune_incompatible(&connection, &stale).expect("stale cleanup ignored");
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

/// Rejects competing persisted identities and on-disk timestamp changes without publishing an artifact.
#[test]
fn conflicting_measurement_identity_and_source_time_changes_reject_publication() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("measurements");
    let data = artifact(&record);
    let connection = Connection::open(temporary_cache_path(&directory)).expect("independent connection");
    connection
        .execute(
            "UPDATE file_analysis SET modified_time_ms = ?1",
            params![record.modified_time_ms + 1],
        )
        .expect("competing identity");
    assert!(
        !cache
            .store_spectrogram(&record, &data)
            .expect("conflicting source identity")
    );
    assert_eq!(artifact_count(&connection), 0);
    let modified =
        UNIX_EPOCH + Duration::from_millis(u64::try_from(record.modified_time_ms + 10_000).expect("positive time"));
    std::fs::File::options()
        .write(true)
        .open(&record.path)
        .expect("source")
        .set_modified(modified)
        .expect("change source time");
    assert!(
        !cache
            .store_spectrogram(&record, &data)
            .expect("source changed without changing size")
    );
}
