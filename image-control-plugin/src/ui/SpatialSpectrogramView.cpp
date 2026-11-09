/// Implements SpatialSpectrogramView: the box around the GPU-drawn plot,
/// the grid and band overlays with their hover animation.
/// The mouse and its gestures are in SpatialSpectrogramViewInput.cpp.

#include "ui/SpatialSpectrogramView.h"

#include "analysis/SpatialAnalyzer.h"
#include "dsp/CrossoverDesign.h"
#include "Parameters.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SpectrogramParticles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Smallest highlight change that still counts as animating. Closer to the target, the highlight snaps to it.
constexpr float HIGHLIGHT_SNAP {0.002F};

/// Time constant of the hover animations, in seconds.
constexpr double HIGHLIGHT_SECONDS {0.08};
/// Opacity of the dark veil over a band that the solos of other bands mute.
constexpr float MUTED_BAND_ALPHA {0.55F};

/// Width of the position labels on the left, in pixels.
constexpr float POSITION_GUTTER {28.0F};
/// Height of the frequency labels at the bottom, in pixels.
constexpr float FREQUENCY_GUTTER {18.0F};
/// Horizontal distance within which a click grabs a crossover line, in pixels.
constexpr float GRAB_DISTANCE {6.0F};
/// Font height of the axis and band labels.
constexpr float LABEL_FONT_HEIGHT {11.0F};
/// Space between a line and its label, in pixels.
constexpr float LABEL_INSET {4.0F};
/// Width of a crossover label, in pixels.
constexpr float CROSSOVER_LABEL_WIDTH {70.0F};
/// Width of a frequency axis label, in pixels.
constexpr float FREQUENCY_LABEL_WIDTH {40.0F};
/// Frequencies with a grid line and a label.
constexpr std::array<double, 10> GRID_FREQUENCIES {20, 50, 100, 200, 500, 1000, 2000, 5000, 10'000, 20'000};
/// Stereo positions with a grid line, from full left to full right.
constexpr std::array<double, 5> GRID_POSITIONS {1.0, 0.5, 0.0, -0.5, -1.0};
/// Labels of the grid positions.
constexpr std::array<const char*, 5> GRID_POSITION_LABELS {"L", "", "C", "", "R"};
/// Index of the centre line in GRID_POSITIONS.
constexpr std::size_t CENTRE_POSITION_INDEX {2};

/// Opacity of the grid lines.
constexpr float GRID_ALPHA {0.1F};
/// Opacity of the centre line.
constexpr float CENTRE_LINE_ALPHA {0.22F};
/// Opacity of the axis labels.
constexpr float AXIS_LABEL_ALPHA {0.55F};
/// Opacity of the band names.
constexpr float BAND_LABEL_ALPHA {0.7F};
/// Opacity at the top of the colour gradient over a hovered band.
constexpr float BAND_HOVER_ALPHA {0.14F};
/// Opacity of a crossover line at rest.
constexpr float CROSSOVER_ALPHA {0.4F};
/// Thickness of a crossover line at rest, in pixels.
constexpr float CROSSOVER_THICKNESS {1.0F};
/// Thickness of a fully hovered crossover line, in pixels.
constexpr float CROSSOVER_ACTIVE_THICKNESS {2.2F};
/// Thickness of the widest glow layer around a fully hovered crossover line, in pixels.
constexpr float CROSSOVER_GLOW_THICKNESS {16.0F};
/// Opacity of the innermost glow layer around a fully hovered crossover line.
constexpr float CROSSOVER_GLOW_ALPHA {0.3F};
/// Number of glow layers around a hovered crossover line.
constexpr int CROSSOVER_GLOW_LAYERS {4};
/// Opacity of a crossover frequency label at rest.
constexpr float CROSSOVER_LABEL_ALPHA {0.55F};
/// Height of a full band response curve, as a share of the plot height.
constexpr float RESPONSE_HEIGHT_SHARE {0.8F};
/// Horizontal distance between the points of a band response curve, in pixels.
constexpr float RESPONSE_STEP_PIXELS {2.0F};
/// Opacity at the top of the fill under the hovered band's response curve.
constexpr float RESPONSE_FILL_ALPHA {0.18F};
/// Opacity of the hovered band's response curve.
constexpr float RESPONSE_LINE_ALPHA {0.85F};
/// Opacity of the other bands' response curves while a band is hovered.
constexpr float NEIGHBOUR_RESPONSE_ALPHA {0.22F};
/// Thickness of a band response curve, in pixels.
constexpr float RESPONSE_LINE_THICKNESS {1.5F};
/// Space around the text of a label pill, in pixels.
constexpr float PILL_PADDING {4.0F};
/// Opacity of the background of a label pill.
constexpr float PILL_BACKGROUND_ALPHA {0.7F};

