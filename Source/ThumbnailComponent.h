#pragma once

#include <JuceHeader.h>

class ThumbnailComponent :
    public juce::Component,
    public juce::ChangeListener,
    public juce::FileDragAndDropTarget,
    public juce::ChangeBroadcaster,
    private juce::Timer
{
public:
    ThumbnailComponent(juce::AudioFormatManager& formatManager, juce::AudioTransportSource& source);
    ~ThumbnailComponent() override;

    bool isInterestedInFileDrag(const juce::StringArray& /*files*/) override;

    juce::URL getLastDroppedFile() const noexcept;

    void changeListenerCallback(ChangeBroadcaster*) override;
    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    void setRange(juce::Range<double> newRange);
    void setURL(const juce::URL& url);
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

    bool isFollowingTransport = false;
};
