/// Implementation of the ThumbnailComponent waveform preview.
/// Covers waveform painting and cache load and save,
/// transport scrubbing through the mouse handlers,
/// wheel-based zoom and gain gestures,
/// and display-synchronized position marker updates during playback.

#include "gui/components/ThumbnailComponent.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/OverloadHistory.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/CustomLookAndFeel.h"
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
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// Alpha of the white playback position marker drawn over the waveform.
constexpr float positionMarkerAlpha {0.85f};

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

/// Number of points a waveform aims for, enough detail to zoom in on files of typical length.
constexpr juce::int64 thumbnailTargetPoints {16'384};

/// Finest waveform detail, in source samples per waveform point, used for short files.
constexpr int minimumSamplesPerThumbnailPoint {16};

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

/// A URL source whose in-memory peaks belong to a captured file version and detail rather than a path alone.
class VersionedThumbnailSource final : public juce::URLInputSource
{
public:
    /// Captures the immutable cache key used for the entire thumbnail read.
    VersionedThumbnailSource(const juce::URL& url, const juce::int64 key) : juce::URLInputSource(url), cacheKey(key) {}

    /// Returns the captured key without querying a possibly replaced file.
    [[nodiscard]] juce::int64 hashCode() const override
    {
        return cacheKey;
    }

private:
    const juce::int64 cacheKey;
};

/// Returns the source samples per waveform point for a file:
/// the power of two that gives it about thumbnailTargetPoints points, within the finest and coarsest detail.
/// A 30 second file gets 128 samples per point, and an unknown length the coarsest detail.
int samplesPerThumbnailPoint(const juce::int64 lengthInSamples)
{
    if (lengthInSamples <= 0) {
        return maximumSamplesPerThumbnailPoint;
    }
    auto samplesPerPoint = minimumSamplesPerThumbnailPoint;
    while (samplesPerPoint < maximumSamplesPerThumbnailPoint
           && lengthInSamples / samplesPerPoint > thumbnailTargetPoints)
    {
        samplesPerPoint *= 2;
    }
    return samplesPerPoint;
}
}  // namespace

