/// Waveform preview for the currently loaded audio file.
/// Declares ThumbnailComponent, a JUCE component that draws the audio thumbnail,
/// follows and scrubs the shared transport source,
/// supports zoom and gain mouse-wheel gestures, Serato hot cue jumps with the number keys, file drag-and-drop,
/// and caching of the rendered waveform data.

#pragma once

#include "SeratoTagTypes.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>

/// Waveform preview component with transport following, scrubbing, zooming, and drag-and-drop.
class ThumbnailComponent :
    public juce::Component,
    public juce::ChangeListener,
    public juce::FileDragAndDropTarget,
    public juce::ChangeBroadcaster,
    juce::Timer
{
public:
    ThumbnailComponent(juce::AudioFormatManager& formatManager, juce::AudioTransportSource& source);

    ~ThumbnailComponent() override;

    ThumbnailComponent(const ThumbnailComponent&) = delete;
    ThumbnailComponent& operator=(const ThumbnailComponent&) = delete;
    ThumbnailComponent(ThumbnailComponent&&) = delete;
    ThumbnailComponent& operator=(ThumbnailComponent&&) = delete;

    bool isInterestedInFileDrag(const juce::StringArray& /*files*/) override;

    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;

    void changeListenerCallback(ChangeBroadcaster* source) override;

    void mouseDown(const juce::MouseEvent& event) override;

    void mouseDrag(const juce::MouseEvent& event) override;

    void mouseUp(const juce::MouseEvent& event) override;

    void mouseDoubleClick(const juce::MouseEvent& event) override;

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

    bool keyPressed(const juce::KeyPress& key) override;

    void focusGained(FocusChangeType cause) override;

    void focusLost(FocusChangeType cause) override;

    void paint(juce::Graphics& graphics) override;

    void resized() override;

    juce::URL getLastDroppedFile() const noexcept;

    juce::StringArray getLastDroppedFiles() const;

    juce::MemoryBlock saveToCacheData() const;

    bool loadFromCacheData(const juce::MemoryBlock& waveformData);

    void setRange(juce::Range<double> newRange);

    void setMouseWheelZoomCallback(std::function<void(double)> callback);

    void setMouseWheelGainCallback(std::function<void(float)> callback);

    void setThumbnailFullyLoadedCallback(std::function<void()> callback);

    void setURL(const juce::URL& url);

    void setZoom(double zoomLevel);

    void setDisplayGain(float linearGain);

    void setSeratoOverview(std::shared_ptr<const audiobatch::serato::Overview> overview);

    void setSeratoMarkers(std::shared_ptr<const audiobatch::serato::Markers> markers);

private:
    [[nodiscard]] bool canMoveTransport() const noexcept;

    [[nodiscard]] double xToTime(float xPosition) const;

    [[nodiscard]] float timeToX(double time) const;

    [[nodiscard]] juce::Rectangle<int> waveformBounds() const;

    void jumpToHotCue(int slot);

    void drawCueMarkers(juce::Graphics& graphics) const;

    void timerCallback() override;

    void updateCursorPosition();

    juce::AudioThumbnail thumbnail;
    juce::AudioThumbnailCache thumbnailCache {128};
    juce::AudioTransportSource& transportSource;
    juce::DrawableRectangle currentPositionMarker;
    juce::DrawableComponent currentPositionMarkerComponent {currentPositionMarker};
    juce::Range<double> visibleRange;
    juce::StringArray lastDroppedFiles;
    juce::URL lastFileDropped;
    std::function<void()> thumbnailFullyLoadedCallback;
    std::function<void(double)> mouseWheelZoomCallback;
    std::function<void(float)> mouseWheelGainCallback;
    std::shared_ptr<const audiobatch::serato::Overview> seratoOverview;
    std::shared_ptr<const audiobatch::serato::Markers> seratoMarkers;

    bool hasNotifiedFullyLoaded = false;
    bool isFollowingTransport = false;
    float displayGain = 1.0f;
};
