/// Implements SpatialSpectrogramView: turning analyzed bins into particles for the GPU renderer,
/// with their brightness, spread, and frequency or phase colours,
/// the grid and band overlays with their hover animation, and the mouse controls.

#include "ui/SpatialSpectrogramView.h"

#include "analysis/SpatialAnalyzer.h"
#include "dsp/CrossoverDesign.h"
#include "Parameters.h"
#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Level shown as the faintest particle, in dB relative to a full-scale sine.
constexpr float DISPLAY_FLOOR_DB {-72.0F};
/// Level range from the faintest to the brightest particle, in dB.
constexpr float DISPLAY_RANGE_DB {72.0F};
/// Exponent from the level's share of the display range to the particle brightness.
/// Above 1 it keeps the noise floor dark, so quiet bins show as faint dots instead of a haze over the plot.
constexpr float LEVEL_GAMMA {2.2F};
/// Weakest particle weight that is still drawn. Fainter particles would not show.
constexpr float MIN_PARTICLE_WEIGHT {1.0e-4F};
/// Time constant over which a new particle reaches its brightness, in seconds.
/// It is short and independent of the persistence, so a single frame already draws a bright dot.
constexpr double FLASH_SECONDS {0.04};
/// Horizontal spread of a particle relative to the distance to its neighbours,
/// so neighbouring bins blend into a continuous trace at the low frequencies.
constexpr float PARTICLE_SIGMA_PER_SPACING {0.6F};
/// Number of steps in the frequency colour table.
constexpr std::size_t FREQUENCY_COLOUR_STEPS {1024};
/// Smallest highlight change that still counts as animating. Closer to the target, the highlight snaps to it.
constexpr float HIGHLIGHT_SNAP {0.002F};

/// Hue of the lowest frequency: red.
constexpr float LOW_FREQUENCY_HUE {0.0F};
/// Hue of the highest frequency: blue towards violet.
constexpr float HIGH_FREQUENCY_HUE {0.7F};
/// Exponent that bends the hue along the frequency axis, so the bass octaves stay red
/// and the hue moves through orange, yellow, and green in the mids before reaching blue in the highs.
constexpr float HUE_CURVE {1.5F};
/// Saturation of the frequency colours.
constexpr float FREQUENCY_SATURATION {0.85F};
/// Colour of in-phase content in the phase colours.
constexpr LinearColour IN_PHASE_COLOUR {.red = 0.35F, .green = 0.7F, .blue = 1.0F};
/// Colour of anti-phase content in the phase colours.
constexpr LinearColour ANTI_PHASE_COLOUR {.red = 1.0F, .green = 0.3F, .blue = 0.35F};

/// Time constant of the hover animations, in seconds.
constexpr double HIGHLIGHT_SECONDS {0.08};
/// Change of a band's side gain per unit of mouse wheel movement, in dB.
constexpr float WHEEL_DECIBELS_PER_UNIT {12.0F};
/// Change of a band's side gain per pixel of vertical drag with Ctrl or Cmd held, in dB.
constexpr float WIDTH_DECIBELS_PER_PIXEL {0.12F};
/// Share of the wheel step used while Shift is held, for fine adjustment.
constexpr float FINE_WHEEL_SHARE {0.2F};

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
/// Corner radius of the spectrogram box, matching the band strips, in pixels.
constexpr float BOX_CORNER_RADIUS {6.0F};
/// HTK mel scale factor, as in AudioBatch's spectrogram.
constexpr double MEL_SCALE {2595.0};
/// HTK mel corner frequency, in Hz.
constexpr double MEL_CORNER_FREQUENCY {700.0};
/// Base of the logarithm in the HTK mel definition.
constexpr double MEL_LOGARITHM_BASE {10.0};
/// Space around the plot inside the box, in pixels.
constexpr float BOX_PADDING {6.0F};

/// Returns the frequency colour of a share of the frequency axis: red at the left edge through to blue at the right.
LinearColour frequencyColour(const float share)
{
    const auto colour = juce::Colour::fromHSV(
        LOW_FREQUENCY_HUE + (HIGH_FREQUENCY_HUE - LOW_FREQUENCY_HUE) * std::pow(share, HUE_CURVE),
        FREQUENCY_SATURATION,
        1.0F,
        1.0F
    );
    return {.red = colour.getFloatRed(), .green = colour.getFloatGreen(), .blue = colour.getFloatBlue()};
}

