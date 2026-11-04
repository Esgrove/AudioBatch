//! Same-pass analysis artifacts and bounded provisional waveform contracts on the public core API.

use std::cell::Cell;
use std::fs::{FileTimes, read};
use std::io::Write;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use audiobatch_core::analysis::analyze_file_with_observer;
use audiobatch_core::discovery::normalize_path;
use audiobatch_core::meter::BlockMeter;
use audiobatch_core::normalize::{NormalizationOptions, normalize_file};
use audiobatch_core::operation::update_cache;
use audiobatch_core::record::{AnalysisError, file_identity};
use audiobatch_core::{
    AnalysisCache, AnalysisCoordinator, AnalysisEvent, AnalysisOptions, OperationOutcome, analyze_file,
};

/// Atomically replaces source bytes while preserving the size and exact modification time used by weak cache keys.
fn replace_with_matching_stats(path: &Path) {
    let before = file_identity(path).expect("source identity");
    let modified = std::fs::metadata(path)
        .expect("source metadata")
        .modified()
        .expect("source mtime");
    let mut bytes = read(path).expect("source bytes");
    let last = bytes.last_mut().expect("nonempty fixture");
    *last ^= 1;
    let mut replacement =
        tempfile::NamedTempFile::new_in(path.parent().expect("source parent")).expect("replacement temporary file");
    replacement.write_all(&bytes).expect("replacement bytes");
    replacement
        .as_file()
        .set_times(FileTimes::new().set_modified(modified))
        .expect("preserves source mtime");
    replacement.persist(path).expect("atomic source replacement");
    assert_eq!(
        file_identity(path),
        Some(before),
        "weak source identity deliberately unchanged"
    );
}

/// Provisional snapshots include buffered partial blocks without consuming or changing final measurements.
#[test]
fn snapshots_include_partial_blocks_and_leave_final_measurements_unchanged() {
    let frames = 10_003;
    let samples: Vec<f32> = (0..frames)
        .map(|frame| if frame % 127 < 63 { 0.25 } else { -0.5 })
        .collect();
    let mut meter = BlockMeter::new(1, 44_100, Some(frames as u64)).expect("meter");
    let mut reference = BlockMeter::new(1, 44_100, Some(frames as u64)).expect("reference meter");
    meter.push(&[&samples[..7]]);
    let prefix = meter.waveform_snapshot(7);
    assert_eq!(prefix.length_in_frames(), 7);
    assert_eq!(prefix.sample_rate(), 44_100);
    assert!(prefix.column_peaks(0, 0.0, 7.0, 1)[0].1 >= 0.25);
    assert_eq!(prefix.column_peaks(0, 7.0, 100.0, 1), [(0.0, 0.0)]);
    meter.push(&[&samples[7..]]);
    let snapshot = meter.waveform_snapshot(frames as u64);
    reference.push(std::slice::from_ref(&samples));
    let output = meter.finish(frames as u64);
    let expected = reference.finish(frames as u64);
    assert_eq!(snapshot, output.waveform);
    assert_eq!(output.waveform, expected.waveform);
    assert_eq!(output.true_peaks, expected.true_peaks);
    assert_eq!(output.history, expected.history);
    assert_eq!(output.integrated_lufs.to_bits(), expected.integrated_lufs.to_bits());
    assert_eq!(output.spectrogram, expected.spectrogram);
}

/// Every observed decoded prefix has correct frame geometry, and a final result carries both artifacts.
#[test]
fn observer_sees_same_pass_prefixes_and_final_artifacts() {
    let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
    let mut positions = Vec::new();
    let record = analyze_file_with_observer(&path, &|| false, &mut |_| {}, &mut |meter, frames, info| {
        let waveform = meter.waveform_snapshot(frames);
        assert_eq!(waveform.length_in_frames(), frames);
        assert_eq!(waveform.sample_rate(), info.sample_rate);
        assert_eq!(waveform.channels(), info.channels);
        positions.push(frames);
    });
    assert!(record.is_ready(), "{}", record.error_message);
    assert_ne!(positions, [] as [u64; 0]);
    assert!(positions.windows(2).all(|pair| pair[0] <= pair[1]));
    assert_eq!(positions.last(), Some(&record.length_in_samples));
    let spectrogram = record.spectrogram.as_ref().expect("same-pass spectrum");
    assert!(spectrogram.matches_stream(record.sample_rate, record.length_in_samples, record.channels));
    assert!(record.waveform.is_some());
}

/// Cancelling from a decoded observer never grants a provisional prefix final-record authority.
#[test]
fn cancelling_observed_decoding_discards_final_artifacts() {
    let cancelled = Cell::new(false);
    let mut observed = false;
    let record = analyze_file_with_observer(
        &test_fixtures::fixture("sine_stereo_44100_16.wav"),
        &|| cancelled.get(),
        &mut |_| {},
        &mut |meter, frames, _| {
            assert!(meter.waveform_snapshot(frames).length_in_frames() > 0);
            observed = true;
            cancelled.set(true);
        },
    );
    assert!(observed);
    assert_eq!(record.error, Some(AnalysisError::Cancelled));
    assert!(record.waveform.is_none());
    assert!(record.spectrogram.is_none());
    assert!(record.overloads.is_none());
}