/// Width of the frame rate readout, in pixels.
constexpr float FPS_LABEL_WIDTH {150.0F};
/// Opacity of the rendering status text in the middle of the plot.
constexpr float STATUS_ALPHA {0.45F};
/// Font height of the rendering status text.
constexpr float STATUS_FONT_HEIGHT {14.0F};
/// Seconds the position curve overlay stays fully visible after the curve changes.
constexpr double CURVE_HOLD_SECONDS {2.5};
/// Seconds the position curve overlay takes to fade out after it was shown.
constexpr double CURVE_FADE_SECONDS {0.5};
/// Level differences with a guide line in the position curve overlay, in dB.
constexpr std::array<float, 7> CURVE_GUIDE_DIFFERENCES {3.0F, 6.0F, 12.0F, 18.0F, 24.0F, 36.0F, 48.0F};
/// Level difference at the right edge of the drawn curve, as a multiple of the pan edge setting.
constexpr float CURVE_RANGE_PER_EDGE {1.5F};
/// Distance between the dB ticks along the centre line of the drawn curve, in dB.
constexpr float CURVE_TICK_DECIBELS {6.0F};
/// Most dB ticks along the centre line. Wider curves tick at multiples of CURVE_TICK_DECIBELS.
constexpr int CURVE_MAX_TICKS {10};
/// Opacity of the guide lines of the position curve overlay.
constexpr float CURVE_GUIDE_ALPHA {0.3F};
/// Opacity of the guide lines at the pan midpoint and edge.
constexpr float CURVE_SETTING_ALPHA {0.8F};
/// Opacity of the drawn curve.
constexpr float CURVE_LINE_ALPHA {0.9F};
/// Thickness of the drawn curve, in pixels.
constexpr float CURVE_LINE_THICKNESS {2.0F};
/// Horizontal distance between the points of the drawn curve, in pixels.
constexpr float CURVE_STEP_PIXELS {2.0F};
/// Width of the dB labels of the guide lines at the right of the plot, in pixels.
constexpr float CURVE_LABEL_WIDTH {56.0F};

/// Corner radius of the spectrogram box, matching the band strips, in pixels.
constexpr float BOX_CORNER_RADIUS {6.0F};
/// Space around the plot inside the box, in pixels.
constexpr float BOX_PADDING {6.0F};

/// Returns the colour of a band: the frequency colour at its centre on the logarithmic axis.
juce::Colour bandColour(const std::size_t band, const Crossovers& crossovers)
{
    const double low = band == 0 ? static_cast<double>(MIN_FREQUENCY) : static_cast<double>(crossovers.at(band - 1));
    const double high
        = band == CROSSOVER_COUNT ? static_cast<double>(MAX_FREQUENCY) : static_cast<double>(crossovers.at(band));
    const double share = frequencyToShare(std::sqrt(low * high), FrequencyScale::logarithmic);
    const LinearColour colour = toSrgb(frequencyColour(static_cast<float>(share)));
    return juce::Colour::fromFloatRGBA(colour.red, colour.green, colour.blue, 1.0F);
}

/// Paints a short text in a rounded pill centred on a point, faded by an opacity.
void paintLabelPill(
    juce::Graphics& graphics,
    const juce::String& text,
    const juce::Point<float> centre,
    const float opacity,
    const juce::Colour textColour = juce::Colour(palette::GREY_LIGHT)
)
{
    const auto font = textFont(LABEL_FONT_HEIGHT);
    const float width = juce::GlyphArrangement::getStringWidth(font, text) + PILL_PADDING * 2.0F;
    const float height = LABEL_FONT_HEIGHT + PILL_PADDING;
    const auto pill = juce::Rectangle<float>(width, height).withCentre(centre);
    graphics.setColour(juce::Colour(palette::GREY_DARK).withAlpha(PILL_BACKGROUND_ALPHA * opacity));
    graphics.fillRoundedRectangle(pill, height * 0.5F);
    graphics.setColour(textColour.withAlpha(opacity));
    graphics.setFont(font);
    graphics.drawText(text, pill, juce::Justification::centred);
}

/// Formats a grid frequency, such as 50 or 2k.
juce::String formatGridFrequency(const double frequency)
{
    const auto kilohertz = static_cast<double>(HERTZ_PER_KILOHERTZ);
    if (frequency >= kilohertz) {
        return juce::String(juce::roundToInt(frequency / kilohertz)) + "k";
    }
    return juce::String(juce::roundToInt(frequency));
}

/// Moves an animated value towards its target with a time constant, and snaps it to the target when close,
/// so a settled highlight stops asking for repaints.
float approach(const float value, const float target, const double seconds)
{
    const auto share = static_cast<float>(1.0 - std::exp(-seconds / HIGHLIGHT_SECONDS));
    const float next = value + (target - value) * share;
    return std::abs(target - next) < HIGHLIGHT_SNAP ? target : next;
}

}  // namespace

