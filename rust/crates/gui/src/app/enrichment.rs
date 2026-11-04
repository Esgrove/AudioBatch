//! Post-load preview enrichment with one admitted blocking reader and source-scoped publication.
//!
//! Cancelling invalidates stages but retains the active slot until the reader actually returns.
//! Pending work contains identities only, and generated unselected audio previews are persisted and released.
//! Selected cache hits can display during loading, but missing-artifact readers wait for completed-event reconciliation.

use std::collections::{HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use audiobatch_core::decode::AudioFileDecoder;
use audiobatch_core::output_file::SourceIdentity;
use audiobatch_core::record::file_identity;
use audiobatch_core::spectrogram::{SpectrogramBuilder, SpectrogramData};
use audiobatch_core::tags::read_display_tags;
use audiobatch_core::waveform::WaveformBuilder;
use audiobatch_core::{AnalysisCache, AnalysisRecord, FileTags, SeratoOverviewState, WaveformOverview};
use crossbeam_channel::{Sender, unbounded};
use gpui_kit::Context;

use super::AppView;
use super::analysis::ANALYSIS_POLL_INTERVAL;
use super::preview::PreviewVersion;

/// Maximum retained display-tag results, including negative results, independent of table size.
const TAG_RESULT_LIMIT: usize = 8;

/// Maximum source channels accepted by native waveform previews.
const MAXIMUM_PREVIEW_CHANNELS: usize = 64;

/// Spectral images support original mono and stereo streams only.
const MAXIMUM_SPECTROGRAM_CHANNELS: u32 = 2;

/// Artifact access permitted when a reader takes the single active slot.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum EnrichmentMode {
    /// Loads existing artifacts without invoking tag readers, decoding, or cache writes.
    CacheOnly,
    /// Repairs missing artifacts after completed-event reconciliation.
    Repair,
}

/// Independent artifact parts delivered before a potentially long waveform pass completes.
enum EnrichmentPart {
    Overview(SeratoOverviewState),
    Tags(Option<Arc<FileTags>>),
    Waveform(Arc<WaveformOverview>),
    Spectrogram(Option<Arc<SpectrogramData>>),
}

/// GUI-owned admission state, separate from playback and selected-preview generations.
#[derive(Default)]
pub(super) struct EnrichmentQueue {
    gate_open: bool,
    generation: u64,
    selected_attempt: Option<EnrichmentIdentity>,
    selected_cache_attempt: Option<EnrichmentIdentity>,
    selected_audio_attempt: Option<SelectedAudioAttempt>,
    pending: VecDeque<EnrichmentIdentity>,
    queued: HashSet<EnrichmentIdentity>,
    completed: HashSet<EnrichmentIdentity>,
    candidates: HashSet<EnrichmentIdentity>,
    active: Option<EnrichmentRequest>,
    tags: VecDeque<(EnrichmentIdentity, Option<Arc<FileTags>>)>,
}

/// A lightweight candidate belonging to exactly one measured source version.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
struct EnrichmentIdentity {
    path: PathBuf,
    file_size: u64,
    modified_time_ms: i64,
}

/// One selected audio attempt, terminal only while the same preview generation remains selected.
struct SelectedAudioAttempt {
    identity: EnrichmentIdentity,
    preview: Option<(PreviewVersion, u64)>,
}

/// One admitted job, whose cancellation does not release its blocking-worker slot.
#[derive(Clone)]
struct EnrichmentRequest {
    identity: EnrichmentIdentity,
    source_identity: Option<SourceIdentity>,
    cancelled: Arc<AtomicBool>,
    selected: Arc<AtomicBool>,
    preview: Option<(PreviewVersion, u64)>,
    generation: u64,
    mode: EnrichmentMode,
    artifacts_attempted: bool,
}

/// Independent audio artifacts produced by a single optional decode pass.
#[derive(Default)]
struct AudioArtifacts {
    waveform: Option<Arc<WaveformOverview>>,
    spectrogram: Option<Arc<SpectrogramData>>,
    geometry: Option<ArtifactGeometry>,
}

/// Actual source geometry captured from the decoder, never inferred from presentation duration.
struct ArtifactGeometry {
    sample_rate: u32,
    channels: u32,
    length_in_frames: u64,
}

impl EnrichmentQueue {
    /// Reports completed and total unique source candidates while background repair is admitted.
    /// Cancelled readers remain pending until their occupied worker slot actually drains.
    pub(super) fn progress(&self) -> Option<(usize, usize)> {
        let total = self.candidates.len();
        let completed = self.completed.len();
        (self.gate_open && completed < total).then_some((completed, total))
    }

    /// Closes admission for a foreground run without dropping its still-running blocking reader.
    pub(super) fn pause(&mut self) {
        self.gate_open = false;
        if let Some(active) = &self.active {
            active.cancelled.store(true, Ordering::Release);
        }
    }

    /// Releases selected ownership without freeing an active blocking slot.
    pub(super) fn deselect(&mut self) {
        self.selected_attempt = None;
        self.selected_cache_attempt = None;
        self.selected_audio_attempt = None;
        if let Some(active) = &mut self.active {
            active.selected.store(false, Ordering::Release);
            active.preview = None;
        }
    }

    /// Starts a new load generation and forgets obsolete candidates and deferred failures.
    pub(super) fn invalidate(&mut self) {
        self.pause();
        self.generation = self.generation.wrapping_add(1);
        self.selected_attempt = None;
        self.selected_cache_attempt = None;
        self.selected_audio_attempt = None;
        self.pending.clear();
        self.queued.clear();
        self.completed.clear();
        self.candidates.clear();
        self.tags.clear();
    }

