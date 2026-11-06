/// Implements SpatialSpectrogramView: the palette, the raster of the analyzer image,
/// the frequency and position grid, the band regions, and dragging the crossover lines.

#include "ui/SpatialSpectrogramView.h"

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// Energy shown as black, in dB relative to a full-scale sine.
constexpr float DISPLAY_FLOOR_DB {-60.0F};
/// Energy shown at full brightness, in dB relative to a full-scale sine.
constexpr float DISPLAY_CEILING_DB {0.0F};
/// Width of the position labels on the left, in pixels.
constexpr float POSITION_GUTTER {28.0F};
/// Height of the frequency labels at the bottom, in pixels.
constexpr float FREQUENCY_GUTTER {18.0F};
/// Horizontal distance within which a click grabs a crossover line, in pixels.
constexpr float GRAB_DISTANCE {6.0F};
/// Font height of the axis and band labels.
constexpr float LABEL_FONT_HEIGHT {11.0F};
/// Frequencies with a grid line and a label.
constexpr std::array<double, 10> GRID_FREQUENCIES {20, 50, 100, 200, 500, 1000, 2000, 5000, 10'000, 20'000};
/// Stereo positions with a grid line, from full left to full right.
constexpr std::array<double, 5> GRID_POSITIONS {1.0, 0.5, 0.0, -0.5, -1.0};
/// Labels of the grid positions.
constexpr std::array<const char*, 5> GRID_POSITION_LABELS {"L", "", "C", "", "R"};

/// Opacity of the band names.
constexpr float BAND_LABEL_ALPHA {0.7F};
/// Opacity of the axis labels.
constexpr float AXIS_LABEL_ALPHA {0.6F};
/// Opacity of a crossover line that is not being dragged.
constexpr float IDLE_CROSSOVER_ALPHA {0.8F};
/// Thickness of a crossover line that is not being dragged, in pixels.
constexpr float IDLE_CROSSOVER_THICKNESS {1.5F};
/// Thickness of the crossover line being dragged, in pixels.
constexpr float DRAGGED_CROSSOVER_THICKNESS {2.5F};
/// Opacity multiplier of the centre line, which is drawn stronger than the other grid lines.
constexpr float CENTRE_LINE_EMPHASIS {1.6F};
/// Index of the centre line in GRID_POSITIONS.
constexpr std::size_t CENTRE_POSITION_INDEX {2};
/// Space between a line and its label, in pixels.
constexpr float LABEL_INSET {4.0F};
/// Width of a crossover label, in pixels.
constexpr float CROSSOVER_LABEL_WIDTH {70.0F};
/// Width of a frequency axis label, in pixels.
constexpr float FREQUENCY_LABEL_WIDTH {40.0F};

/// Background of the plot, as ARGB.
constexpr juce::uint32 BACKGROUND {0xff0b0d12};
/// Grid lines and axis labels, as ARGB.
constexpr juce::uint32 GRID {0x40ffffff};
/// Crossover lines and their labels, as ARGB.
constexpr juce::uint32 CROSSOVER {0xffffc857};

/// The colours of a palette gradient from silence to full scale, as ARGB.
struct PaletteColours {
    juce::uint32 low {0};
    juce::uint32 middle {0};
    juce::uint32 high {0};
};
/// Gradient position of the low colour stop.
constexpr double LOW_STOP {0.35};
/// Gradient position of the middle colour stop.
constexpr double MIDDLE_STOP {0.7};
/// Colours of in-phase content: deep blue, cyan, and white.
constexpr PaletteColours IN_PHASE_COLOURS {.low = 0xff123c8c, .middle = 0xff2bc4d8, .high = 0xffffffff};
/// Colours of anti-phase content: dark red, orange, and pale pink.
constexpr PaletteColours ANTI_PHASE_COLOURS {.low = 0xff7a1020, .middle = 0xfff0552d, .high = 0xffffd6e0};

/// Returns a gradient from black through the low and middle colours to the high colour.
juce::ColourGradient makeGradient(const PaletteColours& colours)
{
    juce::ColourGradient gradient(juce::Colours::black, 0.0F, 0.0F, juce::Colour(colours.high), 1.0F, 0.0F, false);
    gradient.addColour(LOW_STOP, juce::Colour(colours.low));
    gradient.addColour(MIDDLE_STOP, juce::Colour(colours.middle));
    return gradient;
}

