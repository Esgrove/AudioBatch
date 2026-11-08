/// Tests SpatialAnalyzer and the spectrogram's frequency axis:
/// where hard-panned, centred, partly panned, and anti-phase sines land, with their level and coherence,
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

TEST(Analyzer, PositionFollowsTheLevelDifferenceOverTheFocus)
{
    const auto sine = makeSine(2000.0, 0.5F, 48'000);
    std::vector<float> quieter(sine.size());
    // 9 dB quieter on the right is half way to the left edge at the default 18 dB focus.
    const auto gain = static_cast<float>(std::pow(10.0, -9.0 / 20.0));
    std::ranges::transform(sine, quieter.begin(), [gain](const float value) { return value * gain; });
    const auto peak = loudest(analyze(sine, quieter));
    EXPECT_NEAR(peak.position, 0.5F, 0.01F);
    EXPECT_FLOAT_EQ(positionFromMagnitudes(1.0F, 0.0F, 18.0F), 1.0F);
    EXPECT_FLOAT_EQ(positionFromMagnitudes(0.0F, 0.0F, 18.0F), 0.0F);
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