/// Builds the frequency colour of evenly spaced shares of the frequency axis.
std::vector<LinearColour> buildFrequencyColours()
{
    std::vector<LinearColour> table(FREQUENCY_COLOUR_STEPS);
    for (std::size_t step = 0; step < FREQUENCY_COLOUR_STEPS; ++step) {
        table[step] = frequencyColour(static_cast<float>(step) / static_cast<float>(FREQUENCY_COLOUR_STEPS - 1));
    }
    return table;
}

/// Returns the target magnitude of a band at a frequency, from 0 to 1, given the ordered crossovers.
/// The bands are differences of the crossover lowpasses, so their magnitudes sum to one at every frequency.
float bandResponse(const std::size_t band, const double frequency, const Crossovers& crossovers)
{
    const auto lowpass = [frequency, &crossovers](const std::size_t crossover) {
        return crossoverLowpassMagnitude(frequency, static_cast<double>(crossovers.at(crossover)));
    };
    const double below = band == 0 ? 0.0 : lowpass(band - 1);
    const double above = band == CROSSOVER_COUNT ? 1.0 : lowpass(band);
    return static_cast<float>(above - below);
}

/// Returns the colour of a band: the frequency colour at its centre on the logarithmic axis.
juce::Colour bandColour(const std::size_t band, const Crossovers& crossovers)
{
    const double low = band == 0 ? static_cast<double>(MIN_FREQUENCY) : static_cast<double>(crossovers.at(band - 1));
    const double high
        = band == CROSSOVER_COUNT ? static_cast<double>(MAX_FREQUENCY) : static_cast<double>(crossovers.at(band));
    const double share = frequencyToShare(std::sqrt(low * high), FrequencyScale::logarithmic);
    const LinearColour colour = frequencyColour(static_cast<float>(share));
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

/// Returns where a frequency lies on the frequency axis, from 0 at MIN_FREQUENCY to 1 at MAX_FREQUENCY.
double frequencyToShare(const double frequency, const FrequencyScale scale)
{
    switch (scale) {
        case FrequencyScale::logarithmic: {
            const double ratio = static_cast<double>(MAX_FREQUENCY) / static_cast<double>(MIN_FREQUENCY);
            return std::log(frequency / static_cast<double>(MIN_FREQUENCY)) / std::log(ratio);
        }
        case FrequencyScale::mel: {
            const double lowest = frequencyToMel(static_cast<double>(MIN_FREQUENCY));
            const double highest = frequencyToMel(static_cast<double>(MAX_FREQUENCY));
            return (frequencyToMel(frequency) - lowest) / (highest - lowest);
        }
    }
    return 0.0;
}

/// Returns the frequency at a share of the frequency axis, the inverse of frequencyToShare.
double shareToFrequency(const double share, const FrequencyScale scale)
{
    switch (scale) {
        case FrequencyScale::logarithmic: {
            const double ratio = static_cast<double>(MAX_FREQUENCY) / static_cast<double>(MIN_FREQUENCY);
            return static_cast<double>(MIN_FREQUENCY) * std::pow(ratio, share);
        }
        case FrequencyScale::mel: {
            const double lowest = frequencyToMel(static_cast<double>(MIN_FREQUENCY));
            const double highest = frequencyToMel(static_cast<double>(MAX_FREQUENCY));
            return melToFrequency(lowest + share * (highest - lowest));
        }
    }
    return 0.0;
}

/// Returns the HTK mel value of a frequency, the same definition as AudioBatch's spectrogram.
double frequencyToMel(const double frequency)
{
    return MEL_SCALE * std::log10(1.0 + frequency / MEL_CORNER_FREQUENCY);
}

/// Returns the frequency of an HTK mel value, the inverse of frequencyToMel.
double melToFrequency(const double mel)
{
    return MEL_CORNER_FREQUENCY * (std::pow(MEL_LOGARITHM_BASE, mel / MEL_SCALE) - 1.0);
}

/// Creates the view, looking up the parameters it reads and changes.
SpatialSpectrogramView::SpatialSpectrogramView(juce::AudioProcessorValueTreeState& parameters) :
    frequencyColours(buildFrequencyColours())
{
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        crossoverParameters.at(crossover) = parameters.getParameter(crossoverParameterId(crossover));
        jassert(crossoverParameters.at(crossover) != nullptr);
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        midParameters.at(band) = parameters.getParameter(midGainParameterId(band));
        sideParameters.at(band) = parameters.getParameter(sideGainParameterId(band));
        jassert(midParameters.at(band) != nullptr && sideParameters.at(band) != nullptr);
    }
    // The rounded corners show the editor background and the plot shows the GPU picture,
    // so the view does not fill its whole bounds.
    setOpaque(false);
    shownOverlay = currentOverlayState();
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

/// Turns the points of one analyzed frame into particles for the renderer.
/// A particle's brightness follows its level in dB through a power curve, which keeps the noise floor dark.
/// Its weight is the share of a short flash its frame stands for,
/// so a single frame draws a bright dot and the brightness does not depend on the analyzer's frame rate.
/// Long persistence lets steady content build up for longer,
/// so the weight shrinks with the square root of the persistence to keep the picture from saturating.
void SpatialSpectrogramView::addFrame(const std::span<const SpectralPoint> points, const double frameSeconds)
{
    const auto flash = static_cast<float>(
        (1.0 - std::exp(-frameSeconds / FLASH_SECONDS)) * std::sqrt(DEFAULT_PERSISTENCE_SECONDS / persistenceSeconds)
    );
    pendingBatch.particles.reserve(pendingBatch.particles.size() + points.size());
    for (const auto& point : points) {
        const float decibels = 10.0F * std::log10(std::max(point.level, 1.0e-12F));
        const float share = std::clamp((decibels - DISPLAY_FLOOR_DB) / DISPLAY_RANGE_DB, 0.0F, 1.0F);
        const float weight = std::pow(share, LEVEL_GAMMA) * flash;
        if (weight < MIN_PARTICLE_WEIGHT) {
            continue;
        }

        const auto frequency = static_cast<double>(point.frequency);
        // The distance to the neighbouring bins on the axis depends on the frequency and the scale.
        const double halfSpacing = static_cast<double>(point.spacing) * 0.5;
        const double spacingShare = frequencyToShare(frequency + halfSpacing, scale)
            - frequencyToShare(std::max(static_cast<double>(MIN_FREQUENCY), frequency - halfSpacing), scale);
        const LinearColour colour = pointColour(point);
        pendingBatch.particles.push_back({
            .x = static_cast<float>(frequencyToShare(frequency, scale)),
            .y = (1.0F - point.position) * 0.5F,
            .spread = static_cast<float>(spacingShare) * PARTICLE_SIGMA_PER_SPACING,
            .red = colour.red * weight,
            .green = colour.green * weight,
            .blue = colour.blue * weight,
        });
    }
}

/// Returns the particles and the fade since the last call, and starts a new batch.
SpectrogramRenderBatch SpatialSpectrogramView::takeRenderBatch()
{
    pendingBatch.persistenceSeconds = persistenceSeconds;
    return std::exchange(pendingBatch, SpectrogramRenderBatch {.persistenceSeconds = persistenceSeconds});
}

/// Advances the display by the time since the last display refresh:
/// adds the time to the fade of the next render batch, animates the hover highlights,
/// and repaints the overlays when what they show changed.
void SpatialSpectrogramView::advance(const double seconds)
{
    pendingBatch.fadeSeconds += seconds;

    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float target = hoveredBand == band ? 1.0F : 0.0F;
        bandHighlights.at(band) = approach(bandHighlights.at(band), target, seconds);
    }
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const bool movedWithBand = drag == SpectrogramDrag::band && draggedBand.has_value()
            && std::ranges::contains(bandEdges(*draggedBand), crossover);
        const bool active = hoveredCrossover == crossover || draggedCrossover == crossover || movedWithBand;
        crossoverHighlights.at(crossover) = approach(crossoverHighlights.at(crossover), active ? 1.0F : 0.0F, seconds);
    }

    const SpectrogramOverlayState overlay = currentOverlayState();
    if (overlay != shownOverlay) {
        shownOverlay = overlay;
        repaint();
    }
}

