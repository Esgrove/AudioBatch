/// Tests the mouse drags on SpatialSpectrogramView with synthesized mouse events through its real handlers:
/// moving a middle band moves both its crossovers, moving the Low band moves only its upper crossover,
/// a band move stops at the minimum ratio to the crossover that stays,
/// and Ctrl or Cmd with a vertical drag changes the band's side gain.
/// Also tests the particles the view prepares for the GPU renderer:
/// which points become particles, how bright and where they are, and what a render batch carries.

#include "analysis/SpatialAnalyzer.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/SpatialSpectrogramView.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <cstddef>

namespace esgrove::image_control::test
{
namespace
{
/// Size of the view in the tests, in pixels.
constexpr int VIEW_WIDTH {1000};
/// Height of the view in the tests, in pixels.
constexpr int VIEW_HEIGHT {400};

/// Returns the plain value of a parameter of a processor.
float parameterValue(ImageControlProcessor& processor, const juce::String& parameterId)
{
    return plainValue(*processor.getParameters().getParameter(parameterId));
}

/// Returns a mouse event on the view at a position, for a drag that started at another position.
juce::MouseEvent mouseEvent(
    SpatialSpectrogramView& view,
    const juce::Point<float> position,
    const juce::Point<float> downPosition,
    const juce::ModifierKeys modifiers
)
{
    const auto now = juce::Time::getCurrentTime();
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        position,
        modifiers,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        &view,
        &view,
        now,
        downPosition,
        now,
        1,
        position != downPosition,
    };
}

/// Drags on the view from a start to an end position with modifier keys held, through its mouse handlers.
void dragOnView(
    SpatialSpectrogramView& view,
    const juce::Point<float> start,
    const juce::Point<float> end,
    const juce::ModifierKeys modifiers = {}
)
{
    const auto buttons = modifiers.withFlags(juce::ModifierKeys::leftButtonModifier);
    view.mouseDown(mouseEvent(view, start, start, buttons));
    view.mouseDrag(mouseEvent(view, end, start, buttons));
    view.mouseUp(mouseEvent(view, end, start, modifiers));
}

/// Returns the x coordinate of a frequency in the view's plot on the logarithmic axis.
float xOf(const SpatialSpectrogramView& view, const double frequency)
{
    const auto plot = view.getPlotBounds();
    return plot.getX() + static_cast<float>(frequencyToShare(frequency, FrequencyScale::logarithmic)) * plot.getWidth();
}

/// Returns the frequency an octave count above another.
double octavesAbove(const double frequency, const double octaves)
{
    return frequency * std::exp2(octaves);
}

/// Time one analyzed frame stands for in the tests, in seconds: a hop of 512 samples at 48 kHz.
constexpr double FRAME_SECONDS {512.0 / 48'000.0};

/// Returns an analyzed point at a frequency, a stereo position, and a level.
SpectralPoint point(const float frequency, const float position, const float level)
{
    return {.frequency = frequency, .spacing = 10.0F, .position = position, .level = level, .coherence = 1.0F};
}

/// Returns the summed colour channels of a particle, which follow its weight.
float brightness(const SpectrogramParticle& particle)
{
    return particle.red + particle.green + particle.blue;
}
}  // namespace

TEST(SpectrogramView, DraggingAMiddleBandMovesBothItsCrossovers)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    // The Low Mid band spans 100 Hz to 500 Hz. Dragging from its middle up by one octave moves both edges.
    const float centreY = view.getPlotBounds().getCentreY();
    const float start = xOf(view, 220.0);
    const float end = xOf(view, octavesAbove(220.0, 1.0));
    dragOnView(view, {start, centreY}, {end, centreY});

    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 200.0F, 2.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 1000.0F, 10.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(2)), 5000.0F, 1.0F);
}

TEST(SpectrogramView, DraggingTheLowBandMovesOnlyItsUpperCrossover)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const float centreY = view.getPlotBounds().getCentreY();
    dragOnView(view, {xOf(view, 50.0), centreY}, {xOf(view, octavesAbove(50.0, 0.5)), centreY});

    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), octavesAbove(100.0, 0.5), 2.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 500.0F, 1.0F);
}

