/// Implementation of the ThumbnailComponent waveform preview.
/// Covers waveform painting and cache load and save,
/// transport scrubbing through the mouse handlers,
/// wheel-based zoom and gain gestures,
/// and the timer-driven position marker updates during playback.

#include "gui/components/ThumbnailComponent.h"

#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoWaveformColouring.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <algorithm>

namespace audiobatch
{
namespace
{
/// Alpha of the white playback position marker drawn over the waveform.
constexpr float positionMarkerAlpha {0.85f};

/// Rate of the timer that updates the position marker and follows the transport, in hertz.
constexpr int positionUpdateRateHz {60};

/// Fraction of the visible range scrolled per unit of horizontal mouse wheel movement.
constexpr double horizontalScrollStepRatio {0.10};

/// Fraction of the total length near the start of the file within which zooming always anchors at the playhead.
constexpr double zoomStartAnchorRatio {0.05};

/// Opacity of the cue arrow outline.
constexpr float cueArrowOutlineAlpha {0.8f};

/// Number of Serato hot cue slots reachable with the number keys 1 to 8.
constexpr int hotCueSlotCount {8};

/// Opacity of the outline drawn while the waveform has keyboard focus.
constexpr float focusOutlineAlpha {0.6f};
}  // namespace

/// Creates a thumbnail view backed by the shared transport source.
ThumbnailComponent::ThumbnailComponent(juce::AudioFormatManager& formatManager, juce::AudioTransportSource& source) :
    thumbnail(1024, formatManager, thumbnailCache),
    transportSource(source)
{
    thumbnail.addChangeListener(this);

    currentPositionMarker.setFill(juce::Colours::white.withAlpha(positionMarkerAlpha));
    addAndMakeVisible(currentPositionMarkerComponent);
    setWantsKeyboardFocus(true);
}

/// Stops timer activity and detaches listeners.
ThumbnailComponent::~ThumbnailComponent()
{
    thumbnail.removeChangeListener(this);
}

/// Draws the waveform for the visible range, or a placeholder text when no file is loaded.
void ThumbnailComponent::paint(juce::Graphics& graphics)
{
    juce::Path outline;
    outline.addRoundedRectangle(getLocalBounds().toFloat(), layout::waveform::cornerRadius);
    graphics.reduceClipRegion(outline);
    graphics.setColour(juce::CustomLookAndFeel::greyMediumDark());
    graphics.fillPath(outline);
    graphics.setColour(juce::CustomLookAndFeel::blue());

    if (thumbnail.getTotalLength() > 0.0) {
        const auto reduced = waveformBounds();
        const auto gradient = seratoOverview
            ? serato::makeOverviewGradient(
                  *seratoOverview, visibleRange, thumbnail.getTotalLength(), reduced, juce::CustomLookAndFeel::blue()
              )
            : std::nullopt;
        if (gradient) {
            serato::drawColouredThumbnail(graphics, thumbnail, reduced, visibleRange, displayGain, *gradient);
        } else {
            thumbnail.drawChannels(graphics, reduced, visibleRange.getStart(), visibleRange.getEnd(), displayGain);
        }
        drawCueMarkers(graphics);
    } else {
        graphics.setFont(juce::CustomLookAndFeel::getTextFont().withHeight(layout::waveform::placeholderFontHeight));
        graphics.drawFittedText("No audio file selected", getLocalBounds(), juce::Justification::centred, 2);
    }

    if (hasKeyboardFocus(false)) {
        graphics.setColour(juce::CustomLookAndFeel::blue().withAlpha(focusOutlineAlpha));
        graphics.drawRoundedRectangle(
            getLocalBounds().toFloat().reduced(layout::waveform::focusOutlineWidth / 2.0f),
            layout::waveform::cornerRadius,
            layout::waveform::focusOutlineWidth
        );
    }
}

/// No layout work is needed, the position marker is placed by updateCursorPosition instead.
void ThumbnailComponent::resized() {}

/// Restores a previously cached waveform without re-reading the audio file.
bool ThumbnailComponent::loadFromCacheData(const juce::MemoryBlock& waveformData)
{
    seratoOverview.reset();
    seratoMarkers.reset();
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
    startTimerHz(positionUpdateRateHz);
    return true;
}

/// Serializes the current waveform so it can be stored in the cache.
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

/// Sets the callback fired once a full waveform has been loaded from disk.
void ThumbnailComponent::setThumbnailFullyLoadedCallback(std::function<void()> callback)
{
    thumbnailFullyLoadedCallback = std::move(callback);
}

/// Sets the callback used for mouse-wheel zoom gestures.
void ThumbnailComponent::setMouseWheelZoomCallback(std::function<void(double)> callback)
{
    mouseWheelZoomCallback = std::move(callback);
}

/// Sets the callback used for Ctrl/Cmd + mouse-wheel gain gestures. Argument is the delta in dB.
void ThumbnailComponent::setMouseWheelGainCallback(std::function<void(float)> callback)
{
    mouseWheelGainCallback = std::move(callback);
}

/// Loads a new audio resource into the thumbnail display.
void ThumbnailComponent::setURL(const juce::URL& url)
{
    seratoOverview.reset();
    seratoMarkers.reset();
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

        startTimerHz(positionUpdateRateHz);
    }
}