/// Compares the visible overlay values within floating-point rounding precision to avoid redundant repaints.
bool SpectrogramOverlayState::operator==(const SpectrogramOverlayState& other) const
{
    const auto approximatelyEqual
        = [](const auto first, const auto second) { return juce::approximatelyEqual(first, second); };
    return positionCurve == other.positionCurve
        && std::ranges::equal(bandHighlights, other.bandHighlights, approximatelyEqual)
        && std::ranges::equal(crossoverHighlights, other.crossoverHighlights, approximatelyEqual)
        && std::ranges::equal(crossovers, other.crossovers, approximatelyEqual)
        && std::ranges::equal(bandWidths, other.bandWidths, approximatelyEqual) && mutedBands == other.mutedBands
        && juce::approximatelyEqual(positionCurveOpacity, other.positionCurveOpacity)
        && renderingBypassed == other.renderingBypassed && renderingUnavailable == other.renderingUnavailable;
}

/// Creates the view, looking up the parameters it reads and changes,
/// and keeping the editor settings for the Link toggles that its mid gain gestures follow.
SpatialSpectrogramView::SpatialSpectrogramView(
    juce::AudioProcessorValueTreeState& parameters,
    juce::ValueTree editorState,
    std::optional<std::reference_wrapper<ParameterHistory>> localHistory
) :
    editorSettings(std::move(editorState)),
    history(localHistory)
{
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        crossoverParameters.at(crossover) = parameters.getParameter(crossoverParameterId(crossover));
        jassert(crossoverParameters.at(crossover) != nullptr);
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        midParameters.at(band) = parameters.getParameter(midGainParameterId(band));
        sideParameters.at(band) = parameters.getParameter(sideGainParameterId(band));
        soloParameters.at(band) = parameters.getParameter(soloParameterId(band));
        widthParameters.at(band) = parameters.getParameter(widthParameterId(band));
        jassert(
            midParameters.at(band) != nullptr && sideParameters.at(band) != nullptr
            && soloParameters.at(band) != nullptr && widthParameters.at(band) != nullptr
        );
    }
    // The rounded corners show the editor background and the plot shows the GPU picture,
    // so the view does not fill its whole bounds.
    setOpaque(false);
    shownOverlay = currentOverlayState();
}

/// Ends any outstanding host gestures while the processor's parameters still exist.
SpatialSpectrogramView::~SpatialSpectrogramView()
{
    endDrag();
}

/// Returns the area of the spectrogram inside the axis labels.
juce::Rectangle<float> SpatialSpectrogramView::getPlotBounds() const
{
    // Half a label of room above and to the right keeps the top position label and the last frequency label whole.
    return getLocalBounds()
        .toFloat()
        .reduced(BOX_PADDING)
        .withTrimmedLeft(POSITION_GUTTER - BOX_PADDING)
        .withTrimmedBottom(FREQUENCY_GUTTER - BOX_PADDING)
        .withTrimmedTop(LABEL_FONT_HEIGHT * 0.5F)
        .withTrimmedRight(FREQUENCY_LABEL_WIDTH * 0.5F - BOX_PADDING);
}

/// Returns the band under the pointer, or the band being dragged, if any.
std::optional<std::size_t> SpatialSpectrogramView::getHoveredBand() const
{
    return hoveredBand;
}

/// Advances the hover highlights by the time since the last display refresh,
/// and repaints the overlays when what they show changed.
void SpatialSpectrogramView::advance(const double seconds)
{
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float target = hoveredBand == band ? 1.0F : 0.0F;
        bandHighlights.at(band) = approach(bandHighlights.at(band), target, seconds);
    }
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const bool movedWithBand = drag == SpectrogramDrag::band && draggedBand.has_value()
            && (*draggedBand == crossover || *draggedBand == crossover + 1);
        const bool active = hoveredCrossover == crossover || draggedCrossover == crossover || movedWithBand;
        crossoverHighlights.at(crossover) = approach(crossoverHighlights.at(crossover), active ? 1.0F : 0.0F, seconds);
    }
    positionCurveAge += seconds;

    const SpectrogramOverlayState overlay = currentOverlayState();
    if (overlay != shownOverlay) {
        shownOverlay = overlay;
        repaint();
    }
}

/// Sets the frequency scale of the axis, the grid, and the crossover lines.
void SpatialSpectrogramView::setFrequencyScale(const FrequencyScale newScale)
{
    if (scale != newScale) {
        scale = newScale;
        repaint();
    }
}

