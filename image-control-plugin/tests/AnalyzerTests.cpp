/// Tests SpatialAnalyzer and the spectrogram's frequency axis:
/// the stereo position curve, where hard-panned, centred, partly panned, and anti-phase sines land,
/// with their level and coherence, how evenly wide a sine draws across the bass,
/// how finely the lowest octaves are resolved, and the logarithmic and mel axis mappings.

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"
#include "tests/TestSignals.h"
#include "ui/SpectrogramParticles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <span>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Runs a second of stereo audio through a fresh analyzer and returns the points of its last frames.
std::vector<SpectralPoint> analyze(const std::vector<float>& left, const std::vector<float>& right)
{
    SpatialAnalyzer analyzer;
    analyzer.prepare(TEST_SAMPLE_RATE);
    std::vector<SpectralPoint> latest;
    analyzer.setFrameCallback([&latest](const std::span<const SpectralPoint> points, double /*frameSeconds*/) {
        // Each tier reports its own frame, so the latest point of each frequency is kept.
        for (const auto& point : points) {
            const auto existing = std::ranges::find_if(latest, [&point](const SpectralPoint& other) {
                return juce::approximatelyEqual(other.frequency, point.frequency);
            });
            if (existing == latest.end()) {
                latest.push_back(point);
            } else {
                *existing = point;
            }
        }
    });
    analyzer.pushSamples(left, right);
    return latest;
}

/// Returns the loudest point.
SpectralPoint loudest(const std::vector<SpectralPoint>& points)
{
    return *std::ranges::max_element(points, {}, &SpectralPoint::level);
}

/// Returns the loudest point within a frequency range.
SpectralPoint loudestBetween(const std::vector<SpectralPoint>& points, const float low, const float high)
{
    std::vector<SpectralPoint> inRange;
    std::ranges::copy_if(points, std::back_inserter(inRange), [low, high](const SpectralPoint& point) {
        return point.frequency >= low && point.frequency <= high;
    });
    return loudest(inRange);
}
}  // namespace

