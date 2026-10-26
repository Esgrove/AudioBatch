/// State of the audio preview in the main window.
/// Declares PreviewController, which loads and releases the previewed file in the preview player,
/// and reads its Serato tags in the background.
/// The preview is either empty, released with its file kept for reloading, or loaded with its file identity and tags.

#pragma once

#include "analysis/AnalysisCache.h"
#include "gui/AudioPreviewPlayer.h"
#include "serato/SeratoPreviewLoader.h"

#include <JuceHeader.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace audiobatch
{
/// No file is previewed.
struct NoPreview {};

/// A file is selected for the preview, but the player holds no source for it,
/// because the file is being rewritten or could not be loaded.
struct ReleasedPreview {
    juce::File file;
};

/// A file is loaded into the preview player.
struct LoadedPreview {
    juce::File file;

    /// The version of the file that was loaded, compared with analysis results of the same path.
    FileIdentity identity;

    /// True when the waveform came from the analysis cache, so it is not stored again.
    bool waveformLoadedFromCache {false};

    /// The Serato tags of the file, kept through analysis refreshes until the selection changes.
    std::shared_ptr<const serato::SeratoData> seratoData;

    /// The version of the file whose tags were requested or adopted, including files without tags.
    std::optional<FileIdentity> seratoIdentity;
};

using PreviewState = std::variant<NoPreview, ReleasedPreview, LoadedPreview>;

/// Loads the previewed file into the preview player and tracks which file and tags the preview shows.
/// Only used on the message thread.
/// The Serato tags are read on a worker, whose results the owner passes back to adoptSeratoData.
class PreviewController
{
public:
    PreviewController(
        AnalysisCache& cache,
        AudioPreviewPlayer& previewPlayer,
        serato::SeratoPreviewLoader::Completion onSeratoData
    );

    ~PreviewController() = default;

    PreviewController(const PreviewController&) = delete;
    PreviewController& operator=(const PreviewController&) = delete;
    PreviewController(PreviewController&&) = delete;
    PreviewController& operator=(PreviewController&&) = delete;

    [[nodiscard]] const PreviewState& getState() const noexcept;
    [[nodiscard]] juce::File getFile() const;
    [[nodiscard]] std::shared_ptr<const serato::SeratoData> getSeratoData() const;
    [[nodiscard]] bool isSeratoDataOutdated(const juce::File& file) const;
    [[nodiscard]] bool isWaveformLoadedFromCache() const noexcept;

    [[nodiscard]] bool adoptSeratoData(
        const FileIdentity& identity,
        std::uint64_t generation,
        std::shared_ptr<const serato::SeratoData> data
    );
    [[nodiscard]] bool load(const juce::File& file, bool playFromFirstCuePoint);

    void clear();
    void release();
    void requestSeratoData();
    void setWaveformLoadedFromCache(bool loadedFromCache);
    void stop();

private:
    AudioPreviewPlayer& player;
    serato::SeratoPreviewLoader seratoLoader;
    PreviewState state;
};
}  // namespace audiobatch
