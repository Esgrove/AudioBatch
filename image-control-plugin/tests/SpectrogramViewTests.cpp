/// Tests the mouse drags on SpatialSpectrogramView with synthesized mouse events through its real handlers:
/// moving a middle band moves both its crossovers, moving the Low band moves only its upper crossover,
/// a band move stops at the minimum ratio to the crossover that stays,
/// and Ctrl or Cmd with a vertical drag changes the band's side gain.
/// Also tests that the stereo position curve overlay shows after a curve change and fades out.
/// Also tests the particles SpectrogramParticleBuilder prepares for the GPU renderer:
/// which points become particles, how bright and large they are under the spectral tilt and the auto-scale,
/// where they are placed, and the frequency palette.

#include "analysis/SpatialAnalyzer.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

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
    return {
        .frequency = frequency,
        .reassignedFrequency = frequency,
        .spacing = 10.0F,
        .position = position,
        .level = level,
        .coherence = 1.0F,
    };
}

/// Returns the level in power of a point that is a number of dB below full scale.
float levelFromDecibels(const float decibels)
{
    return std::pow(10.0F, decibels / 10.0F);
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

TEST(SpectrogramView, PositionCurveOverlayShowsAfterAChangeAndFades)
{
    ImageControlProcessor processor;
    SpatialSpectrogramView view(processor.getParameters());
    view.setSize(VIEW_WIDTH, VIEW_HEIGHT);
    // Opening the editor sets the saved curve, which does not show the overlay.
    view.setPositionCurve(StereoPositionCurve());
    view.advance(0.1);
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.0F);
    view.setPositionCurve(StereoPositionCurve());
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.0F);

    view.setPositionCurve(StereoPositionCurve(10.0, 32.0));
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
    for (int refresh = 0; refresh < 24; ++refresh) {
        view.advance(0.1);
    }
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
    view.advance(0.35);
    EXPECT_GT(view.positionCurveOpacity(), 0.0F);
    EXPECT_LT(view.positionCurveOpacity(), 1.0F);
    view.advance(0.5);
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 0.0F);

    // Another change shows it again in full, and painting it works at any opacity.
    view.setPositionCurve(StereoPositionCurve(10.0, 40.0));
    EXPECT_FLOAT_EQ(view.positionCurveOpacity(), 1.0F);
    const juce::Image canvas(juce::Image::ARGB, VIEW_WIDTH, VIEW_HEIGHT, true);
    juce::Graphics graphics(canvas);
    view.paintEntireComponent(graphics, false);
}

TEST(SpectrogramParticles, QuietPointsAreDroppedAndLouderPointsAreBrighter)
{
    SpectrogramParticleBuilder builder;
    // The loudest point sets the auto-scale reference to 0 dB.
    // -90 dB is below the display range under it, and -20 dB and 0 dB are inside it.
    const std::array points {point(1000.0F, 0.0F, 1.0e-9F), point(1000.0F, 0.0F, 1.0e-2F), point(1000.0F, 0.0F, 1.0F)};
    builder.addFrame(points, FRAME_SECONDS);

    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 2U);
    EXPECT_GT(brightness(particles[0]), 0.0F);
    EXPECT_GT(brightness(particles[1]), brightness(particles[0]));
}

TEST(SpectrogramParticles, ParticlesArePlacedInSharesOfThePlot)
{
    SpectrogramParticleBuilder builder;
    // Full left is at the top of the plot, and full right at the bottom.
    const std::array points {point(1000.0F, 1.0F, 1.0F), point(100.0F, -1.0F, 1.0F), point(5000.0F, 0.0F, 1.0F)};
    builder.addFrame(points, FRAME_SECONDS);

    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 3U);
    EXPECT_NEAR(particles[0].x, frequencyToShare(1000.0, FrequencyScale::logarithmic), 1.0e-6);
    EXPECT_FLOAT_EQ(particles[0].y, 0.0F);
    EXPECT_NEAR(particles[1].x, frequencyToShare(100.0, FrequencyScale::logarithmic), 1.0e-6);
    EXPECT_FLOAT_EQ(particles[1].y, 1.0F);
    EXPECT_FLOAT_EQ(particles[2].y, 0.5F);
    // A bin is wider on the axis at low frequencies, where the same spacing covers more of an octave.
    EXPECT_GT(particles[1].spread, particles[2].spread);
}

TEST(SpectrogramParticles, ParticlesFollowTheFrequencyScale)
{
    SpectrogramParticleBuilder builder;
    builder.setFrequencyScale(FrequencyScale::mel);
    builder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    ASSERT_EQ(builder.particles().size(), 1U);
    EXPECT_NEAR(builder.particles()[0].x, frequencyToShare(1000.0, FrequencyScale::mel), 1.0e-6);
}

