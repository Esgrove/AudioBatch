/// Bounded asynchronous loading of independent Serato tags for the GUI preview.
/// SeratoPreviewLoader owns its worker and immutable inputs, but never accesses a GUI component.
/// The owner must outlive the loader's cache reference and validate queued completions before adoption.

#pragma once

#include "analysis/AnalysisCache.h"
#include "serato/SeratoTagReader.h"
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
/// Owns the immutable pre-read file identity for a single preview generation.
struct SeratoPreviewRequest {
    juce::File file;
    FileIdentity identity;
    std::uint64_t generation;
};

/// Loads the active selection with at most one active read and one replaceable pending request.
class SeratoPreviewLoader final
{
public:
    using Completion = std::function<void(FileIdentity, std::uint64_t, std::shared_ptr<const SeratoData>)>;
    using Reader = std::function<
        std::vector<TagReadResult>(const juce::File&, std::span<const TagKind>, const utils::CancellationCheck&)>;

    SeratoPreviewLoader(AnalysisCache& cacheToUse, Completion onCompletion, Reader tagReader = readTags);

    ~SeratoPreviewLoader();

    SeratoPreviewLoader(const SeratoPreviewLoader&) = delete;
    SeratoPreviewLoader& operator=(const SeratoPreviewLoader&) = delete;
    SeratoPreviewLoader(SeratoPreviewLoader&&) = delete;
    SeratoPreviewLoader& operator=(SeratoPreviewLoader&&) = delete;

    std::uint64_t request(const juce::File& file, double durationSeconds);

    void cancel();
    void cancelAndWait();

    [[nodiscard]] bool canAdopt(
        const FileIdentity& identity,
        std::uint64_t resultGeneration,
        const juce::File& selectedFile
    ) const;

private:
    using Results = std::array<std::optional<TagReadResult>, allTagKinds.size()>;

    static std::shared_ptr<const SeratoData> collectData(Results& results);

    void run(const std::stop_token& stopToken);
    void process(const SeratoPreviewRequest& active, const std::stop_token& stopToken) const;

    bool readMisses(
        const SeratoPreviewRequest& active,
        std::span<const TagKind> misses,
        Results& results,
        const utils::CancellationCheck& isCancelled
    ) const;

    AnalysisCache& cache;
    Completion completion;
    Reader reader;

    std::atomic<std::uint64_t> generation {0};
    std::atomic<bool> stopped {false};
    std::condition_variable_any condition;
    std::jthread worker;
    std::mutex mutex;
    std::optional<SeratoPreviewRequest> pending;
};
}  // namespace audiobatch::serato
