/// Waveform preview for the currently loaded audio file.
/// Declares ThumbnailComponent, a JUCE component that draws the audio thumbnail,
/// shows the playback position of the shared transport source and scrubs it,
/// overlays optional cached short-term loudness with an adjustable LUFS-S axis,
/// marks source sample and true peak overloads in amber,
/// supports zoom and gain mouse-wheel gestures, Serato hot cue jumps with the number keys, file drag-and-drop,
/// and caching of the rendered waveform data.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentState.h"
#include "serato/SeratoTagTypes.h"
#include "serato/SeratoWaveformColouring.h"
#include "utils/FileUtils.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch
{
class AudioPreviewPlayer;

/// Placement of the two arrows that mark one Serato cue on the waveform, in component coordinates.
/// One arrow hangs from the top edge of the waveform and one rises from its bottom edge, both pointing inwards.
struct CueArrows {
    // Preserve the geometry aggregate contract used by rendering and cue-position tests.
    /// Opaque fill colour of the cue.
    juce::Colour colour;
    /// Horizontal position of the cue, where the tips of both arrows lie.
    float horizontalPosition {0.0f};
    /// Half the width of each arrow base along the waveform edge.
    float halfWidth {0.0f};
    /// Distance from the waveform edge to the arrow tip.
    float height {0.0f};
    /// Top edge of the waveform, where the base of the upper arrow lies.
    float top {0.0f};
    /// Bottom edge of the waveform, where the base of the lower arrow lies.
    float bottom {0.0f};
};

/// The coloured waveform last painted, with the geometry and gain it was rendered for.
/// The waveform paints it again without rendering while none of them, the waveform, or the overview changed.
struct ColouredWaveformCache {
    // Preserve designated construction order alongside the cached render's geometry and gain.
    /// The rendered waveform, which also records the pixel scale and renderer it was rendered for.
    serato::ColouredThumbnailImage rendered;
    /// Waveform bounds in component coordinates.
    juce::Rectangle<int> bounds;
    /// Visible time range in seconds.
    juce::Range<double> visibleRange;
    /// Linear display gain of the waveform.
    float displayGain {1.0f};
};

/// One horizontal run of an overload mark in a channel lane, in component coordinates.
struct OverloadMark {
    // Geometry follows the horizontal run and its lane bounds, matching aggregate rendering requests.
    /// Horizontal extent of the run, at least the minimum mark width.
    juce::Range<float> horizontal;
    /// Top of the channel lane the run marks.
    float laneTop {0.0f};
    /// Bottom of the channel lane the run marks.
    float laneBottom {0.0f};
    /// True for a sample over, drawn solid, and false for a true peak over alone, drawn as an outline.
    bool sampleOver {false};
};

/// Waveform preview component with a playback cursor, scrubbing, zooming, and drag-and-drop.
class ThumbnailComponent :
    public juce::Component,
    public juce::ChangeListener,
    public juce::FileDragAndDropTarget,
    public juce::ChangeBroadcaster,
    public juce::TooltipClient
{
public:
    ThumbnailComponent(juce::AudioFormatManager& formatManager, AudioPreviewPlayer& source);

    ~ThumbnailComponent() override;

    ThumbnailComponent(const ThumbnailComponent&) = delete;
    ThumbnailComponent& operator=(const ThumbnailComponent&) = delete;
    ThumbnailComponent(ThumbnailComponent&&) = delete;
    ThumbnailComponent& operator=(ThumbnailComponent&&) = delete;

    juce::URL getLastDroppedFile() const noexcept;
    juce::StringArray getLastDroppedFiles() const;
    juce::MemoryBlock saveToCacheData() const;

    [[nodiscard]] std::vector<CueArrows> getCueArrows() const;
    [[nodiscard]] std::vector<OverloadMark> getOverloadMarks() const;

    [[nodiscard]] const std::optional<utils::FileIdentity>& getSourceIdentity() const noexcept;

    [[nodiscard]] bool adoptWaveformData(const juce::MemoryBlock& waveformData, const utils::FileIdentity& identity);
    [[nodiscard]] bool isSeratoWaveformColouringEnabled() const noexcept;
    bool loadFromCacheData(
        const juce::MemoryBlock& waveformData,
        std::optional<utils::FileIdentity> identity = std::nullopt
    );

    void setDisplayGain(float linearGain);
    void setLoudnessGraphSettings(bool enabled, double minimum, double maximum);
    void setLoudnessHistory(std::shared_ptr<const ShortTermLoudnessHistory> history);
    void setMouseWheelGainCallback(std::function<void(float)> callback);
    void setMouseWheelZoomCallback(std::function<void(double)> callback);
    void setOverloadHistory(std::shared_ptr<const OverloadHistory> history);
    void setPendingSource(juce::int64 lengthInSamples, double durationSeconds, const utils::FileIdentity& identity);
    void setRange(juce::Range<double> newRange);

    // Serato overlays and source replacement.
    void setSeratoMarkers(std::shared_ptr<const serato::Markers> markers);
    void setSeratoOverview(std::shared_ptr<const serato::Overview> overview);
    void setSeratoWaveformColouringEnabled(bool enabled);
    void setThumbnailFullyLoadedCallback(std::function<void()> callback);
    void setURL(
        const juce::URL& url,
        juce::int64 lengthInSamples = 0,
        std::optional<utils::FileIdentity> identity = std::nullopt
    );
    void setZoom(double zoomLevel);

    // juce::Component
    bool keyPressed(const juce::KeyPress& key) override;

    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

    // juce::ChangeListener
    void changeListenerCallback(ChangeBroadcaster* source) override;

    // juce::FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& /*files*/) override;

    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;

    // juce::TooltipClient
    juce::String getTooltip() override;

private:
    [[nodiscard]] juce::Rectangle<int> waveformBounds() const;

    bool drawColouredWaveform(juce::Graphics& graphics, juce::Rectangle<int> bounds);
    [[nodiscard]] double xToTime(float xPosition) const;
    [[nodiscard]] float loudnessToY(double value, juce::Rectangle<int> bounds) const;
    [[nodiscard]] float timeToX(double time) const;

    void drawCueMarkers(juce::Graphics& graphics) const;
    void drawLoudnessAxis(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void drawLoudnessCurve(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void drawLoudnessGraph(juce::Graphics& graphics) const;
    void drawOverloads(juce::Graphics& graphics) const;

    // Source and playback interaction.
    void jumpToHotCue(int slot);
    void replaceThumbnail(int samplesPerPoint);
    void updateCursorPosition();
    void updatePlaybackPosition();

    /// Preview player, which outlives this component.
    AudioPreviewPlayer& transportSource;
    /// Borrowed formats, outliving the thumbnail and this component.
    juce::AudioFormatManager& thumbnailFormats;
    /// Must outlive the thumbnails referring into it.
    juce::AudioThumbnailCache thumbnailCache {128};

    /// Cursor geometry must precede the child that copies it during initialization.
    juce::DrawableRectangle currentPositionMarker;
    juce::DrawableComponent currentPositionMarkerComponent {currentPositionMarker};
    /// Visible file time range in seconds.
    juce::Range<double> visibleRange;
    /// Files last dropped onto the waveform.
    juce::StringArray lastDroppedFiles;
    /// First file last dropped onto the waveform.
    juce::URL lastFileDropped;

    /// Called once after reading the complete waveform.
    std::function<void()> thumbnailFullyLoadedCallback;
    /// Mouse-wheel zoom factor.
    std::function<void(double)> mouseWheelZoomCallback;
    /// Mouse-wheel gain change in decibels.
    std::function<void(float)> mouseWheelGainCallback;

    /// Last rendered coloured waveform.
    std::optional<ColouredWaveformCache> colouredWaveform;
    /// Source identity captured before decoding, never resampled during publication.
    std::optional<utils::FileIdentity> sourceIdentity;

    /// Analysis histories and Serato tags, null until available.
    std::shared_ptr<const ShortTermLoudnessHistory> loudnessHistory;
    std::shared_ptr<const OverloadHistory> overloadHistory;
    std::shared_ptr<const serato::Markers> seratoMarkers;
    std::shared_ptr<const serato::Overview> seratoOverview;

    /// Recreated for each file, after the cache and before its destruction.
    std::unique_ptr<juce::AudioThumbnail> thumbnail;

    /// Whether the current file's completion callback ran.
    bool hasNotifiedFullyLoaded {false};
    bool loudnessGraphEnabled {false};
    bool seratoWaveformColouringEnabled {true};
    /// Loudness axis limits in LUFS.
    double loudnessMaximum {AudioPreviewPreferences::defaultLoudnessGraphMaximum};
    double loudnessMinimum {AudioPreviewPreferences::defaultLoudnessGraphMinimum};
    /// Linear display gain reflecting the custom gain.
    float displayGain {1.0f};

    /// Kept last so display callbacks detach before any observed state is destroyed.
    juce::VBlankAttachment positionAttachment;
};
}  // namespace audiobatch