    /// Queues a source once, promoting the selected identity and cooperatively interrupting unselected work.
    fn enqueue(&mut self, identity: EnrichmentIdentity, selected: bool) {
        self.candidates.insert(identity.clone());
        if let Some(active) = &self.active {
            if active.identity == identity && active.is_running() {
                if selected {
                    active.selected.store(true, Ordering::Release);
                }
                return;
            }
            if selected && !active.selected.load(Ordering::Acquire) {
                active.cancelled.store(true, Ordering::Release);
            }
        }
        if self.completed.contains(&identity) && !selected {
            return;
        }
        if !self.queued.insert(identity.clone()) {
            if !selected {
                return;
            }
            self.pending.retain(|candidate| candidate != &identity);
        }
        if selected {
            self.pending.push_front(identity);
        } else {
            self.pending.push_back(identity);
        }
    }

    /// Admits one reader, allowing only selected cache loads before completed-event reconciliation.
    fn take_next(
        &mut self,
        selected: Option<&Path>,
        preview: Option<(PreviewVersion, u64)>,
        repair_allowed: bool,
    ) -> Option<EnrichmentRequest> {
        if self.active.is_some() {
            return None;
        }
        let mode = if self.gate_open && repair_allowed {
            EnrichmentMode::Repair
        } else {
            EnrichmentMode::CacheOnly
        };
        let identity = match mode {
            EnrichmentMode::Repair => self.pending.pop_front()?,
            EnrichmentMode::CacheOnly => {
                let index = self.pending.iter().position(|identity| {
                    selected == Some(identity.path.as_path()) && self.selected_cache_attempt.as_ref() != Some(identity)
                })?;
                self.pending.remove(index)?
            }
        };
        self.queued.remove(&identity);
        let is_selected = selected == Some(identity.path.as_path());
        let preview = if is_selected { preview } else { None };
        let artifacts_attempted = if is_selected {
            self.selected_audio_attempt
                .as_ref()
                .is_some_and(|attempt| attempt.identity == identity && attempt.preview == preview)
        } else {
            self.completed.contains(&identity)
        };
        let request = EnrichmentRequest {
            source_identity: SourceIdentity::capture(&identity.path).ok(),
            identity,
            cancelled: Arc::new(AtomicBool::new(false)),
            selected: Arc::new(AtomicBool::new(is_selected)),
            preview,
            generation: self.generation,
            mode,
            artifacts_attempted,
        };
        self.active = Some(request.clone());
        Some(request)
    }

    /// Joins only the same selected-preview generation, restarting obsolete selected reads cooperatively.
    fn select(&mut self, identity: EnrichmentIdentity, preview: Option<&(PreviewVersion, u64)>) {
        if let Some(active) = &self.active {
            let matching = active.identity == identity;
            active.selected.store(matching, Ordering::Release);
            if !matching || active.preview.as_ref() != preview {
                // A reader captures its preview token once.
                // Reauthorizing it would let queued A → B → A deliveries target the new preview.
                active.cancelled.store(true, Ordering::Release);
            }
        }
        self.enqueue(identity, true);
    }

    /// Rejects obsolete load and selected-preview deliveries independently of path equality.
    fn accepts_preview(&self, request: &EnrichmentRequest, source: &super::preview::PreviewSource) -> bool {
        request.generation == self.generation
            && request.is_current()
            && request
                .preview
                .as_ref()
                .is_some_and(|(version, generation)| source.accepts(version, *generation))
    }

    /// Releases admission only after the blocking task has been awaited, never merely cancelled.
    fn complete(&mut self, request: &EnrichmentRequest) {
        self.active = None;
        if request.generation != self.generation {
            return;
        }
        if request.is_current() {
            match request.mode {
                EnrichmentMode::CacheOnly => {
                    if request.selected.load(Ordering::Acquire) {
                        self.selected_cache_attempt = Some(request.identity.clone());
                    }
                    // A cache miss is not completed repair and must wait, without spinning, for the repair gate.
                    self.enqueue(request.identity.clone(), request.selected.load(Ordering::Acquire));
                }
                EnrichmentMode::Repair => {
                    if request.selected.load(Ordering::Acquire) {
                        self.selected_audio_attempt = Some(SelectedAudioAttempt {
                            identity: request.identity.clone(),
                            preview: request.preview.clone(),
                        });
                        if self.tags.iter().any(|(identity, _)| identity == &request.identity) {
                            self.selected_attempt = Some(request.identity.clone());
                        }
                    }
                    self.completed.insert(request.identity.clone());
                }
            }
        } else if request.matches_source() {
            if self.queued.insert(request.identity.clone()) {
                self.pending.push_back(request.identity.clone());
            }
        } else {
            if request.source_identity.is_none() && request.is_running() {
                self.selected_audio_attempt = Some(SelectedAudioAttempt {
                    identity: request.identity.clone(),
                    preview: request.preview.clone(),
                });
            }
            self.discard(&request.identity);
        }
    }

    /// Removes a source that no longer belongs to a usable row so stale candidates cannot hold progress open.
    fn discard(&mut self, identity: &EnrichmentIdentity) {
        self.pending.retain(|candidate| candidate != identity);
        self.queued.remove(identity);
        self.completed.remove(identity);
        self.candidates.remove(identity);
    }

    /// Remembers a small number of display reads so selection changes do not reread the same source repeatedly.
    fn remember_tags(&mut self, identity: EnrichmentIdentity, tags: Option<Arc<FileTags>>) {
        self.tags.retain(|(candidate, _)| candidate != &identity);
        self.tags.push_front((identity, tags));
        self.tags.truncate(TAG_RESULT_LIMIT);
    }
}

impl Drop for EnrichmentQueue {
    fn drop(&mut self) {
        self.pause();
    }
}

impl EnrichmentIdentity {
    /// Captures only the identity already established by successful analysis.
    fn from_record(record: &AnalysisRecord) -> Self {
        Self {
            path: record.path.clone(),
            file_size: record.file_size,
            modified_time_ms: record.modified_time_ms,
        }
    }

    /// Rejects source replacement before every expensive stage and publication.
    fn matches_disk(&self) -> bool {
        file_identity(&self.path) == Some((self.file_size, self.modified_time_ms))
    }