/// Sets the stereo position curve the particles are placed with.
/// A change after the first call shows the curve overlay, which fades out after a few seconds,
/// so adjusting the curve settings shows where each level difference lands.
void SpatialSpectrogramView::setPositionCurve(const StereoPositionCurve& curve)
{
    if (positionCurveSet && curve == positionCurve) {
        return;
    }
    if (positionCurveSet) {
        positionCurveAge = 0.0;
    }
    positionCurve = curve;
    positionCurveSet = true;
}

/// Sets the display refresh rate and the GPU time per frame to show in the top-left corner, or hides them.
/// Only the readout is repainted, and only when its text changes.
void SpatialSpectrogramView::setFrameStatistics(
    const std::optional<double> rate,
    const std::optional<double> gpuMilliseconds
)
{
    const auto rounded = [](const std::optional<double> value, const double steps) {
        return value.has_value() ? std::optional<int>(juce::roundToInt(*value * steps)) : std::nullopt;
    };
    const bool changed = rounded(rate, 1.0) != rounded(framesPerSecond, 1.0)
        || rounded(gpuMilliseconds, 10.0) != rounded(gpuFrameMilliseconds, 10.0);
    framesPerSecond = rate;
    gpuFrameMilliseconds = gpuMilliseconds;
    if (changed) {
        repaint(framesPerSecondArea().getSmallestIntegerContainer());
    }
}

/// Stops or resumes drawing the particles. The view shows that rendering is bypassed while it is.
void SpatialSpectrogramView::setRenderingBypassed(const bool bypassed)
{
    renderingBypassed = bypassed;
}

/// Tells the view that the GPU renderer could not start, so it can say so in the plot.
void SpatialSpectrogramView::setRenderingUnavailable(const bool unavailable)
{
    renderingUnavailable = unavailable;
}

/// The plot itself is left transparent, so the particles the GPU renderer draws under the view show through.
void SpatialSpectrogramView::paint(juce::Graphics& graphics)
{
    const auto plot = getPlotBounds();
    juce::Path plotOutline;
    plotOutline.addRoundedRectangle(plot, SPECTROGRAM_PLOT_CORNER_RADIUS);

    // The box with the rounded plot cut out of it.
    juce::Path box;
    box.addRoundedRectangle(getLocalBounds().toFloat(), BOX_CORNER_RADIUS);
    box.addPath(plotOutline);
    box.setUsingNonZeroWinding(false);
    graphics.setColour(juce::Colour(palette::GREY_SEMI_DARK));
    graphics.fillPath(box);

    {
        const juce::Graphics::ScopedSaveState clip(graphics);
        graphics.reduceClipRegion(plotOutline);
        paintGrid(graphics);
        const Crossovers crossovers = currentCrossovers();
        paintBands(graphics, crossovers);
        paintBandResponses(graphics, crossovers);
        paintCrossovers(graphics, crossovers);
        paintPositionCurve(graphics);
        paintRenderingStatus(graphics);
    }
    paintAxisLabels(graphics);
    paintFramesPerSecond(graphics);
}

/// Returns the band containing a plot x coordinate, or no band outside the plot's horizontal bounds.
std::optional<std::size_t> SpatialSpectrogramView::bandAt(const float x) const
{
    const auto plot = getPlotBounds();
    if (x < plot.getX() || x > plot.getRight()) {
        return std::nullopt;
    }
    const Crossovers crossovers = currentCrossovers();
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        if (x < frequencyToX(static_cast<double>(crossovers.at(crossover)))) {
            return crossover;
        }
    }
    return BAND_COUNT - 1;
}

/// Returns the crossover whose line is within GRAB_DISTANCE of an x coordinate, if any.
std::optional<std::size_t> SpatialSpectrogramView::crossoverNear(const float x) const
{
    const Crossovers crossovers = currentCrossovers();
    std::optional<std::size_t> nearest;
    float nearestDistance = GRAB_DISTANCE;
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const float distance = std::abs(frequencyToX(static_cast<double>(crossovers.at(crossover))) - x);
        if (distance <= nearestDistance) {
            nearest = crossover;
            nearestDistance = distance;
        }
    }
    return nearest;
}

/// Returns the crossovers as the processor applies them.
Crossovers SpatialSpectrogramView::currentCrossovers() const
{
    Crossovers crossovers {};
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        crossovers.at(crossover) = plainValue(*crossoverParameters.at(crossover));
    }
    return orderCrossovers(crossovers);
}

/// Returns what the overlays show now: the hover highlights, the crossovers, the band widths, the muted bands,
/// the position curve overlay, and the rendering status.
SpectrogramOverlayState SpatialSpectrogramView::currentOverlayState() const
{
    SpectrogramOverlayState state {
        .positionCurve = positionCurve,
        .bandHighlights = bandHighlights,
        .crossoverHighlights = crossoverHighlights,
        .crossovers = currentCrossovers(),
        .mutedBands = mutedBands(),
        .positionCurveOpacity = positionCurveOpacity(),
        .renderingBypassed = renderingBypassed,
        .renderingUnavailable = renderingUnavailable,
    };
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        state.bandWidths.at(band) = bandWidth(band);
    }
    return state;
}