/// Clears the particles, such as after switching the analyzer tap.
/// The pending particles are dropped and the renderer clears its picture with the next batch.
void SpatialSpectrogramView::clear()
{
    pendingBatch.particles.clear();
    pendingBatch.clear = true;
}

/// Selects frequency or phase colours for new particles.
void SpatialSpectrogramView::setColours(const SpectrogramColours newColours)
{
    colours = newColours;
}

/// Sets the frequency scale of the axis, and clears the particles drawn on the previous scale.
void SpatialSpectrogramView::setFrequencyScale(const FrequencyScale newScale)
{
    if (scale != newScale) {
        scale = newScale;
        clear();
    }
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

/// Sets the time constant of the particle fade, clamped to the persistence range.
void SpatialSpectrogramView::setPersistence(const double seconds)
{
    persistenceSeconds = std::clamp(seconds, MIN_PERSISTENCE_SECONDS, MAX_PERSISTENCE_SECONDS);
}

/// Resets a double-clicked crossover to its default frequency.
void SpatialSpectrogramView::mouseDoubleClick(const juce::MouseEvent& event)
{
    if (const auto crossover = crossoverNear(event.position.x)) {
        auto& parameter = *crossoverParameters.at(*crossover);
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost(parameter.getDefaultValue());
        parameter.endChangeGesture();
    }
}

/// Starts a drag as one host gesture: of the crossover line under the pointer,
/// of the band's width with Ctrl or Cmd held, or of the whole band under the pointer.
void SpatialSpectrogramView::mouseDown(const juce::MouseEvent& event)
{
    dragStartCrossovers = currentCrossovers();
    if (const auto crossover = crossoverNear(event.position.x)) {
        drag = SpectrogramDrag::crossover;
        draggedCrossover = crossover;
        crossoverParameters.at(*crossover)->beginChangeGesture();
        return;
    }
    const auto band = bandAt(event.position.x);
    if (!band.has_value()) {
        return;
    }
    draggedBand = band;
    if (isWidthModifier(event.mods)) {
        if (plainValue(*midParameters.at(*band)) <= BAND_GAIN_MIN_DB) {
            draggedBand.reset();
            return;
        }
        drag = SpectrogramDrag::width;
        dragStartSide = std::max(plainValue(*sideParameters.at(*band)), BAND_GAIN_MIN_DB);
        sideParameters.at(*band)->beginChangeGesture();
        return;
    }
    drag = SpectrogramDrag::band;
    for (const auto edge : bandEdges(*band)) {
        crossoverParameters.at(edge)->beginChangeGesture();
    }
}

/// Follows the pointer with the drag that mouseDown started.
void SpatialSpectrogramView::mouseDrag(const juce::MouseEvent& event)
{
    switch (drag) {
        case SpectrogramDrag::none:
            return;
        case SpectrogramDrag::crossover:
            moveCrossover(event);
            return;
        case SpectrogramDrag::band:
            moveBand(event);
            return;
        case SpectrogramDrag::width:
            changeBandWidth(event);
            return;
    }
}

/// Clears the hover highlights when the pointer leaves the view.
void SpatialSpectrogramView::mouseExit(const juce::MouseEvent& /*event*/)
{
    hoveredBand.reset();
    hoveredCrossover.reset();
}

/// Highlights the band and the crossover under the pointer, with a resize cursor over the crossover lines.
void SpatialSpectrogramView::mouseMove(const juce::MouseEvent& event)
{
    updateHover(event.position);
}

/// Ends the drag and its host gesture.
void SpatialSpectrogramView::mouseUp(const juce::MouseEvent& event)
{
    switch (drag) {
        case SpectrogramDrag::none:
            break;
        case SpectrogramDrag::crossover:
            if (draggedCrossover.has_value()) {
                crossoverParameters.at(*draggedCrossover)->endChangeGesture();
            }
            break;
        case SpectrogramDrag::band:
            if (draggedBand.has_value()) {
                for (const auto edge : bandEdges(*draggedBand)) {
                    crossoverParameters.at(edge)->endChangeGesture();
                }
            }
            break;
        case SpectrogramDrag::width:
            if (draggedBand.has_value()) {
                sideParameters.at(*draggedBand)->endChangeGesture();
            }
            break;
    }
    drag = SpectrogramDrag::none;
    draggedCrossover.reset();
    draggedBand.reset();
    updateHover(event.position);
}

/// Changes the width of the band under the pointer: the wheel moves its side gain relative to its mid gain.
/// Holding Shift makes the steps finer.
void SpatialSpectrogramView::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    const auto band = bandAt(event.position.x);
    if (!band.has_value() || plainValue(*midParameters.at(*band)) <= BAND_GAIN_MIN_DB) {
        return;
    }
    const float delta = wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
    const float step = WHEEL_DECIBELS_PER_UNIT * (event.mods.isShiftDown() ? FINE_WHEEL_SHARE : 1.0F);
    auto& side = *sideParameters.at(*band);
    const float current = std::max(plainValue(side), BAND_GAIN_MIN_DB);
    setParameterInGesture(side, std::clamp(current + delta * step, BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB));
}

