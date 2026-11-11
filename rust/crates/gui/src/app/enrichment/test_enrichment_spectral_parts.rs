//! Spectral enrichment parts: selected delivery, combined repair, retries, and stale result rejection.

use std::cell::Cell;
use std::time::Duration;

use crate::app::preview::PreviewSource;

use super::test_enrichment_parts::{cache_artifacts, cache_parts, record, request, spectrogram, waveform};
use super::*;

/// A failed strong-identity capture is terminal only for its selected-preview attempt.
#[test]
fn missing_receipt_records_an_attempt_without_automatic_requeue() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let mut request = request(&record, true);
    request.source_identity = None;
    let mut queue = EnrichmentQueue::default();
    queue.enqueue(request.identity.clone(), true);
    queue.active = Some(request.clone());
    queue.complete(&request);
    let attempt = queue
        .selected_audio_attempt
        .as_ref()
        .expect("terminal selected attempt");
    assert_eq!(attempt.identity, request.identity);
    assert_eq!(attempt.preview, request.preview);
    assert!(queue.pending.is_empty());
    queue.deselect();
    assert!(queue.selected_audio_attempt.is_none());
}

/// A fully cached repair, selected or not, must not decode PCM or reread terminal background metadata.
#[test]
fn full_audio_cache_never_invokes_the_combined_reader() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let cache = AnalysisCache::in_memory();
    cache_artifacts(&cache, &record, &SeratoOverviewState::Absent);
    for selected in [false, true] {
        let request = request(&record, selected);
        let (sender, receiver) = unbounded();
        enrich_with_readers(
            &request,
            &record,
            Some(&cache),
            selected.then_some(None),
            &sender,
            |_| panic!("remembered or terminal display-tag outcome must not reread"),
            |_, _| panic!("valid waveform and spectrum must not decode"),
        );
        assert!(matches!(
            receiver.try_recv(),
            Ok(EnrichmentPart::Overview(SeratoOverviewState::Absent))
        ));
        if selected {
            assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(None))));
            assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Waveform(_))));
            assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(Some(_)))));
        }
        assert!(receiver.is_empty());
    }
}

/// A cache hit from different measured geometry remains a spectral miss without starting foreground repair.
#[test]
fn mismatched_cached_spectral_geometry_is_not_published() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let mut record = record(file.path());
    let cache = AnalysisCache::in_memory();
    cache_artifacts(&cache, &record, &SeratoOverviewState::Absent);
    record.length_in_samples += 1;
    let mut request = request(&record, true);
    request.mode = EnrichmentMode::CacheOnly;
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        Some(&cache),
        None,
        &sender,
        |_| panic!("foreground tags must not parse"),
        |_, _| panic!("foreground spectral misses must not decode"),
    );
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Overview(_))));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Waveform(_))));
    assert!(receiver.is_empty());
}

/// Both absent artifacts share one reader invocation, while each partial hit builds only its missing part.
#[test]
fn audio_misses_are_repaired_together_and_partial_hits_are_preserved() {
    for (has_waveform, has_spectrogram) in [(false, false), (true, false), (false, true)] {
        let file = tempfile::NamedTempFile::new().expect("source");
        let record = record(file.path());
        let cache = AnalysisCache::in_memory();
        cache_parts(
            &cache,
            &record,
            &SeratoOverviewState::Absent,
            has_waveform,
            has_spectrogram,
        );
        let request = request(&record, true);
        let calls = Cell::new(0);
        let (sender, receiver) = unbounded();
        enrich_with_readers(
            &request,
            &record,
            Some(&cache),
            Some(None),
            &sender,
            |_| panic!("known metadata"),
            |waveform_missing, spectrogram_missing| {
                calls.set(calls.get() + 1);
                assert_eq!(waveform_missing, !has_waveform);
                assert_eq!(spectrogram_missing, !has_spectrogram);
                assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Overview(_))));
                assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(None))));
                if has_waveform {
                    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Waveform(_))));
                }
                if has_spectrogram {
                    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(Some(_)))));
                }
                assert!(receiver.is_empty());
                AudioArtifacts {
                    waveform: waveform_missing.then(waveform),
                    spectrogram: spectrogram_missing.then(spectrogram),
                    geometry: None,
                }
            },
        );
        assert_eq!(calls.get(), 1);
        assert!(cache.get_waveform(&record.path).is_some());
        assert!(cache.get_spectrogram(&record.path).is_some());
        if !has_waveform {
            assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Waveform(_))));
        }
        if !has_spectrogram {
            assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(Some(_)))));
        }
        assert!(receiver.is_empty());
    }
}