/// Replacing a source during decoding prevents attaching old PCM artifacts to the replacement's identity.
#[test]
fn replacing_source_during_decode_rejects_final_artifacts() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let fixture = test_fixtures::fixture("sine_stereo_44100_16.wav");
    let path = directory.path().join("source.wav");
    std::fs::copy(&fixture, &path).expect("source copy");
    let mut replaced = false;
    let record = analyze_file_with_observer(&path, &|| false, &mut |_| {}, &mut |_, _, _| {
        if !replaced {
            std::fs::rename(&path, directory.path().join("old.wav")).expect("moves decoded source");
            std::fs::copy(&fixture, &path).expect("replacement source");
            replaced = true;
        }
    });
    assert!(replaced);
    assert_eq!(record.error, Some(AnalysisError::Unreadable));
    assert!(record.waveform.is_none());
    assert!(record.spectrogram.is_none());
}

/// Completion callbacks cannot replace a source with equal cache stats and inherit its decoded measurements.
#[test]
fn analysis_completion_callback_rejects_equal_stat_source_replacement() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let path = directory.path().join("source.wav");
    std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.wav"), &path).expect("source copy");
    let mut replaced = false;
    let record = analyze_file(&path, &|| false, &mut |fraction| {
        if fraction >= 1.0 && !replaced {
            replace_with_matching_stats(&path);
            replaced = true;
        }
    });
    assert!(replaced);
    assert_eq!(record.error, Some(AnalysisError::Unreadable));
    assert!(record.source_identity.is_some());
    assert!(!record.matches_file_on_disk());
    assert!(record.waveform.is_none());
    assert!(record.spectrogram.is_none());
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    cache.store_analysis(&record).expect("stale failure is not persisted");
    assert!(cache.get_analysis(&path).is_none());
}

/// A native result retains the pre-decode receipt until delayed measurement and artifact publication.
#[test]
fn delayed_analysis_publication_rejects_equal_stat_source_replacement() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let path = directory.path().join("source.wav");
    std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.wav"), &path).expect("source copy");
    let record = analyze_file(&path, &|| false, &mut |_| {});
    assert!(record.is_ready());
    assert!(record.source_identity.is_some());
    assert!(record.matches_file_on_disk());
    replace_with_matching_stats(&path);
    assert!(!record.matches_file_on_disk());
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    cache.store_analysis(&record).expect("stale measurements ignored");
    assert!(cache.get_analysis(&path).is_none());
    assert!(
        !cache
            .store_waveform(&record, record.waveform.as_deref().expect("waveform"))
            .expect("stale waveform rejected")
    );
    assert!(
        !cache
            .store_spectrogram(&record, record.spectrogram.as_deref().expect("spectrum"))
            .expect("stale spectrum rejected")
    );
    assert!(cache.get_waveform(&path).is_none());
    assert!(cache.get_spectrogram(&path).is_none());
}

/// A final normalization callback cannot transfer the committed render's measurements to an equal-stat replacement.
#[test]
fn normalized_completion_callback_rejects_equal_stat_output_replacement() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let path = directory.path().join("source.wav");
    std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.wav"), &path).expect("source copy");
    let source = analyze_file(&path, &|| false, &mut |_| {});
    let output_path = path.with_extension("aif");
    let options = NormalizationOptions {
        target_peak_dbfs: -3.0,
        dispose: Arc::new(|path| std::fs::remove_file(path).is_ok()),
        ..NormalizationOptions::default()
    };
    let mut replaced = false;
    let result = normalize_file(&source, &options, &|| false, &mut |fraction| {
        if fraction >= 1.0 && !replaced {
            replace_with_matching_stats(&output_path);
            replaced = true;
        }
    });
    assert!(replaced);
    assert!(matches!(
        result.outcome,
        OperationOutcome::CommittedButUnanalyzed { .. }
    ));
    let output = result.output().expect("committed replacement remains visible");
    assert_eq!(output.error, Some(AnalysisError::Unreadable));
    assert!(output.waveform.is_none());
    assert!(output.spectrogram.is_none());
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    update_cache(&cache, &source, &result);
    assert!(cache.get_analysis(&output_path).is_none());
    assert!(cache.get_waveform(&output_path).is_none());
    assert!(cache.get_spectrogram(&output_path).is_none());
}

/// Render receipts survive successful normalization until background persistence rechecks the output identity.
#[test]
fn delayed_normalized_publication_rejects_equal_stat_output_replacement() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let path = directory.path().join("source.wav");
    std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.wav"), &path).expect("source copy");
    let source = analyze_file(&path, &|| false, &mut |_| {});
    let options = NormalizationOptions {
        target_peak_dbfs: -3.0,
        dispose: Arc::new(|path| std::fs::remove_file(path).is_ok()),
        ..NormalizationOptions::default()
    };
    let result = normalize_file(&source, &options, &|| false, &mut |_| {});
    assert!(result.error().is_none(), "{:?}", result.error());
    let output = result.output().expect("committed analysis");
    assert!(output.source_identity.is_some());
    assert!(output.matches_file_on_disk());
    replace_with_matching_stats(&output.path);
    assert!(!output.matches_file_on_disk());
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    update_cache(&cache, &source, &result);
    assert!(cache.get_analysis(&output.path).is_none());
    assert!(cache.get_waveform(&output.path).is_none());
    assert!(cache.get_spectrogram(&output.path).is_none());
}

