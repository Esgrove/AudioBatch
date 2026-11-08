/// Implements SpatialSpectrogramView: splatting analyzed bins into the particle buffer as Gaussian particles,
/// the frequency and phase colours, the time-based fade and blur, the tone curve that renders the buffer,
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
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Share by which the wanted grid size has to differ from the current one before the buffer is reallocated,
/// so dragging the window edge does not reallocate on every pixel.
constexpr int GRID_RESIZE_THRESHOLD_DIVISOR {8};
/// Each physical pixel of the plot height holds this many stereo position rows. The rows cost more than they show.
constexpr float ROWS_PER_PIXEL {0.5F};

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
    toneCurve(buildToneCurve())
{
    allocateGrid(
        BASE_SPECTROGRAM_COLUMNS * MIN_SPECTROGRAM_GRID_SCALE, BASE_SPECTROGRAM_ROWS * MIN_SPECTROGRAM_GRID_SCALE
    );
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

/// Reallocates the particle buffer and the image for a grid size, which clears the particles.
void SpatialSpectrogramView::allocateGrid(const int newColumns, const int newRows)
{
    columns = newColumns;
    rows = newRows;
    columnScale = static_cast<float>(columns) / static_cast<float>(BASE_SPECTROGRAM_COLUMNS);
    rowScale = static_cast<float>(rows) / static_cast<float>(BASE_SPECTROGRAM_ROWS);

    const auto cellCount = static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
    image = juce::Image(juce::Image::ARGB, columns, rows, true);
    for (auto& plane : buffer) {
        plane.assign(cellCount, 0.0F);
    }
    blurScratch.assign(cellCount, 0.0F);

    columnColours.resize(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        columnColours[static_cast<std::size_t>(column)]
            = frequencyColour(static_cast<float>(column) / static_cast<float>(columns - 1));
    }

    maxColumnRadius = static_cast<int>(std::ceil(static_cast<float>(MAX_PARTICLE_RADIUS) * columnScale));
    const int rowRadius = static_cast<int>(std::ceil(PARTICLE_SIGMA_ROWS * rowScale * PARTICLE_RADIUS_SIGMAS));
    columnWeights.assign(static_cast<std::size_t>(maxColumnRadius) * 2 + 1, 0.0F);
    rowWeights.assign(static_cast<std::size_t>(rowRadius) * 2 + 1, 0.0F);
}

/// Sizes the particle buffer to the plot in physical pixels, so the particles stay sharp on large and HiDPI displays.
/// The grid is never smaller than twice the reference grid and is capped to bound the work per display refresh.
/// Small changes are ignored, so dragging the window edge does not reallocate on every pixel.
void SpatialSpectrogramView::updateGrid()
{
    const auto plot = getPlotBounds();
    if (plot.isEmpty()) {
        return;
    }
    float displayScale = 1.0F;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds())) {
        displayScale = static_cast<float>(display->scale);
    }
    const int wantedColumns = std::clamp(
        juce::roundToInt(plot.getWidth() * displayScale),
        BASE_SPECTROGRAM_COLUMNS * MIN_SPECTROGRAM_GRID_SCALE,
        MAX_SPECTROGRAM_COLUMNS
    );
    const int wantedRows = std::clamp(
        juce::roundToInt(plot.getHeight() * displayScale * ROWS_PER_PIXEL),
        BASE_SPECTROGRAM_ROWS * MIN_SPECTROGRAM_GRID_SCALE,
        MAX_SPECTROGRAM_ROWS
    );
    const auto differs = [](const int current, const int wanted, const int lowest, const int highest) {
        const bool atLimit = (wanted == lowest || wanted == highest) && wanted != current;
        return atLimit || std::abs(wanted - current) > current / GRID_RESIZE_THRESHOLD_DIVISOR;
    };
    if (differs(columns, wantedColumns, BASE_SPECTROGRAM_COLUMNS * MIN_SPECTROGRAM_GRID_SCALE, MAX_SPECTROGRAM_COLUMNS)
        || differs(rows, wantedRows, BASE_SPECTROGRAM_ROWS * MIN_SPECTROGRAM_GRID_SCALE, MAX_SPECTROGRAM_ROWS)) {
        allocateGrid(wantedColumns, wantedRows);
    }
}

/// Resizes the particle buffer when the plot size changes.
void SpatialSpectrogramView::resized()
{
    updateGrid();
}