/// Paints the box around the plot, and the grid, the band regions, and the crossover lines over the plot.
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
        paintRenderingStatus(graphics);
    }
    paintAxisLabels(graphics);
    paintFramesPerSecond(graphics);
}

/// Explains what a drag does where the pointer is: on a crossover line, on a band, or nowhere.
juce::String SpatialSpectrogramView::getTooltip()
{
#if JUCE_MAC
    const juce::String widthModifier = "Cmd";
#else
    const juce::String widthModifier = "Ctrl";
#endif
    if (hoveredCrossover.has_value()) {
        return "Drag to move the crossover between two bands. Double-click to reset it";
    }
    if (hoveredBand.has_value()) {
        return "Drag sideways to move the band. " + widthModifier
            + "-drag up or down, or scroll, to change its width. Hold Shift for finer steps";
    }
    return {};
}

/// Returns the band whose region contains an x coordinate, if it is inside the plot.
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

/// Returns what the overlays show now: the hover highlights, the crossovers, the band widths,
/// and the rendering status.
SpectrogramOverlayState SpatialSpectrogramView::currentOverlayState() const
{
    SpectrogramOverlayState state {
        .bandHighlights = bandHighlights,
        .crossoverHighlights = crossoverHighlights,
        .crossovers = currentCrossovers(),
        .renderingBypassed = renderingBypassed,
        .renderingUnavailable = renderingUnavailable,
    };
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        state.bandWidths.at(band) = bandWidth(band);
    }
    return state;
}