TEST(SpectrogramParticles, LongerPersistenceDrawsFainterParticles)
{
    // Long persistence lets steady content build up for longer, so each frame adds less.
    SpectrogramParticleBuilder shortBuilder;
    shortBuilder.setPersistence(0.1);
    shortBuilder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    SpectrogramParticleBuilder longBuilder;
    longBuilder.setPersistence(2.0);
    longBuilder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    EXPECT_GT(brightness(shortBuilder.particles()[0]), brightness(longBuilder.particles()[0]));
}

TEST(SpectrogramParticles, ClearDropsTheParticles)
{
    SpectrogramParticleBuilder builder;
    builder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    builder.clear();
    EXPECT_TRUE(builder.particles().empty());
}

TEST(SpectrogramParticles, TiltShowsContentFallingThreeDecibelsPerOctaveEvenly)
{
    // Music falls by about 3 dB per octave, which the tilt evens out, so the highs are as bright as the bass.
    SpectrogramParticleBuilder builder;
    std::vector<SpectralPoint> points;
    for (const float frequency : {50.0F, 200.0F, 1000.0F, 4000.0F, 10'000.0F}) {
        const float decibels = -20.0F - 3.0F * std::log2(frequency / 1000.0F);
        points.push_back(point(frequency, 0.0F, levelFromDecibels(decibels)));
    }
    // A louder point sets the reference, so the evened out points sit inside the display range.
    points.push_back(point(1000.0F, 0.0F, levelFromDecibels(-5.0F)));
    builder.addFrame(points, FRAME_SECONDS);

    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), points.size());
    // Every evened out point is 21 dB above the bottom of the 36 dB range under the -5 dB reference,
    // so each one's emphasis is that share through the contrast curve at its frequency,
    // which differs from the bass only by the extra contrast of the highs.
    constexpr float share = 21.0F / 36.0F;
    const auto contrast = static_cast<float>(DEFAULT_CONTRAST_PERCENT / MAX_CONTRAST_PERCENT);
    for (std::size_t index = 0; index + 1 < particles.size(); ++index) {
        const float frequency = points[index].frequency;
        EXPECT_NEAR(particles[index].emphasis, contrastCurve(share, frequencyContrast(frequency, contrast)), 1.0e-4F)
            << frequency;
    }
    EXPECT_NEAR(particles[0].emphasis, particles[2].emphasis, 1.0e-5F);
}

TEST(SpectrogramParticles, ContrastCurvePassesThroughItsEndsAndPivot)
{
    for (const float contrast : {0.0F, 0.3F, 0.6F, 1.0F, 1.25F}) {
        SCOPED_TRACE(contrast);
        EXPECT_FLOAT_EQ(contrastCurve(0.0F, contrast), 0.0F);
        EXPECT_FLOAT_EQ(contrastCurve(1.0F, contrast), 1.0F);
        EXPECT_NEAR(contrastCurve(0.5F + 0.2F * contrast, contrast), 0.5F, 1.0e-5F);
        float previous = 0.0F;
        for (int step = 1; step <= 100; ++step) {
            const float value = contrastCurve(static_cast<float>(step) / 100.0F, contrast);
            EXPECT_GT(value, previous) << step;
            previous = value;
        }
    }
    // No contrast leaves the share as it is, and more contrast darkens quiet content and brightens loud content.
    EXPECT_NEAR(contrastCurve(0.3F, 0.0F), 0.3F, 1.0e-6F);
    EXPECT_LT(contrastCurve(0.4F, 1.0F), contrastCurve(0.4F, 0.3F));
    EXPECT_GT(contrastCurve(0.85F, 1.0F), contrastCurve(0.85F, 0.3F));
}