/// Returns which bands the solos mute: the bands that are not soloed while some but not all bands are soloed.
std::array<bool, BAND_COUNT> SpatialSpectrogramView::mutedBands() const
{
    BandSolos soloed {};
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        soloed.at(band) = soloParameters.at(band)->getValue() >= 0.5F;
    }
    const BandGains heard = soloGains(soloed);
    std::array<bool, BAND_COUNT> muted {};
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        muted.at(band) = heard.at(band) < 0.5F;
    }
    return muted;
}

/// Returns the area of the frame rate readout in the top-left corner of the plot.
juce::Rectangle<float> SpatialSpectrogramView::framesPerSecondArea() const
{
    const auto plot = getPlotBounds();
    return {plot.getX() + LABEL_INSET, plot.getY() + LABEL_INSET, FPS_LABEL_WIDTH, LABEL_FONT_HEIGHT};
}

/// Returns the band's independent Width target, regardless of its source trims.
double SpatialSpectrogramView::bandWidth(const std::size_t band) const
{
    return static_cast<double>(plainValue(*widthParameters.at(band)));
}

/// Returns the opacity of the position curve overlay:
/// full for CURVE_HOLD_SECONDS after the curve changes, then fading out over CURVE_FADE_SECONDS.
float SpatialSpectrogramView::positionCurveOpacity() const
{
    if (positionCurveAge <= CURVE_HOLD_SECONDS) {
        return 1.0F;
    }
    return static_cast<float>(std::max(0.0, 1.0 - (positionCurveAge - CURVE_HOLD_SECONDS) / CURVE_FADE_SECONDS));
}

/// Returns the x coordinate of a frequency in the plot.
float SpatialSpectrogramView::frequencyToX(const double frequency) const
{
    const auto plot = getPlotBounds();
    return plot.getX() + static_cast<float>(frequencyToShare(frequency, scale)) * plot.getWidth();
}

/// Returns the y coordinate of a stereo position in the plot, full left at the top.
float SpatialSpectrogramView::positionToY(const double position) const
{
    const auto plot = getPlotBounds();
    const double share = (1.0 - position) * 0.5;
    return plot.getY() + static_cast<float>(share) * plot.getHeight();
}

/// Returns the frequency at an x coordinate in the plot.
double SpatialSpectrogramView::xToFrequency(const float x) const
{
    const auto plot = getPlotBounds();
    return shareToFrequency(static_cast<double>((x - plot.getX()) / plot.getWidth()), scale);
}

/// Paints the band regions: a dark veil over the bands the solos mute,
/// a soft gradient in the band's colour over the hovered band,
/// the band names, and the width of the hovered band in a label pill.
/// Only visible highlights request the response cache, so idle crossover automation builds no geometry.
void SpatialSpectrogramView::paintBands(juce::Graphics& graphics, const Crossovers& crossovers)
{
    const auto plot = getPlotBounds();
    graphics.setFont(textFont(LABEL_FONT_HEIGHT));

    const auto muted = mutedBands();
    float left = plot.getX();
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float right
            = band < CROSSOVER_COUNT ? frequencyToX(static_cast<double>(crossovers.at(band))) : plot.getRight();
        const float highlight = bandHighlights.at(band);
        const juce::Rectangle<float> region(left, plot.getY(), right - left, plot.getHeight());
        if (muted.at(band)) {
            graphics.setColour(juce::Colour(palette::GREY_DARK).withAlpha(MUTED_BAND_ALPHA));
            graphics.fillRect(region);
        }
        if (highlight > 0.0F) {
            const auto colour = bandResponses(crossovers).colours.at(band);
            graphics.setGradientFill(
                juce::ColourGradient(
                    colour.withAlpha(BAND_HOVER_ALPHA * highlight),
                    region.getTopLeft(),
                    colour.withAlpha(0.0F),
                    region.getBottomLeft(),
                    false
                )
            );
            graphics.fillRect(region);
        }

        const juce::Rectangle<float> nameArea(left, plot.getY() + LABEL_INSET, right - left, LABEL_FONT_HEIGHT);
        graphics.setColour(
            juce::Colour(palette::GREY_LIGHT).withAlpha(BAND_LABEL_ALPHA + (1.0F - BAND_LABEL_ALPHA) * highlight)
        );
        graphics.drawText(bandName(band), nameArea, juce::Justification::centred);
        if (highlight > 0.0F) {
            const auto widthText = "Width " + juce::String(bandWidth(band), 2);
            const auto widthArea = nameArea.translated(0.0F, LABEL_FONT_HEIGHT + LABEL_INSET * 2.0F);
            paintLabelPill(graphics, widthText, widthArea.getCentre(), highlight);
        }
        left = right;
    }
}

