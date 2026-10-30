/// Implementation of PreviewController:
/// the transitions between an empty, released, and loaded preview,
/// and the requests and adoption of the Serato tags of the loaded file.

#include "gui/PreviewController.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioPreviewPlayer.h"
#include "serato/SeratoPreviewLoader.h"
#include "serato/SeratoTagTypes.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace audiobatch
{
/// Creates the Serato tag worker, which reads through the cache only once a tag request is made.
/// The completion runs on the worker thread, so it must move the result to the message thread itself.
PreviewController::PreviewController(
    AnalysisCache& cache,
    AudioPreviewPlayer& previewPlayer,
    serato::SeratoPreviewLoader::Completion onSeratoData
) :
    player(previewPlayer),
    seratoLoader(cache, std::move(onSeratoData))
{}

/// Returns the current preview state.
const PreviewState& PreviewController::getState() const noexcept
{
    return state;
}

/// Returns the file selected for the preview, loaded or released, or an empty file when there is none.
juce::File PreviewController::getFile() const
{
    if (const auto* loaded = std::get_if<LoadedPreview>(&state)) {
        return loaded->file;
    }
    if (const auto* released = std::get_if<ReleasedPreview>(&state)) {
        return released->file;
    }
    return {};
}

/// Returns the adopted Serato tags of the loaded file, or null while none are adopted.
std::shared_ptr<const serato::SeratoData> PreviewController::getSeratoData() const
{
    if (const auto* loaded = std::get_if<LoadedPreview>(&state)) {
        return loaded->seratoData;
    }
    return nullptr;
}

/// Reports whether the loaded file has changed on disk since its tags were requested,
/// so a reanalysis of the same path should read them again.
bool PreviewController::isSeratoDataOutdated(const juce::File& file) const
{
    const auto* loaded = std::get_if<LoadedPreview>(&state);
    return loaded != nullptr && loaded->file == file && loaded->seratoIdentity.has_value()
        && !loaded->seratoIdentity->matches(file);
}

/// Reports whether the waveform of the loaded file came from the analysis cache.
bool PreviewController::isWaveformLoadedFromCache() const noexcept
{
    const auto* loaded = std::get_if<LoadedPreview>(&state);
    return loaded != nullptr && loaded->waveformLoadedFromCache;
}

/// Accepts source histories only for complete analysis of the unchanged file backing the loaded waveform.
/// A released preview, another selection, or a replaced file must never inherit source warning marks.
/// Gain and plugin-preview preferences deliberately play no part in this identity check.
bool PreviewController::canDisplayAnalysis(const AudioAnalysisRecord& record) const
{
    const auto* loaded = std::get_if<LoadedPreview>(&state);
    return loaded != nullptr && player.hasLoadedSource() && record.isReady() && record.file == loaded->file
        && record.fullPath == loaded->identity.path && record.fileSize == loaded->identity.fileSize
        && record.modifiedTimeMs == loaded->identity.modifiedTimeMs && loaded->identity.matches(loaded->file);
}

/// Adopts the Serato tags read for the loaded file while the request generation and the file still match.
/// A stale result is ignored.
/// A result for a file that changed on disk is rejected and resolves the pending start position at the beginning.
/// Returns true when the tags were adopted, after which the caller resolves the start position and shows them.
bool PreviewController::adoptSeratoData(
    const utils::FileIdentity& identity,
    const std::uint64_t generation,
    std::shared_ptr<const serato::SeratoData> data
)
{
    auto* loaded = std::get_if<LoadedPreview>(&state);
    if (loaded == nullptr || !player.hasLoadedSource() || identity.path != loaded->file.getFullPathName()
        || !seratoLoader.isCurrentGeneration(generation))
    {
        return false;
    }

    if (!seratoLoader.canAdopt(identity, generation, loaded->file)) {
        player.completeInitialPosition(0.0);
        return false;
    }

    loaded->seratoIdentity = identity;
    loaded->seratoData = std::move(data);
    return true;
}

/// Loads a file into the preview player, cancelling the tag read of the previous file.
/// On failure the player is released and the file stays selected,
/// so a later operation on it can load it again.
/// Returns true when the file was loaded.
bool PreviewController::load(const juce::File& file, const bool playFromFirstCuePoint)
{
    seratoLoader.cancel();
    // The identity is taken before loading, so a change during the load makes the analysis look outdated.
    const utils::FileIdentity identity(file);

    if (!player.load(juce::URL(file), playFromFirstCuePoint)) {
        state = ReleasedPreview {.file = file};
        release();
        return false;
    }

    state = LoadedPreview {
        .file = file,
        .identity = identity,
        .waveformLoadedFromCache = false,
        .seratoData = nullptr,
        .seratoIdentity = std::nullopt,
    };
    return true;
}

/// Releases the preview and forgets its file.
void PreviewController::clear()
{
    release();
    state = NoPreview {};
}

/// Stops playback and closes the preview file, keeping it selected,
/// so the file on disk can be replaced or the preview loaded again.
void PreviewController::release()
{
    seratoLoader.cancel();
    if (const auto* loaded = std::get_if<LoadedPreview>(&state)) {
        state = ReleasedPreview {.file = loaded->file};
    }
    player.release();
}

/// Forgets the tags of the loaded file and requests them again for its current version.
/// Without a loaded file of known length, the pending start position resolves at the beginning instead.
void PreviewController::requestSeratoData()
{
    auto* loaded = std::get_if<LoadedPreview>(&state);
    if (loaded != nullptr) {
        loaded->seratoData.reset();
        loaded->seratoIdentity.reset();
    }

    const auto duration = player.getLoadedDurationSeconds();
    if (loaded == nullptr || !duration.has_value() || !std::isfinite(*duration) || *duration <= 0.0) {
        seratoLoader.cancel();
        player.completeInitialPosition(0.0);
        return;
    }

    loaded->seratoIdentity.emplace(loaded->file);
    if (seratoLoader.request(loaded->file, *duration) == 0) {
        player.completeInitialPosition(0.0);
    }
}

/// Records whether the waveform of the loaded file came from the analysis cache.
void PreviewController::setWaveformLoadedFromCache(const bool loadedFromCache)
{
    if (auto* loaded = std::get_if<LoadedPreview>(&state)) {
        loaded->waveformLoadedFromCache = loadedFromCache;
    }
}

/// Cancels the tag read and waits for the worker to finish, before the cache it reads from is destroyed.
void PreviewController::stop()
{
    seratoLoader.cancelAndWait();
}
}  // namespace audiobatch