/// Builds the palette:
/// energy levels from black to bright, blended by coherence from the in-phase gradient to the anti-phase gradient.
std::vector<juce::Colour> buildPalette()
{
    const juce::ColourGradient inPhase = makeGradient(IN_PHASE_COLOURS);
    const juce::ColourGradient antiPhase = makeGradient(ANTI_PHASE_COLOURS);

    std::vector<juce::Colour> palette;
    palette.reserve(static_cast<std::size_t>(PALETTE_SIZE) * static_cast<std::size_t>(COHERENCE_STEPS));
    for (int step = 0; step < COHERENCE_STEPS; ++step) {
        const float antiPhaseShare = static_cast<float>(step) / static_cast<float>(COHERENCE_STEPS - 1);
        for (int level = 0; level < PALETTE_SIZE; ++level) {
            const double position = static_cast<double>(level) / static_cast<double>(PALETTE_SIZE - 1);
            palette.push_back(inPhase.getColourAtPosition(position).interpolatedWith(
                antiPhase.getColourAtPosition(position), antiPhaseShare
            ));
        }
    }
    return palette;
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
}  // namespace

/// Creates the view over an analyzer the editor feeds, with the crossover parameters of the tree.
SpatialSpectrogramView::SpatialSpectrogramView(
    const SpatialAnalyzer& spectrumAnalyzer,
    juce::AudioProcessorValueTreeState& parameters
) :
    image(juce::Image::RGB, SPECTROGRAM_COLUMNS, SPECTROGRAM_ROWS, true),
    analyzer(spectrumAnalyzer),
    palette(buildPalette())
{
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        crossoverParameters.at(crossover) = parameters.getParameter(crossoverParameterId(crossover));
        jassert(crossoverParameters.at(crossover) != nullptr);
    }
    setOpaque(true);
}

/// Returns the area of the spectrogram inside the axis labels.
juce::Rectangle<float> SpatialSpectrogramView::getPlotBounds() const
{
    // Half a label of room above and to the right keeps the top position label and the last frequency label whole.
    return getLocalBounds()
        .toFloat()
        .withTrimmedLeft(POSITION_GUTTER)
        .withTrimmedBottom(FREQUENCY_GUTTER)
        .withTrimmedTop(LABEL_FONT_HEIGHT * 0.5F)
        .withTrimmedRight(FREQUENCY_LABEL_WIDTH * 0.5F);
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
    repaint();
}

/// Shows a resize cursor over the crossover lines.
void SpatialSpectrogramView::mouseMove(const juce::MouseEvent& event)
{
    setMouseCursor(
        crossoverNear(event.position.x).has_value() ? juce::MouseCursor::LeftRightResizeCursor
                                                    : juce::MouseCursor::NormalCursor
    );
}

/// Ends the drag gesture.
void SpatialSpectrogramView::mouseUp(const juce::MouseEvent& /*event*/)
{
    if (draggedCrossover.has_value()) {
        crossoverParameters.at(*draggedCrossover)->endChangeGesture();
        draggedCrossover.reset();
    }
}

/// Paints the spectrogram, the grid, the band regions, and the crossover lines.
void SpatialSpectrogramView::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(BACKGROUND));
    const auto plot = getPlotBounds();
    graphics.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
    graphics.drawImage(image, plot, juce::RectanglePlacement::stretchToFit);
    paintGrid(graphics);
    paintBands(graphics, currentCrossovers());
}

/// Renders the analyzer's current image and repaints. The editor calls it after feeding the analyzer.
void SpatialSpectrogramView::refresh()
{
    renderImage();
    repaint();
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
        const auto* parameter = crossoverParameters.at(crossover);
        crossovers.at(crossover) = parameter->convertFrom0to1(parameter->getValue());
    }
    return orderCrossovers(crossovers);
}

/// Returns the x coordinate of a frequency in the plot.
float SpatialSpectrogramView::frequencyToX(const double frequency) const
{
    const auto plot = getPlotBounds();
    const double share = SpatialAnalyzer::frequencyToColumn(frequency) / static_cast<double>(SPECTROGRAM_COLUMNS);
    return plot.getX() + static_cast<float>(share) * plot.getWidth();
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
    const auto share = static_cast<double>((x - plot.getX()) / plot.getWidth());
    return SpatialAnalyzer::columnToFrequency(share * static_cast<double>(SPECTROGRAM_COLUMNS));
}