/// Rebuilds fixed response fills, stroke outlines, and band colours only when their projection changes.
/// Each sampled frequency evaluates the three lowpasses once and derives all four complementary responses.
const SpectrogramBandResponses& SpatialSpectrogramView::bandResponses(const Crossovers& crossovers)
{
    const auto plot = getPlotBounds();
    if (cachedBandResponses.has_value() && cachedBandResponses->plot == plot && cachedBandResponses->scale == scale
        && sameCrossovers(cachedBandResponses->crossovers, crossovers))
    {
        return *cachedBandResponses;
    }

    auto& responses = cachedBandResponses.emplace();
    responses.plot = plot;
    responses.scale = scale;
    responses.crossovers = crossovers;
    ++bandResponseBuilds;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        responses.colours.at(band) = bandColour(band, crossovers);
    }
    if (plot.isEmpty()) {
        return responses;
    }

    const float baseline = plot.getBottom();
    const float height = plot.getHeight() * RESPONSE_HEIGHT_SHARE;
    const auto appendPoint = [&](const float pixel, const bool first) {
        const double frequency = xToFrequency(pixel);
        std::array<double, BAND_COUNT + 1> lowpasses {};
        for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
            lowpasses.at(crossover + 1)
                = crossoverLowpassMagnitude(frequency, static_cast<double>(crossovers.at(crossover)));
        }
        lowpasses.back() = 1.0;
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            const auto response = static_cast<float>(lowpasses.at(band + 1) - lowpasses.at(band));
            const float level = baseline - response * height;
            auto& curve = responses.fills.at(band);
            if (first) {
                curve.startNewSubPath(pixel, level);
            } else {
                curve.lineTo(pixel, level);
            }
        }
    };
    appendPoint(plot.getX(), true);
    const int steps = static_cast<int>(std::ceil(plot.getWidth() / RESPONSE_STEP_PIXELS));
    for (int step = 1; step < steps; ++step) {
        appendPoint(plot.getX() + static_cast<float>(step) * RESPONSE_STEP_PIXELS, false);
    }
    // Include the exact right endpoint even when the width is not a multiple of the sample spacing.
    appendPoint(plot.getRight(), false);
    const juce::PathStrokeType stroke(RESPONSE_LINE_THICKNESS, juce::PathStrokeType::curved);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        auto& fill = responses.fills.at(band);
        stroke.createStrokedPath(responses.strokes.at(band), fill);
        fill.lineTo(plot.getRight(), baseline);
        fill.lineTo(plot.getX(), baseline);
        fill.closeSubPath();
    }
    return responses;
}

/// Paints the response of every band as a curve while a band is hovered, so their overlaps at the crossovers show:
/// the hovered band brightest with a soft fill under its curve, and the other bands as faint lines.
/// A band's response is the target magnitude of its crossover filters, one at full height.
void SpatialSpectrogramView::paintBandResponses(juce::Graphics& graphics, const Crossovers& crossovers)
{
    const float strongest = *std::ranges::max_element(bandHighlights);
    if (strongest <= 0.0F) {
        return;
    }
    const auto& responses = bandResponses(crossovers);
    const auto plot = responses.plot;
    const float baseline = plot.getBottom();
    const float height = plot.getHeight() * RESPONSE_HEIGHT_SHARE;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const auto colour = responses.colours.at(band);
        const float highlight = bandHighlights.at(band);
        if (highlight > 0.0F) {
            graphics.setGradientFill(
                juce::ColourGradient(
                    colour.withAlpha(RESPONSE_FILL_ALPHA * highlight),
                    {plot.getX(), baseline - height},
                    colour.withAlpha(0.0F),
                    {plot.getX(), baseline},
                    false
                )
            );
            graphics.fillPath(responses.fills.at(band));
        }
        const float alpha
            = NEIGHBOUR_RESPONSE_ALPHA * strongest + (RESPONSE_LINE_ALPHA - NEIGHBOUR_RESPONSE_ALPHA) * highlight;
        graphics.setColour(colour.withAlpha(alpha));
        graphics.fillPath(responses.strokes.at(band));
    }
}