    /// Checks a result row without replacing any measurement or operation fields.
    fn matches_record(&self, record: &AnalysisRecord) -> bool {
        record.is_ready()
            && record.path == self.path
            && record.file_size == self.file_size
            && record.modified_time_ms == self.modified_time_ms
            && record.matches_file_on_disk()
    }
}

impl EnrichmentRequest {
    /// Checks the cancellation token cheaply between decoded blocks.
    fn is_running(&self) -> bool {
        !self.cancelled.load(Ordering::Acquire)
    }

    /// Checks the original filesystem object as well as the measured row's coarse source key.
    fn matches_source(&self) -> bool {
        self.identity.matches_disk()
            && self
                .source_identity
                .as_ref()
                .is_some_and(|source| source.validate(&self.identity.path).is_ok())
    }

    /// Checks runtime cancellation and the original filesystem object at every stage boundary.
    fn is_current(&self) -> bool {
        self.is_running() && self.matches_source()
    }
}

impl AppView {
    /// Queues every ready row after result application and restored selection, including all-cache-hit runs.
    pub(super) fn resume_enrichment(&mut self, cx: &mut Context<Self>) {
        if self.is_demo() || self.is_running() || self.analysis_workers > 0 || !self.artwork_rewrites.is_empty() {
            return;
        }
        self.enrichment.gate_open = true;
        let delegate = self.table.read(cx).delegate();
        for index in 0..delegate.len() {
            if let Some(record) = delegate.row(index).and_then(|row| row.ready_record()) {
                self.enrichment.enqueue(EnrichmentIdentity::from_record(record), false);
            }
        }
        self.request_selected_enrichment(cx);
        self.start_enrichment(cx);
    }

    /// Joins selected tags, waveform, and spectral requests to the same bounded reader used by directory repair.
    pub(super) fn request_selected_enrichment(&mut self, cx: &mut Context<Self>) {
        let Some(record) = self.selected_record(cx) else {
            return;
        };
        if !self.preview_source.matches_record(record) {
            return;
        }
        let identity = EnrichmentIdentity::from_record(record);
        let preview = self.preview_source.token();
        if self
            .enrichment
            .selected_audio_attempt
            .as_ref()
            .is_some_and(|attempt| attempt.identity == identity && attempt.preview == preview)
        {
            return;
        }
        let has_spectrogram = self.waveform.read(cx).has_spectrogram_result();
        if self.enrichment.selected_attempt.as_ref() == Some(&identity) && has_spectrogram {
            return;
        }
        let has_waveform = record.waveform.is_some();
        let cached_tags = self
            .enrichment
            .tags
            .iter()
            .find(|(candidate, _)| candidate == &identity)
            .cloned();
        let has_tags = cached_tags.is_some();
        if let Some((_, tags)) = cached_tags {
            self.adopt_display_tags(&identity.path, tags, cx);
        }
        if has_spectrogram && has_waveform && has_tags {
            return;
        }
        self.enrichment.select(identity, self.preview_source.token().as_ref());
        self.start_enrichment(cx);
    }

    /// Starts a blocking reader and keeps its drain alive through cancellation and foreground runs.
    fn start_enrichment(&mut self, cx: &Context<Self>) {
        if self.is_demo() || !self.artwork_rewrites.is_empty() {
            return;
        }
        let repair_allowed = !self.is_running() && self.analysis_workers == 0;
        let Some(request) =
            self.enrichment
                .take_next(self.selected.as_deref(), self.preview_source.token(), repair_allowed)
        else {
            return;
        };
        let record = self
            .table
            .read(cx)
            .delegate()
            .row_for(&request.identity.path)
            .and_then(|row| row.ready_record());
        let Some(record) = record.filter(|record| request.identity.matches_record(record)) else {
            self.enrichment.active = None;
            self.enrichment.discard(&request.identity);
            self.start_enrichment(cx);
            return;
        };
        let mut record = record.clone();
        record.short_term_loudness = None;
        record.overloads = None;
        let known_tags = self
            .enrichment
            .tags
            .iter()
            .find(|(identity, _)| identity == &request.identity)
            .map(|(_, tags)| tags.clone());
        let cache = self
            .coordinator
            .as_ref()
            .map(|coordinator| Arc::clone(coordinator.cache()));
        let (sender, receiver) = unbounded();
        self.enrichment_task = Some(cx.spawn(async move |this, cx| {
            let worker_request = request.clone();
            let worker = cx.background_executor().spawn(async move {
                enrich(&worker_request, &record, cache.as_deref(), known_tags, &sender);
            });
            loop {
                let (parts, disconnected) = super::drain_events(&receiver);
                if !parts.is_empty() {
                    let _ = this.update(cx, |view, cx| {
                        for part in parts {
                            view.adopt_enrichment(&request, part, cx);
                        }
                    });
                }
                if disconnected {
                    break;
                }
                cx.background_executor().timer(ANALYSIS_POLL_INTERVAL).await;
            }
            worker.await;
            let _ = this.update(cx, |view, cx| {
                view.enrichment.complete(&request);
                view.request_selected_enrichment(cx);
                view.start_enrichment(cx);
                cx.notify();
            });
        }));
    }

    /// Adopts artifact fields only, leaving measurement provenance, messages, gain, transport and selection intact.
    fn adopt_enrichment(&mut self, request: &EnrichmentRequest, part: EnrichmentPart, cx: &mut Context<Self>) {
        if request.generation != self.enrichment.generation || !request.is_current() {
            return;
        }
        let identity = &request.identity;
        let matches_row = self
            .table
            .read(cx)
            .delegate()
            .row_for(&identity.path)
            .and_then(|row| row.ready_record())
            .is_some_and(|record| identity.matches_record(record));
        if !matches_row || self.artwork_rewrites.contains(&identity.path) {
            return;
        }
        if let EnrichmentPart::Tags(tags) = &part {
            self.enrichment.remember_tags(identity.clone(), tags.clone());
        }
        if self.selected.as_ref() != Some(&identity.path)
            || !self.enrichment.accepts_preview(request, &self.preview_source)
        {
            return;
        }
        match part {
            EnrichmentPart::Overview(state) => self.show_serato_overview(&identity.path, state.overview().cloned(), cx),
            EnrichmentPart::Tags(tags) => self.adopt_display_tags(&identity.path, tags, cx),
            EnrichmentPart::Waveform(waveform) => {
                self.table.update(cx, |state, _| {
                    state.delegate_mut().attach_waveform(
                        &identity.path,
                        identity.file_size,
                        identity.modified_time_ms,
                        waveform,
                    );
                });
                self.refresh_selected_waveform(cx);
            }
            EnrichmentPart::Spectrogram(data) => {
                self.waveform
                    .update(cx, |waveform, cx| waveform.update_spectrogram(data, cx));
            }
        }
    }
}

