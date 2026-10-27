/// Bounded asynchronous loading of independent Serato tags for the GUI preview.
/// SeratoPreviewLoader owns its worker and immutable inputs, but never accesses a GUI component.
/// The owner must outlive the loader's cache reference and validate queued completions before adoption.
/// Identity-rejected current requests complete with null data. Cancelled generations do not complete.

#pragma once

#include "analysis/AnalysisCache.h"
#include "serato/SeratoTagReader.h"
#include "utils/FileUtils.h"
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
/// How reading the tag kinds missing from the cache ended.
/// Cancellation stays silent, while a rejected identity still completes the request with null data,
/// so the preview waiting for it is resolved.
enum class ReadOutcome : std::uint8_t {
    /// Every missing kind was read, or none were missing.
    completed,
    /// The request was cancelled or replaced by a newer one.
    cancelled,
    /// The file changed on disk, so the read results no longer describe the requested file.
    identityRejected,
};

/// Owns the immutable pre-read file identity for a single preview generation.
struct SeratoPreviewRequest {
    /// The file to read the tags of.
    juce::File file;
    /// Size and modification time of the file when it was requested, which the cache entries must match.
    utils::FileIdentity identity;
    /// Number of the request, so a result for an older selection is recognised and dropped.
    std::uint64_t generation {0};
};

/// Loads the active selection with at most one active read and one replaceable pending request.
class SeratoPreviewLoader final
{
public:
    /// Receives the requested identity and generation, with null data for missing metadata or identity rejection.
    /// Before resolving an identity-rejected preview, the owner must check the generation and selected path.
    using Completion = std::function<void(utils::FileIdentity, std::uint64_t, std::shared_ptr<const SeratoData>)>;
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

    [[nodiscard]] bool isCurrentGeneration(std::uint64_t resultGeneration) const;
    [[nodiscard]] bool canAdopt(
        const utils::FileIdentity& identity,
        std::uint64_t resultGeneration,
        const juce::File& selectedFile
    ) const;

private:
    using Results = std::array<std::optional<TagReadResult>, allTagKinds.size()>;

    static std::shared_ptr<const SeratoData> collectData(Results& results);

    void run(const std::stop_token& stopToken);
    void process(const SeratoPreviewRequest& active, const std::stop_token& stopToken) const;

    ReadOutcome readMisses(
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
    std::mutex mutex;
    std::optional<SeratoPreviewRequest> pending;

    /// Starts only after every queue member it accesses has been initialized.
    std::jthread worker;
};
}  // namespace audiobatch::serato
