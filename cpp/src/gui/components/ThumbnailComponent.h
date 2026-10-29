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

/// One horizontal run of an overload mark in a channel lane, in component coordinates.
struct OverloadMark {
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

    juce::String getTooltip() override;

    juce::MemoryBlock saveToCacheData() const;
    juce::StringArray getLastDroppedFiles() const;
    juce::URL getLastDroppedFile() const noexcept;
    [[nodiscard]] std::vector<CueArrows> getCueArrows() const;
    [[nodiscard]] std::vector<OverloadMark> getOverloadMarks() const;

    bool loadFromCacheData(const juce::MemoryBlock& waveformData);
    [[nodiscard]] bool isSeratoWaveformColouringEnabled() const noexcept;

    void setDisplayGain(float linearGain);
    void setLoudnessHistory(std::shared_ptr<const ShortTermLoudnessHistory> history);
    void setLoudnessGraphSettings(bool enabled, double minimum, double maximum);
    void setOverloadHistory(std::shared_ptr<const OverloadHistory> history);
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
    void drawOverloads(juce::Graphics& graphics) const;
    void jumpToHotCue(int slot);
    void replaceThumbnail(int samplesPerPoint);
    void updatePlaybackPosition();
    void updateCursorPosition();

    /// The preview player whose position the cursor shows and the mouse scrubs, which outlives this component.
    AudioPreviewPlayer& transportSource;
    /// Reads the files the thumbnail is drawn from, owned by the main component, which outlives this one.
    juce::AudioFormatManager& thumbnailFormats;
    /// Keeps the waveforms of the recently shown files in memory.
    juce::AudioThumbnailCache thumbnailCache {128};
    /// Recreated for each file, because a thumbnail keeps the detail it was created or last loaded with.
    std::unique_ptr<juce::AudioThumbnail> thumbnail;
    /// The playback cursor drawn over the waveform.
    juce::DrawableRectangle currentPositionMarker;
    /// The child component that shows the cursor, so moving it repaints only the region under it.
    juce::DrawableComponent currentPositionMarkerComponent {currentPositionMarker};
    /// The time range of the file the waveform shows, in seconds.
    juce::Range<double> visibleRange;
    /// The files last dropped onto the waveform.
    juce::StringArray lastDroppedFiles;
    /// The first file last dropped onto the waveform.
    juce::URL lastFileDropped;
    /// Moves the cursor and updates the playback status on every display frame.
    juce::VBlankAttachment positionAttachment;

    /// Called once the waveform of a file was read completely, so the owner can cache it.
    std::function<void()> thumbnailFullyLoadedCallback;
    /// Called with the zoom factor of a mouse wheel zoom.
    std::function<void(double)> mouseWheelZoomCallback;
    /// Called with the gain change in decibels of a mouse wheel gain gesture.
    std::function<void(float)> mouseWheelGainCallback;
    /// The Serato cues of the file, or null without them.
    std::shared_ptr<const serato::Markers> seratoMarkers;
    /// The Serato overview of the file, or null without one.
    std::shared_ptr<const serato::Overview> seratoOverview;
    /// The short-term loudness of the file, or null until its analysis has it.
    std::shared_ptr<const ShortTermLoudnessHistory> loudnessHistory;
    /// The source overloads of the file, or null until its analysis has them.
    std::shared_ptr<const OverloadHistory> overloadHistory;
    /// The coloured waveform of the last paint, cleared when the waveform, the overview, or the colouring changes.
    std::optional<ColouredWaveformCache> colouredWaveform;

    /// True once the fully loaded callback ran for the current file.
    bool hasNotifiedFullyLoaded {false};
    /// True to draw the loudness axis and curve.
    bool loudnessGraphEnabled {false};
    /// True to colour the waveform with the Serato overview when the file has one.
    bool seratoWaveformColouringEnabled {true};
    /// Top of the loudness axis in LUFS.
    double loudnessMaximum {AudioPreviewPreferences::defaultLoudnessGraphMaximum};
    /// Bottom of the loudness axis in LUFS.
    double loudnessMinimum {AudioPreviewPreferences::defaultLoudnessGraphMinimum};
    /// Linear gain the waveform is drawn with, which shows the custom gain of the file.
    float displayGain {1.0f};
};
}  // namespace audiobatch
