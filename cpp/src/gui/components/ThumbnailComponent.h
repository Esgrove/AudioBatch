/// Waveform preview for the currently loaded audio file.
/// Declares ThumbnailComponent, a JUCE component that draws the audio thumbnail,
/// shows the playback position of the shared transport source and scrubs it,
/// overlays optional cached short-term loudness with an adjustable LUFS-S axis,
/// supports zoom and gain mouse-wheel gestures, Serato hot cue jumps with the number keys, file drag-and-drop,
/// and caching of the rendered waveform data.

#pragma once

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentState.h"
#include "serato/SeratoTagTypes.h"
#include "serato/SeratoWaveformColouring.h"

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
    /// The rendered waveform, which also records the pixel scale and renderer it was rendered for.
    serato::ColouredThumbnailImage rendered;
    /// Waveform bounds in component coordinates.
    juce::Rectangle<int> bounds;
    /// Visible time range in seconds.
    juce::Range<double> visibleRange;
    /// Linear display gain of the waveform.
    float displayGain {1.0f};
};

/// Waveform preview component with a playback cursor, scrubbing, zooming, and drag-and-drop.
class ThumbnailComponent :
    public juce::Component,
    public juce::ChangeListener,
    public juce::FileDragAndDropTarget,
    public juce::ChangeBroadcaster
{
public:
    ThumbnailComponent(juce::AudioFormatManager& formatManager, AudioPreviewPlayer& source);

    ~ThumbnailComponent() override;

    ThumbnailComponent(const ThumbnailComponent&) = delete;
    ThumbnailComponent& operator=(const ThumbnailComponent&) = delete;
    ThumbnailComponent(ThumbnailComponent&&) = delete;
    ThumbnailComponent& operator=(ThumbnailComponent&&) = delete;

    bool isInterestedInFileDrag(const juce::StringArray& /*files*/) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void changeListenerCallback(ChangeBroadcaster* source) override;
    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;
    void focusGained(FocusChangeType cause) override;
    void focusLost(FocusChangeType cause) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

    juce::MemoryBlock saveToCacheData() const;
    juce::StringArray getLastDroppedFiles() const;
    juce::URL getLastDroppedFile() const noexcept;
    [[nodiscard]] std::vector<CueArrows> getCueArrows() const;

    bool loadFromCacheData(const juce::MemoryBlock& waveformData);
    [[nodiscard]] bool isSeratoWaveformColouringEnabled() const noexcept;

    void setDisplayGain(float linearGain);
    void setLoudnessHistory(std::shared_ptr<const ShortTermLoudnessHistory> history);
    void setLoudnessGraphSettings(bool enabled, double minimum, double maximum);
    void setMouseWheelGainCallback(std::function<void(float)> callback);
    void setMouseWheelZoomCallback(std::function<void(double)> callback);
    void setRange(juce::Range<double> newRange);
    void setSeratoMarkers(std::shared_ptr<const serato::Markers> markers);
    void setSeratoOverview(std::shared_ptr<const serato::Overview> overview);
    void setSeratoWaveformColouringEnabled(bool enabled);
    void setThumbnailFullyLoadedCallback(std::function<void()> callback);
    void setURL(const juce::URL& url, juce::int64 lengthInSamples = 0);
    void setZoom(double zoomLevel);

private:
    bool drawColouredWaveform(juce::Graphics& graphics, juce::Rectangle<int> bounds);
    [[nodiscard]] double xToTime(float xPosition) const;
    [[nodiscard]] float timeToX(double time) const;
    [[nodiscard]] float loudnessToY(double value, juce::Rectangle<int> bounds) const;
    [[nodiscard]] juce::Rectangle<int> waveformBounds() const;

    void drawCueMarkers(juce::Graphics& graphics) const;
    void drawLoudnessGraph(juce::Graphics& graphics) const;
    void drawLoudnessAxis(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void drawLoudnessCurve(juce::Graphics& graphics, juce::Rectangle<int> bounds) const;
    void jumpToHotCue(int slot);
    void replaceThumbnail(int samplesPerPoint);
    void updatePlaybackPosition();
    void updateCursorPosition();

    AudioPreviewPlayer& transportSource;
    /// Reads the files the thumbnail is drawn from, owned by the main component, which outlives this one.
    juce::AudioFormatManager& thumbnailFormats;
    juce::AudioThumbnailCache thumbnailCache {128};
    /// Recreated for each file, because a thumbnail keeps the detail it was created or last loaded with.
    std::unique_ptr<juce::AudioThumbnail> thumbnail;
    juce::DrawableRectangle currentPositionMarker;
    juce::DrawableComponent currentPositionMarkerComponent {currentPositionMarker};
    juce::Range<double> visibleRange;
    juce::StringArray lastDroppedFiles;
    juce::URL lastFileDropped;
    juce::VBlankAttachment positionAttachment;

    std::function<void()> thumbnailFullyLoadedCallback;
    std::function<void(double)> mouseWheelZoomCallback;
    std::function<void(float)> mouseWheelGainCallback;
    std::shared_ptr<const serato::Markers> seratoMarkers;
    std::shared_ptr<const serato::Overview> seratoOverview;
    std::shared_ptr<const ShortTermLoudnessHistory> loudnessHistory;
    /// The coloured waveform of the last paint, cleared when the waveform, the overview, or the colouring changes.
    std::optional<ColouredWaveformCache> colouredWaveform;

    bool hasNotifiedFullyLoaded {false};
    bool loudnessGraphEnabled {false};
    bool seratoWaveformColouringEnabled {true};
    double loudnessMaximum {AudioPreviewPreferences::defaultLoudnessGraphMaximum};
    double loudnessMinimum {AudioPreviewPreferences::defaultLoudnessGraphMinimum};
    float displayGain {1.0f};
};
}  // namespace audiobatch
