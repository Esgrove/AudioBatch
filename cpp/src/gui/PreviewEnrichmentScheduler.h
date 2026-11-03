/// One GUI-owned admission path for native waveforms, spectrograms, Serato metadata and ordered cache edits.
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

/// A lightweight candidate. Audio histories, waveform blobs and spectra never live in the pending queue.
struct PreviewEnrichmentCandidate {
    juce::File file;
    std::int64_t fileSize {0};
    std::int64_t modifiedTimeMs {0};
};

/// Pollable file-level progress for the current directory's optional preview pass.
/// Cached and negative results finish a file too. This is not sample-level measurement progress.
struct PreviewEnrichmentProgress {
    /// Unique directory candidates retired without cancellation in the current pass.
    int completedFiles {0};
    /// Unique directory candidates accepted when foreground results settled, excluding removed paths.
    int totalFiles {0};
    /// True while the completion gate admits unfinished directory preview work.
    bool active {false};
    /// Current optional audio repair stage, or a general preview-analysis label between stages.
    juce::String activity;
};

/// Independent artifact delivery for the currently selected source.
struct PreviewEnrichmentResult {
    // Keep delivery's designated aggregate contract in identity/generation/artifact order.
    /// Captured source version that must still match at adoption.
    utils::FileIdentity identity;
    /// Selection request that owns this independent delivery.
    std::uint64_t selectionGeneration {0};
    /// Foreground load that owns this independent delivery.
    std::uint64_t loadGeneration {0};
    /// Parsed tags, independently absent from audio artifact deliveries.
    std::shared_ptr<const serato::SeratoData> seratoData;
    /// Native thumbnail bytes, independently absent from other artifact deliveries.
    std::optional<juce::MemoryBlock> waveform;
    /// Immutable spectral data shared with the selected preview.
    std::shared_ptr<const SpectrogramData> spectrogram;
    /// True once tag loading completed, even when no usable tags were found.
    bool tagsReady {false};
    /// True once spectral repair completed, even when unsupported or unavailable.
    bool spectrogramReady {false};
};

/// Owns admission and cancellation, not the lifetime of the detached worker's database connection.
class PreviewEnrichmentScheduler final
{
public:
    /// Receives artifacts on the worker thread and must enqueue GUI delivery without waiting for the message thread.
    /// Captures must own their dependencies or refer safely to owners because delivery can overlap scheduler shutdown.
    using Completion = std::function<void(PreviewEnrichmentResult)>;
    /// Optional synchronous worker injection for admission tests, never used by the application.
    /// The candidate and cancellation callback are borrowed only for the invocation and must not be retained.
    using Work = std::function<void(const PreviewEnrichmentCandidate&, bool, const utils::CancellationCheck&)>;

    PreviewEnrichmentScheduler(const juce::File& databaseFile, Completion completion, Work work = {});

    ~PreviewEnrichmentScheduler();

    PreviewEnrichmentScheduler(const PreviewEnrichmentScheduler&) = delete;
    PreviewEnrichmentScheduler& operator=(const PreviewEnrichmentScheduler&) = delete;
    PreviewEnrichmentScheduler(PreviewEnrichmentScheduler&&) = delete;
    PreviewEnrichmentScheduler& operator=(PreviewEnrichmentScheduler&&) = delete;

    [[nodiscard]] PreviewEnrichmentProgress getProgress() const;
    [[nodiscard]] bool isCurrentLoad(std::uint64_t generation) const;

    void beginForeground();
    void clear();
    void clearSelection();
    void completeForeground(std::vector<PreviewEnrichmentCandidate> candidates);
    void persistOperation(
        AudioAnalysisRecord output,
        std::optional<AudioAnalysisRecord> convertedSource = std::nullopt
    );
    void remove(const juce::StringArray& paths);
    void select(const utils::FileIdentity& identity, std::uint64_t selectionGeneration);
    void stop();
    void storeCustomGains(std::vector<CustomGainUpdate> updates);

private:
    std::shared_ptr<PreviewEnrichmentState> state;
};
}  // namespace audiobatch
