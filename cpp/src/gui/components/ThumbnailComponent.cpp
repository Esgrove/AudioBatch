/// Implementation of the ThumbnailComponent waveform preview.
/// Covers waveform painting and adoption of scheduler-provided native artifacts,
/// transport scrubbing through the mouse handlers,
/// wheel-based zoom and gain gestures,
/// and display-synchronized position marker updates during playback.

#include "gui/components/ThumbnailComponent.h"

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/NativeWaveformBuilder.h"
#include "analysis/NativeWaveformData.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/components/PreviewPlotGeometry.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoOverview.h"
#include "serato/SeratoTagTypes.h"
#include "serato/SeratoWaveformColouring.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// Fraction of the visible range scrolled per unit of horizontal mouse wheel movement.
constexpr double horizontalScrollStepRatio {0.10};

/// The vertical mouse wheel zooms by wheelZoomBase raised to the wheel movement times wheelZoomMovementScale.
constexpr double wheelZoomBase {1.2};
constexpr double wheelZoomMovementScale {8.0};

/// Fraction of the total length near the start of the file within which zooming always anchors at the playhead.
constexpr double zoomStartAnchorRatio {0.05};

/// Opacity of the cue arrow outline.
constexpr float cueArrowOutlineAlpha {0.8f};

/// Number of Serato hot cue slots reachable with the number keys 1 to 8.
constexpr int hotCueSlotCount {8};

/// Coarsest waveform detail, which keeps the cached waveform of a long file small.
constexpr int maximumSamplesPerThumbnailPoint {1024};

/// Opacity of the outline drawn while the waveform has keyboard focus.
constexpr float focusOutlineAlpha {0.6f};

/// Opacity of the white labels of the loudness axis.
constexpr float loudnessLabelAlpha {0.8f};

/// Opacity of the white grid lines at the loudness axis ticks.
constexpr float loudnessGridAlpha {0.12f};

/// Opacity of the black outline drawn under the loudness curve, which keeps it readable over the waveform.
constexpr float loudnessOutlineAlpha {0.65f};

}  // namespace

/// Creates a thumbnail view backed by the shared transport source.
ThumbnailComponent::ThumbnailComponent(juce::AudioFormatManager& formatManager, AudioPreviewPlayer& source) :
    transportSource(source),
    thumbnailFormats(formatManager),
    thumbnail(std::make_unique<juce::AudioThumbnail>(maximumSamplesPerThumbnailPoint, formatManager, thumbnailCache))
{
    thumbnail->addChangeListener(this);

    setWantsKeyboardFocus(true);
    positionAttachment = juce::VBlankAttachment(this, [this](double) { updatePlaybackPosition(); });
}

/// Stops display callbacks and detaches listeners.
ThumbnailComponent::~ThumbnailComponent()
{
    positionAttachment = {};
    thumbnail->removeChangeListener(this);
}

/// Draws the waveform for the visible range, or a placeholder text when no file is loaded.
void ThumbnailComponent::paint(juce::Graphics& graphics)
{
    juce::Path outline;
    outline.addRoundedRectangle(getLocalBounds().toFloat(), layout::waveform::cornerRadius);
    graphics.reduceClipRegion(outline);
    graphics.setColour(CustomLookAndFeel::greyMediumDark());
    graphics.fillPath(outline);
    graphics.setColour(CustomLookAndFeel::blue());

    if (sourceDuration() > 0.0) {
        const auto geometry = getPlotGeometry();
        if (!geometry.waveform.isEmpty()) {
            if (!drawColouredWaveform(graphics, geometry.waveform)) {
                thumbnail->drawChannels(
                    graphics, geometry.waveform, visibleRange.getStart(), visibleRange.getEnd(), displayGain
                );
            }
            drawOverloads(graphics);
            drawWaveformAxis(graphics, geometry);
            drawCueMarkers(graphics, geometry.waveform);
        }
        if (!geometry.spectrogram.isEmpty()) {
            drawSpectrogram(graphics, geometry.spectrogram);
            drawFrequencyAxis(graphics, geometry);
            drawCueMarkers(graphics, geometry.spectrogram);
        }
        drawSourceWarning(graphics, geometry.sourceWarning);
        drawLoudnessGraph(graphics);
        drawPlayheads(graphics);
    } else {
        graphics.setFont(CustomLookAndFeel::getTextFont().withHeight(layout::waveform::placeholderFontHeight));
        graphics.drawFittedText("No audio file selected", getLocalBounds(), juce::Justification::centred, 2);
    }

    if (hasKeyboardFocus(false)) {
        graphics.setColour(CustomLookAndFeel::blue().withAlpha(focusOutlineAlpha));
        graphics.drawRoundedRectangle(
            getLocalBounds().toFloat().reduced(layout::waveform::focusOutlineWidth * 0.5f),
            layout::waveform::cornerRadius,
            layout::waveform::focusOutlineWidth
        );
    }
}

