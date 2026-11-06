/// Implements SpatialSpectrogramView: splatting analyzed bins into the particle buffer as Gaussian particles,
/// the frequency and phase colours, the time-based fade and blur, the tone curve that renders the buffer,
/// the grid and band overlays with their hover animation, and the mouse controls.

#include "ui/SpatialSpectrogramView.h"

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"
#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Number of cells in each plane of the particle buffer.
constexpr auto CELL_COUNT {static_cast<std::size_t>(SPECTROGRAM_COLUMNS) * SPECTROGRAM_ROWS};
/// Number of columns, as an index type.
constexpr auto COLUMNS {static_cast<std::size_t>(SPECTROGRAM_COLUMNS)};

/// Level shown as the faintest particle, in dB relative to a full-scale sine.
constexpr float DISPLAY_FLOOR_DB {-78.0F};
/// Level range from the faintest to the brightest particle, in dB.
constexpr float DISPLAY_RANGE_DB {78.0F};
/// Scale from the settled particle energy to the tone curve input.
constexpr float EXPOSURE {3.0F};
/// Highest exposed energy the tone curve covers. Above it the display is fully saturated.
constexpr float TONE_CURVE_MAX {6.0F};
/// Number of steps in the tone curve.
constexpr std::size_t TONE_CURVE_STEPS {2048};

/// Vertical spread of a particle, in rows.
constexpr float PARTICLE_SIGMA_ROWS {1.8F};
/// Smallest horizontal spread of a particle, in columns.
constexpr float MIN_PARTICLE_SIGMA_COLUMNS {1.2F};
/// Horizontal spread of a particle relative to the distance to its neighbours,
/// so neighbouring bins blend into a continuous trace at the low frequencies.
constexpr float PARTICLE_SIGMA_PER_SPACING {0.6F};
/// How many spreads a particle reaches before it is cut off.
constexpr float PARTICLE_RADIUS_SIGMAS {2.5F};
/// Largest particle radius, in cells.
constexpr int MAX_PARTICLE_RADIUS {48};

/// Time between blur steps of the older particles, in seconds, independent of the display refresh rate.
constexpr double BLUR_INTERVAL_SECONDS {1.0 / 60.0};
/// Share of a cell each neighbour receives per blur step.
constexpr float BLUR_NEIGHBOUR_WEIGHT {0.1F};

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
/// Opacity of the fill of a hovered band.
constexpr float BAND_HOVER_ALPHA {0.05F};
/// Opacity of a crossover line at rest.
constexpr float CROSSOVER_ALPHA {0.75F};
/// Thickness of a crossover line, in pixels.
constexpr float CROSSOVER_THICKNESS {1.5F};
/// Thickness of the glow around a hovered crossover line, in pixels.
constexpr float CROSSOVER_GLOW_THICKNESS {7.0F};
/// Opacity of the glow around a fully hovered crossover line.
constexpr float CROSSOVER_GLOW_ALPHA {0.25F};

/// Background of the plot, as ARGB.
constexpr juce::uint32 BACKGROUND {0xff050608};
/// Colour of the frame rate readout, as ARGB.
constexpr juce::uint32 FPS_COLOUR {0xff9be37a};
/// Width of the frame rate readout, in pixels.
constexpr float FPS_LABEL_WIDTH {70.0F};
/// Corner radius of the spectrogram box, matching the band strips, in pixels.
constexpr float BOX_CORNER_RADIUS {6.0F};
/// Corner radius of the plot inside the box, in pixels.
constexpr float PLOT_CORNER_RADIUS {4.0F};
/// HTK mel scale factor, as in AudioBatch's spectrogram.
constexpr double MEL_SCALE {2595.0};
/// HTK mel corner frequency, in Hz.
constexpr double MEL_CORNER_FREQUENCY {700.0};
/// Base of the logarithm in the HTK mel definition.
constexpr double MEL_LOGARITHM_BASE {10.0};
/// Largest value of an 8-bit colour channel.
constexpr double MAX_CHANNEL_VALUE {255.0};
/// Space around the plot inside the box, in pixels.
constexpr float BOX_PADDING {6.0F};

/// Returns the plane index of a cell.
std::size_t cellIndex(const int column, const int row)
{
    return static_cast<std::size_t>(row) * COLUMNS + static_cast<std::size_t>(column);
}

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