/// Paints the crossover lines, faint at rest, with a layered glow and a brighter line while hovered or dragged,
/// and their frequencies in label pills that brighten with them.
void SpatialSpectrogramView::paintCrossovers(juce::Graphics& graphics, const Crossovers& crossovers) const
{
    const auto plot = getPlotBounds();
    const auto lineColour = juce::Colour(palette::YELLOW);
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const float x = frequencyToX(static_cast<double>(crossovers.at(crossover)));
        const float highlight = crossoverHighlights.at(crossover);
        if (highlight > 0.0F) {
            // Wide faint strokes under narrower stronger ones give the line a soft glow.
            for (int layer = 1; layer <= CROSSOVER_GLOW_LAYERS; ++layer) {
                const float spread = static_cast<float>(layer) / static_cast<float>(CROSSOVER_GLOW_LAYERS);
                graphics.setColour(lineColour.withAlpha(CROSSOVER_GLOW_ALPHA * highlight * (1.0F - spread)));
                graphics.drawLine(x, plot.getY(), x, plot.getBottom(), CROSSOVER_GLOW_THICKNESS * spread * highlight);
            }
        }
        graphics.setColour(lineColour.withAlpha(CROSSOVER_ALPHA + (1.0F - CROSSOVER_ALPHA) * highlight));
        graphics.drawLine(
            x,
            plot.getY(),
            x,
            plot.getBottom(),
            CROSSOVER_THICKNESS + (CROSSOVER_ACTIVE_THICKNESS - CROSSOVER_THICKNESS) * highlight
        );
        const juce::Point<float> labelCentre(
            x + LABEL_INSET + CROSSOVER_LABEL_WIDTH * 0.5F, plot.getBottom() - LABEL_FONT_HEIGHT - LABEL_INSET
        );
        paintLabelPill(
            graphics,
            formatFrequency(crossovers.at(crossover)),
            labelCentre,
            CROSSOVER_LABEL_ALPHA + (1.0F - CROSSOVER_LABEL_ALPHA) * highlight,
            lineColour
        );
    }
}

/// Paints the frequency and stereo position labels around the plot.
void SpatialSpectrogramView::paintAxisLabels(juce::Graphics& graphics) const
{
    const auto plot = getPlotBounds();
    graphics.setFont(textFont(LABEL_FONT_HEIGHT));
    graphics.setColour(juce::Colour(palette::GREY_LIGHT).withAlpha(AXIS_LABEL_ALPHA));
    for (const double frequency : GRID_FREQUENCIES) {
        const float x = frequencyToX(frequency);
        const juce::Rectangle<float> label(
            x - FREQUENCY_LABEL_WIDTH * 0.5F, plot.getBottom(), FREQUENCY_LABEL_WIDTH, FREQUENCY_GUTTER
        );
        graphics.drawText(formatGridFrequency(frequency), label, juce::Justification::centred);
    }
    for (std::size_t index = 0; index < GRID_POSITIONS.size(); ++index) {
        const float y = positionToY(GRID_POSITIONS.at(index));
        const juce::Rectangle<float> label(
            BOX_PADDING, y - LABEL_FONT_HEIGHT * 0.5F, POSITION_GUTTER - LABEL_INSET - BOX_PADDING, LABEL_FONT_HEIGHT
        );
        graphics.drawText(GRID_POSITION_LABELS.at(index), label, juce::Justification::centredRight);
    }
}

/// Paints the display refresh rate in the top-left corner of the plot when it is shown,
/// small and monospaced like a game's frame rate overlay, with the GPU time per frame when it is measured.
void SpatialSpectrogramView::paintFramesPerSecond(juce::Graphics& graphics) const
{
    if (!framesPerSecond.has_value()) {
        return;
    }
    graphics.setFont(monoFont(LABEL_FONT_HEIGHT));
    graphics.setColour(juce::Colour(palette::GREY_LIGHT));
    auto text = juce::String(juce::roundToInt(*framesPerSecond)) + " FPS";
    if (gpuFrameMilliseconds.has_value()) {
        text << " | " << juce::String(*gpuFrameMilliseconds, 1) << " ms GPU";
    }
    graphics.drawText(text, framesPerSecondArea(), juce::Justification::centredLeft);
}

/// Says in the middle of the plot why no particles are drawn: rendering is bypassed, or the GPU renderer failed.
void SpatialSpectrogramView::paintRenderingStatus(juce::Graphics& graphics) const
{
    if (!renderingBypassed && !renderingUnavailable) {
        return;
    }
    const auto* text = renderingUnavailable ? "GPU rendering unavailable" : "Rendering bypassed";
    graphics.setFont(textFont(STATUS_FONT_HEIGHT));
    graphics.setColour(juce::Colour(palette::GREY_LIGHT).withAlpha(STATUS_ALPHA));
    graphics.drawText(text, getPlotBounds(), juce::Justification::centred);
}