/// Cached independent parts are observable while the missing-part reader is genuinely blocked.
#[test]
fn cached_audio_part_is_delivered_before_blocking_missing_artifact_repair() {
    for has_spectrogram in [false, true] {
        let file = tempfile::NamedTempFile::new().expect("source");
        let record = record(file.path());
        let cache = AnalysisCache::in_memory();
        cache_parts(
            &cache,
            &record,
            &SeratoOverviewState::Absent,
            !has_spectrogram,
            has_spectrogram,
        );
        let request = request(&record, true);
        let (sender, receiver) = unbounded();
        let (release, blocked) = unbounded();
        std::thread::scope(|scope| {
            scope.spawn(|| {
                enrich_with_readers(
                    &request,
                    &record,
                    Some(&cache),
                    Some(None),
                    &sender,
                    |_| panic!("known tags"),
                    |_, _| {
                        blocked
                            .recv_timeout(Duration::from_secs(2))
                            .expect("release blocked repair");
                        AudioArtifacts::default()
                    },
                );
            });
            assert!(matches!(
                receiver.recv_timeout(Duration::from_secs(1)),
                Ok(EnrichmentPart::Overview(_))
            ));
            assert!(matches!(
                receiver.recv_timeout(Duration::from_secs(1)),
                Ok(EnrichmentPart::Tags(None))
            ));
            if has_spectrogram {
                assert!(matches!(
                    receiver.recv_timeout(Duration::from_secs(1)),
                    Ok(EnrichmentPart::Spectrogram(Some(_)))
                ));
            } else {
                assert!(matches!(
                    receiver.recv_timeout(Duration::from_secs(1)),
                    Ok(EnrichmentPart::Waveform(_))
                ));
            }
            release.send(()).expect("release repair");
        });
        if !has_spectrogram {
            assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(None))));
        }
        assert!(receiver.is_empty());
    }
}

/// A failed read ends one generation without spinning, but a genuine A to B to A selection may recover.
#[test]
fn transient_audio_failure_retries_only_after_a_new_selected_preview_generation() {
    let first = tempfile::NamedTempFile::new().expect("first source");
    let second = tempfile::NamedTempFile::new().expect("second source");
    let first_record = record(first.path());
    let first_identity = EnrichmentIdentity::from_record(&first_record);
    let second_identity = EnrichmentIdentity::from_record(&record(second.path()));
    let mut source = PreviewSource::default();
    source.begin(first.path());
    let mut queue = EnrichmentQueue::default();
    queue.gate_open = true;
    queue.select(first_identity.clone(), source.token().as_ref());
    let failed = queue
        .take_next(Some(first.path()), source.token(), true)
        .expect("initial selected repair");
    let (sender, receiver) = unbounded();
    let calls = Cell::new(0);
    enrich_with_readers(
        &failed,
        &first_record,
        None,
        Some(None),
        &sender,
        |_| panic!("known tags"),
        |_, _| {
            calls.set(calls.get() + 1);
            AudioArtifacts::default()
        },
    );
    queue.remember_tags(first_identity.clone(), None);
    queue.complete(&failed);
    assert!(queue.pending.is_empty());
    assert_eq!(queue.selected_attempt.as_ref(), Some(&first_identity));
    queue.select(first_identity.clone(), source.token().as_ref());
    let repeated = queue
        .take_next(Some(first.path()), source.token(), true)
        .expect("same-generation lookup");
    assert!(repeated.artifacts_attempted);
    enrich_with_readers(
        &repeated,
        &first_record,
        None,
        Some(None),
        &sender,
        |_| panic!("known tags"),
        |_, _| panic!("completion polling must not repeat a transient decoder failure"),
    );
    queue.complete(&repeated);
    queue.deselect();
    source.begin(second.path());
    queue.select(second_identity, source.token().as_ref());
    queue.deselect();
    source.begin(first.path());
    queue.select(first_identity, source.token().as_ref());
    let recovered = queue
        .take_next(Some(first.path()), source.token(), true)
        .expect("new selection retry");
    assert!(!recovered.artifacts_attempted);
    enrich_with_readers(
        &recovered,
        &first_record,
        None,
        Some(None),
        &sender,
        |_| panic!("known tags"),
        |_, _| {
            calls.set(calls.get() + 1);
            AudioArtifacts {
                waveform: Some(waveform()),
                spectrogram: Some(spectrogram()),
                geometry: None,
            }
        },
    );
    assert_eq!(calls.get(), 2);
    assert!(
        receiver
            .try_iter()
            .any(|part| matches!(part, EnrichmentPart::Spectrogram(Some(_))))
    );
    assert!(!queue.accepts_preview(&failed, &source));
    assert!(queue.accepts_preview(&recovered, &source));
}

/// Failed waveform work cannot suppress a valid independent spectrum or fabricate an overview dependency.
#[test]
fn spectral_completion_is_independent_of_waveform_and_tag_availability() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let request = request(&record, true);
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        None,
        None,
        &sender,
        |_| None,
        |_, _| AudioArtifacts {
            spectrogram: Some(spectrogram()),
            ..AudioArtifacts::default()
        },
    );
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(None))));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(Some(_)))));
    assert!(receiver.is_empty());
}