/// Final artifacts are ready in the cache before the corresponding table event, which retains neither payload.
#[test]
fn finished_event_follows_artifact_persistence_without_retaining_drawing_payloads() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let cache = Arc::new(AnalysisCache::with_path(directory.path().join("analysis.db")));
    let coordinator = AnalysisCoordinator::new(Arc::clone(&cache), 1).expect("coordinator");
    let path = test_fixtures::fixture("sine_stereo_44100_16.wav");
    let normalized_path = normalize_path(&path);
    coordinator.select_preview(Some(&path), 7);
    let (_, receiver) = coordinator.start(AnalysisOptions {
        input_paths: vec![path],
        refresh: true,
        ..AnalysisOptions::default()
    });
    let mut finished = false;
    loop {
        match receiver
            .recv_timeout(Duration::from_secs(10))
            .expect("bounded analysis run")
        {
            AnalysisEvent::Finished { record, .. } => {
                assert!(record.is_ready(), "{}", record.error_message);
                assert!(record.waveform.is_none());
                assert!(record.spectrogram.is_none());
                assert_eq!(
                    record.path, normalized_path,
                    "discovery normalizes cache and preview identity"
                );
                let waveform = cache
                    .get_waveform(&record.path)
                    .expect("waveform persisted under the finished record's normalized path before event");
                let spectrum = cache
                    .get_spectrogram(&record.path)
                    .expect("spectrum persisted under the finished record's normalized path before event");
                assert_eq!(waveform.length_in_frames(), record.length_in_samples);
                assert!(spectrum.matches_stream(record.sample_rate, record.length_in_samples, record.channels));
                assert!(coordinator.take_preview().is_none());
                finished = true;
            }
            AnalysisEvent::Completed { .. } => break,
            _ => {}
        }
    }
    assert!(finished);
    let cached = cache
        .get_analysis(&normalized_path)
        .expect("normalized measurement identity hits");
    assert!(cached.waveform.is_none());
    assert!(cached.spectrogram.is_none());
}

/// Normalization spectra describe the writer's quantized PCM and survive independent cache publication.
#[test]
fn normalized_spectrum_matches_committed_pcm_and_can_be_dropped_after_persistence() {
    let directory = tempfile::tempdir().expect("temporary directory");
    let path = directory.path().join("source.wav");
    std::fs::copy(test_fixtures::fixture("sine_stereo_44100_16.wav"), &path).expect("source copy");
    let source = analyze_file(&path, &|| false, &mut |_| {});
    let options = NormalizationOptions {
        target_peak_dbfs: -3.0,
        dispose: Arc::new(|path| std::fs::remove_file(path).is_ok()),
        ..NormalizationOptions::default()
    };
    let mut result = normalize_file(&source, &options, &|| false, &mut |_| {});
    assert!(result.error().is_none(), "{:?}", result.error());
    let output = result.output().expect("committed analysis");
    let expected = analyze_file(&output.path, &|| false, &mut |_| {});
    assert!(expected.is_ready(), "{}", expected.error_message);
    assert!(output.spectrogram.is_some());
    assert_eq!(output.spectrogram, expected.spectrogram);
    assert_eq!(output.waveform, expected.waveform);
    let output_path = output.path.clone();
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    update_cache(&cache, &source, &result);
    assert_eq!(cache.get_spectrogram(&output_path), expected.spectrogram);
    assert_eq!(cache.get_waveform(&output_path), expected.waveform);
    result.clear_preview_artifacts();
    let output = result.output().expect("measurement result retained");
    assert!(output.waveform.is_none());
    assert!(output.spectrogram.is_none());
    assert!(output.overloads.is_some());
    assert!(output.short_term_loudness.is_some());
}

/// Unsupported multichannel spectral geometry does not prevent existing levels and waveform analysis.
#[test]
fn multichannel_measurement_omits_only_optional_spectrum() {
    let mut meter = BlockMeter::new(3, 48_000, Some(17)).expect("supported meter geometry");
    meter.push(&[vec![0.25; 17], vec![-0.5; 17], vec![0.75; 17]]);
    let output = meter.finish(17);
    assert!(output.spectrogram.is_none());
    assert_eq!(output.waveform.channels(), 3);
    assert_eq!(output.signed_peaks, [0.25, -0.5, 0.75]);
    let record = analyze_file(
        &test_fixtures::fixture("sine_mono_48000_24.aif"),
        &|| false,
        &mut |_| {},
    );
    assert!(record.spectrogram.is_some());
}