/// Draws the waveform in the colours of the Serato overview, and returns false when there are no colours to draw.
/// The coloured image is only rendered again when the bounds, the visible range, the display gain,
/// the pixel scale, or the renderer differ from the last paint, or the waveform or the overview changed,
/// so the repaints under the moving playback cursor only draw the cached image.
bool ThumbnailComponent::drawColouredWaveform(juce::Graphics& graphics, const juce::Rectangle<int> bounds)
{
    if (!seratoWaveformColouringEnabled || !seratoOverview) {
        colouredWaveform.reset();
        return false;
    }

    const auto cacheValid = colouredWaveform.has_value() && colouredWaveform->bounds == bounds
        && colouredWaveform->visibleRange == visibleRange
        && juce::exactlyEqual(colouredWaveform->displayGain, displayGain)
        && serato::isRenderedFor(colouredWaveform->rendered, graphics);

    if (!cacheValid) {
        const auto gradient = serato::makeOverviewGradient(
            *seratoOverview, visibleRange, sourceDuration(), bounds, CustomLookAndFeel::blue()
        );
        if (!gradient) {
            colouredWaveform.reset();
            return false;
        }
        colouredWaveform = ColouredWaveformCache {
            .rendered
            = serato::renderColouredThumbnail(graphics, *thumbnail, bounds, visibleRange, displayGain, *gradient),
            .bounds = bounds,
            .visibleRange = visibleRange,
            .displayGain = displayGain,
        };
    }

    serato::drawColouredThumbnail(graphics, colouredWaveform->rendered);
    return true;
}

/// Keeps the playback marker full-height immediately while the preview workspace resizes.
void ThumbnailComponent::resized()
{
    updateCursorPosition();
}

/// Restores a previously cached waveform without re-reading the audio file.
bool ThumbnailComponent::loadFromCacheData(
    const juce::MemoryBlock& waveformData,
    std::optional<utils::FileIdentity> identity
)
{
    sourceIdentity = std::move(identity);
    spectrogramData.reset();
    spectrogramRaster = {};
    spectrogramCompleted = false;
    pendingDuration = 0.0;
    pendingSampleRate = 0.0;
    sourceLengthInSamples = 0;
    sourceChannels = 0;
    scrubbing = false;
    pendingSeekTime.reset();
    scrubBounds = {};
    colouredWaveform.reset();
    loudnessHistory.reset();
    overloadHistory.reset();
    seratoOverview.reset();
    seratoMarkers.reset();
    thumbnail->clear();
    hasNotifiedFullyLoaded = false;

    if (!isValidWaveformData(waveformData)) {
        visibleRange = {};
        repaint();
        return false;
    }

    if (juce::MemoryInputStream input(waveformData, false); !thumbnail->loadFrom(input)) {
        visibleRange = {};
        repaint();
        return false;
    }

    // A successful loadFrom can still leave the thumbnail empty
    // when the cached blob was written from an incomplete source.
    // Treat that as a cache miss so the caller can re-read the file.
    if (sourceDuration() <= 0.0) {
        thumbnail->clear();
        visibleRange = {};
        repaint();
        return false;
    }

    if (const auto geometry
        = waveformGeometry(std::span(static_cast<const std::byte*>(waveformData.getData()), waveformData.getSize())))
    {
        sourceLengthInSamples = geometry->lengthInSamples;
        pendingSampleRate = geometry->sampleRate;
        sourceChannels = geometry->channels;
    }
    setRange({0.0, sourceDuration()});
    return true;
}