/// Loads cached artifacts during foreground work, or repairs metadata before audio after reconciliation.
#[expect(
    clippy::option_option,
    reason = "A remembered failed read is distinct from a source not read yet"
)]
fn enrich(
    request: &EnrichmentRequest,
    record: &AnalysisRecord,
    cache: Option<&AnalysisCache>,
    known_tags: Option<Option<Arc<FileTags>>>,
    sender: &Sender<EnrichmentPart>,
) {
    let mut record = record.clone();
    if !request.is_current() || !record.capture_source_identity() || !request.is_current() {
        return;
    }
    enrich_with_readers(
        request,
        &record,
        cache,
        known_tags,
        sender,
        read_tags,
        |waveform_missing, spectrogram_missing| match build_audio_artifacts(
            &record,
            waveform_missing,
            spectrogram_missing,
            || request.is_running(),
        ) {
            Ok(artifacts) => artifacts,
            Err(error) => {
                tracing::warn!("Could not build audio previews of {}: {error:#}", record.path.display());
                AudioArtifacts::default()
            }
        },
    );
}

/// Runs independent metadata and audio stages with one injectable combined reader for deterministic lifecycle tests.
#[expect(
    clippy::option_option,
    reason = "A remembered failed read is distinct from a source not read yet"
)]
fn enrich_with_readers(
    request: &EnrichmentRequest,
    record: &AnalysisRecord,
    cache: Option<&AnalysisCache>,
    known_tags: Option<Option<Arc<FileTags>>>,
    sender: &Sender<EnrichmentPart>,
    read_tags: impl FnOnce(&Path) -> Option<Arc<FileTags>>,
    read_audio: impl FnOnce(bool, bool) -> AudioArtifacts,
) {
    if !request.is_current() || !request.identity.matches_record(record) {
        return;
    }
    let cached = cache.and_then(|cache| cache.get_serato_overview(&record.path));
    if let Some(state) = &cached {
        let _ = sender.send(EnrichmentPart::Overview(state.clone()));
    }
    if !request.is_current() {
        return;
    }
    if let Some(tags) = known_tags {
        let _ = sender.send(EnrichmentPart::Tags(tags));
    } else if request.mode == EnrichmentMode::Repair
        && needs_display_tags(cached.as_ref(), request.selected.load(Ordering::Acquire))
    {
        let tags = read_tags(&record.path);
        if !request.is_current() {
            return;
        }
        if let Some(tags) = &tags {
            let state = SeratoOverviewState::from_tags(&record.path, tags);
            if cached.as_ref() != Some(&state)
                && let Some(cache) = cache
                && request.is_current()
                && let Err(error) = cache.store_serato_overview_for_record(record, &state)
            {
                tracing::warn!("Could not cache Serato overview of {}: {error}", record.path.display());
            }
            let _ = sender.send(EnrichmentPart::Overview(state));
        }
        let _ = sender.send(EnrichmentPart::Tags(tags));
    }
    if !request.is_current() {
        return;
    }
    let mut waveform = record
        .waveform
        .clone()
        .or_else(|| cache.and_then(|cache| cache.get_waveform(&record.path)));
    let mut spectrogram = cache
        .and_then(|cache| cache.get_spectrogram(&record.path))
        .filter(|data| data.matches_stream(record.sample_rate, record.length_in_samples, record.channels));
    if !request.is_current() {
        return;
    }
    let cached_waveform = waveform.is_some();
    let cached_spectrogram = spectrogram.is_some();
    if request.selected.load(Ordering::Acquire) {
        if let Some(data) = &waveform {
            let _ = sender.send(EnrichmentPart::Waveform(Arc::clone(data)));
        }
        if let Some(data) = &spectrogram {
            let _ = sender.send(EnrichmentPart::Spectrogram(Some(Arc::clone(data))));
        }
    }
    let repair_audio = request.mode == EnrichmentMode::Repair && !request.artifacts_attempted;
    if repair_audio && (waveform.is_none() || spectrogram.is_none()) && request.is_current() {
        let artifacts = read_audio(waveform.is_none(), spectrogram.is_none());
        if !request.is_current() {
            return;
        }
        // Artifact publication captures decoder geometry but never replaces measured levels or provenance.
        let mut artifact_record = record.clone();
        if let Some(geometry) = artifacts.geometry {
            artifact_record.sample_rate = geometry.sample_rate;
            artifact_record.channels = geometry.channels;
            artifact_record.length_in_samples = geometry.length_in_frames;
            artifact_record.duration_seconds = geometry.length_in_frames as f64 / f64::from(geometry.sample_rate);
        }
        if let Some(built) = artifacts.waveform {
            if let Some(cache) = cache
                && request.is_current()
                && let Err(error) = cache.store_waveform(&artifact_record, &built)
            {
                tracing::warn!("Could not cache waveform of {}: {error}", record.path.display());
            }
            waveform = Some(built);
        }
        if let Some(built) = artifacts.spectrogram {
            if let Some(cache) = cache
                && request.is_current()
                && let Err(error) = cache.store_spectrogram(&artifact_record, &built)
            {
                tracing::warn!("Could not cache spectrogram of {}: {error}", record.path.display());
            }
            spectrogram = Some(built);
        }
    }
    if request.is_current() && request.selected.load(Ordering::Acquire) {
        if !cached_waveform && let Some(waveform) = waveform {
            let _ = sender.send(EnrichmentPart::Waveform(waveform));
        }
        // A missing foreground-cache artifact is still pending, not a terminal unavailable result.
        if !cached_spectrogram && (spectrogram.is_some() || request.mode == EnrichmentMode::Repair) {
            let _ = sender.send(EnrichmentPart::Spectrogram(spectrogram));
        }
    }
}

