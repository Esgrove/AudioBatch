/// Spectrogram artifact adoption, retained source raster rendering and pane-local frequency presentation.
/// The palette encodes source power only and is independent of gain, waveform colours and playback processing.

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/SpectrogramData.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/AudioPreviewPlayer.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/components/PreviewPlotGeometry.h"
#include "gui/components/ThumbnailComponent.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoTagTypes.h"
#include "utils/GuiUtils.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
/// Decimal places retain exact source Nyquist values in compact kHz captions.
constexpr int FREQUENCY_KILOHERTZ_DECIMALS {3};

/// Audited source-intensity palette anchors, ordered from silence to maximum calibrated power.
/// These literal raster data colours deliberately do not reference mutable interface theme tokens.
constexpr std::array<std::uint32_t, 9> SPECTROGRAM_PALETTE {
    0xff080b16,
    0xff111f55,
    0xff283c96,
    0xff6044ad,
    0xffa45183,
    0xffde704e,
    0xfff7aa45,
    0xffffdd83,
    0xfffff5d7,
};

/// Builds the fixed 256-entry intensity lookup once, outside artifact and repaint loops.
std::array<juce::Colour, 256> makeSpectrogramPalette()
{
    std::array<juce::Colour, 256> colours;
    for (std::size_t index = 0; index < colours.size(); ++index) {
        const auto position = static_cast<double>(spectrogramDisplayIntensity(static_cast<std::uint8_t>(index)))
            * static_cast<double>(SPECTROGRAM_PALETTE.size() - 1);
        const auto first = static_cast<std::size_t>(position);
        const auto last = std::min(first + 1, SPECTROGRAM_PALETTE.size() - 1);
        colours[index]
            = juce::Colour(SPECTROGRAM_PALETTE[first])
                  .interpolatedWith(
                      juce::Colour(SPECTROGRAM_PALETTE[last]), static_cast<float>(position - static_cast<double>(first))
                  );
    }
    return colours;
}
}  // namespace

/// Returns semantic plots shared by rendering, cue projection, hover and scrub hit testing.
PreviewPlotGeometry ThumbnailComponent::getPlotGeometry() const
{
    return previewPlotGeometry(getLocalBounds(), viewMode, loudnessGraphEnabled && sourceDuration() > 0.0);
}

/// Reports the preference even when the host is too small to draw a pane.
PreviewViewMode ThumbnailComponent::getViewMode() const noexcept
{
    return viewMode;
}

/// Reports retained-artifact availability without exposing or inspecting raster pixels.
bool ThumbnailComponent::hasSpectrogramRaster() const noexcept
{
    return spectrogramRaster.isValid();
}

/// Changes layout only, retaining artifacts and the shared viewport without touching the transport.
void ThumbnailComponent::setViewMode(const PreviewViewMode mode)
{
    if (viewMode == mode) {
        return;
    }
    viewMode = mode;
    colouredWaveform.reset();
    updateCursorPosition();
    repaint();
}

/// Connects user menu choices to the owner that persists and republishes preview preferences.
void ThumbnailComponent::setViewModeChangedCallback(std::function<void(PreviewViewMode)> callback)
{
    viewModeChangedCallback = std::move(callback);
}

/// Validates bounded immutable data and builds one full-grid image only when the artifact changes.
/// The owner must reject asynchronous results from a different source identity before calling this method.
void ThumbnailComponent::setSpectrogramData(std::shared_ptr<const SpectrogramData> data)
{
    if (data
        && (!isValidSpectrogramData(*data) || sourceLengthInSamples <= 0
            || data->lengthInSamples != sourceLengthInSamples
            || !juce::approximatelyEqual(static_cast<double>(data->sampleRate), pendingSampleRate)
            || (sourceChannels > 0 && data->channelCount != sourceChannels)))
    {
        data.reset();
    }
    if (spectrogramCompleted && spectrogramData == data) {
        return;
    }
    spectrogramCompleted = true;
    spectrogramData = std::move(data);
    spectrogramRaster = {};
    if (spectrogramData) {
        static const auto PALETTE = makeSpectrogramPalette();
        const auto& source = *spectrogramData;
        const auto columns = static_cast<int>(source.columnCount());
        spectrogramRaster = juce::Image(juce::Image::RGB, columns, SpectrogramData::FREQUENCY_ROWS, false);
        const juce::Image::BitmapData pixels(spectrogramRaster, juce::Image::BitmapData::writeOnly);
        for (int column = 0; column < columns; ++column) {
            for (int row = 0; row < SpectrogramData::FREQUENCY_ROWS; ++row) {
                const auto index = static_cast<std::size_t>(column) * SpectrogramData::FREQUENCY_ROWS
                    + static_cast<std::size_t>(row);
                pixels.setPixelColour(
                    column, SpectrogramData::FREQUENCY_ROWS - 1 - row, PALETTE[source.intensities[index]]
                );
            }
        }
    }
    repaint();
}