/// Unavailable outcomes are terminal within one selected-preview attempt, without immediate automatic retries.
#[test]
fn completed_missing_artifacts_are_terminal_for_the_current_preview_attempt() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let mut request = request(&record, true);
    request.artifacts_attempted = true;
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        None,
        Some(None),
        &sender,
        |_| panic!("known metadata"),
        |_, _| panic!("unavailable audio artifacts must not spin"),
    );
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(None))));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(None))));
    assert!(receiver.is_empty());
}

/// Unselected spectra are persisted and released, not retained in the queue or published to a row.
#[test]
fn unselected_spectrum_is_cached_then_released() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let cache = AnalysisCache::in_memory();
    cache_parts(&cache, &record, &SeratoOverviewState::Absent, true, false);
    let request = request(&record, false);
    let data = spectrogram();
    let weak = Arc::downgrade(&data);
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        Some(&cache),
        None,
        &sender,
        |_| panic!("terminal background tags"),
        |_, _| AudioArtifacts {
            spectrogram: Some(data),
            ..AudioArtifacts::default()
        },
    );
    assert!(weak.upgrade().is_none());
    assert!(cache.get_spectrogram(&record.path).is_some());
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Overview(_))));
    assert!(receiver.is_empty());
}

/// A request cannot borrow another row's valid on-disk identity to authorize source readers.
#[test]
fn mismatched_record_is_rejected_before_independent_readers() {
    let first = tempfile::NamedTempFile::new().expect("first source");
    let second = tempfile::NamedTempFile::new().expect("second source");
    let request = request(&record(first.path()), true);
    let record = record(second.path());
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        None,
        None,
        &sender,
        |_| panic!("wrong row must not read metadata"),
        |_, _| panic!("wrong row must not decode"),
    );
    assert!(receiver.is_empty());
}

/// Equal coarse stats cannot authorize artifacts from the filesystem object replaced during decoding.
#[test]
fn equal_size_and_mtime_replacement_during_decode_rejects_both_artifacts() {
    let directory = tempfile::TempDir::new().expect("source directory");
    let path = directory.path().join("source.wav");
    let replacement = directory.path().join("replacement.wav");
    std::fs::write(&path, b"original").expect("original source");
    let modified = std::fs::metadata(&path)
        .expect("source metadata")
        .modified()
        .expect("source time");
    let record = record(&path);
    let cache = AnalysisCache::in_memory();
    cache_parts(&cache, &record, &SeratoOverviewState::Absent, false, false);
    let request = request(&record, true);
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        Some(&cache),
        Some(None),
        &sender,
        |_| panic!("known tags"),
        |_, _| {
            std::fs::write(&replacement, b"replaced").expect("replacement source");
            std::fs::File::options()
                .write(true)
                .open(&replacement)
                .expect("replacement handle")
                .set_times(std::fs::FileTimes::new().set_modified(modified))
                .expect("same modification time");
            std::fs::rename(&replacement, &path).expect("atomic replacement");
            assert_eq!(
                file_identity(&path),
                Some(audiobatch_core::record::FileIdentity {
                    file_size: record.file_size,
                    modified_time_ms: record.modified_time_ms
                })
            );
            AudioArtifacts {
                waveform: Some(waveform()),
                spectrogram: Some(spectrogram()),
                geometry: None,
            }
        },
    );
    assert!(!request.matches_source());
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Overview(_))));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(None))));
    assert!(receiver.is_empty());
    assert!(cache.get_waveform(&path).is_none());
    assert!(cache.get_spectrogram(&path).is_none());
}

/// Replacement during the shared decode rejects both artifacts and leaves optional caches empty.
#[test]
fn replacement_inside_combined_reader_discards_both_parts() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let cache = AnalysisCache::in_memory();
    cache_parts(&cache, &record, &SeratoOverviewState::Absent, false, false);
    let request = request(&record, true);
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        Some(&cache),
        Some(None),
        &sender,
        |_| panic!("known metadata"),
        |_, _| {
            std::fs::write(file.path(), b"replacement source").expect("replace source");
            AudioArtifacts {
                waveform: Some(waveform()),
                spectrogram: Some(spectrogram()),
                geometry: None,
            }
        },
    );
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Overview(_))));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(None))));
    assert!(receiver.is_empty());
    assert!(cache.get_waveform(&record.path).is_none());
    assert!(cache.get_spectrogram(&record.path).is_none());
}

/// Cancelling a blocking combined reader discards both results even if that reader eventually returns data.
#[test]
fn cancellation_inside_combined_reader_discards_both_parts() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let request = request(&record, true);
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        None,
        Some(None),
        &sender,
        |_| panic!("known metadata"),
        |_, _| {
            request.cancelled.store(true, Ordering::Release);
            AudioArtifacts {
                waveform: Some(waveform()),
                spectrogram: Some(spectrogram()),
                geometry: None,
            }
        },
    );
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(None))));
    assert!(receiver.is_empty());
}