/// Treats every valid cached outcome as terminal background metadata work.
/// Selection still needs one display-tag read for cues and inspector fields.
const fn needs_display_tags(cached: Option<&SeratoOverviewState>, selected: bool) -> bool {
    cached.is_none() || selected
}

/// Reads display metadata without turning transient failures into persistent Serato absence.
fn read_tags(path: &Path) -> Option<Arc<FileTags>> {
    match read_display_tags(path) {
        Ok(tags) => Some(Arc::new(tags)),
        Err(error) => {
            tracing::warn!("Could not read display tags of {}: {error}", path.display());
            None
        }
    }
}

/// Builds only missing audio previews in one decode pass, without loudness, true peak or a measurement meter.
/// Cancellation is cooperative at decoded packet boundaries and discards every partial artifact.
fn build_audio_artifacts(
    record: &AnalysisRecord,
    waveform_missing: bool,
    spectrogram_missing: bool,
    running: impl Fn() -> bool,
) -> anyhow::Result<AudioArtifacts> {
    // Unsupported spectral geometry is a completed unavailable result, not a reason to decode valid waveforms again.
    let spectrogram_missing = spectrogram_missing
        && (1..=MAXIMUM_SPECTROGRAM_CHANNELS).contains(&record.channels)
        && record.length_in_samples > 0;
    if !running() || !record.matches_file_on_disk() || !(waveform_missing || spectrogram_missing) {
        return Ok(AudioArtifacts::default());
    }
    let source_identity = SourceIdentity::capture(&record.path)?;
    if !running() || !record.matches_file_on_disk() {
        return Ok(AudioArtifacts::default());
    }
    let mut decoder = AudioFileDecoder::open(&record.path)?;
    let info = decoder.info().clone();
    anyhow::ensure!(
        (1..=MAXIMUM_PREVIEW_CHANNELS).contains(&(info.channels as usize))
            && info.channels == record.channels
            && info.sample_rate == record.sample_rate,
        "Preview stream geometry changed"
    );
    let mut waveform = waveform_missing.then(|| WaveformBuilder::new(info.channels as usize, info.expected_frames));
    let mut spectrogram =
        spectrogram_missing.then(|| SpectrogramBuilder::new(info.channels as usize, info.sample_rate));
    loop {
        if !running() {
            return Ok(AudioArtifacts::default());
        }
        let Some(chunk) = decoder.next_chunk()? else {
            break;
        };
        let mut planes = [&[][..]; MAXIMUM_PREVIEW_CHANNELS];
        for (plane, samples) in planes.iter_mut().zip(chunk) {
            *plane = samples.as_slice();
        }
        let (planes, _) = planes.split_at(info.channels as usize);
        if let Some(builder) = waveform.as_mut() {
            builder.push(planes);
        }
        if let Some(builder) = spectrogram.as_mut() {
            builder.push(planes);
        }
    }
    if !running() || !record.matches_file_on_disk() || source_identity.validate(&record.path).is_err() {
        return Ok(AudioArtifacts::default());
    }
    let length_in_frames = decoder.position();
    let spectrogram = spectrogram.and_then(SpectrogramBuilder::finish);
    if !running() || !record.matches_file_on_disk() || source_identity.validate(&record.path).is_err() {
        return Ok(AudioArtifacts::default());
    }
    Ok(AudioArtifacts {
        waveform: waveform.map(|builder| builder.finish(info.sample_rate, length_in_frames)),
        spectrogram,
        geometry: Some(ArtifactGeometry {
            sample_rate: info.sample_rate,
            channels: info.channels,
            length_in_frames,
        }),
    })
}

#[cfg(test)]
mod test_enrichment_parts {
    use std::cell::RefCell;

    use audiobatch_core::overload::OverloadHistory;
    use audiobatch_core::serato_tags::Overview;
    use audiobatch_core::{AnalysisStatus, ShortTermLoudnessHistory};

    use super::*;

    /// Binds independent artifact tests to a real source without decoding it or opening a window.
    pub(super) fn record(path: &Path) -> AnalysisRecord {
        let (file_size, modified_time_ms) = file_identity(path).expect("source identity");
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
        assert!(
            matches!(receiver.try_recv(), Ok(EnrichmentPart::Spectrogram(Some(actual))) if actual == spectrogram())
        );
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
}

#[cfg(test)]
mod test_enrichment_spectral_parts {
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
                assert_eq!(file_identity(&path), Some((record.file_size, record.modified_time_ms)));
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
}

#[cfg(test)]
mod test_enrichment_cache_admission {
    use super::*;
    use crate::app::preview::PreviewSource;

    /// Captures a source identity without decoding or opening a window.
    fn identity(path: &Path) -> EnrichmentIdentity {
        let (file_size, modified_time_ms) = file_identity(path).expect("source identity");
        EnrichmentIdentity {
            path: path.to_path_buf(),
            file_size,
            modified_time_ms,
        }
    }

