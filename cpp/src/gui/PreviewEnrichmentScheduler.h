/// One GUI-owned admission path for native waveforms, Serato metadata and ordered cache edits.
/// Selected and directory requests share one expensive worker, with a post-measurement completion gate.
/// Shared worker state owns its cache connection and can finish cancellation without joining the GUI thread.

#pragma once

#include "analysis/AnalysisCache.h"
#include "serato/SeratoTagTypes.h"
#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch
{
struct PreviewEnrichmentState;

/// A lightweight candidate. Audio histories and waveform blobs never live in the pending queue.
struct PreviewEnrichmentCandidate {
    juce::File file;
    std::int64_t fileSize {0};
    std::int64_t modifiedTimeMs {0};
};

/// Independent artifact delivery for the currently selected source.
struct PreviewEnrichmentResult {
    utils::FileIdentity identity;
    std::uint64_t selectionGeneration {0};
    std::uint64_t loadGeneration {0};
    std::shared_ptr<const serato::SeratoData> seratoData;
    std::optional<juce::MemoryBlock> waveform;
    bool tagsReady {false};
};

/// Owns admission and cancellation, not the lifetime of the detached worker's database connection.
class PreviewEnrichmentScheduler final
{
public:
    using Completion = std::function<void(PreviewEnrichmentResult)>;
    /// Optional deterministic worker injection for admission tests, never used by the application.
    using Work = std::function<void(const PreviewEnrichmentCandidate&, bool, const utils::CancellationCheck&)>;

    PreviewEnrichmentScheduler(const juce::File& databaseFile, Completion completion, Work work = {});
    ~PreviewEnrichmentScheduler();
    PreviewEnrichmentScheduler(const PreviewEnrichmentScheduler&) = delete;
    PreviewEnrichmentScheduler& operator=(const PreviewEnrichmentScheduler&) = delete;
    PreviewEnrichmentScheduler(PreviewEnrichmentScheduler&&) = delete;
    PreviewEnrichmentScheduler& operator=(PreviewEnrichmentScheduler&&) = delete;

    void beginForeground();
    void completeForeground(std::vector<PreviewEnrichmentCandidate> candidates);
    void select(const utils::FileIdentity& identity, std::uint64_t selectionGeneration);
    void clearSelection();
    void remove(const juce::StringArray& paths);
    void clear();
    void stop();
    void storeCustomGains(std::vector<CustomGainUpdate> updates);
    void persistOperation(
        AudioAnalysisRecord output,
        std::optional<AudioAnalysisRecord> convertedSource = std::nullopt
    );
    [[nodiscard]] bool isCurrentLoad(std::uint64_t generation) const;

private:
    std::shared_ptr<PreviewEnrichmentState> state;
};
}  // namespace audiobatch