/// Shows source geometry while artifact work is gated or queued, without launching a JUCE decoder.
void ThumbnailComponent::setPendingSource(
    const juce::int64 lengthInSamples,
    const double durationSeconds,
    const utils::FileIdentity& identity
)
{
    setURL(juce::URL(juce::File(identity.path)), lengthInSamples, identity);
    if (lengthInSamples > 0 && std::isfinite(durationSeconds) && durationSeconds > 0.0) {
        sourceLengthInSamples = lengthInSamples;
        pendingDuration = durationSeconds;
        pendingSampleRate = static_cast<double>(lengthInSamples) / durationSeconds;
        thumbnail->reset(2, static_cast<double>(lengthInSamples) / durationSeconds, lengthInSamples);
        setRange({0.0, durationSeconds});
    }
}

/// Replaces only native peaks, preserving viewport, cues, histories, gain and transport.
/// A detached worker result must belong to the exact source version already displayed.
bool ThumbnailComponent::adoptWaveformData(const juce::MemoryBlock& waveformData, const utils::FileIdentity& identity)
{
    if (!sourceIdentity || sourceIdentity->path != identity.path || sourceIdentity->fileSize != identity.fileSize
        || sourceIdentity->modifiedTimeMs != identity.modifiedTimeMs
        || sourceIdentity->platformId != identity.platformId || sourceIdentity->preciseTimes != identity.preciseTimes
        || !isValidWaveformData(waveformData))
    {
        return false;
    }
    juce::MemoryInputStream input(waveformData, false);
    if (!thumbnail->loadFrom(input)) {
        return false;
    }
    if (const auto geometry
        = waveformGeometry(std::span(static_cast<const std::byte*>(waveformData.getData()), waveformData.getSize())))
    {
        sourceLengthInSamples = geometry->lengthInSamples;
        pendingSampleRate = geometry->sampleRate;
        sourceChannels = geometry->channels;
    }
    if (spectrogramData) {
        setSpectrogramData(spectrogramData);
    }
    colouredWaveform.reset();
    hasNotifiedFullyLoaded = true;
    updateCursorPosition();
    repaint();
    return true;
}

/// Returns the source identity retained while its waveform is pending and after artifacts are adopted.
const std::optional<utils::FileIdentity>& ThumbnailComponent::getSourceIdentity() const noexcept
{
    return sourceIdentity;
}

/// Serializes the current waveform so it can be stored in the cache.
juce::MemoryBlock ThumbnailComponent::saveToCacheData() const
{
    juce::MemoryBlock waveformData;

    // Refuse to persist an empty thumbnail.
    // AudioThumbnail::isFullyLoaded can briefly return true before any peak data has been generated.
    // Without this guard a zero-length blob can land in the cache,
    // causing the preview to render as "No audio file selected" on the next load.
    if (!thumbnail->isFullyLoaded() || sourceDuration() <= 0.0) {
        return waveformData;
    }

    juce::MemoryOutputStream output(waveformData, false);
    thumbnail->saveTo(output);
    return waveformData;
}

/// Sets the callback fired once the thumbnail reports complete native waveform data.
void ThumbnailComponent::setThumbnailFullyLoadedCallback(std::function<void()> callback)
{
    thumbnailFullyLoadedCallback = std::move(callback);
}

/// Sets the callback used for mouse-wheel zoom gestures.
void ThumbnailComponent::setMouseWheelZoomCallback(std::function<void(double)> callback)
{
    mouseWheelZoomCallback = std::move(callback);
}