/// Builds the tone curve, 1 − exp(−energy), which keeps faint particles visible
/// and lets overlapping bright particles saturate smoothly instead of clipping.
std::vector<juce::uint8> buildToneCurve()
{
    std::vector<juce::uint8> curve(TONE_CURVE_STEPS);
    for (std::size_t step = 0; step < TONE_CURVE_STEPS; ++step) {
        const double energy = static_cast<double>(step) / static_cast<double>(TONE_CURVE_STEPS - 1)
            * static_cast<double>(TONE_CURVE_MAX);
        curve[step] = static_cast<juce::uint8>(juce::roundToInt(MAX_CHANNEL_VALUE * (1.0 - std::exp(-energy))));
    }
    return curve;
}

/// Returns the Gaussian weight of an offset for a spread.
float gaussian(const float offset, const float sigma)
{
    return std::exp(-0.5F * (offset * offset) / (sigma * sigma));
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

/// Moves an animated value towards its target with a time constant.
float approach(const float value, const float target, const double seconds)
{
    const auto share = static_cast<float>(1.0 - std::exp(-seconds / HIGHLIGHT_SECONDS));
    return value + (target - value) * share;
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

/// Creates the view with an empty particle buffer, looking up the parameters it reads and changes.
SpatialSpectrogramView::SpatialSpectrogramView(juce::AudioProcessorValueTreeState& parameters) :
    image(juce::Image::ARGB, SPECTROGRAM_COLUMNS, SPECTROGRAM_ROWS, true),
    blurScratch(CELL_COUNT, 0.0F),
    columnColours(COLUMNS),
    toneCurve(buildToneCurve())
{
    for (auto& plane : buffer) {
        plane.assign(CELL_COUNT, 0.0F);
    }
    for (std::size_t column = 0; column < COLUMNS; ++column) {
        columnColours[column] = frequencyColour(static_cast<float>(column) / static_cast<float>(COLUMNS - 1));
    }
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        crossoverParameters.at(crossover) = parameters.getParameter(crossoverParameterId(crossover));
        jassert(crossoverParameters.at(crossover) != nullptr);
    }
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        midParameters.at(band) = parameters.getParameter(midGainParameterId(band));
        sideParameters.at(band) = parameters.getParameter(sideGainParameterId(band));
        jassert(midParameters.at(band) != nullptr && sideParameters.at(band) != nullptr);
    }
    // The rounded corners show the editor background, so the view does not fill its whole bounds.
    setOpaque(false);
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

/// Draws the points of one analyzed frame into the particle buffer as Gaussian particles.
/// A particle's brightness follows its level in dB,
/// and its contribution is scaled by the share of the fade its frame stands for,
/// so the settled brightness does not depend on the persistence or the frame rate.
void SpatialSpectrogramView::addFrame(const std::span<const SpectralPoint> points, const double frameSeconds)
{
    const auto fadeIn = static_cast<float>(1.0 - std::exp(-frameSeconds / persistenceSeconds));
    std::array<float, static_cast<std::size_t>(MAX_PARTICLE_RADIUS) * 2 + 1> columnWeights {};
    std::array<float, static_cast<std::size_t>(MAX_PARTICLE_RADIUS) * 2 + 1> rowWeights {};

    for (const auto& point : points) {
        const float decibels = 10.0F * std::log10(point.level);
        const float brightness = std::clamp((decibels - DISPLAY_FLOOR_DB) / DISPLAY_RANGE_DB, 0.0F, 1.0F);
        // Brightness follows the level in dB linearly, so quiet content stays visible next to the peaks.
        const float weight = brightness * fadeIn;
        if (weight <= 0.0F) {
            continue;
        }

        const auto frequency = static_cast<double>(point.frequency);
        const auto column
            = static_cast<float>(frequencyToShare(frequency, scale) * static_cast<double>(SPECTROGRAM_COLUMNS));
        const auto row = static_cast<float>(
            (1.0 - static_cast<double>(point.position)) * 0.5 * static_cast<double>(SPECTROGRAM_ROWS - 1)
        );
        // The distance to the neighbouring bins in columns depends on the frequency and the scale.
        const double halfSpacing = static_cast<double>(point.spacing) * 0.5;
        const auto spacingColumns = static_cast<float>(
            (frequencyToShare(frequency + halfSpacing, scale)
             - frequencyToShare(std::max(static_cast<double>(MIN_FREQUENCY), frequency - halfSpacing), scale))
            * static_cast<double>(SPECTROGRAM_COLUMNS)
        );
        const float sigmaColumns = std::max(MIN_PARTICLE_SIGMA_COLUMNS, spacingColumns * PARTICLE_SIGMA_PER_SPACING);
        const int columnRadius
            = std::min(MAX_PARTICLE_RADIUS, static_cast<int>(std::ceil(sigmaColumns * PARTICLE_RADIUS_SIGMAS)));
        const int rowRadius = static_cast<int>(std::ceil(PARTICLE_SIGMA_ROWS * PARTICLE_RADIUS_SIGMAS));

        const int centreColumn = juce::roundToInt(column);
        const int centreRow = juce::roundToInt(row);
        const int firstColumn = std::max(0, centreColumn - columnRadius);
        const int lastColumn = std::min(SPECTROGRAM_COLUMNS - 1, centreColumn + columnRadius);
        const int firstRow = std::max(0, centreRow - rowRadius);
        const int lastRow = std::min(SPECTROGRAM_ROWS - 1, centreRow + rowRadius);
        for (int cell = firstColumn; cell <= lastColumn; ++cell) {
            columnWeights[static_cast<std::size_t>(cell - firstColumn)]
                = gaussian(static_cast<float>(cell) - column, sigmaColumns);
        }
        for (int cell = firstRow; cell <= lastRow; ++cell) {
            rowWeights[static_cast<std::size_t>(cell - firstRow)]
                = gaussian(static_cast<float>(cell) - row, PARTICLE_SIGMA_ROWS) * weight;
        }

        const LinearColour colour = pointColour(point);
        for (int rowCell = firstRow; rowCell <= lastRow; ++rowCell) {
            const float rowWeight = rowWeights[static_cast<std::size_t>(rowCell - firstRow)];
            for (int columnCell = firstColumn; columnCell <= lastColumn; ++columnCell) {
                const float cellWeight = rowWeight * columnWeights[static_cast<std::size_t>(columnCell - firstColumn)];
                const std::size_t index = cellIndex(columnCell, rowCell);
                buffer[0][index] += colour.red * cellWeight;
                buffer[1][index] += colour.green * cellWeight;
                buffer[2][index] += colour.blue * cellWeight;
            }
        }
    }
}

/// Advances the display by the time since the last display refresh:
/// fades the particles, blurs the older ones at a fixed rate, animates the hover highlights,
/// renders the particle buffer, and repaints.
void SpatialSpectrogramView::advance(const double seconds)
{
    const auto fade = static_cast<float>(std::exp(-seconds / persistenceSeconds));
    for (auto& plane : buffer) {
        juce::FloatVectorOperations::multiply(plane.data(), fade, static_cast<int>(plane.size()));
    }
    blurClock += seconds;
    while (blurClock >= BLUR_INTERVAL_SECONDS) {
        blurClock -= BLUR_INTERVAL_SECONDS;
        blurBuffer();
    }

    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float target = hoveredBand == band ? 1.0F : 0.0F;
        bandHighlights.at(band) = approach(bandHighlights.at(band), target, seconds);
    }
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const bool active = hoveredCrossover == crossover || draggedCrossover == crossover;
        crossoverHighlights.at(crossover) = approach(crossoverHighlights.at(crossover), active ? 1.0F : 0.0F, seconds);
    }

    renderImage();
    repaint();
}

