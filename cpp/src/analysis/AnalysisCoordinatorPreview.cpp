/// Bounded progressive waveform publication from the foreground analysis decoder.
/// Selection and run checks surround snapshot construction, with only the latest unread snapshot retained.
/// No preview decoder, delivery callback, persistence, or additional worker is involved.

#include "analysis/AnalysisCoordinator.h"
#include "analysis/AnalysisPreview.h"
#include "analysis/AudioMeasurement.h"
#include "utils/FileUtils.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace audiobatch
{
namespace
{
/// Minimum interval between snapshots for one selection in one analysis run.
constexpr std::chrono::milliseconds PREVIEW_INTERVAL {250};

/// Compares captured identities without filesystem access while the selection mutex is held.
/// Missing native identity never authorizes publication, even if coarse file metadata agrees.
bool samePreviewIdentity(const utils::FileIdentity& first, const utils::FileIdentity& second)
{
    return first.platformId && first.path == second.path && first.platformId == second.platformId
        && first.preciseTimes == second.preciseTimes && first.fileSize == second.fileSize
        && first.modifiedTimeMs == second.modifiedTimeMs;
}
}  // namespace

/// Captures a selection identity, or clears selection without filesystem access for an empty file.
/// Callers that already have an identity should use that overload to avoid duplicate filesystem work.
void AnalysisCoordinator::setPreviewFile(const juce::File& file, const std::uint64_t selectionGeneration)
{
    if (file == juce::File {}) {
        const std::scoped_lock lock(previewMutex);
        previewIdentity.reset();
        latestPreview.reset();
        lastPreviewPublish.reset();
        previewSelectionGeneration = selectionGeneration;
        previewRunId = -1;
        return;
    }
    setPreviewFile(utils::FileIdentity(file), selectionGeneration);
}

/// Changes selection using an already captured identity, without accessing the filesystem.
/// Same-path reselection also clears pending data and immediately admits the next decoded block.
void AnalysisCoordinator::setPreviewFile(const utils::FileIdentity& identity, const std::uint64_t selectionGeneration)
{
    const std::scoped_lock lock(previewMutex);
    previewIdentity = identity;
    latestPreview.reset();
    lastPreviewPublish.reset();
    previewSelectionGeneration = selectionGeneration;
    previewRunId = -1;
}

/// Moves the single unread snapshot to its consumer, dropping cancelled or superseded data.
/// Completed runs remain current, so their last unread preview can still be consumed.
std::optional<AnalysisPreview> AnalysisCoordinator::takePreview()
{
    const std::scoped_lock lock(previewMutex);
    auto preview = std::exchange(latestPreview, std::nullopt);
    if (!preview || !runner.isCurrent(preview->runId) || !previewIdentity
        || preview->selectionGeneration != previewSelectionGeneration
        || !samePreviewIdentity(preview->identity, *previewIdentity))
    {
        return std::nullopt;
    }
    return preview;
}

/// Admits at most one snapshot per interval for the selected source, checked after every decoded block.
/// Serialization happens on the decoder worker without the mutex, then selection and run are checked again.
/// The runner joins all jobs before the coordinator's directly owned preview state is destroyed.
void AnalysisCoordinator::publishPreview(
    const utils::FileIdentity& identity,
    const int runId,
    const AudioMeasurement& measurement,
    const std::int64_t decodedFrames
) const
{
    std::uint64_t selectionGeneration = 0;
    {
        const std::scoped_lock lock(previewMutex);
        if (!runner.isCurrent(runId) || !previewIdentity || decodedFrames <= 0
            || !samePreviewIdentity(identity, *previewIdentity))
        {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (previewRunId == runId && lastPreviewPublish && now - *lastPreviewPublish < PREVIEW_INTERVAL) {
            return;
        }
        previewRunId = runId;
        lastPreviewPublish = now;
        selectionGeneration = previewSelectionGeneration;
    }

    auto waveform = measurement.waveformSnapshot();
    if (!waveform) {
        return;
    }

    const std::scoped_lock lock(previewMutex);
    if (!runner.isCurrent(runId) || !previewIdentity || selectionGeneration != previewSelectionGeneration
        || previewRunId != runId || !samePreviewIdentity(identity, *previewIdentity))
    {
        return;
    }
    latestPreview = AnalysisPreview {
        .identity = identity,
        .runId = runId,
        .selectionGeneration = selectionGeneration,
        .decodedFrames = decodedFrames,
        .waveform = std::move(waveform),
    };
}
}  // namespace audiobatch
