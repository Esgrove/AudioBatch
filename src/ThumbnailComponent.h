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
    /// Creates a thumbnail view backed by the shared transport source.
    ThumbnailComponent(juce::AudioFormatManager& formatManager, juce::AudioTransportSource& source);

    /// Stops timer activity and detaches listeners.
    ~ThumbnailComponent() override;

    ThumbnailComponent(const ThumbnailComponent&) = delete;
    ThumbnailComponent& operator=(const ThumbnailComponent&) = delete;
    ThumbnailComponent(ThumbnailComponent&&) = delete;
    ThumbnailComponent& operator=(ThumbnailComponent&&) = delete;

    /// Accepts every file drag, validation of the dropped files happens in the owning component.
    bool isInterestedInFileDrag(const juce::StringArray& /*files*/) override;

    /// Returns the most recently dropped file as a URL for convenience.
    juce::URL getLastDroppedFile() const noexcept;

    /// Returns the full list of files dropped onto the waveform component.
    juce::StringArray getLastDroppedFiles() const;

    /// Reacts to thumbnail load progress by repainting,
    /// and fires the fully-loaded callback exactly once per loaded source.
    void changeListenerCallback(ChangeBroadcaster* source) override;

    /// Records the dropped files and notifies change listeners so the owner can load them.
    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;

    /// Restarts playback from the beginning of the file.
    void mouseDoubleClick(const juce::MouseEvent& event) override;

    /// Takes keyboard focus so the hot cue keys apply, then starts scrubbing through the drag handler.
    void mouseDown(const juce::MouseEvent& event) override;

    /// Handles the number keys 1 to 8 without command, ctrl or alt modifiers as Serato hot cue jumps.
    /// Every other key returns false, so JUCE passes it on to the parent component's shortcuts.
    bool keyPressed(const juce::KeyPress& key) override;

    /// Repaints to show the keyboard focus outline.
    void focusGained(FocusChangeType cause) override;

    /// Repaints to remove the keyboard focus outline.
    void focusLost(FocusChangeType cause) override;

    /// Scrubs the transport to the time under the cursor while the mouse is held down.
    void mouseDrag(const juce::MouseEvent& event) override;

    /// Resumes playback from the scrubbed position when the mouse is released.
    void mouseUp(const juce::MouseEvent& event) override;

    /// Handles wheel gestures over the waveform.
    /// Plain vertical wheel zooms, Ctrl/Cmd plus wheel adjusts gain in dB steps,
    /// and horizontal wheel scrolls the visible range.
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

    /// Draws the waveform for the visible range, or a placeholder text when no file is loaded.
    void paint(juce::Graphics& graphics) override;

    /// No layout work is needed, the position marker is placed by updateCursorPosition instead.
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

    /// Attaches immutable spectral data on the message thread and repaints only.
    /// Does not reload peaks, reset the viewport or gain, or affect playback.
    /// Null, empty and oversized data remove colouring. The owner must reject stale asynchronous results.
    void setSeratoOverview(std::shared_ptr<const audiobatch::serato::Overview> overview);

    /// Attaches owned cue positions and colours without altering peaks, playback or the viewport.
    /// Null data removes the arrows. The owner must reject stale asynchronous results before adoption.
    void setSeratoMarkers(std::shared_ptr<const audiobatch::serato::Markers> markers);

private:
    /// Moves the transport to the Serato hot cue in the given zero-based slot and starts playback,
    /// matching the click and double-click behaviour. Does nothing when that slot has no cue.
    void jumpToHotCue(int slot);

    /// Draws inward-facing cue arrows over the waveform, below the child playback cursor.
    /// Millisecond positions use the same seconds-based projection as scrubbing and the playhead.
    void drawCueMarkers(juce::Graphics& graphics) const;

    /// Returns the inset rectangle shared by waveform painting, cursor projection and scrubbing.
    juce::Rectangle<int> waveformBounds() const;

    /// Reports whether user interaction may reposition the transport.
    /// Repositioning is blocked while the view is following an actively playing transport.
    bool canMoveTransport() const noexcept;

    /// Converts a horizontal pixel position into a time within the visible range.
    double xToTime(float xPosition) const;

    /// Converts a time into a horizontal pixel position within the visible range.
    float timeToX(double time) const;

    /// Timer tick that keeps the playback cursor in sync,
    /// scrolling the visible range along with the transport when follow mode is active.
    void timerCallback() override;

    /// Moves the playback position marker to the current transport time
    /// and shows it only during playback or while the mouse is held down.
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
