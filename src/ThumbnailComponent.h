#pragma once

#include <JuceHeader.h>

#include <functional>

/// Waveform preview component with transport following, scrubbing, zooming, and drag-and-drop.
class ThumbnailComponent :
    public juce::Component,
    public juce::ChangeListener,
    public juce::FileDragAndDropTarget,
    public juce::ChangeBroadcaster,
    juce::Timer
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

    void changeListenerCallback(ChangeBroadcaster* source) override;
    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

    /// Restores a previously cached waveform without re-reading the audio file.
    bool loadFromCacheData(const juce::MemoryBlock& waveformData);

    /// Serializes the current waveform so it can be stored in the cache.
    juce::MemoryBlock saveToCacheData() const;

    /// Sets the visible time range for the waveform viewport.
    void setRange(juce::Range<double> newRange);

    /// Sets the callback used for mouse-wheel zoom gestures.
    void setMouseWheelZoomCallback(std::function<void(double)> callback);

    /// Sets the callback used for Ctrl/Cmd + mouse-wheel gain gestures. Argument is the delta in dB.
    void setMouseWheelGainCallback(std::function<void(float)> callback);

    /// Sets the callback fired once a full waveform has been loaded from disk.
    void setThumbnailFullyLoadedCallback(std::function<void()> callback);

    /// Loads a new audio resource into the thumbnail display.
    void setURL(const juce::URL& url);

    /// Applies a zoom factor to the waveform viewport.
    void setZoom(double zoomLevel);

    /// Visually scales the waveform vertically by the given linear gain factor (1.0 = unchanged).
    void setDisplayGain(float linearGain);

private:
    bool canMoveTransport() const noexcept;
    double xToTime(float x) const;
    float timeToX(double time) const;
    void timerCallback() override;
    void updateCursorPosition();

    juce::AudioThumbnail thumbnail;
    juce::AudioThumbnailCache thumbnailCache {128};
    juce::AudioTransportSource& transportSource;
    juce::DrawableRectangle currentPositionMarker;
    juce::Range<double> visibleRange;
    juce::StringArray lastDroppedFiles;
    juce::URL lastFileDropped;
    std::function<void()> thumbnailFullyLoadedCallback;
    std::function<void(double)> mouseWheelZoomCallback;
    std::function<void(float)> mouseWheelGainCallback;

    bool hasNotifiedFullyLoaded = false;
    bool isFollowingTransport = false;
    float displayGain = 1.0f;
};