/// Returns the area of the frame rate readout in the top-left corner of the plot.
juce::Rectangle<float> SpatialSpectrogramView::framesPerSecondArea() const
{
    const auto plot = getPlotBounds();
    return {plot.getX() + LABEL_INSET, plot.getY() + LABEL_INSET, FPS_LABEL_WIDTH, LABEL_FONT_HEIGHT};
}

/// Returns the colour of a particle: its frequency colour, or its phase colour from its coherence.
LinearColour SpatialSpectrogramView::pointColour(const SpectralPoint& point) const
{
    if (colours == SpectrogramColours::frequency) {
        const double share
            = std::clamp(frequencyToShare(static_cast<double>(point.frequency), FrequencyScale::logarithmic), 0.0, 1.0);
        return frequencyColours[static_cast<std::size_t>(share * static_cast<double>(FREQUENCY_COLOUR_STEPS - 1))];
    }
    const float antiPhaseShare = std::clamp((1.0F - point.coherence) * 0.5F, 0.0F, 1.0F);
    return {
        .red = IN_PHASE_COLOUR.red + (ANTI_PHASE_COLOUR.red - IN_PHASE_COLOUR.red) * antiPhaseShare,
        .green = IN_PHASE_COLOUR.green + (ANTI_PHASE_COLOUR.green - IN_PHASE_COLOUR.green) * antiPhaseShare,
        .blue = IN_PHASE_COLOUR.blue + (ANTI_PHASE_COLOUR.blue - IN_PHASE_COLOUR.blue) * antiPhaseShare,
    };
}