/// Creates a thumbnail view backed by the shared transport source.
ThumbnailComponent::ThumbnailComponent(juce::AudioFormatManager& formatManager, AudioPreviewPlayer& source) :
    transportSource(source),
    thumbnailFormats(formatManager),
    thumbnail(std::make_unique<juce::AudioThumbnail>(maximumSamplesPerThumbnailPoint, formatManager, thumbnailCache))
{
    thumbnail->addChangeListener(this);

    currentPositionMarker.setFill(juce::Colours::white.withAlpha(positionMarkerAlpha));
    addAndMakeVisible(currentPositionMarkerComponent);
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

    if (thumbnail->getTotalLength() > 0.0) {
        const auto reduced = waveformBounds();
        if (!drawColouredWaveform(graphics, reduced)) {
            thumbnail->drawChannels(graphics, reduced, visibleRange.getStart(), visibleRange.getEnd(), displayGain);
        }
        drawOverloads(graphics);
        drawLoudnessGraph(graphics);
        drawCueMarkers(graphics);
    } else {
        graphics.setFont(CustomLookAndFeel::getTextFont().withHeight(layout::waveform::placeholderFontHeight));
        graphics.drawFittedText("No audio file selected", getLocalBounds(), juce::Justification::centred, 2);
    }

    if (hasKeyboardFocus(false)) {
        graphics.setColour(CustomLookAndFeel::blue().withAlpha(focusOutlineAlpha));
        graphics.drawRoundedRectangle(
            getLocalBounds().toFloat().reduced(layout::waveform::focusOutlineWidth / 2.0f),
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
            *seratoOverview, visibleRange, thumbnail->getTotalLength(), bounds, CustomLookAndFeel::blue()
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
    if (thumbnail->getTotalLength() <= 0.0) {
        thumbnail->clear();
        visibleRange = {};
        repaint();
        return false;
    }

    setRange({0.0, thumbnail->getTotalLength()});
    return true;
}

/// Returns the identity captured for the waveform, including after its decoder finished.
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
    if (!thumbnail->isFullyLoaded() || thumbnail->getTotalLength() <= 0.0) {
        return waveformData;
    }

    juce::MemoryOutputStream output(waveformData, false);
    thumbnail->saveTo(output);
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
/// The length of the file in samples chooses the waveform detail, the finest for short files.
void ThumbnailComponent::setURL(
    const juce::URL& url,
    const juce::int64 lengthInSamples,
    std::optional<utils::FileIdentity> identity
)
{
    sourceIdentity = std::move(identity);
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
    // The thumbnail takes ownership of the source, whose immutable hash prevents same-path stale hits.
    const auto key = sourceIdentity ? waveformCacheKey(*sourceIdentity, detail) : url.toString(false).hashCode64();
    thumbnail->setSource(new VersionedThumbnailSource(url, key));

    if (thumbnail->getTotalLength() > 0.0) {
        setRange({0.0, thumbnail->getTotalLength()});
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
    // The thumbnail reports each step of reading the file, and every step changes the waveform pixels.
    colouredWaveform.reset();
    if (thumbnail->getTotalLength() > 0.0 && visibleRange.getLength() <= 0.0) {
        setRange({0.0, thumbnail->getTotalLength()});
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

/// Focuses the waveform for its visual outline, then starts scrubbing through the drag handler.
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
    if (!seratoMarkers || thumbnail->getTotalLength() <= 0.0) {
        return;
    }

    const auto cue = std::ranges::find_if(seratoMarkers->cues, [slot](const auto& candidate) {
        return static_cast<int>(candidate.index) == slot;
    });
    if (cue == seratoMarkers->cues.end()) {
        return;
    }

    const auto seconds = static_cast<double>(cue->positionMilliseconds) / utils::millisecondsPerSecond;
    transportSource.setPosition(juce::jlimit(0.0, thumbnail->getTotalLength(), seconds));
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
/// Playback first fades out, and the position follows the cursor from the drag events after the fade has rendered,
/// since moving it during the fade would cut the fade off with a click.
void ThumbnailComponent::mouseDrag(const juce::MouseEvent& event)
{
    if (transportSource.isPlaying()) {
        transportSource.stop();
    }
    if (!transportSource.isStopping()) {
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
    if (thumbnail->getTotalLength() <= 0.0) {
        return;
    }

    // Ctrl on Windows/Linux, Cmd on macOS. Both map to commandModifier in JUCE.
    if (event.mods.isCommandDown() && std::abs(wheel.deltaY) > 0.0f && mouseWheelGainCallback) {
        constexpr float dbPerWheelNotch = 6.0f;
        mouseWheelGainCallback(wheel.deltaY * dbPerWheelNotch);
        return;
    }

    if (std::abs(wheel.deltaY) > 0.0f && mouseWheelZoomCallback) {
        const auto zoomFactor = std::pow(wheelZoomBase, static_cast<double>(wheel.deltaY) * wheelZoomMovementScale);
        mouseWheelZoomCallback(zoomFactor);
        return;
    }

    auto newStart = visibleRange.getStart()
        - static_cast<double>(wheel.deltaX) * visibleRange.getLength() * horizontalScrollStepRatio;
    newStart = juce::jlimit(0.0, juce::jmax(0.0, thumbnail->getTotalLength() - visibleRange.getLength()), newStart);
    setRange({newStart, newStart + visibleRange.getLength()});
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
    auto bounds = getLocalBounds().reduced(layout::waveform::contentInset);
    if (loudnessGraphEnabled && thumbnail->getTotalLength() > 0.0) {
        bounds.removeFromLeft(
            std::min(
                layout::waveform::loudnessAxisWidth + layout::waveform::loudnessAxisGap,
                std::max(0, bounds.getWidth() - 1)
            )
        );
    }
    return bounds;
}

/// Attaches source analysis on the message thread without changing playback, gain, or the viewport.
void ThumbnailComponent::setLoudnessHistory(std::shared_ptr<const ShortTermLoudnessHistory> history)
{
    if (loudnessHistory != history) {
        loudnessHistory = std::move(history);
        repaint();
    }
}

/// Attaches the source overloads on the message thread without changing playback, gain, or the viewport.
/// The marks describe the decoded source, so they stay unchanged when the gain or the plugin preview changes.
void ThumbnailComponent::setOverloadHistory(std::shared_ptr<const OverloadHistory> history)
{
    if (overloadHistory != history) {
        overloadHistory = std::move(history);
        repaint();
    }
}

/// Explains the amber marks while the file has source overloads, and shows no tooltip otherwise.
juce::String ThumbnailComponent::getTooltip()
{
    if (!overloadHistory || !overloadHistory->hasOverloads() || thumbnail->getTotalLength() <= 0.0) {
        return {};
    }
    return "Source overs: solid = sample > 0 dBFS, outlined = true peak > 0 dBTP.";
}

/// Returns the runs of overload marks inside the visible range, per channel lane in the waveform's lane layout.
/// Neighbouring columns with the same kind of mark merge into one run,
/// and a sample over hides a true peak over in the same column, since it is the stronger warning.
/// Returns nothing while the history and the waveform disagree on the channel count.
std::vector<OverloadMark> ThumbnailComponent::getOverloadMarks() const
{
    const auto bounds = waveformBounds();
    if (!overloadHistory || !overloadHistory->hasOverloads() || bounds.isEmpty() || visibleRange.getLength() <= 0.0
        || overloadHistory->channelCount != thumbnail->getNumChannels())
    {
        return {};
    }
    const auto channels = overloadHistory->channelCount;
    std::vector<OverloadMark> marks;
    for (int channel = 0; channel < channels; ++channel) {
        const auto columns = projectOverloadColumns(*overloadHistory, channel, visibleRange, bounds.getWidth());
        // The same integer lane split as AudioThumbnail::drawChannels.
        const int laneTopPixel = bounds.getY() + channel * bounds.getHeight() / channels;
        const int laneBottomPixel = bounds.getY() + (channel + 1) * bounds.getHeight() / channels;
        const auto laneTop = static_cast<float>(laneTopPixel);
        const auto laneBottom = static_cast<float>(laneBottomPixel);
        std::size_t column = 0;
        while (column < columns.size()) {
            const auto columnFlags = columns[column];
            if (columnFlags == 0) {
                ++column;
                continue;
            }
            const auto sampleOver = (columnFlags & OverloadHistory::sampleOver) != 0;
            auto end = column + 1;
            while (end < columns.size() && columns[end] != 0
                   && ((columns[end] & OverloadHistory::sampleOver) != 0) == sampleOver)
            {
                ++end;
            }
            auto start = static_cast<float>(bounds.getX()) + static_cast<float>(column);
            auto finish = static_cast<float>(bounds.getX()) + static_cast<float>(end);
            if (finish - start < layout::waveform::overloadMinimumWidth) {
                const auto centre = (start + finish) / 2.0f;
                start = centre - layout::waveform::overloadMinimumWidth / 2.0f;
                finish = centre + layout::waveform::overloadMinimumWidth / 2.0f;
            }
            marks.push_back({
                .horizontal = {start, finish},
                .laneTop = laneTop,
                .laneBottom = laneBottom,
                .sampleOver = sampleOver,
            });
            column = end;
        }
    }
    return marks;
}

/// Draws the source overloads in amber, distinct from the orange-red loudness curve.
/// A sample over gets a light tint over its lane and solid caps at both lane edges,
/// and a true peak over alone gets outlined caps, so the kinds differ in shape as well as colour,
/// also over Serato colours.
void ThumbnailComponent::drawOverloads(juce::Graphics& graphics) const
{
    const auto marks = getOverloadMarks();
    if (marks.empty()) {
        return;
    }
    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.reduceClipRegion(waveformBounds());
    const auto amber = CustomLookAndFeel::yellow();
    const auto capHeight = layout::waveform::overloadCapHeight;
    const auto outline = layout::waveform::overloadOutlineWidth;
    for (const auto& mark : marks) {
        const auto width = mark.horizontal.getLength();
        const auto laneHeight = mark.laneBottom - mark.laneTop;
        const auto height = std::min(capHeight, laneHeight / 4.0f);
        const juce::Rectangle<float> topCap(mark.horizontal.getStart(), mark.laneTop, width, height);
        const juce::Rectangle<float> bottomCap(mark.horizontal.getStart(), mark.laneBottom - height, width, height);
        if (mark.sampleOver) {
            graphics.setColour(amber.withAlpha(layout::waveform::overloadTintAlpha));
            graphics.fillRect(juce::Rectangle<float>(mark.horizontal.getStart(), mark.laneTop, width, laneHeight));
            graphics.setColour(amber);
            graphics.fillRect(topCap);
            graphics.fillRect(bottomCap);
        } else {
            graphics.setColour(amber);
            graphics.drawRect(topCap.reduced(outline / 2.0f), outline);
            graphics.drawRect(bottomCap.reduced(outline / 2.0f), outline);
        }
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
    const auto bounds = waveformBounds();
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
    auto gutter = getLocalBounds().reduced(layout::waveform::contentInset);
    gutter.setRight(std::max(gutter.getX(), bounds.getX() - layout::waveform::loudnessAxisGap));
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
            utils::format("{}", value),
            gutter.withY(labelTop).withHeight(labelHeight),
            juce::Justification::centredRight
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
            const auto horizontal = timeToX(history.timeAt(index));
            const auto vertical = loudnessToY(static_cast<double>(history.values[index]), bounds);
            if (newSegment) {
                line.startNewSubPath(horizontal, vertical);
                const auto diameter = layout::waveform::loudnessLineWidth;
                segmentStarts.addEllipse(horizontal - diameter / 2.0f, vertical - diameter / 2.0f, diameter, diameter);
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
        const auto column = static_cast<int>(
            std::clamp(std::floor(timeToX(history.timeAt(index)) - plot.getX()), -1.0f, plot.getWidth())
        );
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
    if (!isShowing()
        || (!transportSource.isPlaying() && !isMouseButtonDown() && !currentPositionMarkerComponent.isVisible()))
    {
        return;
    }

    updateCursorPosition();
}

/// Moves the playback position marker to the current transport time
/// and shows it only during playback or while the mouse is held down.
void ThumbnailComponent::updateCursorPosition()
{
    currentPositionMarkerComponent.setVisible(transportSource.isPlaying() || isMouseButtonDown());

    currentPositionMarker.setRectangle(
        juce::Rectangle<float>(
            timeToX(transportSource.getCurrentPosition()) - layout::waveform::positionMarkerWidth / 2.0f,
            0.0f,
            layout::waveform::positionMarkerWidth,
            static_cast<float>(getHeight())
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

/// Returns the arrows of the Serato cues inside the visible range, in the order of the cues.
/// Millisecond positions use the same seconds-based projection as scrubbing and the playhead.
/// The arrows shrink on a short waveform, so each one reaches at most a quarter of the way to the other edge.
/// Cues past the end of the track or outside the visible range have no arrows.
std::vector<CueArrows> ThumbnailComponent::getCueArrows() const
{
    const auto bounds = waveformBounds();
    if (!seratoMarkers || bounds.isEmpty() || visibleRange.getLength() <= 0.0) {
        return {};
    }

    const auto arrowHeight = std::min(layout::waveform::cueArrowHeight, static_cast<float>(bounds.getHeight()) / 4.0f);
    const auto halfWidth = std::min(
                               layout::waveform::cueArrowWidth,
                               arrowHeight * layout::waveform::cueArrowWidth / layout::waveform::cueArrowHeight
                           )
        / 2.0f;
    std::vector<CueArrows> arrows;
    for (const auto& cue : seratoMarkers->cues) {
        const auto seconds = static_cast<double>(cue.positionMilliseconds) / utils::millisecondsPerSecond;
        if (seconds > thumbnail->getTotalLength() || seconds < visibleRange.getStart()
            || seconds > visibleRange.getEnd())
        {
            continue;
        }
        arrows.push_back({
            .colour = cue.colour.withAlpha(1.0f),
            .horizontalPosition = timeToX(seconds),
            .halfWidth = halfWidth,
            .height = arrowHeight,
            .top = static_cast<float>(bounds.getY()),
            .bottom = static_cast<float>(bounds.getBottom()),
        });
    }
    return arrows;
}

/// Draws the inward-facing cue arrows over the waveform, below the child playback cursor.
void ThumbnailComponent::drawCueMarkers(juce::Graphics& graphics) const
{
    const auto cueArrows = getCueArrows();
    if (cueArrows.empty()) {
        return;
    }

    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.reduceClipRegion(waveformBounds());
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
    const auto start = transportSource.getCurrentPosition();
    const auto zoom = thumbnail->getTotalLength() / zoomLevel;
    const auto end = juce::jmin(thumbnail->getTotalLength(), start + zoom);
    if (start < thumbnail->getTotalLength() * zoomStartAnchorRatio || thumbnail->getTotalLength() - start > zoom) {
        setRange(juce::Range(start, end));
    } else {
        // The remaining distance is shorter than the zoom length, so zoom from the end instead.
        setRange(juce::Range(thumbnail->getTotalLength() - zoom, end));
    }
}
}  // namespace audiobatch
