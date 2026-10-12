/// Bounded asynchronous Serato Overview loading for the GUI preview.
/// SeratoPreviewLoader owns its worker and immutable inputs, but never accesses a GUI component.
/// The owner must outlive the loader's cache reference and validate queued completions before adoption.

#pragma once

#include "AnalysisCache.h"
#include "SeratoTagReader.h"
#include <condition_variable>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace audiobatch::serato
{
/// Loads the active selection with at most one active read and one replaceable pending request.
class SeratoPreviewLoader final
{
public:
    using Generation = std::uint64_t;
    using Bands = std::shared_ptr<const Overview>;
    using Completion = std::function<void(AnalysisCache::FileIdentity, Generation, Bands)>;
    using Reader = std::function<TagReadResult(const juce::File&, TagKind, const utils::CancellationCheck&)>;

    /// Starts one worker. Completion runs on that worker and must only enqueue delivery, never wait for the UI.
    /// The cache and callback captures must remain alive until cancelAndWait returns.
    /// An injected reader allows deterministic lifecycle tests without changing production cache behavior.
    SeratoPreviewLoader(AnalysisCache& cacheToUse, Completion onCompletion, Reader tagReader = readTag);

    /// Invalidates queued deliveries and joins the worker before releasing callback captures.
    ~SeratoPreviewLoader();

    SeratoPreviewLoader(const SeratoPreviewLoader&) = delete;
    SeratoPreviewLoader& operator=(const SeratoPreviewLoader&) = delete;
    SeratoPreviewLoader(SeratoPreviewLoader&&) = delete;
    SeratoPreviewLoader& operator=(SeratoPreviewLoader&&) = delete;

    /// Replaces pending work and invalidates earlier reads and deliveries, including selection A to B to A.
    /// Invalid duration or file identity cancels prior work without scheduling a read.
    /// Called by the owner thread, never from completion.
    Generation request(const juce::File& file, double durationSeconds);

    /// Invalidates active and queued completions and drops pending work without waiting for TagLib.
    /// Called by the owner thread when the preview is cleared or replaced.
    void cancel();

    /// Stops permanently and joins without waiting for any posted message-thread callback.
    /// Called by the owner thread before destroying the cache or GUI resources. Safe to call repeatedly.
    void cancelAndWait();

    /// Checks the generation, selected path and current file stats again at queued delivery time.
    /// The GUI additionally checks its URL, reader and thumbnail before applying the returned bands.
    [[nodiscard]] bool canAdopt(
        const AnalysisCache::FileIdentity& identity,
        Generation resultGeneration,
        const juce::File& selectedFile
    ) const;

private:
    /// Owns the immutable pre-read file identity for a single preview generation.
    struct Request {
        juce::File file;
        AnalysisCache::FileIdentity identity;
        Generation generation;
    };

    /// Waits interruptibly for the latest request and processes it outside the queue lock.
    void run(const std::stop_token& stopToken);

    /// Performs cache lookup, reading and stable persistence, checking cancellation at each boundary.
    void process(const Request& active, const std::stop_token& stopToken);

    AnalysisCache& cache;
    Completion completion;
    Reader reader;
    std::atomic<Generation> generation {0};
    std::atomic<bool> stopped {false};
    std::mutex mutex;
    std::condition_variable_any condition;
    std::optional<Request> pending;
    std::jthread worker;
};
}  // namespace audiobatch::serato
