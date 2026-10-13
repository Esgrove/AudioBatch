/// Implements the single-worker, latest-pending preview loader.
/// File/cache work stays off the message thread, and stale results are rejected before storage and delivery.

#include "serato/SeratoPreviewLoader.h"

#include "utils/Log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace audiobatch::serato
{
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
/// Invalid duration or file identity cancels prior work without scheduling a read.
/// Called by the owner thread, never from completion.
SeratoPreviewLoader::Generation SeratoPreviewLoader::request(const juce::File& file, const double durationSeconds)
{
    const FileIdentity identity(file);
    Generation nextGeneration = 0;
    {
        const std::scoped_lock lock(mutex);
        nextGeneration = ++generation;
        pending.reset();
        if (!stopped && std::isfinite(durationSeconds) && durationSeconds > 0.0 && identity.matches(file)) {
            pending = SeratoPreviewRequest {.file = file, .identity = identity, .generation = nextGeneration};
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

/// Checks the generation, selected path and current file stats again at queued delivery time.
/// The GUI additionally checks its URL, reader and thumbnail before applying the returned data.
bool SeratoPreviewLoader::canAdopt(
    const FileIdentity& identity,
    const Generation resultGeneration,
    const juce::File& selectedFile
) const
{
    return !stopped && generation == resultGeneration && identity.matches(selectedFile);
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
void SeratoPreviewLoader::process(const SeratoPreviewRequest& active, const std::stop_token& stopToken)
{
    const utils::CancellationCheck isCancelled = [this, &active, stopToken] {
        return stopToken.stop_requested() || stopped || generation != active.generation;
    };
    if (isCancelled() || !active.identity.matches(active.file)) {
        return;
    }

    Data data;
    try {
        Results results;
        std::vector<TagKind> misses;
        for (std::size_t index = 0; index < allTagKinds.size(); ++index) {
            if (isCancelled()) {
                return;
            }
            results[index] = cache.getSeratoTag(active.identity, allTagKinds[index]);
            if (!results[index]) {
                misses.push_back(allTagKinds[index]);
            }
        }
        if (!readMisses(active, misses, results, isCancelled)) {
            return;
        }
        if (isCancelled() || !active.identity.matches(active.file)) {
            return;
        }
        data = collectData(results);
    } catch (const std::exception& exception) {
        utils::logDebug("Serato preview read failed: {}", exception.what());
    }

    if (!isCancelled() && active.identity.matches(active.file) && completion) {
        completion(active.identity, active.generation, std::move(data));
    }
}

/// Reads missing kinds in one batch and persists only stable results while the request remains current.
bool SeratoPreviewLoader::readMisses(
    const SeratoPreviewRequest& active,
    const std::span<const TagKind> misses,
    Results& results,
    const utils::CancellationCheck& isCancelled
) const
{
    if (misses.empty()) {
        return true;
    }
    if (isCancelled() || !active.identity.matches(active.file)) {
        return false;
    }
    auto fresh = reader(active.file, misses, isCancelled);
    if (isCancelled() || !active.identity.matches(active.file)
        || std::ranges::any_of(fresh, [](const auto& result) { return result.status == ReadStatus::cancelled; }))
    {
        return false;
    }
    for (auto& result : fresh) {
        if (std::ranges::find(misses, result.kind) == misses.end()) {
            continue;
        }
        if (isCancelled()) {
            return false;
        }
        // Avoid naming the iterator type, it is a pointer with libc++ but a class with MSVC.
        const auto index = static_cast<std::size_t>(
            std::ranges::distance(allTagKinds.begin(), std::ranges::find(allTagKinds, result.kind))
        );
        if (results[index]) {
            continue;
        }
        if (result.status != ReadStatus::ioError) {
            cache.storeSeratoTag(active.identity, result);
        }
        results[index] = std::move(result);
    }
    return true;
}

/// Moves independently present values into an immutable aggregate, leaving negative kinds unset.
SeratoPreviewLoader::Data SeratoPreviewLoader::collectData(Results& results)
{
    SeratoData loaded;
    for (auto& result : results) {
        if (!result || result->status != ReadStatus::present) {
            continue;
        }
        switch (result->kind) {
            case TagKind::overview:
                loaded.overview = std::move(result->overview);
                break;
            case TagKind::analysis:
                loaded.analysis = result->analysis;
                break;
            case TagKind::autotags:
                loaded.autotags = result->autotags;
                break;
            case TagKind::beatGrid:
                loaded.beatGrid = std::move(result->beatGrid);
                break;
            case TagKind::markers:
                loaded.markers = std::move(result->markers);
                break;
            case TagKind::trackInfo:
                loaded.trackInfo = std::move(result->trackInfo);
                break;
        }
    }
    if (loaded.overview || loaded.analysis || loaded.autotags || loaded.beatGrid || loaded.markers || loaded.trackInfo)
    {
        return std::make_shared<const SeratoData>(std::move(loaded));
    }
    return {};
}
}  // namespace audiobatch::serato