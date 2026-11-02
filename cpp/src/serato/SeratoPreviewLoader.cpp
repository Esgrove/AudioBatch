/// Implements the single-worker, latest-pending preview loader.
/// File/cache work stays off the message thread, and stale metadata is rejected before storage and delivery.
/// Identity rejection completes the current request with null data instead of leaving the preview waiting.

#include "serato/SeratoPreviewLoader.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysisTypes.h"
#include "serato/SeratoOverview.h"
#include "serato/SeratoTagCache.h"
#include "serato/SeratoTagTypes.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/ThreadUtils.h"
#include <stop_token>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch::serato
{
namespace
{
/// Reads only missing kinds and merges independent stable results while the captured source remains unchanged.
/// Transient I/O remains uncached and cancellation discards the current read without manufacturing absence.
bool loadMissingTags(
    AnalysisCache& cache,
    const utils::FileIdentity& identity,
    std::vector<std::optional<TagReadResult>>& results,
    const utils::CancellationCheck& isCancelled,
    const SeratoPreviewLoader::Reader& reader
)
{
    std::vector<TagKind> misses;
    for (std::size_t index = 0; index < allTagKinds.size(); ++index) {
        if (!results[index]) {
            misses.push_back(allTagKinds[index]);
        }
    }
    if (misses.empty()) {
        return true;
    }
    const juce::File file(identity.path);
    if (utils::isCancellationRequested(isCancelled) || !identity.matches(file)) {
        return false;
    }
    auto fresh = reader(file, misses, isCancelled);
    if (utils::isCancellationRequested(isCancelled) || !identity.matches(file)
        || std::ranges::any_of(fresh, [](const auto& result) { return result.status() == ReadStatus::cancelled; }))
    {
        return false;
    }
    for (auto& result : fresh) {
        const auto index = static_cast<std::size_t>(
            std::ranges::distance(allTagKinds.begin(), std::ranges::find(allTagKinds, result.kind))
        );
        if (index == allTagKinds.size() || std::ranges::find(misses, result.kind) == misses.end()) {
            continue;
        }
        if (results[index]) {
            continue;
        }
        if (utils::isCancellationRequested(isCancelled) || !identity.matches(file)) {
            return false;
        }
        if (result.status() != ReadStatus::ioError) {
            static_cast<void>(storeCachedTag(cache, identity, result));
        }
        results[index] = std::move(result);
    }
    return true;
}
}  // namespace

/// Starts one worker. Completion runs on that worker and must only enqueue delivery, never wait for the UI.
/// The cache and callback captures must remain alive until cancelAndWait returns.
/// An injected reader allows deterministic lifecycle tests without changing production cache behavior.
SeratoPreviewLoader::SeratoPreviewLoader(AnalysisCache& cacheToUse, Completion onCompletion, Reader tagReader) :
    cache(cacheToUse),
    completion(std::move(onCompletion)),
    reader(std::move(tagReader)),
    worker([this](const std::stop_token& stopToken) { run(stopToken); })
{}

/// Invalidates queued deliveries and joins the worker before releasing callback captures.
SeratoPreviewLoader::~SeratoPreviewLoader()
{
    cancelAndWait();
}

/// Replaces pending work and invalidates earlier reads and deliveries, including selection A to B to A.
/// Invalid duration or file identity cancels prior work without scheduling a read and returns zero.
/// Called by the owner thread, never from completion.
std::uint64_t SeratoPreviewLoader::request(const juce::File& file, const double durationSeconds)
{
    const utils::FileIdentity identity(file);
    std::uint64_t nextGeneration = 0;
    {
        const std::scoped_lock lock(mutex);
        nextGeneration = ++generation;
        pending.reset();
        if (!stopped && std::isfinite(durationSeconds) && durationSeconds > 0.0 && identity.matches(file)) {
            pending = SeratoPreviewRequest {.file = file, .identity = identity, .generation = nextGeneration};
        } else {
            nextGeneration = 0;
        }
    }
    condition.notify_one();
    return nextGeneration;
}

/// Invalidates active and queued completions and drops pending work without waiting for TagLib.
/// Called by the owner thread when the preview is cleared or replaced.
void SeratoPreviewLoader::cancel()
{
    {
        const std::scoped_lock lock(mutex);
        ++generation;
        pending.reset();
    }
    condition.notify_one();
}

/// Stops permanently and joins without waiting for any posted message-thread callback.
/// Called by the owner thread before destroying the cache or GUI resources. Safe to call repeatedly.
void SeratoPreviewLoader::cancelAndWait()
{
    stopped = true;
    cancel();
    worker.request_stop();
    condition.notify_one();
    if (worker.joinable()) {
        worker.join();
    }
}

/// Checks whether a completion still belongs to the active generation without requiring a valid file identity.
/// The owner must also check the selected path before resolving a pending preview after identity rejection.
bool SeratoPreviewLoader::isCurrentGeneration(const std::uint64_t resultGeneration) const
{
    return !stopped && generation == resultGeneration;
}

/// Checks the generation, selected path and current file stats again at queued delivery time.
/// A false result forbids adopting metadata, but does not imply the generation has been cancelled.
/// The GUI additionally checks its URL, reader and thumbnail before applying the returned data.
bool SeratoPreviewLoader::canAdopt(
    const utils::FileIdentity& identity,
    const std::uint64_t resultGeneration,
    const juce::File& selectedFile
) const
{
    return isCurrentGeneration(resultGeneration) && identity.matches(selectedFile);
}

/// Waits interruptibly for the latest request and processes it outside the queue lock.
void SeratoPreviewLoader::run(const std::stop_token& stopToken)
{
    while (!stopToken.stop_requested()) {
        std::optional<SeratoPreviewRequest> active;
        {
            std::unique_lock lock(mutex);
            if (!condition.wait(lock, stopToken, [this] { return pending.has_value(); })) {
                return;
            }
            active = std::move(pending);
            pending.reset();
        }
        if (active && !stopToken.stop_requested()) {
            process(*active, stopToken);
        }
    }
}

/// Performs cache lookup, reading and stable persistence, checking cancellation at each boundary.
/// The cached kinds are looked up together, so the file is checked against its identity once for all of them.
/// Identity rejection before reading, after reading or at delivery completes with null data if still current.
/// Generation cancellation remains silent.
void SeratoPreviewLoader::process(const SeratoPreviewRequest& active, const std::stop_token& stopToken) const
{
    const utils::CancellationCheck isCancelled
        = [this, &active, stopToken] { return stopToken.stop_requested() || !isCurrentGeneration(active.generation); };
    if (isCancelled()) {
        return;
    }

    std::shared_ptr<const SeratoData> data;
    try {
        data = loadPreviewData(cache, active.identity, isCancelled, true, reader);
    } catch (...) {
        // The worker is a std::jthread, where an escaping exception would terminate the process.
        utils::logDebug("Serato preview read failed: {}", utils::describeCurrentException());
    }

    if (!active.identity.matches(active.file)) {
        data.reset();
    }
    if (!isCancelled() && completion) {
        completion(active.identity, active.generation, std::move(data));
    }
}

/// Synchronously loads independent tags on the caller's admitted worker, without starting another thread.
/// Cache-only requests never invoke TagLib. Stable negatives are complete, while transient I/O remains uncached.
/// Identity is checked before reading, after reading, before each store, and before returning owned artifacts.
std::shared_ptr<const SeratoData> loadPreviewData(
    AnalysisCache& cache,
    const utils::FileIdentity& identity,
    const utils::CancellationCheck& isCancelled,
    const bool readMissing,
    const SeratoPreviewLoader::Reader& reader
)
{
    const juce::File file(identity.path);
    if (utils::isCancellationRequested(isCancelled) || !identity.matches(file)) {
        return {};
    }
    auto results = getCachedTags(cache, identity, allTagKinds);
    if (readMissing && !loadMissingTags(cache, identity, results, isCancelled, reader)) {
        return {};
    }
    if (utils::isCancellationRequested(isCancelled) || !identity.matches(file)) {
        return {};
    }
    SeratoData loaded;
    for (auto& result : results) {
        if (!result || !result->value) {
            continue;
        }
        std::visit(
            Overloaded {
                [&loaded](Overview& overview) { loaded.overview = std::move(overview); },
                [&loaded](const Analysis& analysis) { loaded.analysis = analysis; },
                [&loaded](const Autotags& autotags) { loaded.autotags = autotags; },
                [&loaded](BeatGrid& grid) { loaded.beatGrid = std::move(grid); },
                [&loaded](Markers& markers) { loaded.markers = std::move(markers); },
                [&loaded](TrackInfo& info) { loaded.trackInfo = std::move(info); },
            },
            *result->value
        );
    }
    if (loaded.overview || loaded.analysis || loaded.autotags || loaded.beatGrid || loaded.markers || loaded.trackInfo)
    {
        return std::make_shared<const SeratoData>(std::move(loaded));
    }
    return {};
}
}  // namespace audiobatch::serato
