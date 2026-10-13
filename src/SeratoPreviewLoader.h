/// Bounded asynchronous loading of independent Serato tags for the GUI preview.
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
    using Data = std::shared_ptr<const SeratoData>;
    using Completion = std::function<void(AnalysisCache::FileIdentity, Generation, Data)>;
    using Reader = std::function<
        std::vector<TagReadResult>(const juce::File&, std::span<const TagKind>, const utils::CancellationCheck&)>;

    /// Starts one worker. Completion runs on that worker and must only enqueue delivery, never wait for the UI.
    /// The cache and callback captures must remain alive until cancelAndWait returns.
    /// An injected reader allows deterministic lifecycle tests without changing production cache behavior.
    SeratoPreviewLoader(AnalysisCache& cacheToUse, Completion onCompletion, Reader tagReader = readTags);

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
    /// The GUI additionally checks its URL, reader and thumbnail before applying the returned data.
    [[nodiscard]] bool canAdopt(
        const AnalysisCache::FileIdentity& identity,
        Generation resultGeneration,
        const juce::File& selectedFile
    ) const;

private:
    using Results = std::array<std::optional<TagReadResult>, allTagKinds.size()>;

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

    /// Reads missing kinds in one batch and persists only stable results while the request remains current.
    bool readMisses(
        const Request& active,
        std::span<const TagKind> misses,
        Results& results,
        const utils::CancellationCheck& isCancelled
    ) const;

    /// Moves independently present values into an immutable aggregate, leaving negative kinds unset.
    static Data collectData(Results& results);

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