/// Sets the callback used for Ctrl/Cmd + mouse-wheel gain gestures.
/// Its argument is the gain change in dB of one wheel event, already chosen by the held modifiers.
void ThumbnailComponent::setMouseWheelGainCallback(std::function<void(float)> callback)
{
    mouseWheelGainCallback = std::move(callback);
}

/// Resets the display and source identity while the admitted scheduler prepares waveform artifacts.
/// The length of the file in samples chooses the waveform detail, the finest for short files.
/// This does not attach a source-backed decoder or read audio.
void ThumbnailComponent::setURL(
    const juce::URL& url,
    const juce::int64 lengthInSamples,
    std::optional<utils::FileIdentity> identity
)
{
    sourceIdentity = std::move(identity);
    spectrogramData.reset();
    spectrogramRaster = {};
    spectrogramCompleted = false;
    pendingDuration = 0.0;
    pendingSampleRate = 0.0;
    sourceLengthInSamples = 0;
    sourceChannels = 0;
    scrubbing = false;
    pendingSeekTime.reset();
    scrubBounds = {};
    colouredWaveform.reset();
    loudnessHistory.reset();
    overloadHistory.reset();
    seratoOverview.reset();
    seratoMarkers.reset();
    hasNotifiedFullyLoaded = false;

    if (!url.isWellFormed()) {
        thumbnail->clear();
        visibleRange = {};
        repaint();
        return;
    }

    const auto detail = samplesPerThumbnailPoint(lengthInSamples);
    if (!sourceIdentity && url.isLocalFile()) {
        sourceIdentity.emplace(url.getLocalFile());
    }
    replaceThumbnail(detail);
    // Source-backed thumbnails start their own hidden decoder, so only the admitted scheduler may fill peaks.
    juce::ignoreUnused(url);

    if (sourceDuration() > 0.0) {
        setRange({0.0, sourceDuration()});
    } else {
        visibleRange = {};
        repaint();
    }
}

/// Replaces the thumbnail with an empty one of the given detail, moving the change listener over.
/// A thumbnail keeps its detail from creation or from the last cached data it loaded, so each file needs a new one.
void ThumbnailComponent::replaceThumbnail(const int samplesPerPoint)
{
    colouredWaveform.reset();
    thumbnail->removeChangeListener(this);
    thumbnail = std::make_unique<juce::AudioThumbnail>(samplesPerPoint, thumbnailFormats, thumbnailCache);
    thumbnail->addChangeListener(this);
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
void ThumbnailComponent::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source != thumbnail.get()) {
        return;
    }
    // Native thumbnail changes invalidate the memoized waveform drawing.
    colouredWaveform.reset();
    if (sourceDuration() > 0.0 && visibleRange.getLength() <= 0.0) {
        setRange({0.0, sourceDuration()});
    } else {
        repaint();
    }

    if (thumbnail->isFullyLoaded() && !hasNotifiedFullyLoaded) {
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

/// Latches the clicked pane and focuses the preview without interrupting playback or seeking.
void ThumbnailComponent::mouseDown(const juce::MouseEvent& event)
{
    popupGesture = event.mods.isPopupMenu();
    scrubbing = false;
    pendingSeekTime.reset();
    updateCursorPosition();
    if (popupGesture) {
        showViewModeMenu();
        return;
    }
    if (!event.mods.isLeftButtonDown()) {
        return;
    }
    const auto geometry = getPlotGeometry();
    scrubBounds = {};
    if (geometry.waveform.contains(event.getPosition())) {
        scrubBounds = geometry.waveform;
    } else if (geometry.spectrogram.contains(event.getPosition())) {
        scrubBounds = geometry.spectrogram;
    }
    if (scrubBounds.isEmpty() || sourceDuration() <= 0.0 || visibleRange.getLength() <= 0.0) {
        return;
    }
    if (isShowing()) {
        grabKeyboardFocus();
    }
    scrubRange = visibleRange;
    scrubbing = true;
    // The second press of a double click previews the restart position rather than a seek target.
    pendingSeekTime = event.getNumberOfClicks() >= 2
        ? 0.0
        : std::clamp(previewXToTime(event.position.x, scrubRange, scrubBounds), 0.0, sourceDuration());
    updateCursorPosition();
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
    if (!seratoMarkers || sourceDuration() <= 0.0) {
        return;
    }

    const auto cue = std::ranges::find_if(seratoMarkers->cues, [slot](const auto& candidate) {
        return static_cast<int>(candidate.index) == slot;
    });
    if (cue == seratoMarkers->cues.end()) {
        return;
    }

    const auto seconds = static_cast<double>(cue->positionMilliseconds) / utils::millisecondsPerSecond;
    transportSource.setPosition(juce::jlimit(0.0, sourceDuration(), seconds));
    transportSource.start();
    updateCursorPosition();
}

/// Restarts playback from the beginning of the file.
void ThumbnailComponent::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (!event.mods.isLeftButtonDown() || event.mods.isPopupMenu() || popupGesture || sourceDuration() <= 0.0) {
        return;
    }
    transportSource.setPosition(0.0);
    transportSource.start();
}

