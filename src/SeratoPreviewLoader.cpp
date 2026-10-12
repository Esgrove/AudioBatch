/// Implements the single-worker, latest-pending preview loader.
/// File/cache work stays off the message thread, and stale results are rejected before storage and delivery.

#include "SeratoPreviewLoader.h"

#include "Log.h"

#include <cmath>
#include <exception>
#include <utility>

namespace audiobatch::serato
{
SeratoPreviewLoader::SeratoPreviewLoader(AnalysisCache& cacheToUse, Completion onCompletion, Reader tagReader) :
    cache(cacheToUse),
    completion(std::move(onCompletion)),
    reader(std::move(tagReader)),
    worker([this](const std::stop_token& stopToken) { run(stopToken); })
{ }

SeratoPreviewLoader::~SeratoPreviewLoader()
{
    cancelAndWait();
}

SeratoPreviewLoader::Generation SeratoPreviewLoader::request(const juce::File& file, const double durationSeconds)
{
    const AnalysisCache::FileIdentity identity(file);
    Generation nextGeneration = 0;
    {
        const std::scoped_lock lock(mutex);
        nextGeneration = ++generation;
        pending.reset();
        if (!stopped && std::isfinite(durationSeconds) && durationSeconds > 0.0 && identity.matches(file)) {
            pending = Request {.file = file, .identity = identity, .generation = nextGeneration};
        }
    }
    condition.notify_one();
    return nextGeneration;
}

void SeratoPreviewLoader::cancel()
{
    {
        const std::scoped_lock lock(mutex);
        ++generation;
        pending.reset();
    }
    condition.notify_one();
}

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

bool SeratoPreviewLoader::canAdopt(
    const AnalysisCache::FileIdentity& identity,
    const Generation resultGeneration,
    const juce::File& selectedFile
) const
{
    return !stopped && generation == resultGeneration && identity.matches(selectedFile) && !stopped
        && generation == resultGeneration;
}

void SeratoPreviewLoader::run(const std::stop_token& stopToken)
{
    while (!stopToken.stop_requested()) {
        std::optional<Request> active;
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

void SeratoPreviewLoader::process(const Request& active, const std::stop_token& stopToken)
{
    const utils::CancellationCheck isCancelled = [this, &active, stopToken] {
        return stopToken.stop_requested() || stopped || generation != active.generation;
    };
    if (isCancelled() || !active.identity.matches(active.file) || isCancelled()) {
        return;
    }

    Bands bands;
    try {
        auto result = cache.getSeratoTag(active.identity);
        if (isCancelled()) {
            return;
        }
        if (!result) {
            result = reader(active.file, TagKind::overview, isCancelled);
            if (isCancelled() || !active.identity.matches(active.file) || isCancelled()
                || result->status == ReadStatus::cancelled)
            {
                return;
            }
            if (result->status != ReadStatus::ioError) {
                cache.storeSeratoTag(active.identity, *result);
            }
        }
        if (isCancelled() || !active.identity.matches(active.file) || isCancelled()) {
            return;
        }
        if (result->status == ReadStatus::present && result->overview) {
            bands = std::make_shared<const Overview>(std::move(*result->overview));
        }
    } catch (const std::exception& exception) {
        utils::logDebug("Serato preview read failed: {}", exception.what());
    }

    if (!isCancelled() && active.identity.matches(active.file) && !isCancelled() && completion) {
        completion(active.identity, active.generation, std::move(bands));
    }
}
}  // namespace audiobatch::serato
