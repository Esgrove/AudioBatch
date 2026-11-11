//! One-decode native waveform and spectrogram construction for missing audio artifacts.

use std::cell::Cell;

use audiobatch_core::analyze_file;

use super::*;

/// Proves the combined pass preserves the decoder timeline and independently measured waveform overview.
#[test]
fn combined_decode_matches_analysis_for_lossless_lossy_and_silent_sources() {
    for name in [
        "sine_mono_24000_16.wav",
        "sine_stereo_44100_16.wav",
        "sine_stereo_44100_320k.mp3",
        "silence_stereo_44100_16.wav",
    ] {
        let record = analyze_file(&test_fixtures::fixture(name), &|| false, &mut |_| {});
        assert!(record.is_ready(), "{name}: {}", record.error_message);
        let actual = build_audio_artifacts(&record, true, true, || true).expect("combined artifact decode");
        assert_eq!(actual.waveform, record.waveform, "{name}");
        let spectral = actual.spectrogram.expect("supported source spectrum");
        assert!(spectral.matches_stream(record.sample_rate, record.length_in_samples, record.channels));
    }
}

/// Rejects cancellation and changed identities before trying to open an invalid or replaced source.
#[test]
fn cancelled_and_replaced_sources_do_not_begin_decoding() {
    let directory = tempfile::TempDir::new().expect("source directory");
    let path = test_fixtures::copy_fixture(&directory, "sine_mono_24000_16.wav", None);
    let record = analyze_file(&path, &|| false, &mut |_| {});
    assert!(
        build_audio_artifacts(&record, true, true, || false)
            .expect("cancelled")
            .geometry
            .is_none()
    );
    std::fs::write(&path, b"replacement").expect("replaces source");
    assert!(
        build_audio_artifacts(&record, true, true, || true)
            .expect("stale identity")
            .geometry
            .is_none()
    );
}

/// An existing identity does not authorize geometry from a different stream or a failed decoder.
#[test]
fn mismatched_geometry_and_decode_errors_do_not_publish_waveforms() {
    let mut record = analyze_file(
        &test_fixtures::fixture("sine_mono_24000_16.wav"),
        &|| false,
        &mut |_| {},
    );
    record.channels += 1;
    assert!(build_audio_artifacts(&record, true, true, || true).is_err());
    record.channels -= 1;
    record.sample_rate += 1;
    assert!(build_audio_artifacts(&record, true, true, || true).is_err());
    let broken = analyze_file(&test_fixtures::fixture("broken.wav"), &|| false, &mut |_| {});
    assert!(build_audio_artifacts(&broken, true, true, || true).is_err());
}

/// Cancellation after decoding starts must discard every accumulated bucket instead of publishing partial PCM.
#[test]
fn cancellation_between_packets_discards_the_partial_waveform() {
    let record = analyze_file(
        &test_fixtures::fixture("sine_stereo_44100_16.wav"),
        &|| false,
        &mut |_| {},
    );
    let checks = Cell::new(0);
    let actual = build_audio_artifacts(&record, true, true, || {
        checks.set(checks.get() + 1);
        checks.get() < 3
    })
    .expect("cancelled waveform decode");
    assert!(actual.waveform.is_none());
    assert!(actual.spectrogram.is_none());
    assert!(actual.geometry.is_none());
    assert_eq!(checks.get(), 3);
}

/// An already-open decoder cannot publish original PCM after equal-stat path replacement.
#[test]
fn replacement_after_decoder_open_discards_both_native_artifacts() {
    let directory = tempfile::TempDir::new().expect("source directory");
    let path = test_fixtures::copy_fixture(&directory, "sine_mono_24000_16.wav", None);
    let mut record = analyze_file(&path, &|| false, &mut |_| {});
    assert!(record.capture_source_identity());
    let modified = std::fs::metadata(&path)
        .expect("source metadata")
        .modified()
        .expect("source time");
    let replacement = directory.path().join("replacement.wav");
    std::fs::copy(&path, &replacement).expect("independent replacement object");
    std::fs::File::options()
        .write(true)
        .open(&replacement)
        .expect("replacement handle")
        .set_times(std::fs::FileTimes::new().set_modified(modified))
        .expect("same modification time");
    let checks = Cell::new(0);
    let actual = build_audio_artifacts(&record, true, true, || {
        checks.set(checks.get() + 1);
        if checks.get() == 3 {
            std::fs::rename(&replacement, &path).expect("replace opened decoder source");
            assert_eq!(
                file_identity(&path),
                Some(audiobatch_core::record::FileIdentity {
                    file_size: record.file_size,
                    modified_time_ms: record.modified_time_ms
                })
            );
        }
        true
    })
    .expect("source-replaced decode");
    assert!(checks.get() >= 3);
    assert!(actual.waveform.is_none());
    assert!(actual.spectrogram.is_none());
    assert!(actual.geometry.is_none());
    assert!(!record.matches_file_on_disk());
}

/// Each independent builder can be omitted without changing the other artifact's decoded timeline.
#[test]
fn single_missing_audio_artifact_builds_only_its_requested_part() {
    let record = analyze_file(
        &test_fixtures::fixture("sine_mono_24000_16.wav"),
        &|| false,
        &mut |_| {},
    );
    let waveform = build_audio_artifacts(&record, true, false, || true).expect("waveform-only pass");
    assert_eq!(waveform.waveform, record.waveform);
    assert!(waveform.spectrogram.is_none());
    let spectrum = build_audio_artifacts(&record, false, true, || true).expect("spectral-only pass");
    assert!(spectrum.waveform.is_none());
    assert!(spectrum.spectrogram.is_some());
}

/// Complete audio caches and unsupported spectral-only requests never open or decode the source.
#[test]
fn absent_audio_work_and_unsupported_spectral_geometry_do_not_open_a_decoder() {
    let file = tempfile::NamedTempFile::new().expect("not an audio source");
    let mut record = super::test_enrichment_parts::record(file.path());
    assert!(
        build_audio_artifacts(&record, false, false, || true)
            .expect("no missing artifacts")
            .geometry
            .is_none()
    );
    record.channels = 3;
    assert!(
        build_audio_artifacts(&record, false, true, || true)
            .expect("unsupported spectrum")
            .geometry
            .is_none()
    );
}

/// Transient metadata failures stay retryable rather than becoming a fabricated absent-tag result.
#[test]
fn display_tag_reader_distinguishes_valid_absence_from_failed_reads() {
    let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
    assert!(read_tags(&path).is_some());
    let directory = tempfile::TempDir::new().expect("source directory");
    assert!(read_tags(&directory.path().join("missing.wav")).is_none());
    assert!(read_tags(&test_fixtures::fixture("broken.wav")).is_none());
}