/// Draws a time-cropped full-grid image, preserving the final partial column's true sample footprint.
/// A separate source-duration clip prevents bucket padding from being stretched into the file.
void ThumbnailComponent::drawSpectrogram(juce::Graphics& graphics, const juce::Rectangle<int> bounds) const
{
    const juce::Graphics::ScopedSaveState savedState(graphics);
    graphics.reduceClipRegion(bounds);
    graphics.setColour(juce::Colour(SPECTROGRAM_PALETTE.front()));
    graphics.fillRect(bounds);
    if (!spectrogramData || !spectrogramRaster.isValid() || visibleRange.getLength() <= 0.0) {
        graphics.setColour(juce::Colours::white.withAlpha(layout::waveform::SPECTROGRAM_PLACEHOLDER_ALPHA));
        graphics.setFont(CustomLookAndFeel::getTextFont().withHeight(layout::waveform::PLACEHOLDER_FONT_HEIGHT));
        graphics.drawFittedText(
            spectrogramCompleted ? "Spectrogram unavailable" : "Computing spectrogram…",
            bounds,
            juce::Justification::centred,
            2
        );
        return;
    }
    const auto& source = *spectrogramData;
    const auto duration = static_cast<double>(source.lengthInSamples) / source.sampleRate;
    const auto gridDuration
        = static_cast<double>(source.samplesPerColumn) * static_cast<double>(source.columnCount()) / source.sampleRate;
    const auto start = previewTimeToX(0.0, visibleRange, bounds);
    const auto finish = previewTimeToX(duration, visibleRange, bounds);
    const auto gridFinish = previewTimeToX(gridDuration, visibleRange, bounds);
    graphics.reduceClipRegion(
        juce::Rectangle<float>(
            start, static_cast<float>(bounds.getY()), finish - start, static_cast<float>(bounds.getHeight())
        )
            .getSmallestIntegerContainer()
    );
    const auto scaleX = (gridFinish - start) / static_cast<float>(spectrogramRaster.getWidth());
    const auto nyquistMel = frequencyToMel(source.sampleRate * 0.5);
    const auto frequencyFraction
        = nyquistMel / frequencyToMel(std::max(source.sampleRate * 0.5, PREVIEW_FREQUENCIES.back()));
    const auto imageHeight = static_cast<float>(bounds.getHeight() * frequencyFraction);
    const auto imageTop = static_cast<float>(bounds.getBottom()) - imageHeight;
    const auto scaleY = imageHeight / static_cast<float>(spectrogramRaster.getHeight());
    graphics.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    graphics.drawImageTransformed(
        spectrogramRaster, juce::AffineTransform::scale(scaleX, scaleY).translated(start, imageTop)
    );
}

/// Draws fixed reference ticks across a Mel range extending to at least 20 kHz.
/// Occupied label bands suppress collisions rather than allowing compact layouts to overlap text.
void ThumbnailComponent::drawFrequencyAxis(juce::Graphics& graphics, const PreviewPlotGeometry& geometry) const
{
    if (geometry.frequencyAxis.isEmpty() || geometry.spectrogram.isEmpty()) {
        return;
    }
    const auto sampleRate = spectrogramData ? static_cast<double>(spectrogramData->sampleRate) : pendingSampleRate;
    if (sampleRate <= 0.0) {
        return;
    }
    const auto nyquist = sampleRate * 0.5;
    const auto maximumMel = frequencyToMel(std::max(nyquist, PREVIEW_FREQUENCIES.back()));
    const auto plot = geometry.spectrogram;
    const auto gutter = geometry.frequencyAxis;
    const auto labelHeight = juce::roundToInt(layout::waveform::LOUDNESS_FONT_HEIGHT);
    std::vector<juce::Range<int>> occupied;
    graphics.setFont(CustomLookAndFeel::getTextFont().withHeight(layout::waveform::LOUDNESS_FONT_HEIGHT));
    for (const auto frequency : PREVIEW_FREQUENCIES) {
        const auto vertical = static_cast<float>(plot.getBottom())
            - static_cast<float>(frequencyToMel(frequency) / maximumMel) * static_cast<float>(plot.getHeight());
        const auto top = std::clamp(
            juce::roundToInt(vertical) - labelHeight / 2,
            gutter.getY(),
            std::max(gutter.getY(), gutter.getBottom() - labelHeight)
        );
        const juce::Range<int> band(top, top + layout::waveform::FREQUENCY_TICK_SPACING);
        if (std::ranges::any_of(occupied, [&](const auto other) { return other.intersects(band); })) {
            continue;
        }
        occupied.push_back(band);
        const auto label = frequency >= 1000.0 ? utils::format(
                                                     "{}k",
                                                     juce::String(frequency / 1000.0, FREQUENCY_KILOHERTZ_DECIMALS)
                                                         .trimCharactersAtEnd("0")
                                                         .trimCharactersAtEnd(".")
                                                 )
                                               : utils::format("{:.0f}", frequency);
        graphics.setColour(juce::Colours::white.withAlpha(layout::waveform::FREQUENCY_LABEL_ALPHA));
        graphics.drawText(
            label,
            gutter.withY(top).withHeight(labelHeight),
            gutter.getX() >= plot.getRight() ? juce::Justification::centredLeft : juce::Justification::centredRight
        );
        graphics.setColour(juce::Colours::white.withAlpha(layout::waveform::FREQUENCY_GRID_ALPHA));
        graphics.drawHorizontalLine(
            juce::roundToInt(vertical), static_cast<float>(plot.getX()), static_cast<float>(plot.getRight())
        );
    }
}