/// Resizes the particle buffer when the view moves to a display with another scale.
void SpatialSpectrogramView::parentHierarchyChanged()
{
    updateGrid();
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
    const float sigmaRows = PARTICLE_SIGMA_ROWS * rowScale;
    const float minSigmaColumns = MIN_PARTICLE_SIGMA_COLUMNS * columnScale;
    const auto columnCount = static_cast<double>(columns);
    const auto columnStride = static_cast<std::size_t>(columns);

    for (const auto& point : points) {
        const float decibels = 10.0F * std::log10(point.level);
        const float brightness = std::clamp((decibels - DISPLAY_FLOOR_DB) / DISPLAY_RANGE_DB, 0.0F, 1.0F);
        // Brightness follows the level in dB linearly, so quiet content stays visible next to the peaks.
        const float weight = brightness * fadeIn;
        if (weight <= 0.0F) {
            continue;
        }

        const auto frequency = static_cast<double>(point.frequency);
        const auto column = static_cast<float>(frequencyToShare(frequency, scale) * columnCount);
        const auto row
            = static_cast<float>((1.0 - static_cast<double>(point.position)) * 0.5 * static_cast<double>(rows - 1));
        // The distance to the neighbouring bins in columns depends on the frequency and the scale.
        const double halfSpacing = static_cast<double>(point.spacing) * 0.5;
        const auto spacingColumns = static_cast<float>(
            (frequencyToShare(frequency + halfSpacing, scale)
             - frequencyToShare(std::max(static_cast<double>(MIN_FREQUENCY), frequency - halfSpacing), scale))
            * columnCount
        );
        const float sigmaColumns = std::max(minSigmaColumns, spacingColumns * PARTICLE_SIGMA_PER_SPACING);
        const int columnRadius
            = std::min(maxColumnRadius, static_cast<int>(std::ceil(sigmaColumns * PARTICLE_RADIUS_SIGMAS)));
        const int rowRadius = static_cast<int>(std::ceil(sigmaRows * PARTICLE_RADIUS_SIGMAS));

        const int centreColumn = juce::roundToInt(column);
        const int centreRow = juce::roundToInt(row);
        const int firstColumn = std::max(0, centreColumn - columnRadius);
        const int lastColumn = std::min(columns - 1, centreColumn + columnRadius);
        const int firstRow = std::max(0, centreRow - rowRadius);
        const int lastRow = std::min(rows - 1, centreRow + rowRadius);
        for (int cell = firstColumn; cell <= lastColumn; ++cell) {
            columnWeights[static_cast<std::size_t>(cell - firstColumn)]
                = gaussian(static_cast<float>(cell) - column, sigmaColumns);
        }
        for (int cell = firstRow; cell <= lastRow; ++cell) {
            rowWeights[static_cast<std::size_t>(cell - firstRow)]
                = gaussian(static_cast<float>(cell) - row, sigmaRows) * weight;
        }

        const LinearColour colour = pointColour(point);
        for (int rowCell = firstRow; rowCell <= lastRow; ++rowCell) {
            const float rowWeight = rowWeights[static_cast<std::size_t>(rowCell - firstRow)];
            for (int columnCell = firstColumn; columnCell <= lastColumn; ++columnCell) {
                const float cellWeight = rowWeight * columnWeights[static_cast<std::size_t>(columnCell - firstColumn)];
                const std::size_t index
                    = static_cast<std::size_t>(rowCell) * columnStride + static_cast<std::size_t>(columnCell);
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
        const bool movedWithBand = drag == SpectrogramDrag::band && draggedBand.has_value()
            && std::ranges::contains(bandEdges(*draggedBand), crossover);
        const bool active = hoveredCrossover == crossover || draggedCrossover == crossover || movedWithBand;
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
        const Crossovers crossovers = currentCrossovers();
        paintBands(graphics, crossovers);
        paintBandResponses(graphics, crossovers);
        paintCrossovers(graphics, crossovers);
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

/// Returns the colour of a particle: its frequency colour, or its phase colour from its coherence.
LinearColour SpatialSpectrogramView::pointColour(const SpectralPoint& point) const
{
    if (colours == SpectrogramColours::frequency) {
        const double share
            = std::clamp(frequencyToShare(static_cast<double>(point.frequency), FrequencyScale::logarithmic), 0.0, 1.0);
        return columnColours.at(static_cast<std::size_t>(share * static_cast<double>(columns - 1)));
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
    // A cell is smaller on a finer grid, so the neighbour weight shrinks with the square of the grid scale
    // to keep the blur the same size on screen.
    const float columnWeight = BLUR_NEIGHBOUR_WEIGHT / (columnScale * columnScale);
    const float rowWeight = BLUR_NEIGHBOUR_WEIGHT / (rowScale * rowScale);
    const float columnCentre = 1.0F - 2.0F * columnWeight;
    const float rowCentre = 1.0F - 2.0F * rowWeight;
    const auto columnCount = static_cast<std::size_t>(columns);
    const auto rowOf = [columnCount](std::span<float> plane, const int row) {
        return plane.subspan(static_cast<std::size_t>(row) * columnCount).first(columnCount);
    };
    for (auto& plane : buffer) {
        for (int row = 0; row < rows; ++row) {
            const auto source = rowOf(plane, row);
            const auto blurred = rowOf(blurScratch, row);
            blurred.front() = source.front() * columnCentre + (source.front() + source[1]) * columnWeight;
            for (std::size_t column = 1; column + 1 < columnCount; ++column) {
                blurred[column] = source[column] * columnCentre + (source[column - 1] + source[column + 1]) * columnWeight;
            }
            blurred.back() = source.back() * columnCentre + (source[columnCount - 2] + source.back()) * columnWeight;
        }
        for (int row = 0; row < rows; ++row) {
            const auto above = rowOf(blurScratch, std::max(0, row - 1));
            const auto middle = rowOf(blurScratch, row);
            const auto below = rowOf(blurScratch, std::min(rows - 1, row + 1));
            const auto destination = rowOf(plane, row);
            for (std::size_t column = 0; column < columnCount; ++column) {
                destination[column] = middle[column] * rowCentre + (above[column] + below[column]) * rowWeight;
            }
        }
    }
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
    const auto columnStride = static_cast<std::size_t>(columns);
    for (int row = 0; row < rows; ++row) {
        auto* destination = pixels.getLinePointer(row);
        const auto rowStart = static_cast<std::size_t>(row) * columnStride;
        for (std::size_t column = 0; column < columnStride; ++column) {
            const std::size_t index = rowStart + column;
            const juce::PixelARGB pixel(
                255, toCurve(buffer[0][index]), toCurve(buffer[1][index]), toCurve(buffer[2][index])
            );
            std::memcpy(destination + column * sizeof(pixel), &pixel, sizeof(pixel));
        }
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
