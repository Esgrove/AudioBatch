//! Enrichment part adoption, artifact identity checks, and shared test builders for the enrichment tests.

use std::cell::RefCell;

use audiobatch_core::overload::OverloadHistory;
use audiobatch_core::serato_tags::Overview;
use audiobatch_core::{AnalysisStatus, ShortTermLoudnessHistory};

use super::*;

/// Binds independent artifact tests to a real source without decoding it or opening a window.
pub(super) fn record(path: &Path) -> AnalysisRecord {
    let audiobatch_core::record::FileIdentity {
        file_size,
        modified_time_ms,
    } = file_identity(path).expect("source identity");
    AnalysisRecord {
        path: path.to_path_buf(),
        file_size,
        modified_time_ms,
        sample_rate: 48_000,
        channels: 1,
        length_in_samples: 64,
        duration_seconds: 64.0 / 48_000.0,
        status: AnalysisStatus::Cached,
        ..AnalysisRecord::default()
    }
}

/// Creates one bounded native artifact without invoking any measurement code.
pub(super) fn waveform() -> Arc<WaveformOverview> {
    let mut builder = WaveformBuilder::new(1, Some(64));
    builder.push(&[&[0.25; 64]]);
    builder.finish(48_000, 64)
}

/// Creates one valid spectral artifact without decoding audio or calculating measurements.
pub(super) fn spectrogram() -> Arc<SpectrogramData> {
    let mut builder = SpectrogramBuilder::new(1, 48_000);
    builder.push(&[&[0.25; 64]]);
    builder.finish().expect("supported nonempty spectrum")
}

/// Captures a selected or background request with no GUI dependencies.
pub(super) fn request(record: &AnalysisRecord, selected: bool) -> EnrichmentRequest {
    EnrichmentRequest {
        identity: EnrichmentIdentity::from_record(record),
        source_identity: SourceIdentity::capture(&record.path).ok(),
        cancelled: Arc::new(AtomicBool::new(false)),
        selected: Arc::new(AtomicBool::new(selected)),
        preview: None,
        generation: 0,
        mode: EnrichmentMode::Repair,
        artifacts_attempted: false,
    }
}

/// Keeps a cache-only miss free of source I/O so foreground work cannot start optional repair readers.
#[test]
fn cache_only_miss_never_invokes_artifact_readers() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let mut request = request(&record, true);
    request.mode = EnrichmentMode::CacheOnly;
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        None,
        None,
        &sender,
        |_| panic!("cache-only metadata must not parse the source"),
        |_, _| panic!("cache-only waveform must not decode the source"),
    );
    assert!(receiver.is_empty());
}

/// Stores synthetic, valid measurements and native previews without reading source audio or tags.
pub(super) fn cache_artifacts(cache: &AnalysisCache, record: &AnalysisRecord, overview: &SeratoOverviewState) {
    cache_parts(cache, record, overview, true, true);
}

/// Persists independently optional audio artifacts beside complete synthetic measurement histories.
pub(super) fn cache_parts(
    cache: &AnalysisCache,
    record: &AnalysisRecord,
    overview: &SeratoOverviewState,
    include_waveform: bool,
    include_spectrogram: bool,
) {
    let mut measured = record.clone();
    measured.short_term_loudness = Some(Arc::new(ShortTermLoudnessHistory {
        values: vec![-12.0],
        sample_rate: f64::from(record.sample_rate),
        length_in_samples: record.length_in_samples,
        samples_per_point: record.sample_rate / 10,
    }));
    measured.overloads = Some(Arc::new(OverloadHistory {
        sample_rate: record.sample_rate,
        length_in_samples: record.length_in_samples,
        samples_per_bucket: u64::from(record.sample_rate / 10),
        channels: record.channels,
        flags: vec![0],
    }));
    measured.waveform = include_waveform.then(waveform);
    cache
        .store_analysis(&measured)
        .expect("synthetic measurements and waveform");
    cache
        .store_serato_overview_for_record(record, overview)
        .expect("cached overview");
    if include_spectrogram {
        assert!(
            cache
                .store_spectrogram(record, &spectrogram())
                .expect("cached spectrum")
        );
    }
}

/// Makes persisted overview and waveform artifacts available during foreground work without parsing or decoding.
#[test]
fn cache_only_load_delivers_existing_parts_without_source_readers() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let cache = AnalysisCache::in_memory();
    let overview = SeratoOverviewState::Present(Arc::new(Overview::from_slices(vec![[1; 16]])));
    cache_artifacts(&cache, &record, &overview);
    let mut request = request(&record, true);
    request.mode = EnrichmentMode::CacheOnly;
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        Some(&cache),
        None,
        &sender,
        |_| panic!("cache-only work must not read display tags"),
        |_, _| panic!("cache-only work must not decode"),
    );
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Overview(actual)) if actual == overview));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Waveform(actual)) if actual == waveform()));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(Some(actual))) if actual == spectrogram()));
    assert!(receiver.is_empty());
}

