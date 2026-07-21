/// Implementation of the ThumbnailComponent waveform preview.
/// Covers waveform painting and cache load and save,
/// transport scrubbing through the mouse handlers,
/// wheel-based zoom and gain gestures,
/// and the timer-driven position marker updates during playback.

#include "ThumbnailComponent.h"

#include "CustomLookAndFeel.h"

#include <JuceHeader.h>

ThumbnailComponent::ThumbnailComponent(juce::AudioFormatManager& formatManager, juce::AudioTransportSource& source) :
    thumbnail(1024, formatManager, thumbnailCache),
    transportSource(source)
{
    thumbnail.addChangeListener(this);

    currentPositionMarker.setFill(juce::Colours::white.withAlpha(0.85f));
    addAndMakeVisible(currentPositionMarkerComponent);
}

ThumbnailComponent::~ThumbnailComponent()
{
    thumbnail.removeChangeListener(this);
}

void ThumbnailComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::CustomLookAndFeel::greyMediumDark);
    graphics.setColour(juce::CustomLookAndFeel::blue);

    if (thumbnail.getTotalLength() > 0.0) {
        const auto reduced = getLocalBounds().reduced(2);

        // drawChannels scales each channel around its own strip midpoint and clamps the waveform to the strip bounds,
        // so over-0 dBFS peaks are naturally clipped at the channel boundary.
        thumbnail.drawChannels(graphics, reduced, visibleRange.getStart(), visibleRange.getEnd(), displayGain);
    } else {
        graphics.setFont(14.0f);
        graphics.drawFittedText("No audio file selected", getLocalBounds(), juce::Justification::centred, 2);
    }
}

void ThumbnailComponent::resized() { }

bool ThumbnailComponent::loadFromCacheData(const juce::MemoryBlock& waveformData)
{
    thumbnail.clear();
    hasNotifiedFullyLoaded = false;

    if (waveformData.getSize() == 0) {
        visibleRange = {};
        stopTimer();
        repaint();
        return false;
    }

    if (juce::MemoryInputStream input(waveformData, false); !thumbnail.loadFrom(input)) {
        visibleRange = {};
        stopTimer();
        repaint();
        return false;
    }

    // A successful loadFrom can still leave the thumbnail empty
    // when the cached blob was written from an incomplete source.
    // Treat that as a cache miss so the caller can re-read the file.
    if (thumbnail.getTotalLength() <= 0.0) {
        thumbnail.clear();
        visibleRange = {};
        stopTimer();
        repaint();
        return false;
    }

    setRange({0.0, thumbnail.getTotalLength()});
    startTimerHz(60);
    return true;
}

juce::MemoryBlock ThumbnailComponent::saveToCacheData() const
{
    juce::MemoryBlock waveformData;

    // Refuse to persist an empty thumbnail.
    // AudioThumbnail::isFullyLoaded can briefly return true before any peak data has been generated.
    // Without this guard a zero-length blob can land in the cache,
    // causing the preview to render as "No audio file selected" on the next load.
    if (!thumbnail.isFullyLoaded() || thumbnail.getTotalLength() <= 0.0) {
        return waveformData;
    }

    juce::MemoryOutputStream output(waveformData, false);
    thumbnail.saveTo(output);
    return waveformData;
}

void ThumbnailComponent::setThumbnailFullyLoadedCallback(std::function<void()> callback)
{
    thumbnailFullyLoadedCallback = std::move(callback);
}

void ThumbnailComponent::setMouseWheelZoomCallback(std::function<void(double)> callback)
{
    mouseWheelZoomCallback = std::move(callback);
}

void ThumbnailComponent::setMouseWheelGainCallback(std::function<void(float)> callback)
{
    mouseWheelGainCallback = std::move(callback);
}

void ThumbnailComponent::setURL(const juce::URL& url)
{
    hasNotifiedFullyLoaded = false;

    if (!url.isWellFormed()) {
        thumbnail.clear();
        visibleRange = {};
        stopTimer();
        repaint();
        return;
    }

    if (juce::InputSource* inputSource = new juce::URLInputSource(url); inputSource != nullptr) {
        thumbnail.setSource(inputSource);

        if (thumbnail.getTotalLength() > 0.0) {
            setRange({0.0, thumbnail.getTotalLength()});
        } else {
            visibleRange = {};
            repaint();
        }

        startTimerHz(60);
    }
}

juce::URL ThumbnailComponent::getLastDroppedFile() const noexcept
{
    return lastFileDropped;
}

juce::StringArray ThumbnailComponent::getLastDroppedFiles() const
{
    return lastDroppedFiles;
}