/// Moves only the grey release-target marker while actual playback continues independently.
void ThumbnailComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (popupGesture || event.mods.isPopupMenu() || !event.mods.isLeftButtonDown() || !scrubbing
        || event.getNumberOfClicks() >= 2)
    {
        return;
    }
    pendingSeekTime = std::clamp(previewXToTime(event.position.x, scrubRange, scrubBounds), 0.0, sourceDuration());
    updateCursorPosition();
}

/// Seeks once on release, preserving active playback and starting a stopped preview.
/// Starting an already playing preview would reposition and wait for read-ahead a second time.
/// Scrubbing identifies a left-button gesture, since a real release may have no button modifier.
/// The release that completes a double click leaves the restart to mouseDoubleClick, without seeking first.
void ThumbnailComponent::mouseUp(const juce::MouseEvent& event)
{
    const auto commit = scrubbing && !popupGesture && !event.mods.isPopupMenu() && event.getNumberOfClicks() < 2;
    if (commit) {
        const auto wasPlaying = transportSource.isPlaying();
        transportSource.setPosition(
            std::clamp(previewXToTime(event.position.x, scrubRange, scrubBounds), 0.0, sourceDuration())
        );
        if (!wasPlaying) {
            transportSource.start();
        }
    }
    popupGesture = false;
    scrubbing = false;
    pendingSeekTime.reset();
    scrubBounds = {};
    scrubRange = {};
    updateCursorPosition();
}

/// Routes trackpad magnification through the same owner-controlled zoom as the wheel.
void ThumbnailComponent::mouseMagnify(const juce::MouseEvent& event, const float scaleFactor)
{
    if (!popupGesture && !event.mods.isPopupMenu() && sourceDuration() > 0.0 && std::isfinite(scaleFactor)
        && scaleFactor > 0.0f && mouseWheelZoomCallback)
    {
        mouseWheelZoomCallback(static_cast<double>(scaleFactor * scaleFactor));
    }
}