/// Paints the band names along the top of their regions and the crossover lines with their frequencies.
void SpatialSpectrogramView::paintBands(juce::Graphics& graphics, const Crossovers& crossovers) const
{
    const auto plot = getPlotBounds();
    graphics.setFont(juce::FontOptions(LABEL_FONT_HEIGHT));

    float left = plot.getX();
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        const float right
            = band < CROSSOVER_COUNT ? frequencyToX(static_cast<double>(crossovers.at(band))) : plot.getRight();
        graphics.setColour(juce::Colours::white.withAlpha(BAND_LABEL_ALPHA));
        graphics.drawText(
            bandName(band),
            juce::Rectangle<float>(left, plot.getY() + LABEL_INSET, right - left, LABEL_FONT_HEIGHT),
            juce::Justification::centred
        );
        left = right;
    }

    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const float x = frequencyToX(static_cast<double>(crossovers.at(crossover)));
        const bool dragged = draggedCrossover == crossover;
        graphics.setColour(juce::Colour(CROSSOVER).withAlpha(dragged ? 1.0F : IDLE_CROSSOVER_ALPHA));
        graphics.drawLine(
            x, plot.getY(), x, plot.getBottom(), dragged ? DRAGGED_CROSSOVER_THICKNESS : IDLE_CROSSOVER_THICKNESS
        );
        const juce::Rectangle<float> label(
            x + LABEL_INSET,
            plot.getY() + LABEL_FONT_HEIGHT + LABEL_INSET * 2.0F,
            CROSSOVER_LABEL_WIDTH,
            LABEL_FONT_HEIGHT
        );
        graphics.drawText(formatFrequency(crossovers.at(crossover)), label, juce::Justification::centredLeft);
    }
}

/// Paints the frequency and position grid lines and their labels.
void SpatialSpectrogramView::paintGrid(juce::Graphics& graphics) const
{
    const auto plot = getPlotBounds();
    graphics.setFont(juce::FontOptions(LABEL_FONT_HEIGHT));
    for (const double frequency : GRID_FREQUENCIES) {
        const float x = frequencyToX(frequency);
        graphics.setColour(juce::Colour(GRID));
        graphics.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        graphics.setColour(juce::Colours::white.withAlpha(AXIS_LABEL_ALPHA));
        const juce::Rectangle<float> label(
            x - FREQUENCY_LABEL_WIDTH * 0.5F, plot.getBottom(), FREQUENCY_LABEL_WIDTH, FREQUENCY_GUTTER
        );
        graphics.drawText(formatGridFrequency(frequency), label, juce::Justification::centred);
    }
    for (std::size_t index = 0; index < GRID_POSITIONS.size(); ++index) {
        const float y = positionToY(GRID_POSITIONS.at(index));
        graphics.setColour(
            index == CENTRE_POSITION_INDEX ? juce::Colour(GRID).withMultipliedAlpha(CENTRE_LINE_EMPHASIS)
                                           : juce::Colour(GRID)
        );
        graphics.drawHorizontalLine(juce::roundToInt(y), plot.getX(), plot.getRight());
        graphics.setColour(juce::Colours::white.withAlpha(AXIS_LABEL_ALPHA));
        const juce::Rectangle<float> label(
            0.0F, y - LABEL_FONT_HEIGHT * 0.5F, POSITION_GUTTER - LABEL_INSET, LABEL_FONT_HEIGHT
        );
        graphics.drawText(GRID_POSITION_LABELS.at(index), label, juce::Justification::centredRight);
    }
}

/// Converts the analyzer's energy and coherence into image pixels:
/// the energy in dB picks the brightness, and the mean coherence picks the hue.
void SpatialSpectrogramView::renderImage()
{
    const auto energy = analyzer.getEnergy();
    const auto coherentEnergy = analyzer.getCoherentEnergy();
    const float floorEnergy = std::pow(10.0F, DISPLAY_FLOOR_DB * 0.1F);
    constexpr float levelScale = static_cast<float>(PALETTE_SIZE - 1) / (DISPLAY_CEILING_DB - DISPLAY_FLOOR_DB);
    constexpr float coherenceScale = static_cast<float>(COHERENCE_STEPS - 1) * 0.5F;

    const juce::Image::BitmapData pixels(image, juce::Image::BitmapData::writeOnly);
    for (int row = 0; row < SPECTROGRAM_ROWS; ++row) {
        for (int column = 0; column < SPECTROGRAM_COLUMNS; ++column) {
            const auto cell = static_cast<std::size_t>(row) * static_cast<std::size_t>(SPECTROGRAM_COLUMNS)
                + static_cast<std::size_t>(column);
            const float cellEnergy = energy[cell];
            if (cellEnergy <= floorEnergy) {
                pixels.setPixelColour(column, row, palette.front());
                continue;
            }
            const float decibels = 10.0F * std::log10(cellEnergy);
            const int level
                = std::clamp(juce::roundToInt((decibels - DISPLAY_FLOOR_DB) * levelScale), 0, PALETTE_SIZE - 1);
            const float coherence = std::clamp(coherentEnergy[cell] / cellEnergy, -1.0F, 1.0F);
            const int step = std::clamp(juce::roundToInt((1.0F - coherence) * coherenceScale), 0, COHERENCE_STEPS - 1);
            pixels.setPixelColour(
                column,
                row,
                palette
                    [static_cast<std::size_t>(step) * static_cast<std::size_t>(PALETTE_SIZE)
                     + static_cast<std::size_t>(level)]
            );
        }
    }
}
}  // namespace esgrove::image_control