    /// Restricts foreground admission to the selected cache lookup even when a prior repair gate remains open.
    /// Background work and additional selected readers must wait for the occupied slot.
    #[test]
    fn foreground_admits_only_the_selected_cache_load_even_with_an_open_gate() {
        let first = tempfile::NamedTempFile::new().expect("background source");
        let selected = tempfile::NamedTempFile::new().expect("selected source");
        for gate_open in [false, true] {
            let mut queue = EnrichmentQueue::default();
            queue.gate_open = gate_open;
            queue.enqueue(identity(first.path()), false);
            queue.enqueue(identity(selected.path()), true);
            let request = queue
                .take_next(Some(selected.path()), None, false)
                .expect("selected cache load");
            assert_eq!(request.mode, EnrichmentMode::CacheOnly);
            assert_eq!(request.identity, identity(selected.path()));
            assert!(request.selected.load(Ordering::Acquire));
            assert_eq!(queue.pending, [identity(first.path())]);
            assert!(queue.take_next(Some(selected.path()), None, false).is_none());
        }
    }

    /// Retains one deferred repair after a cache-only attempt without spinning selected retries during foreground work.
    #[test]
    fn cache_completion_defers_repair_without_an_immediate_selected_retry() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let identity = identity(file.path());
        let mut queue = EnrichmentQueue::default();
        queue.select(identity.clone(), None);
        let request = queue.take_next(Some(file.path()), None, true).expect("cache load");
        assert_eq!(request.mode, EnrichmentMode::CacheOnly);
        queue.complete(&request);
        assert_eq!(queue.selected_cache_attempt, Some(identity.clone()));
        assert!(queue.selected_attempt.is_none());
        assert!(queue.completed.is_empty());
        for _ in 0..3 {
            queue.select(identity.clone(), None);
            assert!(queue.take_next(Some(file.path()), None, true).is_none());
            assert_eq!(queue.pending.len(), 1);
            assert_eq!(queue.pending.front(), Some(&identity));
        }
        queue.gate_open = true;
        assert!(queue.take_next(Some(file.path()), None, false).is_none());
        let repair = queue.take_next(Some(file.path()), None, true).expect("deferred repair");
        assert_eq!(repair.mode, EnrichmentMode::Repair);
        assert_eq!(repair.identity, identity);
    }

    /// Keeps an admitted reader's cache-only mode fixed so opening repair admission cannot change its I/O contract.
    #[test]
    fn completion_reconciliation_does_not_upgrade_an_active_cache_only_reader() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let mut queue = EnrichmentQueue::default();
        queue.select(identity(file.path()), None);
        let request = queue.take_next(Some(file.path()), None, false).expect("cache load");
        queue.gate_open = true;
        queue.select(identity(file.path()), None);
        assert!(queue.pending.is_empty());
        assert!(queue.take_next(Some(file.path()), None, true).is_none());
        assert_eq!(request.mode, EnrichmentMode::CacheOnly);
        queue.complete(&request);
        assert_eq!(
            queue.take_next(Some(file.path()), None, true).expect("repair").mode,
            EnrichmentMode::Repair
        );
    }

    /// Holds admission through cancellation so changing selection cannot overlap two blocking cache readers.
    #[test]
    fn cancelled_cache_load_retains_the_slot_until_its_worker_returns() {
        let first = tempfile::NamedTempFile::new().expect("first source");
        let second = tempfile::NamedTempFile::new().expect("second source");
        let mut source = PreviewSource::default();
        source.begin(first.path());
        let mut queue = EnrichmentQueue::default();
        queue.select(identity(first.path()), source.token().as_ref());
        let request = queue
            .take_next(Some(first.path()), source.token(), false)
            .expect("first cache load");
        queue.deselect();
        source.begin(second.path());
        queue.select(identity(second.path()), source.token().as_ref());
        assert!(!request.is_running());
        assert!(queue.take_next(Some(second.path()), source.token(), false).is_none());
        queue.complete(&request);
        let next = queue
            .take_next(Some(second.path()), source.token(), false)
            .expect("second cache load");
        assert_eq!(next.identity, identity(second.path()));
        assert_eq!(next.mode, EnrichmentMode::CacheOnly);
    }

    /// Rejects a cache delivery from an earlier visit to the same path so a selection round trip cannot revive it.
    #[test]
    fn cache_load_round_trip_accepts_only_the_latest_preview_token() {
        let first = tempfile::NamedTempFile::new().expect("first source");
        let second = tempfile::NamedTempFile::new().expect("second source");
        let mut source = PreviewSource::default();
        source.begin(first.path());
        let mut queue = EnrichmentQueue::default();
        queue.select(identity(first.path()), source.token().as_ref());
        let old = queue
            .take_next(Some(first.path()), source.token(), false)
            .expect("first cache load");
        queue.deselect();
        source.begin(second.path());
        queue.select(identity(second.path()), source.token().as_ref());
        queue.deselect();
        source.begin(first.path());
        queue.select(identity(first.path()), source.token().as_ref());
        assert!(!queue.accepts_preview(&old, &source));
        assert!(queue.take_next(Some(first.path()), source.token(), false).is_none());
        queue.complete(&old);
        let current = queue
            .take_next(Some(first.path()), source.token(), false)
            .expect("latest cache load");
        assert!(queue.accepts_preview(&current, &source));
        assert!(!queue.accepts_preview(&old, &source));
        queue.select(identity(first.path()), source.token().as_ref());
        assert!(current.is_running());
    }

    /// Forgets old cache attempts on load invalidation while retaining admission until the obsolete reader drains.
    #[test]
    fn new_load_generation_discards_cache_attempts_but_not_active_admission() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let mut queue = EnrichmentQueue::default();
        queue.select(identity(file.path()), None);
        let old = queue.take_next(Some(file.path()), None, false).expect("cache load");
        queue.invalidate();
        queue.select(identity(file.path()), None);
        assert!(queue.take_next(Some(file.path()), None, false).is_none());
        queue.complete(&old);
        assert!(queue.selected_cache_attempt.is_none());
        assert!(queue.completed.is_empty());
        assert!(queue.take_next(Some(file.path()), None, false).is_some());
    }
}

#[cfg(test)]
mod test_enrichment_admission {
    use super::*;

    /// Creates a candidate without filesystem or GUI dependencies.
    fn identity(path: &str) -> EnrichmentIdentity {
        EnrichmentIdentity {
            path: path.into(),
            file_size: 1,
            modified_time_ms: 2,
        }
    }

