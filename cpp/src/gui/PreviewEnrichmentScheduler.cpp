/// Serializes optional artifact repair and explicit GUI cache edits on one owned connection.
/// The single detached worker owns all of its dependencies, never waits for UI delivery,
/// and polls cancellation between combined waveform/spectrogram blocks and metadata stages.

#include "gui/PreviewEnrichmentScheduler.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/PreviewArtifactDecoder.h"
#include "serato/SeratoPreviewLoader.h"
#include "serato/SeratoTagCache.h"
#include "serato/SeratoTagTypes.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/ThreadUtils.h"
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace audiobatch
{
/// State shared only because a cancelled worker may outlive its GUI owner.
struct PreviewEnrichmentState {
    /// Constructs a worker-owned connection without opening it on the message thread.
    PreviewEnrichmentState(
        const juce::File& databaseFile,
        PreviewEnrichmentScheduler::Completion onCompletion,
        PreviewEnrichmentScheduler::Work onWork
    ) :
        cache(databaseFile),
        completion(std::move(onCompletion)),
        work(std::move(onWork))
    {}

    AnalysisCache cache;
    PreviewEnrichmentScheduler::Completion completion;
    PreviewEnrichmentScheduler::Work work;
    juce::String activePath;
    /// Optional audio repair stage, protected by mutex and cleared at job retirement.
    juce::String activity;

    std::atomic<bool> stopped {false};
    std::atomic<std::uint64_t> interruption {0};
    std::atomic<std::uint64_t> loadGeneration {1};

    std::condition_variable condition;
    std::mutex mutex;

    std::deque<std::pair<AudioAnalysisRecord, std::optional<AudioAnalysisRecord>>> outputs;
    std::deque<PreviewEnrichmentCandidate> pending;
    std::optional<PreviewEnrichmentCandidate> activeCandidate;
    std::optional<utils::FileIdentity> selected;
    std::unordered_map<juce::String, CustomGainUpdate> deferredGains;
    std::unordered_map<juce::String, CustomGainUpdate> gains;

    /// Directory paths accepted for this pass. Selected paths outside this set do not inflate its total.
    std::unordered_set<juce::String> candidatePaths;
    /// Successfully retired directory paths, preventing selected promotion from counting a file twice.
    std::unordered_set<juce::String> completedPaths;

    bool gateOpen {false};
    bool retryDeferredGains {false};
    bool selectionPending {false};

    std::uint64_t activeInterruption {0};
    std::uint64_t activeSelectionGeneration {0};
    std::uint64_t selectionGeneration {0};
};

namespace
{
/// A committed output and the captured source row to remove if conversion changed its path.
struct EnrichmentOutput {
    AudioAnalysisRecord output;
    std::optional<AudioAnalysisRecord> convertedSource;
};

/// One admitted unit of work, keeping source ownership separate from foreground and selection generations.
struct EnrichmentJob {
    std::optional<PreviewEnrichmentCandidate> candidate;
    std::optional<EnrichmentOutput> output;
    std::vector<CustomGainUpdate> gains;

    bool readMissing {false};
    bool retryingGains {false};

    std::uint64_t interruption {0};
    std::uint64_t loadGeneration {0};
};

/// Publishes artifacts only for the latest selected identity, generation and foreground load.
void deliver(
    PreviewEnrichmentState& state,
    const utils::FileIdentity& identity,
    const std::uint64_t loadGeneration,
    std::shared_ptr<const serato::SeratoData> tags,
    std::optional<juce::MemoryBlock> waveform,
    const bool tagsReady,
    std::shared_ptr<const SpectrogramData> spectrogram = {},
    const bool spectrogramReady = false
)
{
    std::uint64_t selectionGeneration = 0;
    {
        const std::scoped_lock lock(state.mutex);
        if (state.stopped || state.loadGeneration != loadGeneration || !state.selected
            || state.activeSelectionGeneration != state.selectionGeneration || state.selected->path != identity.path
            || state.selected->platformId != identity.platformId
            || state.selected->preciseTimes != identity.preciseTimes || state.selected->fileSize != identity.fileSize
            || state.selected->modifiedTimeMs != identity.modifiedTimeMs)
        {
            return;
        }
        selectionGeneration = state.selectionGeneration;
    }
    if (identity.matches(juce::File(identity.path)) && !state.stopped && state.completion) {
        state.completion({
            .identity = identity,
            .selectionGeneration = selectionGeneration,
            .loadGeneration = loadGeneration,
            .seratoData = std::move(tags),
            .waveform = std::move(waveform),
            .spectrogram = std::move(spectrogram),
            .tagsReady = tagsReady,
            .spectrogramReady = spectrogramReady,
        });
    }
}

/// Reads and publishes metadata before audio repair, retaining independent tag failure and cancellation behavior.
void enrichTags(
    PreviewEnrichmentState& state,
    const utils::FileIdentity& identity,
    const bool readMissing,
    const std::uint64_t loadGeneration,
    const utils::CancellationCheck& cancelled
)
{
    const auto cachedTags = serato::getCachedTags(state.cache, identity, serato::ALL_TAG_KINDS);
    const auto tagsCached = std::ranges::all_of(cachedTags, [](const auto& result) { return result.has_value(); });
    std::shared_ptr<const serato::SeratoData> tags;
    try {
        tags = serato::loadPreviewData(state.cache, identity, cancelled, readMissing);
    } catch (...) {
        // Metadata failure must not prevent independent audio-artifact repair.
        utils::logDebug("Preview metadata enrichment failed: {}", utils::describeCurrentException());
    }
    if (cancelled() || !identity.matches(juce::File(identity.path))) {
        return;
    }
    const auto finalTags = readMissing && !tagsCached
        ? serato::getCachedTags(state.cache, identity, serato::ALL_TAG_KINDS)
        : std::vector<std::optional<serato::TagReadResult>> {};
    const auto tagsReady = tagsCached
        || (readMissing && std::ranges::all_of(finalTags, [](const auto& result) { return result.has_value(); }));
    // Tag success is independently visible even if audio decoding is interrupted or fails.
    // A completed transient read resolves cue positioning too, but is retried only on a later request or load.
    if (tagsReady || tags || readMissing) {
        deliver(state, identity, loadGeneration, std::move(tags), std::nullopt, true);
    }
}

/// Loads each optional cache independently, retaining neither bulk artifact in the directory queue.
PreviewAudioArtifacts loadCachedArtifacts(AnalysisCache& cache, const juce::File& file)
{
    PreviewAudioArtifacts artifacts;
    try {
        artifacts.waveform = cache.getWaveformData(file);
    } catch (...) {
        utils::logDebug("Preview waveform cache read failed: {}", utils::describeCurrentException());
    }
    try {
        artifacts.spectrogram = cache.getSpectrogramData(file);
    } catch (...) {
        utils::logDebug("Preview spectrogram cache read failed: {}", utils::describeCurrentException());
    }
    return artifacts;
}

/// Decodes only missing audio artifacts and persists each independently before selected delivery.
void repairArtifacts(
    PreviewEnrichmentState& state,
    const utils::FileIdentity& identity,
    const bool needWaveform,
    const bool needSpectrogram,
    const std::uint64_t loadGeneration,
    const utils::CancellationCheck& cancelled
)
{
    const juce::File file(identity.path);
    {
        const std::scoped_lock lock(state.mutex);
        if (cancelled()) {
            return;
        }
        state.activity = "spectrogram";
        if (needWaveform) {
            state.activity = needSpectrogram ? "waveform and spectrogram" : "waveform";
        }
    }
    PreviewAudioArtifacts artifacts;
    try {
        artifacts = buildPreviewAudioArtifacts(identity, needWaveform, needSpectrogram, cancelled);
    } catch (...) {
        utils::logDebug("Preview audio artifact decoding failed: {}", utils::describeCurrentException());
    }
    if (cancelled() || !identity.matches(file)) {
        return;
    }
    if (artifacts.waveform) {
        try {
            static_cast<void>(state.cache.storeWaveformData(identity, *artifacts.waveform));
        } catch (...) {
            utils::logDebug("Preview waveform cache write failed: {}", utils::describeCurrentException());
        }
        if (!cancelled()) {
            deliver(state, identity, loadGeneration, {}, std::move(artifacts.waveform), false);
        }
    }
    if (artifacts.spectrogram && !cancelled() && identity.matches(file)) {
        try {
            static_cast<void>(state.cache.storeSpectrogramData(identity, *artifacts.spectrogram));
        } catch (...) {
            utils::logDebug("Preview spectrogram cache write failed: {}", utils::describeCurrentException());
        }
    }
    if (needSpectrogram && !cancelled() && identity.matches(file)) {
        deliver(state, identity, loadGeneration, {}, std::nullopt, false, std::move(artifacts.spectrogram), true);
    }
}

/// Reads tags early and publishes cached audio before one combined missing-artifact repair pass.
/// Source checks and cancellation surround every stage, without changing single-worker admission.
void enrich(
    PreviewEnrichmentState& state,
    const PreviewEnrichmentCandidate& candidate,
    const bool readMissing,
    const std::uint64_t loadGeneration,
    const std::uint64_t interruption
)
{
    const utils::FileIdentity identity(candidate.file);
    const auto cancelled = [&state, loadGeneration, interruption] {
        return state.stopped || state.loadGeneration != loadGeneration || state.interruption != interruption;
    };
    if (identity.fileSize != candidate.fileSize || identity.modifiedTimeMs != candidate.modifiedTimeMs
        || !identity.matches(candidate.file) || cancelled())
    {
        return;
    }
    enrichTags(state, identity, readMissing, loadGeneration, cancelled);
    if (cancelled() || !identity.matches(candidate.file)) {
        return;
    }
    auto cached = loadCachedArtifacts(state.cache, candidate.file);
    if (cancelled() || !identity.matches(candidate.file)) {
        return;
    }
    const auto needWaveform = !cached.waveform;
    const auto needSpectrogram = !cached.spectrogram;
    if (cached.waveform || cached.spectrogram) {
        const auto spectrogramReady = cached.spectrogram != nullptr;
        deliver(
            state,
            identity,
            loadGeneration,
            {},
            std::move(cached.waveform),
            false,
            std::move(cached.spectrogram),
            spectrogramReady
        );
    }
    if (readMissing && (needWaveform || needSpectrogram) && !cancelled()) {
        repairArtifacts(state, identity, needWaveform, needSpectrogram, loadGeneration, cancelled);
    }
}

/// Moves explicit edits first, retrying an unavailable source row only once after foreground completion.
void takeGains(PreviewEnrichmentState& state, EnrichmentJob& job)
{
    job.retryingGains = state.gains.empty() && state.outputs.empty() && state.retryDeferredGains;
    auto& source = job.retryingGains ? state.deferredGains : state.gains;
    if (job.retryingGains) {
        state.retryDeferredGains = false;
    }
    for (auto& [path, gain] : source) {
        juce::ignoreUnused(path);
        job.gains.push_back(std::move(gain));
    }
    source.clear();
}

/// Chooses the selected request ahead of directory repair while the queue mutex is held.
void takeCandidate(PreviewEnrichmentState& state, EnrichmentJob& job)
{
    if (state.selectionPending && state.selected) {
        const auto& identity = *state.selected;
        job.candidate = PreviewEnrichmentCandidate {
            .file = juce::File(identity.path),
            .fileSize = identity.fileSize,
            .modifiedTimeMs = identity.modifiedTimeMs,
        };
        state.selectionPending = false;
        job.readMissing = state.gateOpen;
        if (job.readMissing) {
            std::erase_if(state.pending, [&](const auto& item) { return item.file == job.candidate->file; });
        }
    } else if (state.gateOpen && !state.pending.empty()) {
        job.candidate = std::move(state.pending.front());
        state.pending.pop_front();
        job.readMissing = true;
    } else {
        state.selectionPending = false;
    }
}

/// Waits for one admissible job without holding the queue lock across filesystem, SQLite or decoding work.
std::optional<EnrichmentJob> nextJob(PreviewEnrichmentState& state)
{
    std::unique_lock lock(state.mutex);
    state.condition.wait(lock, [&] {
        return state.stopped || !state.gains.empty() || !state.outputs.empty() || state.retryDeferredGains
            || state.selectionPending || (state.gateOpen && !state.pending.empty());
    });
    if (state.stopped && state.gains.empty() && state.outputs.empty()) {
        return std::nullopt;
    }
    EnrichmentJob job;
    takeGains(state, job);
    if (job.gains.empty()) {
        if (!state.outputs.empty()) {
            auto& [output, convertedSource] = state.outputs.front();
            job.output = EnrichmentOutput {.output = std::move(output), .convertedSource = std::move(convertedSource)};
            state.outputs.pop_front();
        } else {
            takeCandidate(state, job);
        }
    }
    job.loadGeneration = state.loadGeneration;
    job.interruption = state.interruption;
    state.activeCandidate = job.candidate;
    state.activePath = job.candidate ? job.candidate->file.getFullPathName() : juce::String();
    state.activeInterruption = job.interruption;
    state.activeSelectionGeneration = state.selectionGeneration;
    return job;
}

/// Persists coalesced explicit edits, retaining only a bounded retry when the measured row is not ready yet.
void persistGains(PreviewEnrichmentState& state, const EnrichmentJob& job)
{
    for (const auto& gain : job.gains) {
        const auto stored = state.cache.storeCustomGains({gain}) > 0;
        const std::scoped_lock lock(state.mutex);
        if (!stored && !job.retryingGains && !state.gains.contains(gain.file.getFullPathName())) {
            state.deferredGains.insert_or_assign(gain.file.getFullPathName(), gain);
        }
    }
}

/// Dispatches one already admitted job without creating another worker or waiting for GUI delivery.
/// Injected work borrows the state, job and cancellation callback only until the synchronous call returns.
void executeJob(PreviewEnrichmentState& state, const EnrichmentJob& job)
{
    if (!job.gains.empty()) {
        persistGains(state, job);
    } else if (job.output) {
        // Cached no-ops own no fresh measurements and must not republish a superseded cache snapshot.
        if (!job.output->output.fromCache) {
            static_cast<void>(state.cache.storeAnalysis(job.output->output));
        }
        if (job.output->convertedSource) {
            static_cast<void>(state.cache.removeAnalysis(*job.output->convertedSource));
        }
    } else if (job.candidate) {
        if (state.work) {
            state.work(*job.candidate, job.readMissing, [&state, &job] {
                return state.stopped || state.loadGeneration != job.loadGeneration
                    || state.interruption != job.interruption;
            });
        } else {
            enrich(state, *job.candidate, job.readMissing, job.loadGeneration, job.interruption);
        }
    }
}

/// Atomically retires admission and requeues only interrupted work in the surviving load generation.
void retireJob(PreviewEnrichmentState& state, const EnrichmentJob& job, const bool succeeded)
{
    const std::scoped_lock lock(state.mutex);
    if (succeeded && job.candidate && job.readMissing && !state.stopped && state.loadGeneration == job.loadGeneration
        && state.interruption == job.interruption
        && state.candidatePaths.contains(job.candidate->file.getFullPathName()))
    {
        state.completedPaths.insert(job.candidate->file.getFullPathName());
    }
    if (job.candidate && !state.stopped && state.loadGeneration == job.loadGeneration
        && state.interruption != job.interruption && job.readMissing
        && std::ranges::none_of(state.pending, [&](const auto& item) { return item.file == job.candidate->file; }))
    {
        state.pending.push_back(*job.candidate);
    }
    if (job.candidate && !state.stopped && state.loadGeneration == job.loadGeneration
        && state.interruption != job.interruption && state.selected
        && state.selected->path == job.candidate->file.getFullPathName())
    {
        // Explicit gain edits can interrupt a cache-only selection too, which still needs its artifacts delivered.
        state.selectionPending = true;
    }
    // Retirement and requeue share the selection lock, so a late request cannot join an exited job.
    state.activePath.clear();
    state.activeCandidate.reset();
    state.activity.clear();
}

/// Runs one job at a time, giving coalesced explicit gains precedence over optional audio work.
/// Borrows state kept alive by the detached worker's owning capture until this loop exits.
void run(PreviewEnrichmentState& state)
{
    while (auto job = nextJob(state)) {
        bool succeeded = false;
        try {
            executeJob(state, *job);
            succeeded = true;
        } catch (...) {
            utils::logDebug("Preview enrichment failed: {}", utils::describeCurrentException());
        }
        retireJob(state, *job, succeeded);
    }
}
}  // namespace

/// Starts the sole worker with independently owned cache and callback captures.
/// Completion runs on that worker and must enqueue GUI delivery without waiting for the message thread.
/// Callback captures must own their dependencies or use safe references because shutdown does not join the worker.
/// Injected work runs synchronously and must not retain its borrowed candidate or cancellation callback.
PreviewEnrichmentScheduler::PreviewEnrichmentScheduler(
    const juce::File& databaseFile,
    Completion completion,
    Work work
) :
    state(std::make_shared<PreviewEnrichmentState>(databaseFile, std::move(completion), std::move(work)))
{
    std::thread([shared = state] { run(*shared); }).detach();
}

/// Requests cancellation without a message-thread join or worker-to-UI rendezvous.
PreviewEnrichmentScheduler::~PreviewEnrichmentScheduler()
{
    stop();
}

/// Closes admission before scans or file writers start, invalidating queued artifact deliveries.
void PreviewEnrichmentScheduler::beginForeground()
{
    const std::scoped_lock lock(state->mutex);
    state->gateOpen = false;
    state->selectionPending = false;
    ++state->loadGeneration;
    ++state->interruption;
    state->pending.clear();
    state->candidatePaths.clear();
    state->completedPaths.clear();
    state->activity.clear();
}

/// Opens admission only after applied results and restored selection, including all-cache-hit runs.
void PreviewEnrichmentScheduler::completeForeground(std::vector<PreviewEnrichmentCandidate> candidates)
{
    {
        const std::scoped_lock lock(state->mutex);
        state->pending.clear();
        state->candidatePaths.clear();
        state->completedPaths.clear();
        for (auto& candidate : candidates) {
            if (state->candidatePaths.insert(candidate.file.getFullPathName()).second) {
                state->pending.push_back(std::move(candidate));
            }
        }
        state->gateOpen = true;
        state->retryDeferredGains = true;
        state->selectionPending = state->selected.has_value();
    }
    state->condition.notify_one();
}

/// Promotes or joins the selected path and cooperatively interrupts unselected optional-artifact decoding.
void PreviewEnrichmentScheduler::select(const utils::FileIdentity& identity, const std::uint64_t selectionGeneration)
{
    {
        const std::scoped_lock lock(state->mutex);
        if (state->activePath.isNotEmpty() && state->activePath != identity.path) {
            ++state->interruption;
        }
        state->selected = identity;
        state->selectionGeneration = selectionGeneration;
        state->selectionPending = state->activePath != identity.path || state->activeInterruption != state->interruption
            || state->activeSelectionGeneration != selectionGeneration;
    }
    state->condition.notify_one();
}

/// Drops selected delivery ownership without interrupting useful directory repair.
void PreviewEnrichmentScheduler::clearSelection()
{
    const std::scoped_lock lock(state->mutex);
    state->selected.reset();
    state->selectionPending = false;
}

/// Invalidates active publication and drops removed candidates without waiting for a decoder.
void PreviewEnrichmentScheduler::remove(const juce::StringArray& paths)
{
    const std::scoped_lock lock(state->mutex);
    ++state->loadGeneration;
    ++state->interruption;
    std::erase_if(state->pending, [&](const auto& item) { return paths.contains(item.file.getFullPathName()); });
    for (const auto& path : paths) {
        state->candidatePaths.erase(path);
        state->completedPaths.erase(path);
    }
    if (state->gateOpen && state->activeCandidate && !paths.contains(state->activePath)
        && std::ranges::none_of(
            state->pending, [&](const auto& item) { return item.file == state->activeCandidate->file; }
        ))
    {
        // The global generation cancels every active job, including useful repair of paths that survived removal.
        state->pending.push_front(*state->activeCandidate);
    }
    if (state->selected && paths.contains(state->selected->path)) {
        state->selected.reset();
        state->selectionPending = false;
    } else {
        // Removal invalidates queued delivery for every path, so refresh the surviving selected artifacts.
        state->selectionPending = state->selected.has_value();
    }
    state->condition.notify_one();
}

/// Invalidates a cleared result set and closes automatic admission.
void PreviewEnrichmentScheduler::clear()
{
    beginForeground();
    clearSelection();
}

/// Invalidates all publication and drains queued explicit writes after the current cancellable stage exits.
/// Returns without joining, so callback captures remain worker-owned and must tolerate overlapping shutdown.
void PreviewEnrichmentScheduler::stop()
{
    state->stopped = true;
    ++state->interruption;
    state->condition.notify_one();
}

/// Coalesces only explicit edits by path and preserves their latest captured source identity.
void PreviewEnrichmentScheduler::storeCustomGains(std::vector<CustomGainUpdate> updates)
{
    {
        const std::scoped_lock lock(state->mutex);
        for (auto& update : updates) {
            state->deferredGains.erase(update.file.getFullPathName());
            state->gains.insert_or_assign(update.file.getFullPathName(), std::move(update));
        }
        ++state->interruption;
    }
    state->condition.notify_one();
}

/// Moves committed measurement-row persistence off the GUI thread, before optional repair can publish artifacts.
/// Callers persist transient audio artifacts before this handoff. This queue deliberately retains no spectral product.
void PreviewEnrichmentScheduler::persistOperation(
    AudioAnalysisRecord output,
    std::optional<AudioAnalysisRecord> convertedSource
)
{
    // Processing workers persist transient audio artifacts before events. The write queue retains only the row.
    output.spectrogram.reset();
    output.nativeWaveform.reset();
    if (convertedSource) {
        convertedSource->spectrogram.reset();
        convertedSource->nativeWaveform.reset();
    }
    {
        const std::scoped_lock lock(state->mutex);
        state->outputs.emplace_back(std::move(output), std::move(convertedSource));
    }
    state->condition.notify_one();
}

/// Copies file-level optional-preview progress under the queue mutex without filesystem or cache work.
/// Only directory candidates contribute counts. Cache-only requests before the gate never report activity.
PreviewEnrichmentProgress PreviewEnrichmentScheduler::getProgress() const
{
    const std::scoped_lock lock(state->mutex);
    const auto completed = static_cast<int>(state->completedPaths.size());
    const auto total = static_cast<int>(state->candidatePaths.size());
    const auto active = !state->stopped && state->gateOpen && total > completed
        && (state->activeCandidate.has_value() || !state->pending.empty());
    juce::String activity;
    if (active) {
        activity = state->activity.isNotEmpty() ? state->activity : juce::String("preview analysis");
    }
    return {
        .completedFiles = completed,
        .totalFiles = total,
        .active = active,
        .activity = std::move(activity),
    };
}

/// Rejects an artifact message queued before a newer foreground generation or removal.
bool PreviewEnrichmentScheduler::isCurrentLoad(const std::uint64_t generation) const
{
    return !state->stopped && state->loadGeneration == generation;
}
}  // namespace audiobatch