/// Handles wheel gestures over the waveform.
/// Plain vertical wheel zooms, Ctrl/Cmd plus wheel adjusts gain by one step per event,
/// with a finer step when Shift is also held, and horizontal wheel scrolls the visible range.
void ThumbnailComponent::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (sourceDuration() <= 0.0 || popupGesture || event.mods.isPopupMenu()) {
        return;
    }

    // Ctrl on Windows, Cmd on macOS. Both map to commandModifier in JUCE.
    if (event.mods.isCommandDown() && mouseWheelGainCallback) {
        const auto fine = event.mods.isShiftDown();
        // macOS turns a vertical mouse wheel into a horizontal one while Shift is held,
        // so the fine gesture also reads the horizontal delta.
        auto delta = wheel.deltaY;
        if (fine && std::abs(delta) <= 0.0f) {
            delta = wheel.deltaX;
        }
        if (std::abs(delta) > 0.0f) {
            const auto direction = delta > 0.0f ? 1 : -1;
            if (!fine) {
                fineGainWheelEvents = 0;
                mouseWheelGainCallback(static_cast<float>(direction) * gain_range::wheelStepDecibels);
                return;
            }
            // Reversing the wheel starts the count again, so the gain never moves against the wheel.
            if (fineGainWheelEvents * direction < 0) {
                fineGainWheelEvents = 0;
            }
            fineGainWheelEvents += direction;
            if (std::abs(fineGainWheelEvents) >= gain_range::fineWheelEventsPerStep) {
                fineGainWheelEvents = 0;
                mouseWheelGainCallback(static_cast<float>(direction) * gain_range::wheelStepDecibels);
            }
            return;
        }
    }

    if (std::abs(wheel.deltaY) > 0.0f && mouseWheelZoomCallback) {
        const auto zoomFactor = std::pow(wheelZoomBase, static_cast<double>(wheel.deltaY) * wheelZoomMovementScale);
        mouseWheelZoomCallback(zoomFactor);
        return;
    }

    auto newStart = visibleRange.getStart()
        - static_cast<double>(wheel.deltaX) * visibleRange.getLength() * horizontalScrollStepRatio;
    newStart = juce::jlimit(0.0, juce::jmax(0.0, sourceDuration() - visibleRange.getLength()), newStart);
    setRange({newStart, newStart + visibleRange.getLength()});
}

/// Returns the waveform pane independently of spectral artifact readiness.
juce::Rectangle<int> ThumbnailComponent::waveformBounds() const
{
    return getPlotGeometry().waveform;
}

/// Uses source metadata while peaks are pending, without coupling spectral readiness to the thumbnail.
double ThumbnailComponent::sourceDuration() const
{
    return pendingDuration > 0.0 ? pendingDuration : thumbnail->getTotalLength();
}

/// Attaches source analysis on the message thread without changing playback, gain, or the viewport.
void ThumbnailComponent::setLoudnessHistory(std::shared_ptr<const ShortTermLoudnessHistory> history)
{
    if (loudnessHistory != history) {
        loudnessHistory = std::move(history);
        repaint();
    }
}

/// Changes display-only preferences and reprojects the cursor when the axis gutter changes.
void ThumbnailComponent::setLoudnessGraphSettings(const bool enabled, const double minimum, const double maximum)
{
    loudnessGraphEnabled = enabled;
    loudnessMinimum = minimum;
    loudnessMaximum = maximum;
    updateCursorPosition();
    repaint();
}

/// Converts source LUFS to a waveform ordinate without clamping out-of-range measurements.
float ThumbnailComponent::loudnessToY(const double value, const juce::Rectangle<int> bounds) const
{
    return static_cast<float>(bounds.getBottom())
        - static_cast<float>((value - loudnessMinimum) / (loudnessMaximum - loudnessMinimum))
        * static_cast<float>(bounds.getHeight());
}

/// Draws the axis even while analysis is pending, adding the source curve once complete history is available.
void ThumbnailComponent::drawLoudnessGraph(juce::Graphics& graphics) const
{
    const auto bounds = getPlotGeometry().loudness;
    if (!loudnessGraphEnabled || bounds.isEmpty() || visibleRange.getLength() <= 0.0) {
        return;
    }
    drawLoudnessAxis(graphics, bounds);
    if (loudnessHistory && !loudnessHistory->values.empty()) {
        drawLoudnessCurve(graphics, bounds);
    }
}

