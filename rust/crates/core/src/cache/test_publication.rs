//! Captured-source gain and preview publication without overwriting newer independent cache work.

use rusqlite::Connection;
use tempfile::TempDir;

use super::AnalysisCache;
use crate::record::{MeasurementProducer, same_custom_gain};
use crate::tags::SeratoOverviewState;
use crate::test_helpers::{analyze_fixture_copy, temporary_cache, temporary_cache_path, update_cached_measurements};
use crate::waveform::WaveformBuilder;

/// Preserves explicit gain changes and clears when another connection publishes a stale measurement record.
#[test]
fn explicit_gain_edits_and_clears_win_over_stale_measurements_on_two_connections() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let writer = AnalysisCache::with_path(temporary_cache_path(&directory));
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

/// Publishes Rust previews independently without changing gains, measurement origin, or foreign waveform bytes.
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
    update_cached_measurements(&directory, "waveform_data = x'01020304', waveform_version = 77");
    let waveform = record.waveform.take().expect("native waveform");
    assert!(cache.store_waveform(&record, &waveform).expect("artifact write"));
    cache.store_analysis(&record).expect("waveform-free measurement write");
    let cached = cache.get_analysis(&record.path).expect("cache hit");
    assert_eq!(cached.measurement_producer, MeasurementProducer::Cpp);
    assert!(same_custom_gain(cached.custom_gain_db, Some(-3.0)));
    assert_eq!(cache.get_waveform(&record.path).as_deref(), Some(waveform.as_ref()));
    let bytes: Vec<u8> = Connection::open(temporary_cache_path(&directory))
        .expect("connection")
        .query_row("SELECT waveform_data FROM file_analysis", [], |row| row.get(0))
        .expect("cpp bytes");
    assert_eq!(bytes, [1, 2, 3, 4]);
}

/// Treats corrupt optional artifacts as independent misses
/// without discarding valid measurements or negative tag states.
#[test]
fn malformed_optional_blobs_do_not_invalidate_measurements_or_negative_tag_outcomes() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("stores");
    cache
        .store_serato_overview(&record.path, &SeratoOverviewState::Invalid)
        .expect("completed invalid outcome");
    let connection = Connection::open(temporary_cache_path(&directory)).expect("connection");
    connection
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
    connection
        .execute("UPDATE serato_tag_cache SET decoded_data = NULL", [])
        .expect("restores valid negative outcome");
    assert_eq!(
        cache.get_serato_overview(&record.path),
        Some(SeratoOverviewState::Invalid)
    );
    connection
        .execute(
            "UPDATE serato_tag_cache SET result_state = 1, decoded_data = zeroblob(4194304)",
            [],
        )
        .expect("corrupt present outcome");
    assert!(cache.get_serato_overview(&record.path).is_none());
    assert!(cache.get_analysis(&record.path).is_some());
}

/// Rejects delayed measurements, artifacts, gain edits, and deletion after the on-disk source has been replaced.
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

/// Distinguishes explicit unity gain from no edit and rejects nonfinite edits without replaying stale gain values.
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

/// Rejects valid previews of the wrong stream without overwriting the existing preview or independent row fields.
#[test]
fn mismatched_waveform_geometry_preserves_the_existing_preview_and_measurements() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let mut record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    record.measurement_producer = MeasurementProducer::Cpp;
    record.custom_gain_db = Some(-3.0);
    cache.store_analysis(&record).expect("seed");
    for (rate, channels, length) in [
        (record.sample_rate + 1, record.channels, record.length_in_samples),
        (record.sample_rate, record.channels + 1, record.length_in_samples),
        (record.sample_rate, record.channels, record.length_in_samples + 1),
    ] {
        let mut builder = WaveformBuilder::new(channels as usize, Some(length));
        let samples = vec![0.25; usize::try_from(length).expect("small fixture length")];
        let planes = vec![samples.as_slice(); channels as usize];
        builder.push(&planes);
        let waveform = builder.finish(rate, length);
        assert!(!cache.store_waveform(&record, &waveform).expect("geometry rejected"));
        assert_eq!(cache.get_waveform(&record.path), record.waveform);
        let cached = cache.get_analysis(&record.path).expect("measurements retained");
        assert_eq!(cached.measurement_producer, MeasurementProducer::Cpp);
        assert!(same_custom_gain(cached.custom_gain_db, Some(-3.0)));
        assert_eq!(cached.short_term_loudness, record.short_term_loudness);
        assert_eq!(cached.overloads, record.overloads);
    }
}

/// Leaves newer parser work and conflicting stored measurement identities untouched when publishing tag outcomes.
#[test]
fn serato_publication_rejects_newer_parser_and_conflicting_measurement_identity() {
    let directory = TempDir::new().expect("temporary directory");
    let cache = temporary_cache(&directory);
    let record = analyze_fixture_copy(&directory, "sine_mono_24000_16.wav");
    cache.store_analysis(&record).expect("seed");
    cache
        .store_serato_overview(&record.path, &SeratoOverviewState::Absent)
        .expect("tag outcome");
    let connection = Connection::open(temporary_cache_path(&directory)).expect("connection");
    connection
        .execute("UPDATE serato_tag_cache SET parser_version = parser_version + 1", [])
        .expect("newer parser");
    assert!(
        !cache
            .store_serato_overview_for_record(&record, &SeratoOverviewState::Invalid)
            .expect("newer parser retained")
    );
    let state: i64 = connection
        .query_row("SELECT result_state FROM serato_tag_cache", [], |row| row.get(0))
        .expect("retained state");
    assert_eq!(state, 2);
    connection
        .execute("UPDATE serato_tag_cache SET parser_version = parser_version - 1", [])
        .expect("current parser");
    update_cached_measurements(&directory, "modified_time_ms = modified_time_ms + 1");
    assert!(
        !cache
            .store_serato_overview_for_record(&record, &SeratoOverviewState::Invalid)
            .expect("conflicting row retained")
    );
    assert_eq!(
        cache.get_serato_overview(&record.path),
        Some(SeratoOverviewState::Absent)
    );
}