/// Labels displayed magnitude on both halves of each linear-amplitude channel lane.
/// Without the loudness grid, subtle reference lines follow signed amplitude rather than clamped caption centres.
void ThumbnailComponent::drawWaveformAxis(juce::Graphics& graphics, const PreviewPlotGeometry& geometry) const
{
    graphics.setFont(CustomLookAndFeel::getTextFont().withHeight(layout::waveform::LOUDNESS_FONT_HEIGHT));
    graphics.setColour(juce::Colours::white.withAlpha(layout::waveform::FREQUENCY_LABEL_ALPHA));
    const auto justification = geometry.amplitudeAxis.getX() >= geometry.waveform.getRight()
        ? juce::Justification::centredLeft
        : juce::Justification::centredRight;
    for (const auto& label : previewAmplitudeLabels(geometry, sourceChannels)) {
        const auto text = utils::format("{:.0f}", label.decibels);
        graphics.setColour(juce::Colours::white.withAlpha(layout::waveform::FREQUENCY_LABEL_ALPHA));
        graphics.drawText(text, label.bounds, justification);
        if (!loudnessGraphEnabled) {
            const juce::Graphics::ScopedSaveState savedState(graphics);
            graphics.reduceClipRegion(geometry.waveform);
            graphics.setColour(juce::Colours::white.withAlpha(layout::waveform::AMPLITUDE_GRID_ALPHA));
            graphics.drawHorizontalLine(
                juce::roundToInt(label.vertical),
                static_cast<float>(geometry.waveform.getX()),
                static_cast<float>(geometry.waveform.getRight())
            );
        }
    }
}

/// Collects cue arrows from every visible pane in waveform-then-spectrogram order.
std::vector<CueArrows> ThumbnailComponent::getCueArrows() const
{
    const auto geometry = getPlotGeometry();
    auto arrows = getCueArrows(geometry.waveform);
    const auto spectralArrows = getCueArrows(geometry.spectrogram);
    arrows.insert(arrows.end(), spectralArrows.begin(), spectralArrows.end());
    return arrows;
}

/// Projects immutable cues into an explicitly chosen pane with the shared viewport.
/// The arrows shrink to a quarter of short plots, and cues beyond the source or viewport have no arrows.
std::vector<CueArrows> ThumbnailComponent::getCueArrows(const juce::Rectangle<int> bounds) const
{
    if (!seratoMarkers || bounds.isEmpty() || visibleRange.getLength() <= 0.0) {
        return {};
    }
    const auto arrowHeight
        = std::min(layout::waveform::CUE_ARROW_HEIGHT, static_cast<float>(bounds.getHeight()) / 4.0f);
    const auto halfWidth = std::min(
                               layout::waveform::CUE_ARROW_WIDTH,
                               arrowHeight * layout::waveform::CUE_ARROW_WIDTH / layout::waveform::CUE_ARROW_HEIGHT
                           )
        * 0.5f;
    std::vector<CueArrows> arrows;
    for (const auto& cue : seratoMarkers->cues) {
        const auto seconds = static_cast<double>(cue.positionMilliseconds) / utils::MILLISECONDS_PER_SECOND;
        if (seconds > sourceDuration() || seconds < visibleRange.getStart() || seconds > visibleRange.getEnd()) {
            continue;
        }
        arrows.push_back({
            .colour = cue.colour.withAlpha(1.0f),
            .horizontalPosition = previewTimeToX(seconds, visibleRange, bounds),
            .halfWidth = halfWidth,
            .height = arrowHeight,
            .top = static_cast<float>(bounds.getY()),
            .bottom = static_cast<float>(bounds.getBottom()),
        });
    }
    return arrows;
}

