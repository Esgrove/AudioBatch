/// Tests SpectrogramParticleBuilder's geometry, brightness, size, auto-scale timing,
/// invalid-input handling, and frequency and phase colour palettes.

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"
#include "ui/SpectrogramParticles.h"
#include <initializer_list>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
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

TEST(SpectrogramParticles, SharedReleaseUsesAudioTimeOnceAcrossInterleavedTiers)
{
    SpectrogramParticleBuilder builder;
    const std::array loud {point(1000.0F, 0.0F, 1.0F)};
    const auto frame = [](const std::size_t tier, const double end, const bool last) {
        return SpectralFrameInfo {.tierIndex = tier, .frameSeconds = 0.01, .endSeconds = end, .lastAtTime = last};
    };
    builder.addFrame(loud, frame(0, 0.0, true));
    // A quieter replacement releases the retained loud tier. All other tiers also report silence.
    for (std::size_t tier = 0; tier < ANALYSIS_TIER_COUNT; ++tier) {
        builder.addFrame({}, frame(tier, 0.0, tier + 1 == ANALYSIS_TIER_COUNT));
    }
    for (int step = 1; step <= 100; ++step) {
        const double end = static_cast<double>(step) * 0.01;
        builder.addFrame({}, frame(4, end, false));
        builder.addFrame({}, frame(5, end, true));
    }
    EXPECT_NEAR(builder.referenceLevel(), -12.0F, 1.0e-4F);
}

TEST(SpectrogramParticles, LoudTierPeakIsRetainedBetweenItsDeadlines)
{
    SpectrogramParticleBuilder builder;
    const std::array loud {point(1000.0F, 0.0F, 1.0F)};
    builder.addFrame(loud, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.04});
    for (int step = 5; step <= 20; ++step) {
        builder.addFrame({}, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = step * 0.01});
        EXPECT_FLOAT_EQ(builder.referenceLevel(), 0.0F);
    }
    builder.addFrame({}, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.24});
    EXPECT_NEAR(builder.referenceLevel(), -0.48F, 1.0e-5F);
}

TEST(SpectrogramParticles, SimultaneousTierFramesShareOneReferenceRegardlessOfOrder)
{
    const std::array quiet {point(1000.0F, 0.0F, levelFromDecibels(-20.0F))};
    const std::array loud {point(1000.0F, 0.0F, 1.0F)};
    SpectrogramParticleBuilder quietFirst;
    quietFirst.addFrame(
        quiet, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.04, .lastAtTime = false}
    );
    EXPECT_TRUE(quietFirst.particles().empty());
    quietFirst.addFrame(loud, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = 0.04});
    SpectrogramParticleBuilder loudFirst;
    loudFirst.addFrame(
        loud, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = 0.04, .lastAtTime = false}
    );
    loudFirst.addFrame(quiet, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.04});
    ASSERT_EQ(quietFirst.particles().size(), 2U);
    ASSERT_EQ(loudFirst.particles().size(), 2U);
    EXPECT_FLOAT_EQ(quietFirst.referenceLevel(), loudFirst.referenceLevel());
    EXPECT_FLOAT_EQ(quietFirst.particles()[0].emphasis, loudFirst.particles()[1].emphasis);
    EXPECT_FLOAT_EQ(brightness(quietFirst.particles()[0]), brightness(loudFirst.particles()[1]));
    EXPECT_FLOAT_EQ(brightness(quietFirst.particles()[1]), brightness(loudFirst.particles()[0]));
    // The same peak and emphasis still receive the weight of their own tier's hop.
    SpectrogramParticleBuilder hopWeights;
    hopWeights.addFrame(
        loud, SpectralFrameInfo {.tierIndex = 0, .frameSeconds = 0.04, .endSeconds = 0.04, .lastAtTime = false}
    );
    hopWeights.addFrame(loud, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = 0.04});
    ASSERT_EQ(hopWeights.particles().size(), 2U);
    EXPECT_NEAR(
        brightness(hopWeights.particles()[0]) / brightness(hopWeights.particles()[1]),
        (1.0 - std::exp(-0.04 / 0.04)) / (1.0 - std::exp(-0.01 / 0.04)),
        1.0e-5
    );
}