/// Returns the most recently dropped file as a URL for convenience.
juce::URL ThumbnailComponent::getLastDroppedFile() const noexcept
{
    return lastFileDropped;
}

/// Returns the full list of files dropped onto the waveform component.
juce::StringArray ThumbnailComponent::getLastDroppedFiles() const
{
    return lastDroppedFiles;
}

/// Sets the visible time range for the waveform viewport.
void ThumbnailComponent::setRange(const juce::Range<double> newRange)
{
    visibleRange = newRange;
    updateCursorPosition();
    repaint();
}

/// Reacts to thumbnail load progress by repainting,
/// and fires the fully-loaded callback exactly once per loaded source.
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

/// Accepts every file drag, validation of the dropped files happens in the owning component.
bool ThumbnailComponent::isInterestedInFileDrag(const juce::StringArray& /*files*/)
{
    return true;
}

/// Records the dropped files and notifies change listeners so the owner can load them.
void ThumbnailComponent::filesDropped(const juce::StringArray& files, int /*x*/, int /*y*/)
{
    lastDroppedFiles = files;
    lastFileDropped = juce::URL(juce::File(files[0]));
    sendChangeMessage();
}

/// Takes keyboard focus so the hot cue keys apply, then starts scrubbing through the drag handler.
void ThumbnailComponent::mouseDown(const juce::MouseEvent& event)
{
    grabKeyboardFocus();
    mouseDrag(event);
}

/// Handles the number keys 1 to 8 without command, ctrl or alt modifiers as Serato hot cue jumps.
/// Every other key returns false, so JUCE passes it on to the parent component's shortcuts.
bool ThumbnailComponent::keyPressed(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    const auto character = key.getTextCharacter();
    if (modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown() || character < '1'
        || character >= '1' + hotCueSlotCount)
    {
        return false;
    }

    jumpToHotCue(static_cast<int>(character - '1'));
    return true;
}

/// Repaints to show the keyboard focus outline.
void ThumbnailComponent::focusGained(const FocusChangeType /*cause*/)
{
    repaint();
}

/// Repaints to remove the keyboard focus outline.
void ThumbnailComponent::focusLost(const FocusChangeType /*cause*/)
{
    repaint();
}

/// Moves the transport to the Serato hot cue in the given zero-based slot and starts playback,
/// matching the click and double-click behaviour.
/// Does nothing when that slot has no cue.
void ThumbnailComponent::jumpToHotCue(const int slot)
{
    if (!seratoMarkers || thumbnail.getTotalLength() <= 0.0) {
        return;
    }

    const auto cue = std::ranges::find_if(seratoMarkers->cues, [slot](const auto& candidate) {
        return static_cast<int>(candidate.index) == slot;
    });
    if (cue == seratoMarkers->cues.end()) {
        return;
    }

    const auto seconds = static_cast<double>(cue->positionMilliseconds) / utils::millisecondsPerSecond;
    transportSource.setPosition(juce::jlimit(0.0, thumbnail.getTotalLength(), seconds));
    transportSource.start();
    updateCursorPosition();
}

