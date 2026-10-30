//! Exact comparisons of render-time normalization measurements with a fresh decode of the committed output.
//!
//! Fixtures are copied before normalization,
//! and extended fixture samples exercise multichannel streams longer
//! than the short-term window with a final partial measurement block.

use std::cell::Cell;
use std::fs::File;

use super::{CANCELLED_MESSAGE, NormalizationOptions, normalization_output_path, normalize_file};
use crate::aiff::AiffWriter;
use crate::decode::AudioFileDecoder;
use crate::golden::GoldenText;
use crate::operation::OperationOutcome;
use crate::output_file::delete_disposer;
use crate::record::AnalysisRecord;
use crate::test_helpers::{analyze, half_fixture_copy, ready_fixture_copy};

/// Builds options that delete temporary test sources instead of sending them to the system trash.
fn options() -> NormalizationOptions {
    NormalizationOptions {
        dispose: delete_disposer(),
        ..NormalizationOptions::default()
    }
}

/// Requires exact numeric record fields, history bits, waveform bytes, overload bytes, and committed file identity.
fn assert_exact(actual: &AnalysisRecord, expected: &AnalysisRecord) {
    let mut actual_text = GoldenText::default();
    actual_text.record(actual);
    let mut expected_text = GoldenText::default();
    expected_text.record(expected);
    assert_eq!(actual_text.text(), expected_text.text());
    assert_eq!(actual.path, expected.path);
    assert_eq!(actual.file_name, expected.file_name);
    assert_eq!(actual.file_size, expected.file_size);
    assert_eq!(actual.modified_time_ms, expected.modified_time_ms);
    assert_eq!(actual.from_cache, expected.from_cache);
    assert_eq!(
        actual.custom_gain_db.map(f32::to_bits),
        expected.custom_gain_db.map(f32::to_bits),
    );
    let actual_history = actual.short_term_loudness.as_ref().expect("render history");
    let expected_history = expected.short_term_loudness.as_ref().expect("decoded history");
    assert_eq!(
        actual_history.sample_rate.to_bits(),
        expected_history.sample_rate.to_bits()
    );
    assert_eq!(actual_history.length_in_samples, expected_history.length_in_samples);
    assert_eq!(actual_history.samples_per_point, expected_history.samples_per_point);
    assert_eq!(
        actual_history
            .values
            .iter()
            .map(|value| value.to_bits())
            .collect::<Vec<_>>(),
        expected_history
            .values
            .iter()
            .map(|value| value.to_bits())
            .collect::<Vec<_>>(),
    );
    assert_eq!(
        actual.waveform.as_ref().expect("render waveform").to_bytes(),
        expected.waveform.as_ref().expect("decoded waveform").to_bytes(),
    );
    assert_eq!(
        actual
            .overloads
            .as_ref()
            .expect("render overloads")
            .to_bytes()
            .expect("valid overloads"),
        expected
            .overloads
            .as_ref()
            .expect("decoded overloads")
            .to_bytes()
            .expect("valid overloads"),
    );
}

/// Normalizes a copied fixture and compares every output measurement with independent file analysis.
fn check_fixture(fixture: &str) {
    let directory = tempfile::tempdir().expect("temporary directory");
    let source = ready_fixture_copy(&directory, fixture, fixture);
    let result = normalize_file(&source, &options(), &|| false, &mut |_| {});
    assert!(result.error().is_none(), "{fixture}: {:?}", result.error());
    let output = result.output().expect("normalized output");
    assert_exact(output, &analyze(&output.path));
    assert!(output.matches_file_on_disk());
}

/// Extends a copied fixture into distinct channels with a length beyond three seconds and a partial last block.
fn extended_fixture(directory: &tempfile::TempDir, channels: usize, bits: u32) -> AnalysisRecord {
    let fixture = if bits == 16 {
        "sine_stereo_44100_16.wav"
    } else {
        "sine_stereo_48000_24.wav"
    };
    let copied = test_fixtures::copy_fixture(directory, fixture, None);
    let mut decoder = AudioFileDecoder::open(&copied).expect("fixture decoder");
    let sample_rate = decoder.info().sample_rate;
    let mut samples = Vec::new();
    while let Some(chunk) = decoder.next_chunk().expect("decodes fixture") {
        samples.extend_from_slice(chunk.first().expect("fixture first channel"));
    }
    drop(decoder);
    let frames = sample_rate as usize * 3 + 137;
    let planes: Vec<Vec<f32>> = (0..channels)
        .map(|channel| {
            (0..frames)
                .map(|frame| {
                    let sample = samples[(frame + channel * 113) % samples.len()];
                    let envelope = 0.2 + 0.8 * frame as f32 / frames as f32;
                    sample * envelope * (channel as f32).mul_add(-0.07, 1.0)
                })
                .collect()
        })
        .collect();
    let source = directory.path().join("extended.aif");
    let mut writer = AiffWriter::new(
        File::create(&source).expect("source file"),
        channels as u32,
        sample_rate,
        bits,
    )
    .expect("source writer");
    let references: Vec<_> = planes.iter().map(Vec::as_slice).collect();
    writer.write_planar(&references).expect("writes extended fixture");
    writer.finish().expect("finishes extended fixture");
    let record = analyze(&source);
    assert!(record.is_ready(), "{}", record.error_message);
    record
}

