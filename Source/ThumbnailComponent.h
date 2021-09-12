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

    void setURL(const juce::URL& url);

    juce::URL getLastDroppedFile() const noexcept;

    void setRange(juce::Range<double> newRange);

    void paint(juce::Graphics& g) override;

    void resized() override;

    void changeListenerCallback(ChangeBroadcaster*) override;

    bool isInterestedInFileDrag(const juce::StringArray& /*files*/) override;

    void filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/) override;

    void mouseDown(const juce::MouseEvent& e) override;

    void mouseDoubleClick(const juce::MouseEvent&) override;

    void mouseDrag(const juce::MouseEvent& e) override;

    void mouseUp(const juce::MouseEvent&) override;

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override;

private:
    juce::AudioTransportSource& transportSource;

    juce::AudioThumbnailCache thumbnailCache {128};
    juce::AudioThumbnail thumbnail;
    juce::Range<double> visibleRange;
    bool isFollowingTransport = false;
    juce::URL lastFileDropped;

    juce::DrawableRectangle currentPositionMarker;

    float timeToX(const double time) const;

    double xToTime(const float x) const;

    bool canMoveTransport() const noexcept;

    void timerCallback() override;

    void updateCursorPosition();
};