TEST(SpectrogramParticles, HighsGetExtraContrast)
{
    EXPECT_FLOAT_EQ(frequencyContrast(100.0F, 0.6F), 0.6F);
    EXPECT_FLOAT_EQ(frequencyContrast(1000.0F, 0.6F), 0.6F);
    EXPECT_GT(frequencyContrast(4000.0F, 0.6F), 0.6F);
    EXPECT_LT(frequencyContrast(4000.0F, 0.6F), frequencyContrast(16'000.0F, 0.6F));
    EXPECT_FLOAT_EQ(frequencyContrast(16'000.0F, 0.6F), frequencyContrast(20'000.0F, 0.6F));

    // A quiet point is fainter in the highs than in the mids, and the halo follows the emphasis.
    SpectrogramParticleBuilder builder;
    const std::array points {
        point(1000.0F, 0.0F, levelFromDecibels(-20.0F)),
        point(1000.0F, 0.0F, 1.0F),
        point(1000.0F * 16.0F, 0.0F, levelFromDecibels(-20.0F - 12.0F)),
    };
    builder.addFrame(points, FRAME_SECONDS);
    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 3U);
    EXPECT_LT(particles[2].emphasis, particles[0].emphasis);
    EXPECT_FLOAT_EQ(particles[1].emphasis, 1.0F);
}

TEST(SpectrogramParticles, ParticlesSitAtTheReassignedFrequency)
{
    SpectrogramParticleBuilder builder;
    auto reassigned = point(1000.0F, 0.0F, 1.0F);
    reassigned.reassignedFrequency = 1010.0F;
    builder.addFrame(std::array {reassigned}, FRAME_SECONDS);
    ASSERT_EQ(builder.particles().size(), 1U);
    EXPECT_NEAR(builder.particles()[0].x, frequencyToShare(1010.0, FrequencyScale::logarithmic), 1.0e-6);
}

TEST(SpectrogramParticles, AutoScaleDrawsQuietAndLoudMaterialAlike)
{
    SpectrogramParticleBuilder quietBuilder;
    quietBuilder.addFrame(
        std::array {point(1000.0F, 0.0F, levelFromDecibels(-40.0F)), point(1000.0F, 0.0F, levelFromDecibels(-50.0F))},
        FRAME_SECONDS
    );
    SpectrogramParticleBuilder loudBuilder;
    loudBuilder.addFrame(
        std::array {point(1000.0F, 0.0F, levelFromDecibels(-10.0F)), point(1000.0F, 0.0F, levelFromDecibels(-20.0F))},
        FRAME_SECONDS
    );

    ASSERT_EQ(quietBuilder.particles().size(), 2U);
    ASSERT_EQ(loudBuilder.particles().size(), 2U);
    for (std::size_t index = 0; index < 2; ++index) {
        EXPECT_NEAR(brightness(quietBuilder.particles()[index]), brightness(loudBuilder.particles()[index]), 1.0e-5F);
    }
}

TEST(SpectrogramParticles, AutoScaleRisesAtOnceAndFallsSlowly)
{
    SpectrogramParticleBuilder builder;
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
    builder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    EXPECT_NEAR(builder.referenceLevel(), 0.0F, 1.0e-4F);

    // After the signal drops by 30 dB, the reference falls gradually instead of jumping down.
    const std::array quiet {point(1000.0F, 0.0F, levelFromDecibels(-30.0F))};
    builder.addFrame(quiet, FRAME_SECONDS);
    EXPECT_LT(builder.referenceLevel(), 0.0F);
    EXPECT_GT(builder.referenceLevel(), -1.0F);
    // Five seconds later it has reached the quieter level and stays there.
    for (int frame = 0; frame < static_cast<int>(5.0 / FRAME_SECONDS); ++frame) {
        builder.addFrame(quiet, FRAME_SECONDS);
    }
    EXPECT_NEAR(builder.referenceLevel(), -30.0F, 1.0e-3F);

    builder.resetLevel();
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
}

TEST(SpectrogramParticles, LouderParticlesAreLarger)
{
    SpectrogramParticleBuilder builder;
    const std::array points {
        point(1000.0F, 0.0F, levelFromDecibels(-30.0F)),
        point(1000.0F, 0.0F, levelFromDecibels(-15.0F)),
        point(1000.0F, 0.0F, 1.0F),
    };
    builder.addFrame(points, FRAME_SECONDS);

    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 3U);
    EXPECT_LT(particles[0].size, particles[1].size);
    EXPECT_LT(particles[1].size, particles[2].size);
    EXPECT_FLOAT_EQ(particles[2].size, 1.0F);
    EXPECT_GE(particles[0].size, 0.5F);
}

TEST(SpectrogramParticles, FrequencyColoursRunFromRedThroughGreenToBlueAndViolet)
{
    const auto colourAt = [](const double frequency) {
        return frequencyColour(static_cast<float>(frequencyToShare(frequency, FrequencyScale::logarithmic)));
    };
    const LinearColour bass = colourAt(40.0);
    EXPECT_GT(bass.red, bass.green);
    EXPECT_GT(bass.red, bass.blue);
    const LinearColour mids = colourAt(1000.0);
    EXPECT_GT(mids.green, mids.red);
    EXPECT_GT(mids.green, mids.blue);
    const LinearColour highs = colourAt(8000.0);
    EXPECT_GT(highs.blue, highs.red);
    EXPECT_GT(highs.blue, highs.green);
    const LinearColour air = colourAt(16'000.0);
    EXPECT_GT(air.red, air.green);
    EXPECT_GT(air.blue, air.green);

    // The sRGB encoding for drawing with JUCE brightens the linear values and keeps the ends fixed.
    const LinearColour encoded = toSrgb({.red = 0.0F, .green = 0.2F, .blue = 1.0F});
    EXPECT_FLOAT_EQ(encoded.red, 0.0F);
    EXPECT_GT(encoded.green, 0.2F);
    EXPECT_NEAR(encoded.blue, 1.0F, 1.0e-5F);
}
}  // namespace esgrove::image_control::test