/// Fits uniformly spaced, zero-aligned ticks to the expanded or compact waveform height.
/// Custom limits clip the axis rather than introducing irregular endpoint intervals.
void ThumbnailComponent::drawLoudnessAxis(juce::Graphics& graphics, const juce::Rectangle<int> bounds) const
{
    const auto plot = bounds.toFloat();
    graphics.setFont(CustomLookAndFeel::getTextFont().withHeight(layout::waveform::loudnessFontHeight));
    const auto gutter = getPlotGeometry().loudnessAxis;
    const auto interval = layout::waveform::loudnessTickInterval(
        juce::roundToInt(loudnessMinimum), juce::roundToInt(loudnessMaximum), bounds.getHeight()
    );
    const auto firstTick = static_cast<int>(std::ceil(loudnessMinimum / interval)) * interval;
    for (int value = firstTick; value <= loudnessMaximum; value += interval) {
        const auto vertical = loudnessToY(value, bounds);
        const auto labelHeight = juce::roundToInt(layout::waveform::loudnessFontHeight);
        const auto labelTop = std::clamp(
            juce::roundToInt(vertical) - labelHeight / 2,
            gutter.getY(),
            std::max(gutter.getY(), gutter.getBottom() - labelHeight)
        );
        graphics.setColour(juce::Colours::white.withAlpha(loudnessLabelAlpha));
        graphics.drawText(
            utils::format("{}", value), gutter.withY(labelTop).withHeight(labelHeight), juce::Justification::centredLeft
        );
        graphics.setColour(juce::Colours::white.withAlpha(loudnessGridAlpha));
        graphics.drawHorizontalLine(juce::roundToInt(vertical), plot.getX(), plot.getRight());
    }
}

/// Draws nonempty history using time-ordered pixel-bucket extrema.
/// Undefined readings break the line, and the plot clip preserves out-of-range crossings.
void ThumbnailComponent::drawLoudnessCurve(juce::Graphics& graphics, const juce::Rectangle<int> bounds) const
{
    const auto plot = bounds.toFloat();
    const auto& history = *loudnessHistory;
    const auto interval = static_cast<double>(history.samplesPerPoint) / history.sampleRate;
    const auto lastIndex = history.values.size() - 1;
    const auto first = static_cast<std::size_t>(
        std::clamp(std::floor(visibleRange.getStart() / interval) - 1.0, 0.0, static_cast<double>(lastIndex))
    );
    const auto last = static_cast<std::size_t>(
        std::clamp(std::ceil(visibleRange.getEnd() / interval), 0.0, static_cast<double>(lastIndex))
    );
    juce::Path line;
    juce::Path segmentStarts;
    bool newSegment = true;
    std::array<std::size_t, 4> bucket {};
    bool bucketActive = false;
    int bucketColumn = 0;
    const auto flushBucket = [&] {
        if (!bucketActive) {
            return;
        }
        auto ordered = bucket;
        std::ranges::sort(ordered);
        const auto uniqueIndices = std::ranges::subrange(ordered.begin(), std::ranges::unique(ordered).begin());
        for (const auto index : uniqueIndices) {
            const auto horizontal = previewTimeToX(history.timeAt(index), visibleRange, bounds);
            const auto vertical = loudnessToY(static_cast<double>(history.values[index]), bounds);
            if (newSegment) {
                line.startNewSubPath(horizontal, vertical);
                const auto diameter = layout::waveform::loudnessLineWidth;
                segmentStarts.addEllipse(horizontal - diameter * 0.5f, vertical - diameter * 0.5f, diameter, diameter);
                newSegment = false;
            } else {
                line.lineTo(horizontal, vertical);
            }
        }
        bucketActive = false;
    };
    for (auto index = first; index <= last; ++index) {
        const auto value = history.values[index];
        if (!std::isfinite(value) || static_cast<double>(value) <= AudioAnalysisRecord::negativeInfinityLoudness) {
            flushBucket();
            newSegment = true;
            continue;
        }
        const auto column = static_cast<int>(std::clamp(
            std::floor(previewTimeToX(history.timeAt(index), visibleRange, bounds) - plot.getX()),
            -1.0f,
            plot.getWidth()
        ));
        if (bucketActive && column != bucketColumn) {
            flushBucket();
        }
        if (!bucketActive) {
            bucket.fill(index);
            bucketColumn = column;
            bucketActive = true;
        } else {
            if (value < history.values[bucket[1]]) {
                bucket[1] = index;
            }
            if (value > history.values[bucket[2]]) {
                bucket[2] = index;
            }
            bucket[3] = index;
        }
    }
    flushBucket();
    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.reduceClipRegion(bounds);
    graphics.setColour(juce::Colours::black.withAlpha(loudnessOutlineAlpha));
    graphics.strokePath(
        line,
        juce::PathStrokeType(
            layout::waveform::loudnessLineWidth + layout::waveform::loudnessOutlineWidth,
            juce::PathStrokeType::curved,
            juce::PathStrokeType::rounded
        )
    );
    graphics.setColour(juce::Colours::orangered);
    graphics.fillPath(segmentStarts);
    graphics.strokePath(
        line,
        juce::PathStrokeType(
            layout::waveform::loudnessLineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded
        )
    );
}

