/// Tests SpatialAnalyzer and the spectrogram's frequency axis:
/// the stereo position curve, where hard-panned, centred, partly panned, and anti-phase sines,
/// sine combs, and noise land, with their level and coherence, how evenly wide a sine draws across the bass,
/// how finely the lowest octaves are resolved, the reassignment of a sine's bins to its frequency,
/// the averaged position of uncorrelated highs, and the logarithmic and mel axis mappings.

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"
#include "tests/TestSignals.h"
#include "ui/SpectrogramParticles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Full callback observation, including its tier identity and shared audio time.
struct CapturedFrame {
    SpectralFrameInfo metadata;
    std::vector<SpectralPoint> points;
};

/// Runs a second of stereo audio through a fresh analyzer and returns the points of its last frames.
std::vector<SpectralPoint> analyze(const std::vector<float>& left, const std::vector<float>& right)
{
    SpatialAnalyzer analyzer;
    analyzer.prepare(TEST_SAMPLE_RATE);
    std::vector<SpectralPoint> latest;
    analyzer.setFrameCallback(
        [&latest](const std::span<const SpectralPoint> points, const SpectralFrameInfo& /*frame*/) {
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
        }
    );
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

/// Captures every frame, including empty frames, without combining or sorting their points.
std::vector<CapturedFrame> analyzeFrames(
    const std::span<const float> left,
    const std::span<const float> right,
    const double sampleRate,
    const std::size_t chunkSize
)
{
    SpatialAnalyzer analyzer;
    analyzer.prepare(sampleRate);
    std::vector<CapturedFrame> frames;
    analyzer.setFrameCallback([&frames](const std::span<const SpectralPoint> points, const SpectralFrameInfo& frame) {
        frames.push_back({.metadata = frame, .points = {points.begin(), points.end()}});
    });
    for (std::size_t first = 0; first < left.size(); first += chunkSize) {
        const auto count = std::min(chunkSize, left.size() - first);
        analyzer.pushSamples(left.subspan(first, count), right.subspan(first, count));
    }
    return frames;
}

/// Compares every reported field in its original frame and bin order.
void expectSameFrames(const std::vector<CapturedFrame>& expected, const std::vector<CapturedFrame>& actual)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        SCOPED_TRACE(frame);
        EXPECT_EQ(actual[frame].metadata.tierIndex, expected[frame].metadata.tierIndex);
        EXPECT_DOUBLE_EQ(actual[frame].metadata.frameSeconds, expected[frame].metadata.frameSeconds);
        EXPECT_DOUBLE_EQ(actual[frame].metadata.endSeconds, expected[frame].metadata.endSeconds);
        EXPECT_EQ(actual[frame].metadata.lastAtTime, expected[frame].metadata.lastAtTime);
        ASSERT_EQ(actual[frame].points.size(), expected[frame].points.size());
        for (std::size_t index = 0; index < expected[frame].points.size(); ++index) {
            SCOPED_TRACE(index);
            const auto& first = expected[frame].points[index];
            const auto& second = actual[frame].points[index];
            EXPECT_FLOAT_EQ(second.frequency, first.frequency);
            EXPECT_FLOAT_EQ(second.reassignedFrequency, first.reassignedFrequency);
            EXPECT_FLOAT_EQ(second.spacing, first.spacing);
            EXPECT_FLOAT_EQ(second.position, first.position);
            EXPECT_FLOAT_EQ(second.level, first.level);
            EXPECT_FLOAT_EQ(second.coherence, first.coherence);
        }
    }
}
}  // namespace