void ThumbnailComponent::setRange(const juce::Range<double> newRange)
{
    visibleRange = newRange;
    updateCursorPosition();
    repaint();
}

void ThumbnailComponent::changeListenerCallback(juce::ChangeBroadcaster* /*source*/)
{
    if (thumbnail.getTotalLength() > 0.0 && visibleRange.getLength() <= 0.0) {
        setRange({0.0, thumbnail.getTotalLength()});
    } else {
        repaint();
    }

    if (thumbnail.isFullyLoaded() && !hasNotifiedFullyLoaded) {
        hasNotifiedFullyLoaded = true;

        if (thumbnailFullyLoadedCallback) {
            thumbnailFullyLoadedCallback();
        }
    }
}

bool ThumbnailComponent::isInterestedInFileDrag(const juce::StringArray& /*files*/)
{
    return true;
}

void ThumbnailComponent::filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/)
{
    lastDroppedFiles = files;
    lastFileDropped = juce::URL(juce::File(files[0]));
    sendChangeMessage();
}

void ThumbnailComponent::mouseDown(const juce::MouseEvent& event)
{
    mouseDrag(event);
}

void ThumbnailComponent::mouseDoubleClick(const juce::MouseEvent& /*event*/)
{
    transportSource.setPosition(0.0);
    transportSource.start();
}

void ThumbnailComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (canMoveTransport()) {
        transportSource.stop();
        transportSource.setPosition(juce::jmax(0.0, xToTime(static_cast<float>(event.x))));
    }
}

void ThumbnailComponent::mouseUp(const juce::MouseEvent& /*event*/)
{
    transportSource.start();
}

void ThumbnailComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (thumbnail.getTotalLength() <= 0.0) {
        return;
    }

    // Ctrl on Windows/Linux, Cmd on macOS. Both map to commandModifier in JUCE.
    if (event.mods.isCommandDown() && std::abs(wheel.deltaY) > 0.0f && mouseWheelGainCallback) {
        constexpr float dbPerWheelNotch = 6.0f;
        mouseWheelGainCallback(wheel.deltaY * dbPerWheelNotch);
        return;
    }

    if (std::abs(wheel.deltaY) > 0.0f && mouseWheelZoomCallback) {
        const auto zoomFactor = std::pow(1.2, static_cast<double>(wheel.deltaY) * 8.0);
        mouseWheelZoomCallback(zoomFactor);
        return;
    }

    auto newStart = visibleRange.getStart() - wheel.deltaX * visibleRange.getLength() * 0.10;
    newStart = juce::jlimit(0.0, juce::jmax(0.0, thumbnail.getTotalLength() - visibleRange.getLength()), newStart);

    if (canMoveTransport()) {
        setRange({newStart, newStart + visibleRange.getLength()});
    }

    repaint();
}

float ThumbnailComponent::timeToX(const double time) const
{
    if (visibleRange.getLength() <= 0) {
        return 0;
    }

    return static_cast<float>(getWidth())
        * static_cast<float>((time - visibleRange.getStart()) / visibleRange.getLength());
}

double ThumbnailComponent::xToTime(const float xPosition) const
{
    return xPosition / static_cast<float>(getWidth()) * visibleRange.getLength() + visibleRange.getStart();
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
        setRange(visibleRange.movedToStartAt(transportSource.getCurrentPosition() - visibleRange.getLength() / 2.0));
    }
}

void ThumbnailComponent::updateCursorPosition()
{
    currentPositionMarkerComponent.setVisible(transportSource.isPlaying() || isMouseButtonDown());

    currentPositionMarker.setRectangle(
        juce::Rectangle<float>(
            timeToX(transportSource.getCurrentPosition()) - 0.75f, 0, 1.5f, static_cast<float>(getHeight())
        )
    );
}

void ThumbnailComponent::setDisplayGain(const float linearGain)
{
    const auto clampedGain = juce::jlimit(0.0f, 100.0f, linearGain);
    if (juce::approximatelyEqual(displayGain, clampedGain)) {
        return;
    }

    displayGain = clampedGain;
    repaint();
}

void ThumbnailComponent::setZoom(const double zoomLevel)
{
    const auto start = transportSource.getCurrentPosition();
    const auto zoom = thumbnail.getTotalLength() / zoomLevel;
    const auto end = juce::jmin(thumbnail.getTotalLength(), start + zoom);
    if (start < thumbnail.getTotalLength() * 0.05 || thumbnail.getTotalLength() - start > zoom) {
        setRange(juce::Range(start, end));
    } else {
        // The remaining distance is shorter than the zoom length, so zoom from the end instead.
        setRange(juce::Range(thumbnail.getTotalLength() - zoom, end));
    }
}