/// Keeps the playback cursor in sync with the display refresh.
/// Updates playback status before skipping hidden or idle drawing, and hides the marker once playback stops.
void ThumbnailComponent::updatePlaybackPosition()
{
    transportSource.updatePlaybackStatus();
    if (!isShowing() || (!transportSource.isPlaying() && !scrubbing && playheadBounds.empty())) {
        return;
    }

    updateCursorPosition();
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
    colouredWaveform.reset();
    seratoOverview = std::move(overview);
    repaint();
}

/// Reports the view preference independently of whether the current file contains spectral metadata.
bool ThumbnailComponent::isSeratoWaveformColouringEnabled() const noexcept
{
    return seratoWaveformColouringEnabled;
}

/// Changes only the spectral brush, retaining the overview so it can be restored without reloading audio.
void ThumbnailComponent::setSeratoWaveformColouringEnabled(const bool enabled)
{
    if (seratoWaveformColouringEnabled != enabled) {
        seratoWaveformColouringEnabled = enabled;
        colouredWaveform.reset();
        repaint();
    }
}

/// Attaches owned cue positions and colours without altering peaks, playback or the viewport.
/// Null data removes the arrows.
/// The owner must reject stale asynchronous results before adoption.
void ThumbnailComponent::setSeratoMarkers(std::shared_ptr<const serato::Markers> markers)
{
    seratoMarkers = std::move(markers);
    repaint();
}

/// Draws the inward-facing cue arrows over the waveform, below the child playback cursor.
void ThumbnailComponent::drawCueMarkers(juce::Graphics& graphics, const juce::Rectangle<int> bounds) const
{
    const auto cueArrows = getCueArrows(bounds);
    if (cueArrows.empty()) {
        return;
    }

    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.reduceClipRegion(bounds);
    for (const auto& cue : cueArrows) {
        juce::Path arrows;
        arrows.addTriangle(
            cue.horizontalPosition - cue.halfWidth,
            cue.top,
            cue.horizontalPosition + cue.halfWidth,
            cue.top,
            cue.horizontalPosition,
            cue.top + cue.height
        );
        arrows.addTriangle(
            cue.horizontalPosition - cue.halfWidth,
            cue.bottom,
            cue.horizontalPosition + cue.halfWidth,
            cue.bottom,
            cue.horizontalPosition,
            cue.bottom - cue.height
        );
        graphics.setColour(juce::Colours::black.withAlpha(cueArrowOutlineAlpha));
        graphics.strokePath(arrows, juce::PathStrokeType(layout::waveform::cueArrowOutlineWidth));
        graphics.setColour(cue.colour);
        graphics.fillPath(arrows);
    }
}

/// Applies a zoom factor to the waveform viewport.
void ThumbnailComponent::setZoom(const double zoomLevel)
{
    if (!std::isfinite(zoomLevel) || zoomLevel <= 0.0 || sourceDuration() <= 0.0) {
        return;
    }
    const auto start = transportSource.getCurrentPosition();
    const auto zoom = sourceDuration() / zoomLevel;
    const auto end = juce::jmin(sourceDuration(), start + zoom);
    if (start < sourceDuration() * zoomStartAnchorRatio || sourceDuration() - start > zoom) {
        setRange(juce::Range(start, end));
    } else {
        // The remaining distance is shorter than the zoom length, so zoom from the end instead.
        setRange(juce::Range(sourceDuration() - zoom, end));
    }
}
}  // namespace audiobatch