TEST(Analyzer, FrameSequenceIsIndependentOfInputChunkSize)
{
    for (const double sampleRate : {48'000.0, 96'000.0}) {
        SCOPED_TRACE(sampleRate);
        const auto count = static_cast<std::size_t>(8192.0 * sampleRate / 48'000.0);
        const auto left = makeNoise(0.5F, count, 87);
        const auto right = makeNoise(0.3F, count, 91);
        const auto expected = analyzeFrames(left, right, sampleRate, count);
        ASSERT_FALSE(expected.empty());
        for (const std::size_t chunk : {1U, 37U, 255U, 256U, 257U, 511U, 777U, 4096U}) {
            SCOPED_TRACE(chunk);
            expectSameFrames(expected, analyzeFrames(left, right, sampleRate, chunk));
        }
    }
}

TEST(Analyzer, TierDeadlinesAreGroupedLowToHighOnOneAudioClockAndClearRestartsIt)
{
    for (const double sampleRate : {48'000.0, 96'000.0}) {
        SCOPED_TRACE(sampleRate);
        const auto multiplier = static_cast<std::size_t>(sampleRate / 48'000.0);
        const std::vector<float> silence(4096 * multiplier, 0.0F);
        const auto frames = analyzeFrames(silence, silence, sampleRate, 37);
        constexpr std::array<std::size_t, ANALYSIS_TIER_COUNT> hops {2048, 1024, 512, 512, 512, 512};
        std::size_t callback = 0;
        for (std::size_t end = 512; end <= 4096; end += 512) {
            for (std::size_t tier = 0; tier < hops.size(); ++tier) {
                if (end % hops[tier] != 0) {
                    continue;
                }
                ASSERT_LT(callback, frames.size());
                const auto& frame = frames[callback++];
                EXPECT_TRUE(frame.points.empty());
                EXPECT_EQ(frame.metadata.tierIndex, tier);
                EXPECT_DOUBLE_EQ(
                    frame.metadata.frameSeconds, static_cast<double>(hops[tier] * multiplier) / sampleRate
                );
                EXPECT_DOUBLE_EQ(frame.metadata.endSeconds, static_cast<double>(end * multiplier) / sampleRate);
                EXPECT_EQ(frame.metadata.lastAtTime, tier + 1 == ANALYSIS_TIER_COUNT);
            }
        }
        EXPECT_EQ(callback, frames.size());

        SpatialAnalyzer analyzer;
        analyzer.prepare(sampleRate);
        std::vector<SpectralFrameInfo> observed;
        analyzer.setFrameCallback([&observed](
                                      std::span<const SpectralPoint> /*points*/, const SpectralFrameInfo& frame
                                  ) { observed.push_back(frame); });
        analyzer.pushSamples(silence, silence);
        analyzer.clear();
        observed.clear();
        const auto firstHop = std::span<const float>(silence).first(512 * multiplier);
        analyzer.pushSamples(firstHop, firstHop);
        ASSERT_EQ(observed.size(), 4U);
        EXPECT_DOUBLE_EQ(observed.front().endSeconds, 512.0 / 48'000.0);
        EXPECT_EQ(observed.front().tierIndex, 2U);
        EXPECT_TRUE(observed.back().lastAtTime);
    }
}

TEST(Analyzer, CachedPowersMatchDirectNeighbourhoodSummationInBinOrder)
{
    constexpr std::size_t WINDOW_LENGTH {1024};
    constexpr std::size_t FFT_SIZE {WINDOW_LENGTH * 2};
    constexpr double BIN_WIDTH {TEST_SAMPLE_RATE / static_cast<double>(FFT_SIZE)};
    constexpr std::size_t FIRST_BIN {171};
    constexpr std::size_t LAST_BIN {853};
    const auto left = makeNoise(0.5F, WINDOW_LENGTH, 47);
    const auto right = makeNoise(0.3F, WINDOW_LENGTH, 49);
    const auto frames = analyzeFrames(left, right, TEST_SAMPLE_RATE, WINDOW_LENGTH);
    ASSERT_FALSE(frames.empty());
    ASSERT_EQ(frames.back().metadata.tierIndex, ANALYSIS_TIER_COUNT - 1);

    const juce::dsp::FFT transform(11);
    std::vector<std::complex<float>> samples(FFT_SIZE);
    std::vector<std::complex<float>> spectrum(FFT_SIZE);
    for (std::size_t index = 0; index < WINDOW_LENGTH; ++index) {
        samples[index] = {left[index], right[index]};
    }
    transform.perform(samples.data(), spectrum.data(), false);
    const auto channels = [&spectrum](const std::size_t bin) {
        const auto packed = spectrum[bin];
        const auto mirrored = std::conj(spectrum[(FFT_SIZE - bin) % FFT_SIZE]);
        const auto difference = (packed - mirrored) * 0.5F;
        return std::pair {(packed + mirrored) * 0.5F, std::complex<float>(difference.imag(), -difference.real())};
    };
    std::vector<std::complex<float>> hannLeft(LAST_BIN - FIRST_BIN + 1);
    std::vector<std::complex<float>> hannRight(hannLeft.size());
    for (std::size_t bin = FIRST_BIN; bin <= LAST_BIN; ++bin) {
        const auto [centreLeft, centreRight] = channels(bin);
        const auto [lowerLeft, lowerRight] = channels(bin - 2);
        const auto [upperLeft, upperRight] = channels(bin + 2);
        hannLeft[bin - FIRST_BIN] = centreLeft * 0.5F - (lowerLeft + upperLeft) * 0.25F;
        hannRight[bin - FIRST_BIN] = centreRight * 0.5F - (lowerRight + upperRight) * 0.25F;
    }
    const double smoothingRatio = std::exp2(POSITION_SMOOTHING_OCTAVES * 0.5) - 1.0;
    const StereoPositionCurve curve;
    const auto& points = frames.back().points;
    ASSERT_EQ(points.size(), hannLeft.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
        const double frequency = static_cast<double>(FIRST_BIN + index) * BIN_WIDTH;
        const auto reach = static_cast<std::size_t>(std::floor(frequency * smoothingRatio / BIN_WIDTH));
        const std::size_t first = index - std::min(index, reach);
        const std::size_t last = std::min(points.size() - 1, index + reach);
        float leftPower = 0.0F;
        float rightPower = 0.0F;
        float crossPower = 0.0F;
        for (std::size_t neighbour = first; neighbour <= last; ++neighbour) {
            // Production stores each bin's float powers before summing the neighbourhood.
            // Volatile preserves that rounding boundary under -ffp-contract=fast,
            // preventing fusion with the accumulators without changing the comparison tolerance.
            const volatile float neighbourLeftPower = std::norm(hannLeft[neighbour]);
            const volatile float neighbourRightPower = std::norm(hannRight[neighbour]);
            const volatile float neighbourCrossPower = (hannLeft[neighbour] * std::conj(hannRight[neighbour])).real();
            leftPower += neighbourLeftPower;
            rightPower += neighbourRightPower;
            crossPower += neighbourCrossPower;
        }
        const float level = (std::norm(hannLeft[index]) + std::norm(hannRight[index])) / (256.0F * 256.0F);
        EXPECT_FLOAT_EQ(points[index].level, level);
        EXPECT_FLOAT_EQ(
            points[index].position, positionFromMagnitudes(std::sqrt(leftPower), std::sqrt(rightPower), curve)
        );
        EXPECT_FLOAT_EQ(
            points[index].coherence, std::clamp(crossPower / std::sqrt(leftPower * rightPower), -1.0F, 1.0F)
        );
    }
}

TEST(Analyzer, NonFiniteAudioIsDisplaySilenceWithoutMutatingTheInputAndRecovers)
{
    auto left = makeNoise(0.5F, 4096, 18);
    auto right = makeNoise(0.4F, 4096, 19);
    left[17] = std::numeric_limits<float>::quiet_NaN();
    left[1027] = std::numeric_limits<float>::infinity();
    right[233] = -std::numeric_limits<float>::infinity();
    auto cleanLeft = left;
    auto cleanRight = right;
    for (const auto channel : {std::ref(cleanLeft), std::ref(cleanRight)}) {
        for (float& value : channel.get()) {
            if (!std::isfinite(value)) {
                value = 0.0F;
            }
        }
    }
    expectSameFrames(analyzeFrames(cleanLeft, cleanRight, 48'000.0, 37), analyzeFrames(left, right, 48'000.0, 37));
    EXPECT_TRUE(std::isnan(left[17]));
    EXPECT_TRUE(std::isinf(left[1027]));
    EXPECT_TRUE(std::isinf(right[233]));

    SpatialAnalyzer analyzer;
    std::vector<SpectralPoint> latest;
    analyzer.setFrameCallback([&latest](const std::span<const SpectralPoint> points, const auto& /*frame*/) {
        latest.assign(points.begin(), points.end());
        for (const auto& value : points) {
            EXPECT_TRUE(std::isfinite(value.frequency));
            EXPECT_TRUE(std::isfinite(value.reassignedFrequency));
            EXPECT_TRUE(std::isfinite(value.spacing));
            EXPECT_TRUE(std::isfinite(value.level));
            EXPECT_TRUE(std::isfinite(value.position));
            EXPECT_TRUE(std::isfinite(value.coherence));
            EXPECT_GE(value.coherence, -1.0F);
            EXPECT_LE(value.coherence, 1.0F);
        }
    });
    const std::vector<float> excessive(2048, std::numeric_limits<float>::max());
    analyzer.pushSamples(excessive, excessive);
    const auto recovered = makeSine(8000.0, 0.5F, 32'768);
    analyzer.pushSamples(recovered, recovered);
    ASSERT_FALSE(latest.empty());
    EXPECT_NEAR(loudest(latest).frequency, 8000.0F, 25.0F);
    EXPECT_NEAR(loudest(latest).coherence, 1.0F, 1.0e-4F);
}

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

TEST(Analyzer, PositionCurveEqualityTracksBothClampedSettings)
{
    const StereoPositionCurve defaults;
    EXPECT_EQ(defaults, StereoPositionCurve(DEFAULT_PAN_MIDPOINT_DB, DEFAULT_PAN_EDGE_DB));
    EXPECT_NE(defaults, StereoPositionCurve(DEFAULT_PAN_MIDPOINT_DB + 1.0, DEFAULT_PAN_EDGE_DB));
    EXPECT_NE(defaults, StereoPositionCurve(DEFAULT_PAN_MIDPOINT_DB, DEFAULT_PAN_EDGE_DB + 1.0));
    EXPECT_EQ(StereoPositionCurve(20.0, 15.0), StereoPositionCurve(20.0, 23.0));
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

TEST(Analyzer, CombWithAlternatingHardPansPeaksAtEachToneAndEdge)
{
    // The odd tones are in the left channel only and the even tones in the right channel only.
    const std::vector<double> leftTones {40.0, 160.0, 640.0, 2560.0, 10'240.0};
    const std::vector<double> rightTones {80.0, 320.0, 1280.0, 5120.0};
    const auto points = analyze(makeComb(leftTones, 0.1F, 48'000), makeComb(rightTones, 0.1F, 48'000));
    for (const auto& [tones, edge] : {std::pair {leftTones, 1.0F}, std::pair {rightTones, -1.0F}}) {
        for (const double tone : tones) {
            const auto frequency = static_cast<float>(tone);
            const auto peak = loudestBetween(points, frequency / 1.2F, frequency * 1.2F);
            EXPECT_NEAR(peak.frequency, frequency, frequency * 0.02F) << tone;
            EXPECT_NEAR(peak.position, edge, 1.0e-3F) << tone;
        }
    }
}

TEST(Analyzer, CombAtOnePanSitsAtOnePositionInEveryTier)
{
    const std::vector<double> tones {40.0, 80.0, 160.0, 320.0, 640.0, 1280.0, 2560.0, 5120.0, 10'240.0};
    const auto left = makeComb(tones, 0.1F, 48'000);
    const auto points = analyze(left, withLevelDifference(left, 12.0));
    for (const double tone : tones) {
        const auto frequency = static_cast<float>(tone);
        EXPECT_NEAR(loudestBetween(points, frequency / 1.2F, frequency * 1.2F).position, 0.5F, 0.01F) << tone;
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
        const float threshold = loudestBetween(points, tone * 0.5F, tone * 2.0F).level * std::pow(10.0F, -3.6F);
        float lowest = tone;
        float highest = tone;
        for (const auto& point : points) {
            if (point.frequency > tone * 0.5F && point.frequency < tone * 2.0F && point.level >= threshold) {
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

TEST(Analyzer, ReassignsTheBinsOfASineToItsFrequency)
{
    // Every bin of the main lobe, within 30 dB of the peak, moves to the sine frequency,
    // so the display draws a thin line however wide the lobe spreads on the axis.
    for (const double frequency : {31.7, 57.3, 133.3, 1003.7, 7777.7, 15'013.0}) {
        SCOPED_TRACE(frequency);
        const auto sine = makeSine(frequency, 0.5F, 48'000);
        const auto points = analyze(sine, withLevelDifference(sine, 6.0));
        const auto tone = static_cast<float>(frequency);
        const float threshold = loudestBetween(points, tone / 1.5F, tone * 1.5F).level * 1.0e-3F;
        int lobeBins = 0;
        for (const auto& point : points) {
            if (point.frequency > tone / 1.5F && point.frequency < tone * 1.5F && point.level >= threshold) {
                EXPECT_NEAR(point.reassignedFrequency, tone, 0.2F + tone * 2.0e-3F) << point.frequency;
                ++lobeBins;
            }
        }
        EXPECT_GE(lobeBins, 3);
    }
}

TEST(Analyzer, UncorrelatedHighsSitCloserToTheirAveragePosition)
{
    // Uncorrelated noise has a random level difference in every bin,
    // and measuring the position over neighbouring bins draws the dense highs close to the centre,
    // instead of spraying them over the whole height.
    const auto points = analyze(makeNoise(0.5F, 48'000, 11), makeNoise(0.5F, 48'000, 12));
    double spread = 0.0;
    double totalLevel = 0.0;
    for (const auto& point : points) {
        if (point.frequency > 8000.0F) {
            spread += static_cast<double>(std::abs(point.position) * point.level);
            totalLevel += static_cast<double>(point.level);
        }
    }
    EXPECT_LT(spread / totalLevel, 0.15);
}

TEST(Analyzer, CentredPinkNoiseSitsInTheCentreInPhase)
{
    const auto noise = makePinkNoise(0.5F, 48'000, 7);
    for (const auto& point : analyze(noise, noise)) {
        EXPECT_NEAR(point.position, 0.0F, 1.0e-3F) << point.frequency;
        EXPECT_NEAR(point.coherence, 1.0F, 1.0e-3F) << point.frequency;
    }
}

TEST(Analyzer, LeftOnlyNoiseSitsAtTheLeftEdge)
{
    const auto noise = makeNoise(0.5F, 48'000, 8);
    for (const auto& point : analyze(noise, std::vector<float>(noise.size(), 0.0F))) {
        EXPECT_FLOAT_EQ(point.position, 1.0F) << point.frequency;
    }
}

TEST(Analyzer, UncorrelatedNoiseSpreadsAroundTheCentre)
{
    const auto points = analyze(makePinkNoise(0.5F, 48'000, 9), makePinkNoise(0.5F, 48'000, 10));
    double weightedPosition = 0.0;
    double weightedSpread = 0.0;
    double totalLevel = 0.0;
    for (const auto& point : points) {
        weightedPosition += static_cast<double>(point.position * point.level);
        weightedSpread += static_cast<double>(std::abs(point.position) * point.level);
        totalLevel += static_cast<double>(point.level);
    }
    EXPECT_NEAR(weightedPosition / totalLevel, 0.0, 0.1);
    EXPECT_GT(weightedSpread / totalLevel, 0.1);
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
