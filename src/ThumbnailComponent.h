#pragma once

#include <JuceHeader.h>

#include <functional>

/// Waveform preview component with transport following, scrubbing, zooming, and drag-and-drop.
class ThumbnailComponent :
    public juce::Component,
    public juce::ChangeListener,
    public juce::FileDragAndDropTarget,
    public juce::ChangeBroadcaster,
    private juce::Timer
{
public:
    /// Creates a thumbnail view backed by the shared transport source.
    ThumbnailComponent(juce::AudioFormatManager& formatManager, juce::AudioTransportSource& source);

    /// Stops timer activity and detaches listeners.
    ~ThumbnailComponent() override;

    bool isInterestedInFileDrag(const juce::StringArray& /*files*/) override;

    /// Returns the most recently dropped file as a URL for convenience.
    juce::URL getLastDroppedFile() const noexcept;

    /// Returns the full list of files dropped onto the waveform component.
    juce::StringArray getLastDroppedFiles() const;

    void changeListenerCallback(ChangeBroadcaster*) override;
    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override;
    void paint(juce::Graphics& g) override;
    void resized() override;

    /// Restores a previously cached waveform without re-reading the audio file.
    bool loadFromCacheData(const juce::MemoryBlock& waveformData);

    /// Serializes the current waveform so it can be stored in the cache.
    juce::MemoryBlock saveToCacheData() const;

    /// Sets the visible time range for the waveform viewport.
    void setRange(juce::Range<double> newRange);

    /// Sets the callback used for mouse-wheel zoom gestures.
    void setMouseWheelZoomCallback(std::function<void(double)> callback);

    /// Sets the callback fired once a full waveform has been loaded from disk.
    void setThumbnailFullyLoadedCallback(std::function<void()> callback);

    /// Loads a new audio resource into the thumbnail display.
    void setURL(const juce::URL& url);

    /// Applies a zoom factor to the waveform viewport.
    void setZoom(double zoomLevel);

private:
    bool canMoveTransport() const noexcept;
    double xToTime(const float x) const;
    float timeToX(const double time) const;
    void timerCallback() override;
    void updateCursorPosition();

    juce::AudioThumbnail thumbnail;
    juce::AudioThumbnailCache thumbnailCache {128};
    juce::AudioTransportSource& transportSource;
    juce::DrawableRectangle currentPositionMarker;
    juce::Range<double> visibleRange;
    juce::URL lastFileDropped;
    juce::StringArray lastDroppedFiles;
    std::function<void(double)> mouseWheelZoomCallback;
    std::function<void()> thumbnailFullyLoadedCallback;

    bool hasNotifiedFullyLoaded = false;
    bool isFollowingTransport = false;
};