/// Clears the particles, such as after switching the analyzer tap.
void SpatialSpectrogramView::clear()
{
    for (auto& plane : buffer) {
        std::ranges::fill(plane, 0.0F);
    }
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

/// Sets the display refresh rate to show in the top-left corner, or hides it.
void SpatialSpectrogramView::setFramesPerSecond(const std::optional<double> rate)
{
    framesPerSecond = rate;
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

/// Starts dragging the crossover line under the pointer, if any, as one host gesture.
void SpatialSpectrogramView::mouseDown(const juce::MouseEvent& event)
{
    draggedCrossover = crossoverNear(event.position.x);
    if (draggedCrossover.has_value()) {
        crossoverParameters.at(*draggedCrossover)->beginChangeGesture();
    }
}

/// Moves the dragged crossover to the pointer, kept the minimum ratio away from its neighbours.
void SpatialSpectrogramView::mouseDrag(const juce::MouseEvent& event)
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

/// Ends the drag gesture.
void SpatialSpectrogramView::mouseUp(const juce::MouseEvent& event)
{
    if (draggedCrossover.has_value()) {
        crossoverParameters.at(*draggedCrossover)->endChangeGesture();
        draggedCrossover.reset();
    }
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

/// Paints the particle image, the grid, the band regions, and the crossover lines.
void SpatialSpectrogramView::paint(juce::Graphics& graphics)
{
    graphics.setColour(juce::Colour(palette::GREY_SEMI_DARK));
    graphics.fillRoundedRectangle(getLocalBounds().toFloat(), BOX_CORNER_RADIUS);
    const auto plot = getPlotBounds();
    graphics.setColour(juce::Colour(BACKGROUND));
    graphics.fillRoundedRectangle(plot, PLOT_CORNER_RADIUS);

    juce::Path plotOutline;
    plotOutline.addRoundedRectangle(plot, PLOT_CORNER_RADIUS);
    {
        const juce::Graphics::ScopedSaveState clip(graphics);
        graphics.reduceClipRegion(plotOutline);
        graphics.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        graphics.drawImage(image, plot, juce::RectanglePlacement::stretchToFit);
        paintGrid(graphics);
        paintBands(graphics, currentCrossovers());
    }
    paintAxisLabels(graphics);
    paintFramesPerSecond(graphics);
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

/// Returns the colour of a particle: its frequency colour, or its phase colour from its coherence.
LinearColour SpatialSpectrogramView::pointColour(const SpectralPoint& point) const
{
    if (colours == SpectrogramColours::frequency) {
        const double share
            = std::clamp(frequencyToShare(static_cast<double>(point.frequency), FrequencyScale::logarithmic), 0.0, 1.0);
        return columnColours.at(static_cast<std::size_t>(share * static_cast<double>(COLUMNS - 1)));
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

/// Spreads every cell a little into its four neighbours, so older particles smear while new ones stay sharp.
/// Edge cells use themselves as their missing neighbour.
/// Both passes run along whole rows of the same length without index clamping,
/// so the compiler vectorizes them for NEON and AVX2.
void SpatialSpectrogramView::blurBuffer()
{
    constexpr float centreWeight = 1.0F - 2.0F * BLUR_NEIGHBOUR_WEIGHT;
    const auto rowOf = [](std::span<float> plane, const int row) {
        return plane.subspan(static_cast<std::size_t>(row) * COLUMNS).first(COLUMNS);
    };
    for (auto& plane : buffer) {
        for (int row = 0; row < SPECTROGRAM_ROWS; ++row) {
            const auto source = rowOf(plane, row);
            const auto blurred = rowOf(blurScratch, row);
            blurred.front() = source.front() * centreWeight + (source.front() + source[1]) * BLUR_NEIGHBOUR_WEIGHT;
            for (std::size_t column = 1; column + 1 < COLUMNS; ++column) {
                blurred[column]
                    = source[column] * centreWeight + (source[column - 1] + source[column + 1]) * BLUR_NEIGHBOUR_WEIGHT;
            }
            blurred.back()
                = source.back() * centreWeight + (source[COLUMNS - 2] + source.back()) * BLUR_NEIGHBOUR_WEIGHT;
        }
        for (int row = 0; row < SPECTROGRAM_ROWS; ++row) {
            const auto above = rowOf(blurScratch, std::max(0, row - 1));
            const auto middle = rowOf(blurScratch, row);
            const auto below = rowOf(blurScratch, std::min(SPECTROGRAM_ROWS - 1, row + 1));
            const auto destination = rowOf(plane, row);
            for (std::size_t column = 0; column < COLUMNS; ++column) {
                destination[column]
                    = middle[column] * centreWeight + (above[column] + below[column]) * BLUR_NEIGHBOUR_WEIGHT;
            }
        }
    }
}

/// Paints a light fill over the hovered band, the band names with the width of the hovered band,
/// and the crossover lines with their frequencies and a glow that follows the hover.
void SpatialSpectrogramView::paintBands(juce::Graphics& graphics, const Crossovers& crossovers) const
{
    const auto plot = getPlotBounds();
    graphics.setFont(textFont(LABEL_FONT_HEIGHT));

    float left = plot.getX();
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float right
            = band < CROSSOVER_COUNT ? frequencyToX(static_cast<double>(crossovers.at(band))) : plot.getRight();
        const float highlight = bandHighlights.at(band);
        if (highlight > 0.0F) {
            graphics.setColour(juce::Colours::white.withAlpha(BAND_HOVER_ALPHA * highlight));
            graphics.fillRect(juce::Rectangle<float>(left, plot.getY(), right - left, plot.getHeight()));
        }
        const juce::Rectangle<float> nameArea(left, plot.getY() + LABEL_INSET, right - left, LABEL_FONT_HEIGHT);
        graphics.setColour(juce::Colour(palette::GREY_LIGHT).withAlpha(BAND_LABEL_ALPHA));
        graphics.drawText(bandName(band), nameArea, juce::Justification::centred);
        if (highlight > 0.0F) {
            graphics.setColour(juce::Colour(palette::GREY_LIGHT).withAlpha(BAND_LABEL_ALPHA * highlight));
            graphics.drawText(
                "Width " + juce::String(bandWidth(band), 2),
                nameArea.translated(0.0F, LABEL_FONT_HEIGHT + LABEL_INSET),
                juce::Justification::centred
            );
        }
        left = right;
    }

    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const float x = frequencyToX(static_cast<double>(crossovers.at(crossover)));
        const float highlight = crossoverHighlights.at(crossover);
        const auto lineColour = juce::Colour(palette::YELLOW);
        if (highlight > 0.0F) {
            graphics.setColour(lineColour.withAlpha(CROSSOVER_GLOW_ALPHA * highlight));
            graphics.drawLine(x, plot.getY(), x, plot.getBottom(), CROSSOVER_GLOW_THICKNESS * highlight);
        }
        graphics.setColour(lineColour.withAlpha(CROSSOVER_ALPHA + (1.0F - CROSSOVER_ALPHA) * highlight));
        graphics.drawLine(x, plot.getY(), x, plot.getBottom(), CROSSOVER_THICKNESS);
        const juce::Rectangle<float> label(
            x + LABEL_INSET,
            plot.getBottom() - LABEL_FONT_HEIGHT - LABEL_INSET,
            CROSSOVER_LABEL_WIDTH,
            LABEL_FONT_HEIGHT
        );
        graphics.drawText(formatFrequency(crossovers.at(crossover)), label, juce::Justification::centredLeft);
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
/// small and monospaced like a game's frame rate overlay.
void SpatialSpectrogramView::paintFramesPerSecond(juce::Graphics& graphics) const
{
    if (!framesPerSecond.has_value()) {
        return;
    }
    const auto plot = getPlotBounds();
    graphics.setFont(monoFont(LABEL_FONT_HEIGHT));
    graphics.setColour(juce::Colour(FPS_COLOUR));
    const juce::Rectangle<float> area(
        plot.getX() + LABEL_INSET, plot.getY() + LABEL_INSET, FPS_LABEL_WIDTH, LABEL_FONT_HEIGHT
    );
    graphics.drawText(
        juce::String(juce::roundToInt(*framesPerSecond)) + " FPS", area, juce::Justification::centredLeft
    );
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

/// Renders the particle buffer into the image through the tone curve, channel by channel.
void SpatialSpectrogramView::renderImage()
{
    constexpr float curveScale = static_cast<float>(TONE_CURVE_STEPS - 1) / TONE_CURVE_MAX * EXPOSURE;
    const auto toCurve = [this](const float energy) {
        const auto step = std::min(static_cast<std::size_t>(energy * curveScale), TONE_CURVE_STEPS - 1);
        return toneCurve[step];
    };
    const juce::Image::BitmapData pixels(image, juce::Image::BitmapData::writeOnly);
    for (int row = 0; row < SPECTROGRAM_ROWS; ++row) {
        for (int column = 0; column < SPECTROGRAM_COLUMNS; ++column) {
            const std::size_t index = cellIndex(column, row);
            const juce::PixelARGB pixel(
                255, toCurve(buffer[0][index]), toCurve(buffer[1][index]), toCurve(buffer[2][index])
            );
            std::memcpy(pixels.getPixelPointer(column, row), &pixel, sizeof(pixel));
        }
    }
}

/// Updates which band and crossover the pointer is over, and the cursor.
void SpatialSpectrogramView::updateHover(const juce::Point<float> position)
{
    hoveredCrossover = draggedCrossover.has_value() ? draggedCrossover : crossoverNear(position.x);
    hoveredBand = hoveredCrossover.has_value() ? std::nullopt : bandAt(position.x);
    setMouseCursor(
        hoveredCrossover.has_value() ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor
    );
}
}  // namespace esgrove::image_control