    /// Requires completed-load admission and an empty active slot so background repair stays deferred and serial.
    #[test]
    fn completion_gate_and_one_active_reader_are_required() {
        let mut queue = EnrichmentQueue::default();
        queue.enqueue(identity("a"), false);
        assert!(queue.take_next(None, None, true).is_none());
        queue.gate_open = true;
        assert!(queue.take_next(None, None, true).is_some());
        queue.enqueue(identity("b"), false);
        assert!(queue.take_next(None, None, true).is_none());
    }

    /// Allows the selected preview to reuse its cache during a foreground run while reserving repair for completion.
    #[test]
    fn foreground_run_allows_selected_cache_reads_but_defers_repairs() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let (file_size, modified_time_ms) = file_identity(file.path()).expect("source identity");
        let selected = EnrichmentIdentity {
            path: file.path().to_path_buf(),
            file_size,
            modified_time_ms,
        };
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = true;
        queue.enqueue(identity("background"), false);
        queue.enqueue(selected.clone(), true);
        let active = queue
            .take_next(Some(file.path()), None, false)
            .expect("selected cached artifacts remain available");
        assert_eq!(active.identity, selected);
        assert_eq!(active.mode, EnrichmentMode::CacheOnly);
        queue.complete(&active);
        assert!(queue.take_next(Some(file.path()), None, false).is_none());
        let repair = queue
            .take_next(Some(file.path()), None, true)
            .expect("foreground completion resumes repairs");
        assert_eq!(repair.mode, EnrichmentMode::Repair);
    }

    /// Promotes selected work ahead of background candidates without duplicating source identities in the queue.
    #[test]
    fn selection_promotes_and_deduplicates_candidates() {
        let mut queue = EnrichmentQueue::default();
        queue.enqueue(identity("a"), false);
        queue.enqueue(identity("b"), false);
        queue.enqueue(identity("b"), true);
        queue.enqueue(identity("a"), false);
        assert_eq!(queue.pending.len(), 2);
        queue.gate_open = true;
        assert_eq!(
            queue
                .take_next(Some(Path::new("b")), None, true)
                .expect("selected first")
                .identity,
            identity("b")
        );
    }

    /// Retains the cancelled reader's slot until completion so reopening the gate cannot create concurrent repairs.
    #[test]
    fn cancellation_does_not_release_a_blocking_slot() {
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = true;
        queue.enqueue(identity("a"), false);
        let active = queue.take_next(None, None, true).expect("active");
        queue.pause();
        assert!(!active.is_running());
        queue.gate_open = true;
        queue.enqueue(identity("b"), true);
        assert!(queue.take_next(None, None, true).is_none());
        queue.complete(&active);
        assert_eq!(
            queue.take_next(None, None, true).expect("drained").identity,
            identity("b")
        );
    }

    /// Avoids background reads for terminal Serato outcomes while still reading selected display tags for cue data.
    #[test]
    fn valid_negative_serato_outcomes_skip_background_metadata_but_not_selected_cues() {
        for state in [
            SeratoOverviewState::Absent,
            SeratoOverviewState::Unsupported,
            SeratoOverviewState::Invalid,
        ] {
            assert!(!needs_display_tags(Some(&state), false));
            assert!(needs_display_tags(Some(&state), true));
        }
        assert!(needs_display_tags(None, false));
    }

    /// Prevents an invalidated load from marking new work complete or releasing its reader slot before the drain.
    #[test]
    fn load_invalidation_keeps_the_active_slot_and_rejects_the_old_generation() {
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = true;
        queue.enqueue(identity("a"), false);
        let active = queue.take_next(None, None, true).expect("active");
        queue.invalidate();
        queue.gate_open = true;
        queue.enqueue(identity("b"), true);
        assert!(queue.take_next(None, None, true).is_none());
        queue.complete(&active);
        assert!(queue.completed.is_empty());
        assert_eq!(
            queue.take_next(None, None, true).expect("new generation").identity,
            identity("b")
        );
    }

    /// Invalidates an earlier preview generation even for the same path and waits for its reader before restarting.
    #[test]
    fn selection_round_trip_rejects_old_preview_without_freeing_the_reader() {
        let first = tempfile::NamedTempFile::new().expect("first source");
        let second = tempfile::NamedTempFile::new().expect("second source");
        let first_identity = EnrichmentIdentity::from_record(&super::test_enrichment_parts::record(first.path()));
        let second_identity = EnrichmentIdentity::from_record(&super::test_enrichment_parts::record(second.path()));
        let mut source = super::super::preview::PreviewSource::default();
        source.begin(first.path());
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = true;
        queue.select(first_identity.clone(), source.token().as_ref());
        let active = queue
            .take_next(Some(first.path()), source.token(), true)
            .expect("first selected reader");
        assert!(queue.accepts_preview(&active, &source));
        queue.deselect();
        source.begin(second.path());
        queue.select(second_identity, source.token().as_ref());
        queue.deselect();
        source.begin(first.path());
        queue.select(first_identity.clone(), source.token().as_ref());
        assert!(!active.is_running());
        assert!(!queue.accepts_preview(&active, &source));
        assert!(queue.take_next(Some(first.path()), source.token(), true).is_none());
        queue.complete(&active);
        assert!(queue.completed.is_empty());
        let current = queue
            .take_next(Some(first.path()), source.token(), true)
            .expect("unchanged source restarts only after the old reader drains");
        assert_eq!(current.identity, first_identity);
        assert!(!current.artifacts_attempted);
        assert!(queue.accepts_preview(&current, &source));
        assert!(!queue.accepts_preview(&active, &source));
    }

    /// Interrupts background work for selection but joins an already matching preview
    /// so repeated requests do not restart it.
    #[test]
    fn selection_cancels_unselected_work_but_joins_the_current_preview() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let selected_identity = EnrichmentIdentity::from_record(&super::test_enrichment_parts::record(file.path()));
        let mut source = super::super::preview::PreviewSource::default();
        source.begin(file.path());
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = true;
        queue.enqueue(identity("background"), false);
        let background = queue.take_next(None, None, true).expect("background reader");
        queue.select(selected_identity.clone(), source.token().as_ref());
        assert!(!background.is_running());
        assert!(queue.take_next(Some(file.path()), source.token(), true).is_none());
        queue.complete(&background);
        let selected = queue
            .take_next(Some(file.path()), source.token(), true)
            .expect("selected reader");
        queue.select(selected_identity, source.token().as_ref());
        assert!(selected.is_running());
        assert!(queue.pending.is_empty());
        assert!(queue.accepts_preview(&selected, &source));
    }

    /// Retains negative display-tag results within the fixed limit to avoid repeated reads without unbounded memory.
    /// Load invalidation clears those results and closes admission so they cannot leak into a new load.
    #[test]
    fn terminal_negative_tags_are_retained_but_bounded() {
        let mut queue = EnrichmentQueue::default();
        for index in 0..20 {
            queue.remember_tags(identity(&index.to_string()), None);
        }
        assert_eq!(queue.tags.len(), TAG_RESULT_LIMIT);
        assert!(queue.tags.front().expect("newest").1.is_none());
        queue.invalidate();
        assert!(queue.tags.is_empty());
        assert!(!queue.gate_open);
    }
}