/// Leaves missing artifacts uncached so deferred repair can distinguish an unread source from confirmed absence.
#[test]
fn cache_only_misses_neither_invoke_readers_nor_manufacture_negative_tags() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let cache = AnalysisCache::in_memory();
    let mut request = request(&record, true);
    request.mode = EnrichmentMode::CacheOnly;
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        Some(&cache),
        None,
        &sender,
        |_| panic!("missing tags wait for repair"),
        |_, _| panic!("missing waveform waits for repair"),
    );
    assert!(receiver.is_empty());
    assert!(cache.get_serato_overview(&record.path).is_none());
    assert!(cache.get_waveform(&record.path).is_none());
}

/// Reuses in-memory selected artifacts without source I/O so foreground selection can retain an available preview.
#[test]
fn cache_only_load_reuses_remembered_tags_and_an_attached_waveform() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let mut record = record(file.path());
    record.waveform = Some(waveform());
    let mut request = request(&record, true);
    request.mode = EnrichmentMode::CacheOnly;
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        None,
        Some(Some(Arc::new(FileTags::default()))),
        &sender,
        |_| panic!("remembered tags must not reread"),
        |_, _| panic!("attached waveform must not decode"),
    );
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(Some(_)))));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Waveform(_))));
    assert!(receiver.is_empty());
}

/// Publishes metadata before waveform construction so cues and inspector fields remain usable if decoding fails.
#[test]
fn metadata_is_delivered_before_failed_waveform_work() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let request = request(&record, true);
    let (sender, receiver) = unbounded();
    let order = RefCell::new(Vec::new());
    enrich_with_readers(
        &request,
        &record,
        None,
        None,
        &sender,
        |_| {
            order.borrow_mut().push("metadata");
            Some(Arc::new(FileTags::default()))
        },
        |waveform_missing, spectrogram_missing| {
            assert!(waveform_missing && spectrogram_missing);
            order.borrow_mut().push("waveform");
            assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Overview(_))));
            assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(Some(_)))));
            AudioArtifacts::default()
        },
    );
    assert_eq!(*order.borrow(), ["metadata", "waveform"]);
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(None))));
    assert!(receiver.is_empty());
}

/// Keeps waveform repair independent of tag failure and avoids persisting a transient read error as tag absence.
#[test]
fn transient_metadata_failure_does_not_block_waveform_or_cache_false_absence() {
    let directory = tempfile::TempDir::new().expect("cache directory");
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    let request = request(&record, true);
    let (sender, receiver) = unbounded();
    enrich_with_readers(
        &request,
        &record,
        Some(&cache),
        None,
        &sender,
        |_| None,
        |_, _| AudioArtifacts {
            waveform: Some(waveform()),
            ..AudioArtifacts::default()
        },
    );
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(None))));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Waveform(_))));
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(None))));
    assert!(cache.get_serato_overview(&record.path).is_none());
}

/// Releases background waveform ownership after construction so unselected results cannot accumulate in memory.
#[test]
fn unselected_generated_waveform_is_released_not_delivered_or_retained() {
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let request = request(&record, false);
    let (sender, receiver) = unbounded();
    let waveform = waveform();
    let weak = Arc::downgrade(&waveform);
    enrich_with_readers(
        &request,
        &record,
        None,
        Some(Some(Arc::new(FileTags::default()))),
        &sender,
        |_| panic!("remembered tags must not reread"),
        |_, _| AudioArtifacts {
            waveform: Some(waveform),
            ..AudioArtifacts::default()
        },
    );
    assert!(weak.upgrade().is_none());
    assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Tags(Some(_)))));
    assert!(receiver.is_empty());
}

/// Treats persisted negative Serato outcomes as terminal so background repair does not repeatedly parse them.
#[test]
fn stable_negative_serato_outcomes_do_not_invoke_the_background_reader() {
    let directory = tempfile::TempDir::new().expect("cache directory");
    let file = tempfile::NamedTempFile::new().expect("source");
    let record = record(file.path());
    let cache = AnalysisCache::with_path(directory.path().join("analysis.db"));
    let request = request(&record, false);
    for state in [
        SeratoOverviewState::Absent,
        SeratoOverviewState::Unsupported,
        SeratoOverviewState::Invalid,
    ] {
        cache
            .store_serato_overview_for_record(&record, &state)
            .expect("terminal metadata");
        let (sender, receiver) = unbounded();
        enrich_with_readers(
            &request,
            &record,
            Some(&cache),
            None,
            &sender,
            |_| panic!("terminal metadata must not reread"),
            |_, _| AudioArtifacts::default(),
        );
        assert!(matches!(receiver.try_recv(), Ok(EnrichmentPart::Overview(actual)) if actual == state));
        assert!(receiver.is_empty());
    }
}

/// Rejects metadata read across a source replacement and skips decoding
/// so stale artifacts cannot reach the preview.
#[test]
fn replacement_during_metadata_rejects_all_parts_and_skips_waveform() {
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
        |path| {
            std::fs::write(path, b"replacement source").expect("replace source");
            Some(Arc::new(FileTags::default()))
        },
        |_, _| panic!("stale source must not decode"),
    );
    assert!(receiver.is_empty());
}