TEST(SpectrogramView, BandMoveStopsAtTheMinimumRatioToTheCrossoverThatStays)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    // The High Mid band spans 500 Hz to 5 kHz.
    // A drag far to the left stops when 500 Hz reaches a third of an octave above the low crossover at 100 Hz,
    // and the band keeps its width on the axis.
    const float centreY = view.getPlotBounds().getCentreY();
    dragOnView(view, {xOf(view, 1500.0), centreY}, {xOf(view, 25.0), centreY});

    const float lowest = 100.0F * MIN_CROSSOVER_RATIO;
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 100.0F, 0.5F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), lowest, 1.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(2)), lowest * 10.0F, 10.0F);
}

TEST(SpectrogramView, CommandDragChangesTheBandWidth)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    const float x = xOf(view, 2000.0);
    const float centreY = view.getPlotBounds().getCentreY();
    // 50 pixels up at 0.12 dB per pixel raises the High Mid side gain by 6 dB, leaving the crossovers alone.
    dragOnView(view, {x, centreY}, {x, centreY - 50.0F}, juce::ModifierKeys::commandModifier);

    EXPECT_NEAR(parameterValue(processor, sideGainParameterId(2)), 6.0F, 0.05F);
    EXPECT_NEAR(parameterValue(processor, midGainParameterId(2)), 0.0F, 0.01F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 500.0F, 1.0F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(2)), 5000.0F, 1.0F);
}

TEST(SpectrogramView, QuietPointsAreDroppedAndLouderPointsAreBrighter)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    // -90 dB is below the display floor, -20 dB and 0 dB are inside the display range.
    const std::array points {point(1000.0F, 0.0F, 1.0e-9F), point(1000.0F, 0.0F, 1.0e-2F), point(1000.0F, 0.0F, 1.0F)};
    view.addFrame(points, FRAME_SECONDS);

    const auto batch = view.takeRenderBatch();
    ASSERT_EQ(batch.particles.size(), 2U);
    EXPECT_GT(brightness(batch.particles[0]), 0.0F);
    EXPECT_GT(brightness(batch.particles[1]), brightness(batch.particles[0]));
}

TEST(SpectrogramView, ParticlesArePlacedInSharesOfThePlot)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    // Full left is at the top of the plot, and full right at the bottom.
    const std::array points {point(1000.0F, 1.0F, 1.0F), point(100.0F, -1.0F, 1.0F), point(5000.0F, 0.0F, 1.0F)};
    view.addFrame(points, FRAME_SECONDS);

    const auto batch = view.takeRenderBatch();
    ASSERT_EQ(batch.particles.size(), 3U);
    EXPECT_NEAR(batch.particles[0].x, frequencyToShare(1000.0, FrequencyScale::logarithmic), 1.0e-6);
    EXPECT_FLOAT_EQ(batch.particles[0].y, 0.0F);
    EXPECT_NEAR(batch.particles[1].x, frequencyToShare(100.0, FrequencyScale::logarithmic), 1.0e-6);
    EXPECT_FLOAT_EQ(batch.particles[1].y, 1.0F);
    EXPECT_FLOAT_EQ(batch.particles[2].y, 0.5F);
    // A bin is wider on the axis at low frequencies, where the same spacing covers more of an octave.
    EXPECT_GT(batch.particles[1].spread, batch.particles[2].spread);
}

TEST(SpectrogramView, RenderBatchCarriesTheFadeAndStartsOver)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    view.setPersistence(1.0);
    view.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    view.advance(0.01);
    view.advance(0.02);

    const auto batch = view.takeRenderBatch();
    EXPECT_EQ(batch.particles.size(), 1U);
    EXPECT_NEAR(batch.fadeSeconds, 0.03, 1.0e-12);
    EXPECT_DOUBLE_EQ(batch.persistenceSeconds, 1.0);
    EXPECT_FALSE(batch.clear);

    const auto next = view.takeRenderBatch();
    EXPECT_TRUE(next.particles.empty());
    EXPECT_DOUBLE_EQ(next.fadeSeconds, 0.0);
}

TEST(SpectrogramView, ClearingAndChangingTheScaleClearThePicture)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    view.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    view.clear();
    const auto cleared = view.takeRenderBatch();
    EXPECT_TRUE(cleared.clear);
    EXPECT_TRUE(cleared.particles.empty());

    view.setFrequencyScale(FrequencyScale::mel);
    EXPECT_TRUE(view.takeRenderBatch().clear);
    view.setFrequencyScale(FrequencyScale::mel);
    EXPECT_FALSE(view.takeRenderBatch().clear);
}
}  // namespace esgrove::image_control::test