/// Keeps spectral plots and frequency gutters tooltip-free, retaining source-over help only on the waveform.
juce::String ThumbnailComponent::getTooltip()
{
    const auto geometry = getPlotGeometry();
    const auto pointer = getMouseXYRelative();
    if (geometry.spectrogram.contains(pointer) || geometry.frequencyAxis.contains(pointer)
        || viewMode == PreviewViewMode::spectrogram)
    {
        return {};
    }
    if ((showSampleOverloads || showTruePeakOverloads) && overloadHistory && overloadHistory->hasOverloads()
        && sourceDuration() > 0.0)
    {
        return "Source overs: solid yellow = sample > 0 dBFS, solid orange = true peak > 0 dBTP. Orange wins overlaps.";
    }
    return {};
}

/// Reports only the display target, never the source player's current playback position.
std::optional<double> ThumbnailComponent::getPendingSeekPosition() const noexcept
{
    return pendingSeekTime;
}

/// Paints the pending grey target behind the independently advancing white playback cursor.
void ThumbnailComponent::drawPlayheads(juce::Graphics& graphics) const
{
    if (pendingSeekTime) {
        drawPositionMarker(
            graphics,
            *pendingSeekTime,
            CustomLookAndFeel::greyMiddleLight().withAlpha(layout::waveform::SEEK_PREVIEW_ALPHA)
        );
    }
    if (transportSource.isPlaying() || scrubbing) {
        drawPositionMarker(graphics, playheadTime, juce::Colours::white.withAlpha(layout::waveform::PLAYHEAD_ALPHA));
    }
}

/// Projects a source time into both synchronized plots, leaving all axis gutters untouched.
void ThumbnailComponent::drawPositionMarker(
    juce::Graphics& graphics,
    const double time,
    const juce::Colour colour
) const
{
    const auto geometry = getPlotGeometry();
    graphics.setColour(colour);
    for (const auto plot : {geometry.waveform, geometry.spectrogram}) {
        const juce::Graphics::ScopedSaveState savedState(graphics);
        graphics.reduceClipRegion(plot);
        const auto horizontal = previewTimeToX(time, visibleRange, plot);
        graphics.fillRect(
            horizontal - layout::waveform::POSITION_MARKER_WIDTH * 0.5f,
            static_cast<float>(plot.getY()),
            layout::waveform::POSITION_MARKER_WIDTH,
            static_cast<float>(plot.getHeight())
        );
    }
}

/// Invalidates only old and new marker strips on display refresh, independently for each plot.
void ThumbnailComponent::updateCursorPosition()
{
    playheadTime = transportSource.getCurrentPosition();
    for (const auto bounds : playheadBounds) {
        repaint(bounds);
    }
    playheadBounds.clear();
    if (!transportSource.isPlaying() && !scrubbing) {
        return;
    }
    const auto geometry = getPlotGeometry();
    const std::array<std::optional<double>, 2> markerTimes {playheadTime, pendingSeekTime};
    for (const auto& time : markerTimes) {
        if (!time) {
            continue;
        }
        for (const auto plot : {geometry.waveform, geometry.spectrogram}) {
            if (plot.isEmpty()) {
                continue;
            }
            const auto horizontal = previewTimeToX(time.value_or(playheadTime), visibleRange, plot);
            const auto marker = juce::Rectangle<float>(
                                    horizontal - layout::waveform::POSITION_MARKER_WIDTH * 0.5f,
                                    static_cast<float>(plot.getY()),
                                    layout::waveform::POSITION_MARKER_WIDTH,
                                    static_cast<float>(plot.getHeight())
            )
                                    .getSmallestIntegerContainer()
                                    .expanded(1, 0)
                                    .getIntersection(plot);
            if (!marker.isEmpty()) {
                playheadBounds.push_back(marker);
                repaint(marker);
            }
        }
    }
}

/// Shows the same four persisted mode labels without making a popup gesture a seek or playback action.
void ThumbnailComponent::showViewModeMenu()
{
    juce::PopupMenu menu;
    constexpr std::array modes {
        PreviewViewMode::waveform,
        PreviewViewMode::spectrogram,
        PreviewViewMode::sideBySide,
        PreviewViewMode::stacked,
    };
    for (const auto mode : modes) {
        menu.addItem(static_cast<int>(mode) + 1, previewViewModeLabel(mode), true, mode == viewMode);
    }
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this),
        [safeThis = juce::Component::SafePointer<ThumbnailComponent>(this)](const int result) {
            if (safeThis == nullptr || result <= 0) {
                return;
            }
            const auto mode
                = utils::enumFromId<PreviewViewMode, PreviewViewMode::waveform, PreviewViewMode::stacked>(result - 1);
            if (mode && safeThis->viewModeChangedCallback) {
                safeThis->viewModeChangedCallback(*mode);
            }
        }
    );
}
}  // namespace audiobatch