TEST(Analyzer, HardLeftSineIsAtTheLeftEdgeWithFullLevel)
{
    const auto sine = makeSine(1000.0, 1.0F, 48'000);
    const auto peak = loudest(analyze(sine, std::vector<float>(sine.size(), 0.0F)));
    EXPECT_NEAR(peak.frequency, 1000.0F, 15.0F);
    EXPECT_FLOAT_EQ(peak.position, 1.0F);
    EXPECT_NEAR(peak.level, 1.0F, 0.15F);
}

TEST(Analyzer, HardRightSineIsAtTheRightEdge)
{
    const auto sine = makeSine(1000.0, 1.0F, 48'000);
    const auto peak = loudest(analyze(std::vector<float>(sine.size(), 0.0F), sine));
    EXPECT_FLOAT_EQ(peak.position, -1.0F);
}

TEST(Analyzer, CentredSineIsInPhaseAtTheCentre)
{
    const auto sine = makeSine(1000.0, 0.5F, 48'000);
    const auto peak = loudest(analyze(sine, sine));
    EXPECT_NEAR(peak.position, 0.0F, 1.0e-3F);
    EXPECT_NEAR(peak.coherence, 1.0F, 1.0e-3F);
}

TEST(Analyzer, AntiPhaseSineIsAtTheCentreWithNegativeCoherence)
{
    const auto sine = makeSine(1000.0, 0.5F, 48'000);
    std::vector<float> inverted(sine.size());
    std::ranges::transform(sine, inverted.begin(), [](const float value) { return -value; });
    const auto peak = loudest(analyze(sine, inverted));
    EXPECT_NEAR(peak.position, 0.0F, 1.0e-3F);
    EXPECT_NEAR(peak.coherence, -1.0F, 1.0e-3F);
}

TEST(Analyzer, PositionCurveDrawsTheMidpointHalfWayAndTheEdgeNearlyAtTheEdge)
{
    const StereoPositionCurve curve(12.0, 32.0);
    EXPECT_FLOAT_EQ(curve.position(0.0F), 0.0F);
    EXPECT_NEAR(curve.position(12.0F), 0.5F, 1.0e-5F);
    EXPECT_NEAR(curve.position(32.0F), 0.99F, 1.0e-5F);
    EXPECT_FLOAT_EQ(curve.position(-12.0F), curve.position(12.0F));
    // Rising everywhere, gently below the midpoint and flattening towards the edge.
    float previous = 0.0F;
    for (int step = 1; step <= 80; ++step) {
        const float share = curve.position(static_cast<float>(step) * 0.5F);
        EXPECT_GT(share, previous) << step;
        previous = share;
    }
    EXPECT_LT(curve.position(6.0F), 0.25F);
    EXPECT_GT(curve.position(20.0F), 0.75F);

    EXPECT_FLOAT_EQ(positionFromMagnitudes(1.0F, 0.0F, curve), 1.0F);
    EXPECT_FLOAT_EQ(positionFromMagnitudes(0.0F, 1.0F, curve), -1.0F);
    EXPECT_FLOAT_EQ(positionFromMagnitudes(0.0F, 0.0F, curve), 0.0F);
    // 12 dB louder on the right is half way to the right edge.
    EXPECT_NEAR(positionFromMagnitudes(std::pow(10.0F, -12.0F / 20.0F), 1.0F, curve), -0.5F, 1.0e-5F);
}

TEST(Analyzer, PositionCurveKeepsItsSettingsInRangeAndInOrder)
{
    const StereoPositionCurve crossed(20.0, 15.0);
    EXPECT_FLOAT_EQ(crossed.midpoint(), 20.0F);
    EXPECT_FLOAT_EQ(crossed.edge(), 20.0F + static_cast<float>(MIN_PAN_CURVE_GAP_DB));
    EXPECT_NEAR(crossed.position(crossed.edge()), 0.99F, 1.0e-5F);

    const StereoPositionCurve outside(1.0, 100.0);
    EXPECT_FLOAT_EQ(outside.midpoint(), static_cast<float>(MIN_PAN_MIDPOINT_DB));
    EXPECT_FLOAT_EQ(outside.edge(), static_cast<float>(MAX_PAN_EDGE_DB));

    const StereoPositionCurve defaults;
    EXPECT_FLOAT_EQ(defaults.midpoint(), static_cast<float>(DEFAULT_PAN_MIDPOINT_DB));
    EXPECT_FLOAT_EQ(defaults.edge(), static_cast<float>(DEFAULT_PAN_EDGE_DB));
}

TEST(Analyzer, PannedSinesLandOnTheCurve)
{
    const StereoPositionCurve curve;
    const auto sine = makeSine(1000.0, 0.5F, 48'000);
    for (const double difference : {0.0, 6.0, 12.0, 20.0, 32.0}) {
        const auto quieter = withLevelDifference(sine, difference);
        const float expected = curve.position(static_cast<float>(difference));
        EXPECT_NEAR(loudest(analyze(sine, quieter)).position, expected, 0.01F) << difference;
        EXPECT_NEAR(loudest(analyze(quieter, sine)).position, -expected, 0.01F) << difference;
    }
}

TEST(Analyzer, SineWidthIsEvenAcrossTheBass)
{
    // A sine spreads over the bins within the 36 dB display range below its peak.
    // The tiers start where the window length times the frequency is the same,
    // so the spread stays between about a sixth and a quarter of an octave across the bass and the low mids.
    // A tier that took over from a four times longer window would widen it to over 0.4 octave.
    const auto displayedWidth = [](const double frequency) {
        const auto sine = makeSine(frequency, 0.5F, 48'000);
        const auto points = analyze(sine, sine);
        const auto tone = static_cast<float>(frequency);
        const float threshold = loudestBetween(points, tone / 2.0F, tone * 2.0F).level * std::pow(10.0F, -3.6F);
        float lowest = tone;
        float highest = tone;
        for (const auto& point : points) {
            if (point.frequency > tone / 2.0F && point.frequency < tone * 2.0F && point.level >= threshold) {
                lowest = std::min(lowest, point.frequency);
                highest = std::max(highest, point.frequency);
            }
        }
        return std::log2(highest / lowest);
    };
    float narrowest = 1.0F;
    float widest = 0.0F;
    for (const double frequency : {60.0, 79.0, 81.0, 90.0, 100.0, 159.0, 161.0, 300.0, 330.0, 700.0}) {
        const float width = displayedWidth(frequency);
        EXPECT_LT(width, 0.28F) << frequency;
        narrowest = std::min(narrowest, width);
        widest = std::max(widest, width);
    }
    EXPECT_LT(widest, narrowest * 1.6F);
}

TEST(Analyzer, ResolvesNeighbouringBassTones)
{
    // Tones 5 Hz apart in the lowest octaves must show as two peaks with a clear dip between them.
    auto mixed = makeSine(40.0, 0.4F, 48'000);
    const auto upper = makeSine(45.0, 0.4F, 48'000);
    std::ranges::transform(mixed, upper, mixed.begin(), std::plus {});
    const auto points = analyze(mixed, mixed);

    const auto lowPeak = loudestBetween(points, 38.0F, 41.5F);
    const auto highPeak = loudestBetween(points, 43.5F, 47.0F);
    const auto dip = loudestBetween(points, 42.3F, 42.7F);
    EXPECT_NEAR(lowPeak.frequency, 40.0F, 0.8F);
    EXPECT_NEAR(highPeak.frequency, 45.0F, 0.8F);
    EXPECT_LT(dip.level * 4.0F, std::min(lowPeak.level, highPeak.level));
}

TEST(Analyzer, AxisMappingsRoundTrip)
{
    for (const auto scale : {FrequencyScale::logarithmic, FrequencyScale::mel}) {
        EXPECT_NEAR(frequencyToShare(static_cast<double>(MIN_FREQUENCY), scale), 0.0, 1.0e-9);
        EXPECT_NEAR(frequencyToShare(static_cast<double>(MAX_FREQUENCY), scale), 1.0, 1.0e-9);
        EXPECT_NEAR(shareToFrequency(frequencyToShare(1234.0, scale), scale), 1234.0, 1.0e-6);
    }
    EXPECT_NEAR(frequencyToMel(1000.0), 1000.0, 0.5);
    EXPECT_NEAR(melToFrequency(frequencyToMel(440.0)), 440.0, 1.0e-9);
    // The mel scale gives the bass less room than the logarithmic scale does, and the highs more.
    EXPECT_LT(frequencyToShare(100.0, FrequencyScale::mel), frequencyToShare(100.0, FrequencyScale::logarithmic));
    EXPECT_LT(frequencyToShare(5000.0, FrequencyScale::mel), frequencyToShare(5000.0, FrequencyScale::logarithmic));
}
}  // namespace esgrove::image_control::test