/// Paints the frequency and position grid lines inside the plot.
void SpatialSpectrogramView::paintGrid(juce::Graphics& graphics) const
{
    const auto plot = getPlotBounds();
    const auto lineColour = juce::Colour(palette::GREY_LIGHT);
    graphics.setColour(lineColour.withAlpha(GRID_ALPHA));
    for (const double frequency : GRID_FREQUENCIES) {
        graphics.drawVerticalLine(juce::roundToInt(frequencyToX(frequency)), plot.getY(), plot.getBottom());
    }
    for (std::size_t index = 0; index < GRID_POSITIONS.size(); ++index) {
        graphics.setColour(lineColour.withAlpha(index == CENTRE_POSITION_INDEX ? CENTRE_LINE_ALPHA : GRID_ALPHA));
        const float y = positionToY(GRID_POSITIONS.at(index));
        graphics.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
    }
}

/// Paints the position curve overlay while it shows after a curve change.
/// Guide lines on both halves mark where common level differences land, labelled in dB at the right,
/// with the pan midpoint and edge brighter.
/// The curve itself runs from the centre line towards both edges,
/// with the level difference along the centre line, from 0 dB at the left to 1.5 times the pan edge at the right.
void SpatialSpectrogramView::paintPositionCurve(juce::Graphics& graphics) const
{
    const float opacity = positionCurveOpacity();
    if (opacity <= 0.0F) {
        return;
    }
    const auto plot = getPlotBounds();
    const auto guideColour = juce::Colour(palette::GREY_LIGHT);
    const auto settingColour = juce::Colour(palette::YELLOW);
    const float labelX = plot.getRight() - LABEL_INSET - CURVE_LABEL_WIDTH * 0.5F;
    const float labelHeight = LABEL_FONT_HEIGHT + PILL_PADDING;

    // The settings are labelled first, so the other guides skip labels that would overlap them.
    const std::array labelledDifferences {positionCurve.midpoint(), positionCurve.edge()};
    std::vector<float> labelledYs;
    const auto paintGuide = [&](const float difference, const bool setting, const bool labelled) {
        const auto colour = setting ? settingColour : guideColour;
        const float alpha = (setting ? CURVE_SETTING_ALPHA : CURVE_GUIDE_ALPHA) * opacity;
        const float share = positionCurve.position(difference);
        for (const float sign : {1.0F, -1.0F}) {
            const float y = positionToY(static_cast<double>(sign * share));
            graphics.setColour(colour.withAlpha(alpha));
            graphics.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
            if (!labelled) {
                continue;
            }
            const bool overlaps = std::ranges::any_of(labelledYs, [y, labelHeight](const float other) {
                return std::abs(other - y) < labelHeight;
            });
            if (setting || !overlaps) {
                labelledYs.push_back(y);
                paintLabelPill(
                    graphics, juce::String(juce::roundToInt(difference)) + " dB", {labelX, y}, opacity, colour
                );
            }
        }
    };
    for (const float difference : labelledDifferences) {
        paintGuide(difference, true, true);
    }
    for (const float difference : CURVE_GUIDE_DIFFERENCES) {
        if (!std::ranges::contains(labelledDifferences, difference)) {
            paintGuide(difference, false, true);
        }
    }

    const float range = positionCurve.edge() * CURVE_RANGE_PER_EDGE;
    const auto differenceAt = [&plot, range](const float x) { return (x - plot.getX()) / plot.getWidth() * range; };
    for (const float sign : {1.0F, -1.0F}) {
        juce::Path curve;
        curve.startNewSubPath(plot.getX(), positionToY(0.0));
        const int steps = static_cast<int>(std::ceil(plot.getWidth() / CURVE_STEP_PIXELS));
        for (int step = 1; step <= steps; ++step) {
            const float clamped = std::min(plot.getX() + static_cast<float>(step) * CURVE_STEP_PIXELS, plot.getRight());
            curve.lineTo(
                clamped, positionToY(static_cast<double>(sign * positionCurve.position(differenceAt(clamped))))
            );
        }
        graphics.setColour(settingColour.withAlpha(CURVE_LINE_ALPHA * opacity));
        graphics.strokePath(curve, juce::PathStrokeType(CURVE_LINE_THICKNESS));
    }

    // The dB ticks along the centre line, the level difference axis of the curve.
    float tick = CURVE_TICK_DECIBELS;
    while (range / tick > static_cast<float>(CURVE_MAX_TICKS)) {
        tick += CURVE_TICK_DECIBELS;
    }
    const float centreY = positionToY(0.0);
    for (int index = 1; static_cast<float>(index) * tick < range; ++index) {
        const float difference = static_cast<float>(index) * tick;
        const float x = plot.getX() + difference / range * plot.getWidth();
        paintLabelPill(graphics, juce::String(juce::roundToInt(difference)), {x, centreY}, opacity, guideColour);
    }
}

}  // namespace esgrove::image_control