/// Checks exact render-time measurements for one extended fixture layout and output bit depth.
fn check_extended(channels: usize, bits: u32) {
    let directory = tempfile::tempdir().expect("temporary directory");
    let source = extended_fixture(&directory, channels, bits);
    let result = normalize_file(&source, &options(), &|| false, &mut |_| {});
    assert!(result.error().is_none(), "{:?}", result.error());
    let output = result.output().expect("normalized output");
    assert!(output.duration_seconds > 3.0);
    assert_eq!(output.channels, channels as u32);
    assert_eq!(output.bits_per_sample, bits);
    let history = output.short_term_loudness.as_ref().expect("history");
    assert_ne!(output.length_in_samples % u64::from(history.samples_per_point), 0);
    assert_exact(output, &analyze(&output.path));
}

#[test]
fn copied_16_bit_fixture_matches_fresh_analysis() {
    check_fixture("sine_stereo_44100_16.wav");
}

#[test]
fn copied_24_bit_fixture_matches_fresh_analysis() {
    check_fixture("sine_stereo_48000_24.flac");
}

#[test]
fn copied_float_fixture_matches_fresh_analysis() {
    check_fixture("sine_stereo_44100_32f.wav");
}

#[test]
fn copied_lossy_fixture_matches_fresh_analysis() {
    check_fixture("sine_stereo_44100_320k.mp3");
}

#[test]
fn intersample_overload_flags_match_fresh_analysis() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let fixture = test_fixtures::INTERSAMPLE_FIXTURE;
    let source = ready_fixture_copy(&directory, fixture, fixture);
    let result = normalize_file(&source, &options(), &|| false, &mut |_| {});
    assert!(result.error().is_none(), "{:?}", result.error());
    let output = result.output().expect("normalized output");
    assert!(output.overall_true_peak > 1.0);
    assert!(
        output
            .overloads
            .as_ref()
            .expect("overloads")
            .flags
            .iter()
            .any(|&flag| flag != 0)
    );
    assert_exact(output, &analyze(&output.path));
}

#[test]
fn mono_16_bit_partial_block_after_three_seconds() {
    check_extended(1, 16);
}

#[test]
fn mono_24_bit_partial_block_after_three_seconds() {
    check_extended(1, 24);
}

#[test]
fn stereo_16_bit_partial_block_after_three_seconds() {
    check_extended(2, 16);
}

#[test]
fn stereo_24_bit_partial_block_after_three_seconds() {
    check_extended(2, 24);
}

#[test]
fn surround_16_bit_partial_block_after_three_seconds() {
    check_extended(6, 16);
}

#[test]
fn surround_24_bit_partial_block_after_three_seconds() {
    check_extended(6, 24);
}

#[test]
fn truncated_source_notice_survives_render_time_analysis() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let path = half_fixture_copy(&directory, "sine_stereo_44100_16.wav", "truncated.wav");
    let source = analyze(&path);
    assert!(source.is_ready(), "{}", source.error_message);
    let result = normalize_file(&source, &options(), &|| false, &mut |_| {});
    assert!(result.error().is_none(), "{:?}", result.error());
    assert!(matches!(
        &result.outcome,
        OperationOutcome::Succeeded { notice: Some(notice), .. } if notice.contains("ended at sample")
    ));
    let output = result.output().expect("output");
    assert_exact(output, &analyze(&output.path));
}

#[test]
fn at_target_returns_the_current_record_and_completes_progress() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let mut source = extended_fixture(&directory, 1, 16);
    source.custom_gain_db = Some(-3.0);
    let options = NormalizationOptions {
        target_peak_dbfs: crate::format::gain_to_decibels(f64::from(source.overall_peak.abs()), -1000.0),
        ..options()
    };
    let mut progress = Vec::new();
    let result = normalize_file(&source, &options, &|| false, &mut |fraction| progress.push(fraction));
    assert!(result.error().is_none(), "{:?}", result.error());
    assert_exact(result.output().expect("current output"), &source);
    assert_eq!(progress, vec![1.0]);
}

#[test]
fn cancelled_at_target_does_not_return_a_successful_record() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let source = ready_fixture_copy(&directory, "sine_stereo_44100_16.aif", "source.aif");
    let options = NormalizationOptions {
        target_peak_dbfs: crate::format::gain_to_decibels(f64::from(source.overall_peak.abs()), -1000.0),
        ..options()
    };
    let result = normalize_file(&source, &options, &|| true, &mut |_| panic!("cancelled progress"));
    assert_eq!(result.error(), Some(CANCELLED_MESSAGE));
}

#[test]
fn changed_at_target_source_is_not_returned_as_current() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let source = ready_fixture_copy(&directory, "sine_stereo_44100_16.aif", "source.aif");
    let options = NormalizationOptions {
        target_peak_dbfs: crate::format::gain_to_decibels(f64::from(source.overall_peak.abs()), -1000.0),
        ..options()
    };
    let result = normalize_file(
        &source,
        &options,
        &|| {
            std::fs::write(&source.path, b"external replacement").expect("replaces source");
            false
        },
        &mut |_| panic!("changed source progress"),
    );
    assert!(
        result
            .error()
            .is_some_and(|error| error.contains("source file changed"))
    );
    assert_eq!(
        std::fs::read(&source.path).expect("replacement"),
        b"external replacement"
    );
}

#[test]
fn cancelled_render_never_publishes_measured_output() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let source = ready_fixture_copy(&directory, "sine_stereo_44100_16.wav", "source.wav");
    let cancelled = Cell::new(false);
    let result = normalize_file(&source, &options(), &|| cancelled.get(), &mut |_| cancelled.set(true));
    assert_eq!(result.error(), Some(CANCELLED_MESSAGE));
    assert!(source.path.is_file());
    assert!(!normalization_output_path(&source.path).exists());
}