/// Restarts playback from the beginning of the file.
void ThumbnailComponent::mouseDoubleClick(const juce::MouseEvent& /*event*/)
{
    transportSource.setPosition(0.0);
    transportSource.start();
}

/// Scrubs the transport to the time under the cursor while the mouse is held down.
void ThumbnailComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (canMoveTransport()) {
        transportSource.stop();
        transportSource.setPosition(juce::jmax(0.0, xToTime(static_cast<float>(event.x))));
    }
}

/// Resumes playback from the scrubbed position when the mouse is released.
void ThumbnailComponent::mouseUp(const juce::MouseEvent& /*event*/)
{
    transportSource.start();
}

/// Handles wheel gestures over the waveform.
/// Plain vertical wheel zooms, Ctrl/Cmd plus wheel adjusts gain in dB steps,
/// and horizontal wheel scrolls the visible range.
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

    auto newStart = visibleRange.getStart()
        - static_cast<double>(wheel.deltaX) * visibleRange.getLength() * horizontalScrollStepRatio;
    newStart = juce::jlimit(0.0, juce::jmax(0.0, thumbnail.getTotalLength() - visibleRange.getLength()), newStart);

    if (canMoveTransport()) {
        setRange({newStart, newStart + visibleRange.getLength()});
    }

    repaint();
}

/// Converts a time into a horizontal pixel position within the visible range.
float ThumbnailComponent::timeToX(const double time) const
{
    const auto bounds = waveformBounds();
    if (visibleRange.getLength() <= 0.0 || bounds.getWidth() <= 0) {
        return static_cast<float>(bounds.getX());
    }

    return static_cast<float>(bounds.getX())
        + static_cast<float>(bounds.getWidth())
        * static_cast<float>((time - visibleRange.getStart()) / visibleRange.getLength());
}

/// Converts a horizontal pixel position into a time within the visible range.
double ThumbnailComponent::xToTime(const float xPosition) const
{
    const auto bounds = waveformBounds();
    if (bounds.getWidth() <= 0 || visibleRange.getLength() <= 0.0) {
        return visibleRange.getStart();
    }

    return static_cast<double>((xPosition - static_cast<float>(bounds.getX())) / static_cast<float>(bounds.getWidth()))
        * visibleRange.getLength()
        + visibleRange.getStart();
}

/// Returns the inset rectangle shared by waveform painting, cursor projection and scrubbing.
juce::Rectangle<int> ThumbnailComponent::waveformBounds() const
{
    return getLocalBounds().reduced(2);
}

/// Reports whether user interaction may reposition the transport.
/// Repositioning is blocked while the view is following an actively playing transport.
bool ThumbnailComponent::canMoveTransport() const noexcept
{
    return !(isFollowingTransport && transportSource.isPlaying());
}

/// Timer tick that keeps the playback cursor in sync,
/// scrolling the visible range along with the transport when follow mode is active.
void ThumbnailComponent::timerCallback()
{
    if (canMoveTransport()) {
        updateCursorPosition();
    } else {
        setRange(visibleRange.movedToStartAt(transportSource.getCurrentPosition() - visibleRange.getLength() / 2.0));
    }
}

/// Moves the playback position marker to the current transport time
/// and shows it only during playback or while the mouse is held down.
void ThumbnailComponent::updateCursorPosition()
{
    currentPositionMarkerComponent.setVisible(transportSource.isPlaying() || isMouseButtonDown());

    currentPositionMarker.setRectangle(
        juce::Rectangle<float>(
            timeToX(transportSource.getCurrentPosition()) - layout::waveform::positionMarkerWidth / 2.0f,
            layout::waveform::cornerRadius,
            layout::waveform::positionMarkerWidth,
            juce::jmax(0.0f, static_cast<float>(getHeight()) - layout::waveform::cornerRadius * 2.0f)
        )
    );
}

