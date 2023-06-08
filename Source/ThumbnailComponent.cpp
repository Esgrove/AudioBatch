#include "ThumbnailComponent.h"

#include "CustomLookAndFeel.h"

#include <JuceHeader.h>

ThumbnailComponent::ThumbnailComponent(juce::AudioFormatManager& formatManager, juce::AudioTransportSource& source)
    : thumbnail(1024, formatManager, thumbnailCache)
    , transportSource(source)
{
    thumbnail.addChangeListener(this);

    currentPositionMarker.setFill(juce::Colours::white.withAlpha(0.85f));
    addAndMakeVisible(currentPositionMarker);
}

ThumbnailComponent::~ThumbnailComponent()
{
    thumbnail.removeChangeListener(this);
}

void ThumbnailComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::CustomLookAndFeel::greyMediumDark);
    g.setColour(juce::CustomLookAndFeel::blue);

    if (thumbnail.getTotalLength() > 0.0) {
        auto thumbArea = getLocalBounds();
        thumbnail.drawChannels(g, thumbArea.reduced(2), visibleRange.getStart(), visibleRange.getEnd(), 1.0f);
    } else {
        g.setFont(14.0f);
        g.drawFittedText("No audio file selected", getLocalBounds(), juce::Justification::centred, 2);
    }
}

void ThumbnailComponent::resized() {}

void ThumbnailComponent::setURL(const juce::URL& url)
{
    juce::InputSource* inputSource = nullptr;

    if (inputSource == nullptr) {
        inputSource = new juce::URLInputSource(url);
    }

    if (inputSource != nullptr) {
        thumbnail.setSource(inputSource);

        juce::Range<double> newRange(0.0, thumbnail.getTotalLength());
        setRange(newRange);

        startTimerHz(60);
    }
}

juce::URL ThumbnailComponent::getLastDroppedFile() const noexcept
{
    return lastFileDropped;
}

void ThumbnailComponent::setRange(juce::Range<double> newRange)
{
    visibleRange = newRange;
    updateCursorPosition();
    repaint();
}

void ThumbnailComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // this method is called by the thumbnail when it has changed, so we should repaint it..
    repaint();
}

bool ThumbnailComponent::isInterestedInFileDrag(const juce::StringArray& /*files*/)
{
    return true;
}

void ThumbnailComponent::filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/)
{
    lastFileDropped = juce::URL(juce::File(files[0]));
    sendChangeMessage();
}

void ThumbnailComponent::mouseDown(const juce::MouseEvent& e)
{
    mouseDrag(e);
}

void ThumbnailComponent::mouseDoubleClick(const juce::MouseEvent&)
{
    transportSource.setPosition(0.0);
    transportSource.start();
}

void ThumbnailComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (canMoveTransport()) {
        transportSource.stop();
        transportSource.setPosition(juce::jmax(0.0, xToTime((float)e.x)));
    }
}

void ThumbnailComponent::mouseUp(const juce::MouseEvent&)
{
    transportSource.start();
}

void ThumbnailComponent::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (thumbnail.getTotalLength() > 0.0) {
        auto newStart = visibleRange.getStart() - wheel.deltaX * (visibleRange.getLength()) / 10.0;
        newStart
            = juce::jlimit(0.0, juce::jmax(0.0, thumbnail.getTotalLength() - (visibleRange.getLength())), newStart);

        if (canMoveTransport()) {
            setRange({newStart, newStart + visibleRange.getLength()});
        }

        repaint();
    }
}

float ThumbnailComponent::timeToX(const double time) const
{
    if (visibleRange.getLength() <= 0) {
        return 0;
    }

    return (float)getWidth() * (float)((time - visibleRange.getStart()) / visibleRange.getLength());
}

double ThumbnailComponent::xToTime(const float x) const
{
    return (x / (float)getWidth()) * (visibleRange.getLength()) + visibleRange.getStart();
}

bool ThumbnailComponent::canMoveTransport() const noexcept
{
    return !(isFollowingTransport && transportSource.isPlaying());
}

void ThumbnailComponent::timerCallback()
{
    if (canMoveTransport()) {
        updateCursorPosition();
    } else {
        setRange(visibleRange.movedToStartAt(transportSource.getCurrentPosition() - (visibleRange.getLength() / 2.0)));
    }
}

void ThumbnailComponent::updateCursorPosition()
{
    currentPositionMarker.setVisible(transportSource.isPlaying() || isMouseButtonDown());

    currentPositionMarker.setRectangle(
        juce::Rectangle<float>(timeToX(transportSource.getCurrentPosition()) - 0.75f, 0, 1.5f, (float)(getHeight())));
}

void ThumbnailComponent::setZoom(double zoomLevel)
{
    auto start = transportSource.getCurrentPosition();
    auto zoom = thumbnail.getTotalLength() / zoomLevel;
    auto end = juce::jmin(thumbnail.getTotalLength(), start + zoom);
    if (start < thumbnail.getTotalLength() * 0.05 || thumbnail.getTotalLength() - start > zoom) {
        setRange(juce::Range<double>(start, end));
    } else {
        // remaining distance is shorter than zoom length -> zoom from end
        setRange(juce::Range<double>(thumbnail.getTotalLength() - zoom, end));
    }
}