#[cfg(test)]
mod test_enrichment_progress {
    use super::*;

    /// Captures real identities so completion tests exercise ordinary source validation.
    fn identity(path: &Path) -> EnrichmentIdentity {
        let (file_size, modified_time_ms) = file_identity(path).expect("source identity");
        EnrichmentIdentity {
            path: path.to_path_buf(),
            file_size,
            modified_time_ms,
        }
    }

    /// Candidate counts stay unique across queued, active, completed, and selected retry states.
    #[test]
    fn progress_counts_unique_candidates_not_selected_retries() {
        let first = tempfile::NamedTempFile::new().expect("first source");
        let second = tempfile::NamedTempFile::new().expect("second source");
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = true;
        queue.enqueue(identity(first.path()), false);
        queue.enqueue(identity(first.path()), true);
        queue.enqueue(identity(second.path()), false);
        assert_eq!(queue.progress(), Some((0, 2)));
        let active = queue.take_next(Some(first.path()), None, true).expect("first repair");
        assert_eq!(queue.progress(), Some((0, 2)));
        queue.complete(&active);
        assert_eq!(queue.progress(), Some((1, 2)));
        queue.enqueue(identity(first.path()), true);
        assert_eq!(queue.progress(), Some((1, 2)));
        let retry = queue
            .take_next(Some(first.path()), None, true)
            .expect("selected lookup");
        assert!(retry.artifacts_attempted);
        queue.complete(&retry);
        assert_eq!(queue.progress(), Some((1, 2)));
        let remaining = queue.take_next(None, None, true).expect("remaining repair");
        queue.complete(&remaining);
        assert_eq!(queue.progress(), None);
        queue.enqueue(identity(first.path()), false);
        assert!(queue.pending.is_empty());
    }

    /// Cache-only foreground work never advances completed repair, and cancellation never advances it early.
    #[test]
    fn foreground_and_cancelled_readers_do_not_report_completed_repairs() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let mut queue = EnrichmentQueue::default();
        queue.enqueue(identity(file.path()), true);
        let cached = queue
            .take_next(Some(file.path()), None, false)
            .expect("foreground cache");
        assert_eq!(queue.progress(), None);
        queue.complete(&cached);
        queue.gate_open = true;
        assert_eq!(queue.progress(), Some((0, 1)));
        let repair = queue.take_next(Some(file.path()), None, true).expect("repair");
        queue.pause();
        assert_eq!(queue.progress(), None);
        queue.gate_open = true;
        assert_eq!(queue.progress(), Some((0, 1)));
        assert!(queue.take_next(Some(file.path()), None, true).is_none());
        queue.complete(&repair);
        assert_eq!(queue.progress(), Some((0, 1)));
        assert!(
            !queue
                .take_next(Some(file.path()), None, true)
                .expect("retry")
                .artifacts_attempted
        );
    }

    /// Invalidating a load clears totals but does not free an older still-running reader's slot.
    #[test]
    fn invalidated_completion_cannot_change_new_generation_progress() {
        let first = tempfile::NamedTempFile::new().expect("first source");
        let second = tempfile::NamedTempFile::new().expect("second source");
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = true;
        queue.enqueue(identity(first.path()), false);
        let old = queue.take_next(None, None, true).expect("old repair");
        queue.invalidate();
        assert_eq!(queue.progress(), None);
        queue.gate_open = true;
        queue.enqueue(identity(second.path()), false);
        assert_eq!(queue.progress(), Some((0, 1)));
        assert!(queue.take_next(None, None, true).is_none());
        queue.complete(&old);
        assert_eq!(queue.progress(), Some((0, 1)));
        let current = queue.take_next(None, None, true).expect("new repair");
        queue.complete(&current);
        assert_eq!(queue.progress(), None);
    }

    /// Replaced sources leave candidate accounting rather than keeping an unreachable repair pending forever.
    #[test]
    fn stale_source_is_removed_from_progress_when_its_reader_returns() {
        let file = tempfile::NamedTempFile::new().expect("source");
        let source = identity(file.path());
        let mut queue = EnrichmentQueue::default();
        queue.gate_open = true;
        queue.enqueue(source, false);
        let active = queue.take_next(None, None, true).expect("repair");
        std::fs::write(file.path(), b"replacement source").expect("replace source");
        queue.complete(&active);
        assert!(queue.candidates.is_empty());
        assert!(queue.pending.is_empty());
        assert_eq!(queue.progress(), None);
    }
}

#[cfg(test)]
mod test_native_audio_artifacts {
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
                assert_eq!(file_identity(&path), Some((record.file_size, record.modified_time_ms)));
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
}