/// Visually scales the waveform vertically by the given linear gain factor (1.0 = unchanged).
void ThumbnailComponent::setDisplayGain(const float linearGain)
{
    const auto clampedGain = juce::jlimit(0.0f, 100.0f, linearGain);
    if (juce::approximatelyEqual(displayGain, clampedGain)) {
        return;
    }

    displayGain = clampedGain;
    repaint();
}

/// Attaches immutable spectral data on the message thread and repaints only.
/// Does not reload peaks, reset the viewport or gain, or affect playback.
/// Null, empty and oversized data remove colouring. The owner must reject stale asynchronous results.
void ThumbnailComponent::setSeratoOverview(std::shared_ptr<const serato::Overview> overview)
{
    if (overview && (overview->slices.empty() || overview->slices.size() > serato::Overview::maxSlices)) {
        overview.reset();
    }
    seratoOverview = std::move(overview);
    repaint();
}

/// Attaches owned cue positions and colours without altering peaks, playback or the viewport.
/// Null data removes the arrows.
/// The owner must reject stale asynchronous results before adoption.
void ThumbnailComponent::setSeratoMarkers(std::shared_ptr<const serato::Markers> markers)
{
    seratoMarkers = std::move(markers);
    repaint();
}

/// Draws inward-facing cue arrows over the waveform, below the child playback cursor.
/// Millisecond positions use the same seconds-based projection as scrubbing and the playhead.
void ThumbnailComponent::drawCueMarkers(juce::Graphics& graphics) const
{
    const auto bounds = waveformBounds();
    if (!seratoMarkers || bounds.isEmpty() || visibleRange.getLength() <= 0.0) {
        return;
    }
    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.reduceClipRegion(bounds);
    const auto arrowHeight = std::min(layout::waveform::cueArrowHeight, static_cast<float>(bounds.getHeight()) / 4.0f);
    const auto halfWidth = std::min(
                               layout::waveform::cueArrowWidth,
                               arrowHeight * layout::waveform::cueArrowWidth / layout::waveform::cueArrowHeight
                           )
        / 2.0f;
    for (const auto& cue : seratoMarkers->cues) {
        const auto seconds = static_cast<double>(cue.positionMilliseconds) / utils::millisecondsPerSecond;
        if (seconds > thumbnail.getTotalLength() || seconds < visibleRange.getStart()
            || seconds > visibleRange.getEnd())
        {
            continue;
        }
        const auto horizontalPosition = timeToX(seconds);
        const auto top = static_cast<float>(bounds.getY());
        const auto bottom = static_cast<float>(bounds.getBottom());
        juce::Path arrows;
        arrows.addTriangle(
            horizontalPosition - halfWidth,
            top,
            horizontalPosition + halfWidth,
            top,
            horizontalPosition,
            top + arrowHeight
        );
        arrows.addTriangle(
            horizontalPosition - halfWidth,
            bottom,
            horizontalPosition + halfWidth,
            bottom,
            horizontalPosition,
            bottom - arrowHeight
        );
        graphics.setColour(juce::Colours::black.withAlpha(cueArrowOutlineAlpha));
        graphics.strokePath(arrows, juce::PathStrokeType(layout::waveform::cueArrowOutlineWidth));
        graphics.setColour(cue.colour.withAlpha(1.0f));
        graphics.fillPath(arrows);
    }
}

/// Applies a zoom factor to the waveform viewport.
void ThumbnailComponent::setZoom(const double zoomLevel)
{
    const auto start = transportSource.getCurrentPosition();
    const auto zoom = thumbnail.getTotalLength() / zoomLevel;
    const auto end = juce::jmin(thumbnail.getTotalLength(), start + zoom);
    if (start < thumbnail.getTotalLength() * zoomStartAnchorRatio || thumbnail.getTotalLength() - start > zoom) {
        setRange(juce::Range(start, end));
    } else {
        // The remaining distance is shorter than the zoom length, so zoom from the end instead.
        setRange(juce::Range(thumbnail.getTotalLength() - zoom, end));
    }
}
}  // namespace audiobatch