TEST(SpectrogramParticles, PhaseColoursCoverBothEndsTheMidpointAndClamp)
{
    SpectrogramParticleBuilder builder;
    builder.setColours(SpectrogramColours::phase);
    std::vector<SpectralPoint> points;
    for (const float coherence : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F}) {
        auto value = point(1000.0F, 0.0F, 1.0F);
        value.coherence = coherence;
        points.push_back(value);
    }
    builder.addFrame(points, FRAME_SECONDS);
    const auto particles = builder.particles();
    ASSERT_EQ(particles.size(), 5U);
    const auto flash = static_cast<float>(1.0 - std::exp(-FRAME_SECONDS / 0.04));
    constexpr std::array red {1.0F, 1.0F, 0.55F, 0.1F, 0.1F};
    constexpr std::array green {0.07F, 0.07F, 0.26F, 0.45F, 0.45F};
    constexpr std::array blue {0.1F, 0.1F, 0.55F, 1.0F, 1.0F};
    for (std::size_t index = 0; index < particles.size(); ++index) {
        EXPECT_NEAR(particles[index].red / flash, red[index], 1.0e-6F);
        EXPECT_NEAR(particles[index].green / flash, green[index], 1.0e-6F);
        EXPECT_NEAR(particles[index].blue / flash, blue[index], 1.0e-6F);
    }
}

TEST(SpectrogramParticles, NonFiniteResultsAndInvalidGeometryAreDroppedWithoutPoisoningTheReference)
{
    SpectrogramParticleBuilder builder;
    std::vector<SpectralPoint> invalid;
    constexpr std::array members {
        &SpectralPoint::frequency,
        &SpectralPoint::reassignedFrequency,
        &SpectralPoint::spacing,
        &SpectralPoint::position,
        &SpectralPoint::level,
        &SpectralPoint::coherence,
    };
    for (const auto member : members) {
        for (const float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            auto result = point(1000.0F, 0.0F, 1.0F);
            result.*member = value;
            invalid.push_back(result);
        }
    }
    invalid.push_back(point(0.0F, 0.0F, 1.0F));
    invalid.push_back(point(1000.0F, 0.0F, -1.0F));
    builder.addFrame(invalid, FRAME_SECONDS);
    EXPECT_TRUE(builder.particles().empty());
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
    builder.addFrame(std::array {point(1000.0F, 0.0F, 1.0F)}, FRAME_SECONDS);
    ASSERT_EQ(builder.particles().size(), 1U);
    EXPECT_FLOAT_EQ(builder.referenceLevel(), 0.0F);
    EXPECT_TRUE(std::isfinite(brightness(builder.particles()[0])));
}

TEST(SpectrogramParticles, InvalidFrameTimesAndSettingsCannotPoisonLaterParticles)
{
    SpectrogramParticleBuilder builder;
    const std::array points {point(1000.0F, 0.0F, 1.0F)};
    for (const double invalid :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0})
    {
        builder.addFrame(points, SpectralFrameInfo {.frameSeconds = invalid, .endSeconds = 0.1});
        builder.addFrame(points, SpectralFrameInfo {.frameSeconds = 0.01, .endSeconds = invalid});
    }
    EXPECT_TRUE(builder.particles().empty());
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
    builder.setContrast(std::numeric_limits<double>::quiet_NaN());
    builder.setPersistence(std::numeric_limits<double>::quiet_NaN());
    builder.addFrame(points, FRAME_SECONDS);
    ASSERT_EQ(builder.particles().size(), 1U);
    EXPECT_TRUE(std::isfinite(brightness(builder.particles()[0])));
    EXPECT_FLOAT_EQ(builder.referenceLevel(), 0.0F);
    builder.resetLevel();
    EXPECT_FLOAT_EQ(builder.referenceLevel(), MIN_REFERENCE_DB);
    builder.addFrame({}, SpectralFrameInfo {.tierIndex = 5, .frameSeconds = 0.01, .endSeconds = 0.01});
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
