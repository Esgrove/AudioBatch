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

    /// Shares expensive decoder admission with the transient gain prediction reader.
    pub(super) const fn has_active_reader(&self) -> bool {
        self.active.is_some()
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
    pub(super) fn start_enrichment(&mut self, cx: &Context<Self>) {
        if self.is_demo() || !self.artwork_rewrites.is_empty() || self.gain_prediction.has_active_reader() {
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
        let (wake, woken) = async_channel::unbounded::<()>();
        self.enrichment_task = Some(cx.spawn(async move |this, cx| {
            let worker_request = request.clone();
            let worker = cx.background_executor().spawn(async move {
                enrich(
                    &worker_request,
                    &record,
                    cache.as_deref(),
                    known_tags,
                    &WakingSender { sender, wake },
                );
            });
            // Each part wakes this task, and the worker's exit closes the wake channel,
            // so parts are adopted promptly without a polling timer.
            loop {
                let disconnected = woken.recv().await.is_err();
                let (parts, _) = super::drain_events(&receiver);
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

/// Receives enrichment parts from the worker, in the order they become available.
/// A receiving task that has gone discards the part, since the request it belonged to is stale.
trait PartSink {
    /// Delivers one part, or discards it when nothing receives parts anymore.
    fn deliver(&self, part: EnrichmentPart);
}

impl PartSink for Sender<EnrichmentPart> {
    fn deliver(&self, part: EnrichmentPart) {
        let _ = self.send(part);
    }
}

/// Queues each part and then wakes the adopting task, so it never polls on a timer.
/// Dropping it at worker exit closes the wake channel, which tells the task to drain the remaining parts.
struct WakingSender {
    sender: Sender<EnrichmentPart>,
    wake: async_channel::Sender<()>,
}

impl PartSink for WakingSender {
    fn deliver(&self, part: EnrichmentPart) {
        if self.sender.send(part).is_ok() {
            let _ = self.wake.try_send(());
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
    sender: &impl PartSink,
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
    sender: &impl PartSink,
    read_tags: impl FnOnce(&Path) -> Option<Arc<FileTags>>,
    read_audio: impl FnOnce(bool, bool) -> AudioArtifacts,
) {
    if !request.is_current() || !request.identity.matches_record(record) {
        return;
    }
    let cached = cache.and_then(|cache| cache.get_serato_overview(&record.path));
    if let Some(state) = &cached {
        sender.deliver(EnrichmentPart::Overview(state.clone()));
    }
    if !request.is_current() {
        return;
    }
    if let Some(tags) = known_tags {
        sender.deliver(EnrichmentPart::Tags(tags));
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
            sender.deliver(EnrichmentPart::Overview(state));
        }
        sender.deliver(EnrichmentPart::Tags(tags));
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
            sender.deliver(EnrichmentPart::Waveform(Arc::clone(data)));
        }
        if let Some(data) = &spectrogram {
            sender.deliver(EnrichmentPart::Spectrogram(Some(Arc::clone(data))));
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
            sender.deliver(EnrichmentPart::Waveform(waveform));
        }
        // A missing foreground-cache artifact is still pending, not a terminal unavailable result.
        if !cached_spectrogram && (spectrogram.is_some() || request.mode == EnrichmentMode::Repair) {
            sender.deliver(EnrichmentPart::Spectrogram(spectrogram));
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
mod test_enrichment_admission;
#[cfg(test)]
mod test_enrichment_cache_admission;
#[cfg(test)]
mod test_enrichment_parts;
#[cfg(test)]
mod test_enrichment_progress;
#[cfg(test)]
mod test_enrichment_spectral_parts;
#[cfg(test)]
mod test_native_audio_artifacts;