/// Returns a band's width, its side gain relative to its mid gain as a linear ratio, or 0 when its side is off.
double SpatialSpectrogramView::bandWidth(const std::size_t band) const
{
    return bandWidthFromGains(
        static_cast<double>(plainValue(*midParameters.at(band))),
        static_cast<double>(plainValue(*sideParameters.at(band)))
    );
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

/// Paints the band regions: a soft gradient in the band's colour over the hovered band,
/// the band names, and the width of the hovered band in a label pill.
void SpatialSpectrogramView::paintBands(juce::Graphics& graphics, const Crossovers& crossovers) const
{
    const auto plot = getPlotBounds();
    graphics.setFont(textFont(LABEL_FONT_HEIGHT));

    float left = plot.getX();
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float right
            = band < CROSSOVER_COUNT ? frequencyToX(static_cast<double>(crossovers.at(band))) : plot.getRight();
        const float highlight = bandHighlights.at(band);
        const juce::Rectangle<float> region(left, plot.getY(), right - left, plot.getHeight());
        if (highlight > 0.0F) {
            const auto colour = bandColour(band, crossovers);
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

/// Paints the response of every band as a curve while a band is hovered, so their overlaps at the crossovers show:
/// the hovered band brightest with a soft fill under its curve, and the other bands as faint lines.
/// A band's response is the target magnitude of its crossover filters, one at full height.
void SpatialSpectrogramView::paintBandResponses(juce::Graphics& graphics, const Crossovers& crossovers) const
{
    const float strongest = *std::ranges::max_element(bandHighlights);
    if (strongest <= 0.0F) {
        return;
    }
    const auto plot = getPlotBounds();
    const float baseline = plot.getBottom();
    const float height = plot.getHeight() * RESPONSE_HEIGHT_SHARE;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        juce::Path curve;
        const auto levelAt = [&](const float pixel) {
            return baseline - bandResponse(band, xToFrequency(pixel), crossovers) * height;
        };
        // A path counts as empty until it has a line, so the first point starts it explicitly.
        curve.startNewSubPath(plot.getX(), levelAt(plot.getX()));
        const int steps = static_cast<int>(plot.getWidth() / RESPONSE_STEP_PIXELS);
        for (int step = 1; step <= steps; ++step) {
            const float pixel = plot.getX() + static_cast<float>(step) * RESPONSE_STEP_PIXELS;
            curve.lineTo(pixel, levelAt(pixel));
        }
        const auto colour = bandColour(band, crossovers);
        const float highlight = bandHighlights.at(band);
        if (highlight > 0.0F) {
            juce::Path fill(curve);
            fill.lineTo(plot.getRight(), baseline);
            fill.lineTo(plot.getX(), baseline);
            fill.closeSubPath();
            graphics.setGradientFill(
                juce::ColourGradient(
                    colour.withAlpha(RESPONSE_FILL_ALPHA * highlight),
                    {plot.getX(), baseline - height},
                    colour.withAlpha(0.0F),
                    {plot.getX(), baseline},
                    false
                )
            );
            graphics.fillPath(fill);
        }
        const float alpha
            = NEIGHBOUR_RESPONSE_ALPHA * strongest + (RESPONSE_LINE_ALPHA - NEIGHBOUR_RESPONSE_ALPHA) * highlight;
        graphics.setColour(colour.withAlpha(alpha));
        graphics.strokePath(curve, juce::PathStrokeType(RESPONSE_LINE_THICKNESS, juce::PathStrokeType::curved));
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

/// Moves the dragged crossover to the pointer, kept the minimum ratio away from its neighbours.
void SpatialSpectrogramView::moveCrossover(const juce::MouseEvent& event)
{
    if (!draggedCrossover.has_value()) {
        return;
    }
    const std::size_t index = *draggedCrossover;
    const Crossovers crossovers = currentCrossovers();
    const float lowerLimit = index == 0 ? MIN_FREQUENCY : crossovers.at(index - 1) * MIN_CROSSOVER_RATIO;
    const float upperLimit
        = index + 1 == CROSSOVER_COUNT ? MAX_FREQUENCY : crossovers.at(index + 1) / MIN_CROSSOVER_RATIO;
    const auto frequency
        = std::clamp(static_cast<float>(xToFrequency(event.position.x)), lowerLimit, std::max(lowerLimit, upperLimit));
    auto* parameter = crossoverParameters.at(index);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(frequency));
}

/// Moves the dragged band with the pointer by moving its crossovers the same distance on the frequency axis,
/// so a middle band keeps its width on the axis, and the Low and High bands move only their inner crossover.
/// The move stops where a moved crossover would come closer than the minimum ratio to one that stays,
/// or would leave the frequency range.
void SpatialSpectrogramView::moveBand(const juce::MouseEvent& event)
{
    if (!draggedBand.has_value()) {
        return;
    }
    const auto edges = bandEdges(*draggedBand);
    const auto isMoved = [&edges](const std::size_t crossover) { return std::ranges::contains(edges, crossover); };
    const auto shareOf
        = [this](const float frequency) { return frequencyToShare(static_cast<double>(frequency), scale); };

    double lowestOffset = -1.0;
    double highestOffset = 1.0;
    for (const auto edge : edges) {
        const double start = shareOf(dragStartCrossovers.at(edge));
        if (edge == 0) {
            lowestOffset = std::max(lowestOffset, shareOf(MIN_FREQUENCY) - start);
        } else if (!isMoved(edge - 1)) {
            lowestOffset
                = std::max(lowestOffset, shareOf(dragStartCrossovers.at(edge - 1) * MIN_CROSSOVER_RATIO) - start);
        }
        if (edge + 1 == CROSSOVER_COUNT) {
            highestOffset = std::min(highestOffset, shareOf(MAX_FREQUENCY) - start);
        } else if (!isMoved(edge + 1)) {
            highestOffset
                = std::min(highestOffset, shareOf(dragStartCrossovers.at(edge + 1) / MIN_CROSSOVER_RATIO) - start);
        }
    }

    const auto plot = getPlotBounds();
    const auto pointerOffset = static_cast<double>((event.position.x - event.mouseDownPosition.x) / plot.getWidth());
    const double offset = lowestOffset > highestOffset ? 0.0 : std::clamp(pointerOffset, lowestOffset, highestOffset);
    for (const auto edge : edges) {
        const auto frequency
            = static_cast<float>(shareToFrequency(shareOf(dragStartCrossovers.at(edge)) + offset, scale));
        auto* parameter = crossoverParameters.at(edge);
        parameter->setValueNotifyingHost(parameter->convertTo0to1(frequency));
    }
}

/// Changes the width of the dragged band with the pointer's vertical movement:
/// moving up raises its side gain relative to its mid gain, and Shift makes it finer.
void SpatialSpectrogramView::changeBandWidth(const juce::MouseEvent& event)
{
    if (!draggedBand.has_value()) {
        return;
    }
    const float distance = event.mouseDownPosition.y - event.position.y;
    const float step = WIDTH_DECIBELS_PER_PIXEL * (event.mods.isShiftDown() ? FINE_WHEEL_SHARE : 1.0F);
    auto& side = *sideParameters.at(*draggedBand);
    const float decibels = std::clamp(dragStartSide + distance * step, BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB);
    side.setValueNotifyingHost(side.convertTo0to1(decibels));
}

/// Returns the crossovers that bound a band: the one below it unless it is the Low band,
/// and the one above it unless it is the High band.
std::vector<std::size_t> SpatialSpectrogramView::bandEdges(const std::size_t band)
{
    std::vector<std::size_t> edges;
    if (band > 0) {
        edges.push_back(band - 1);
    }
    if (band < CROSSOVER_COUNT) {
        edges.push_back(band);
    }
    return edges;
}

/// Returns whether the modifier keys turn a band drag into a width drag: Ctrl, or Cmd on macOS.
bool SpatialSpectrogramView::isWidthModifier(const juce::ModifierKeys& modifiers)
{
    return modifiers.isCommandDown() || modifiers.isCtrlDown();
}

/// Updates the cursor for the pointer position and the modifier keys when a modifier key is pressed or released.
void SpatialSpectrogramView::modifierKeysChanged(const juce::ModifierKeys& modifiers)
{
    updateCursor(modifiers);
}

/// Shows what a drag from the pointer position would change:
/// a horizontal resize cursor over a crossover line, a vertical one over a band with Ctrl or Cmd held,
/// and a hand over a band, which moves it.
void SpatialSpectrogramView::updateCursor(const juce::ModifierKeys& modifiers)
{
    if (drag == SpectrogramDrag::crossover || hoveredCrossover.has_value()) {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    } else if (drag == SpectrogramDrag::width || (hoveredBand.has_value() && isWidthModifier(modifiers))) {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    } else if (drag == SpectrogramDrag::band || hoveredBand.has_value()) {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    } else {
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }
}

/// Updates which band and crossover the pointer is over, and the cursor.
/// During a drag, the dragged band or crossover stays highlighted wherever the pointer goes.
void SpatialSpectrogramView::updateHover(const juce::Point<float> position)
{
    if (drag == SpectrogramDrag::none) {
        hoveredCrossover = crossoverNear(position.x);
        hoveredBand = hoveredCrossover.has_value() ? std::nullopt : bandAt(position.x);
    } else {
        hoveredCrossover = draggedCrossover;
        hoveredBand = draggedBand;
    }
    updateCursor(juce::ModifierKeys::getCurrentModifiers());
}
}  // namespace esgrove::image_control
