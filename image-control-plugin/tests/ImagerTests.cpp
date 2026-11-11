/// Tests MultibandImager end to end:
/// the latency, the bit-exact pass-through at neutral settings in any block size,
/// mono and anti-phase output with the side or mid off, gains acting only inside their band,
/// mono processing, band pans that move a band toward one channel after its gains,
/// solos that keep only the soloed bands and their delta, and crossover moves that switch kernels without a click.

#include "dsp/CrossoverDesign.h"
#include "dsp/MultibandImager.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Latency of the imager at the test sample rate.
constexpr int LATENCY {4096 + PARTITION_SIZE};
/// Block sizes cycled through while processing, including ones that do not divide the partition size.
constexpr std::array<int, 6> BLOCK_SIZES {1, 37, 256, 511, 4097, 64};

/// Nonuniform controls make ordinary and quadrature crossover handoffs observable independently.
constexpr BandGains HANDOFF_MID_GAINS {0.4F, 1.2F, 0.7F, 1.4F};
constexpr BandGains HANDOFF_SIDE_GAINS {1.1F, 0.3F, 1.5F, 0.6F};
constexpr BandWidths HANDOFF_WIDTHS {0.25F, 0.5F, 0.0F, 0.75F};

/// Sparse source events permit direct full-support convolution without another streaming imager as the oracle.
struct HandoffImpulse {
    int sample;
    float mid;
    float side;
};

/// Each case covers one recovery, destination, and rendering-mode combination with no worker-timing assumptions.
class OfflineHandoff : public testing::TestWithParam<std::tuple<bool, bool, bool>>
{};

/// Builds a fixed-control impulse response from ordinary and quadrature taps, independently of convolution and mixing.
std::array<std::vector<double>, 2> handoffImpulseResponse(
    const Crossovers& crossovers,
    const BandRecoveries& recoveries,
    const HandoffImpulse& impulse
)
{
    constexpr int halfLength {4096};
    constexpr std::size_t taps {2 * halfLength + 1};
    std::array<std::vector<float>, CROSSOVER_COUNT> ordinary;
    std::array<std::vector<float>, CROSSOVER_COUNT> quadrature;
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        const auto frequency = static_cast<double>(crossovers[crossover]);
        ordinary[crossover] = designLowpassKernel(frequency, TEST_SAMPLE_RATE, halfLength);
        quadrature[crossover] = designQuadratureKernel(TEST_SAMPLE_RATE, halfLength, frequency);
    }
    const auto fullQuadrature = designQuadratureKernel(TEST_SAMPLE_RATE, halfLength);
    std::array<std::vector<double>, 2> result {std::vector<double>(taps), std::vector<double>(taps)};
    for (std::size_t tap = 0; tap < taps; ++tap) {
        double mid = 0.0;
        double side = 0.0;
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            const double delayed = tap == halfLength ? 1.0 : 0.0;
            const double upper = band < CROSSOVER_COUNT ? static_cast<double>(ordinary[band][tap]) : delayed;
            const double lower = band > 0 ? static_cast<double>(ordinary[band - 1][tap]) : 0.0;
            const auto quadratureUpper
                = static_cast<double>(band < CROSSOVER_COUNT ? quadrature[band][tap] : fullQuadrature[tap]);
            const double quadratureLower = band > 0 ? static_cast<double>(quadrature[band - 1][tap]) : 0.0;
            const auto midGain = static_cast<double>(HANDOFF_MID_GAINS[band]);
            const auto sideGain = static_cast<double>(HANDOFF_SIDE_GAINS[band]);
            const auto width = static_cast<double>(HANDOFF_WIDTHS[band]);
            const auto recovery = static_cast<double>(recoveries[band]);
            mid += static_cast<double>(impulse.mid) * midGain * (upper - lower)
                + static_cast<double>(impulse.side) * sideGain * recovery * (1.0 - width)
                    * (quadratureUpper - quadratureLower);
            side += static_cast<double>(impulse.side) * sideGain * width * (upper - lower);
        }
        result[0][tap] = mid + side;
        result[1][tap] = mid - side;
    }
    return result;
}

/// Tests request identity without using the editor's approximate crossover comparison.
bool exactCrossoverValues(const Crossovers& first, const Crossovers& second)
{
    return std::ranges::equal(first, second, [](const float left, const float right) {
        return juce::exactlyEqual(left, right);
    });
}

/// Returns gains of the same value for every band.
BandGains uniformGains(const float gain)
{
    BandGains gains {};
    gains.fill(gain);
    return gains;
}

/// Returns pans of the same value for every band.
BandPans uniformPans(const float pan)
{
    BandPans pans {};
    pans.fill(pan);
    return pans;
}

/// Processes a buffer through the imager in place, in blocks of varying sizes.
void processInBlocks(MultibandImager& imager, juce::AudioBuffer<float>& buffer)
{
    int start = 0;
    std::size_t blockIndex = 0;
    while (start < buffer.getNumSamples()) {
        const int size = std::min(BLOCK_SIZES.at(blockIndex % BLOCK_SIZES.size()), buffer.getNumSamples() - start);
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, size);
        imager.process(block);
        start += size;
        ++blockIndex;
    }
}

/// Compares whole partitions exactly so warm-history tests observe the first resumed output samples.
void expectSamePartition(const juce::AudioBuffer<float>& actual, const juce::AudioBuffer<float>& expected)
{
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < PARTITION_SIZE; ++sample) {
            ASSERT_EQ(actual.getSample(channel, sample), expected.getSample(channel, sample))
                << "channel " << channel << " sample " << sample;
        }
    }
}

/// Returns the RMS of the side signal of a stereo buffer from a start sample up to an end sample, or to the end.
double sideRms(const juce::AudioBuffer<float>& buffer, const int start, const int end = -1)
{
    std::vector<float> side;
    const int stop = end < 0 ? buffer.getNumSamples() : end;
    for (int sample = start; sample < stop; ++sample) {
        side.push_back((buffer.getSample(0, sample) - buffer.getSample(1, sample)) * 0.5F);
    }
    return rms(side);
}

/// Returns the RMS of the mid signal of a stereo buffer from a start sample to the end.
double midRms(const juce::AudioBuffer<float>& buffer, const int start)
{
    std::vector<float> mid;
    for (int sample = start; sample < buffer.getNumSamples(); ++sample) {
        mid.push_back((buffer.getSample(0, sample) + buffer.getSample(1, sample)) * 0.5F);
    }
    return rms(mid);
}
}  // namespace

TEST(Imager, ReportsKernelHalfLengthPlusOnePartition)
{
    MultibandImager imager;
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    EXPECT_EQ(imager.getLatencySamples(), LATENCY);
    imager.prepare(96'000.0, 2, DEFAULT_CROSSOVERS);
    EXPECT_EQ(imager.getLatencySamples(), 8192 + PARTITION_SIZE);
}

TEST(Imager, NeutralSettingsDelayTheInputBitExactly)
{
    const auto left = makeNoise(0.8F, 20'000, 10);
    const auto right = makeNoise(0.8F, 20'000, 11);
    auto buffer = makeBuffer({left, right});

    MultibandImager imager;
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    processInBlocks(imager, buffer);

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
        const auto source = static_cast<std::size_t>(sample - LATENCY);
        const float expectedLeft = sample < LATENCY ? 0.0F : left[source];
        const float expectedRight = sample < LATENCY ? 0.0F : right[source];
        ASSERT_EQ(buffer.getSample(0, sample), expectedLeft) << "sample " << sample;
        ASSERT_EQ(buffer.getSample(1, sample), expectedRight) << "sample " << sample;
    }
}

TEST(Imager, SettledNearUnityDirectGainPreservesTheNeutralCopyAndSilentDelta)
{
    const auto left = makeNoise(0.4F, 12'288, 172);
    const auto right = makeNoise(0.4F, 12'288, 173);
    for (const bool delta : {false, true}) {
        SCOPED_TRACE(delta);
        auto buffer = makeBuffer({left, right});
        MultibandImager imager;
        const float gain = std::nextafter(1.0F, 2.0F);
        // A distinct prior target lets JUCE accept this near-unity value rather than coalescing it with unity.
        imager.setGains(uniformGains(1.0F), uniformGains(1.0F), 0.5F);
        imager.setGains(uniformGains(1.0F), uniformGains(1.0F), gain);
        imager.setDelta(delta);
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        processInBlocks(imager, buffer);
        // The established settled-unity predicate treats this direct target as neutral.
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            const auto source = static_cast<std::size_t>(std::max(0, sample - LATENCY));
            const bool silent = delta || sample < LATENCY;
            ASSERT_EQ(buffer.getSample(0, sample), silent ? 0.0F : left[source]) << sample;
            ASSERT_EQ(buffer.getSample(1, sample), silent ? 0.0F : right[source]) << sample;
        }
    }
}

TEST(Imager, NeutralHistoryAndConsumedCrossoversFeedTheFirstActivePartitions)
{
    const Crossovers moved {80.0F, 1800.0F, 8000.0F};
    const auto left = makeNoise(0.4F, PARTITION_SIZE, 170);
    const auto right = makeNoise(0.4F, PARTITION_SIZE, 171);
    for (const bool offline : {false, true}) {
        for (const bool delta : {false, true}) {
            SCOPED_TRACE(offline);
            SCOPED_TRACE(delta);
            MultibandImager actual;
            MultibandImager reference;
            actual.setDelta(delta);
            reference.setDelta(delta);
            actual.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
            reference.prepare(TEST_SAMPLE_RATE, 2, moved);
            // Visible correlation forces the reference to compute every ordinary output while neutral.
            reference.setCorrelationEnabled(true);
            const auto advance = [&] {
                auto actualBlock = makeBuffer({left, right});
                auto referenceBlock = makeBuffer({left, right});
                actual.process(actualBlock);
                reference.process(referenceBlock);
                expectSamePartition(actualBlock, referenceBlock);
            };
            for (int partition = 0; partition < 40; ++partition) {
                advance();
            }
            actual.setCrossovers(moved, offline);
            for (int attempt = 0; attempt < 400 && !sameCrossovers(actual.getActiveCrossovers(), moved); ++attempt) {
                advance();
                if (!offline) {
                    juce::Thread::sleep(5);
                }
            }
            ASSERT_TRUE(sameCrossovers(actual.getActiveCrossovers(), moved));
            for (auto* imager : {&actual, &reference}) {
                imager->setGains({0.6F, 1.2F, 0.3F, 0.9F}, {1.3F, 0.4F, 1.1F, 0.7F}, 0.8F);
                imager->setPans({-0.2F, 0.3F, 0.0F, 0.5F});
                imager->setSolos({1.0F, 0.0F, 1.0F, 0.0F});
            }
            // Observe immediately, without allowing another FIR latency to refill missing history.
            for (int partition = 0; partition < 8; ++partition) {
                advance();
            }
        }
    }
}

TEST(Imager, EnteringOfflineDesignsAnUnchangedRealtimeRequestBeforeProcessing)
{
    const Crossovers moved {80.0F, 1800.0F, 8000.0F};
    const auto tone = makeSine(1000.0, 0.4F, PARTITION_SIZE);
    MultibandImager imager;
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    imager.setCrossovers(moved, false);
    // No realtime partition has consumed the request. Offline mode must take over with the same raw values.
    imager.setCrossovers(moved, true);
    auto block = makeBuffer({tone, tone});
    imager.process(block);
    EXPECT_TRUE(sameCrossovers(imager.getActiveCrossovers(), moved));
}

TEST_P(OfflineHandoff, StalePendingSlotsMatchDirectConvolution)
{
    constexpr int change {4096};
    constexpr int samples {15'000};
    constexpr int partialPartition {37};
    constexpr std::array impulses {
        HandoffImpulse {.sample = 19, .mid = 0.25F, .side = -0.5F},
        HandoffImpulse {.sample = change - 91, .mid = -0.125F, .side = 0.375F},
        HandoffImpulse {.sample = change + 83, .mid = 0.5F, .side = 0.25F},
    };
    const Crossovers stale {80.0F, 1800.0F, 8000.0F};
    const Crossovers latest {120.0F, 2400.0F, 9000.0F};
    const auto [recovering, returnToActive, enterOffline] = GetParam();
    const BandRecoveries recoveries = recovering ? BandRecoveries {0.25F, 1.5F, 0.75F, 2.0F} : BandRecoveries {};
    const auto& target = returnToActive ? DEFAULT_CROSSOVERS : latest;
    juce::AudioBuffer<float> input(2, samples);
    input.clear();
    for (const auto& impulse : impulses) {
        input.setSample(0, impulse.sample, impulse.mid + impulse.side);
        input.setSample(1, impulse.sample, impulse.mid - impulse.side);
    }
    juce::AudioBuffer<float> actual;
    juce::AudioBuffer<float> reference;
    actual.makeCopyOf(input);
    reference.makeCopyOf(input);
    MultibandImager staged;
    MultibandImager direct;
    for (auto* imager : {&staged, &direct}) {
        imager->setGains(HANDOFF_MID_GAINS, HANDOFF_SIDE_GAINS, 1.0F);
        imager->setWidths(HANDOFF_WIDTHS);
        imager->setRecoveries(recoveries);
        imager->prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    }
    juce::AudioBuffer<float> actualStart(actual.getArrayOfWritePointers(), 2, 0, change + partialPartition);
    juce::AudioBuffer<float> referenceStart(reference.getArrayOfWritePointers(), 2, 0, change + partialPartition);
    processInBlocks(staged, actualStart);
    processInBlocks(direct, referenceStart);
    // Synchronous design establishes exactly the slot the realtime worker might have published.
    // No processing consumes it before the latest request takes ownership of offline rendering.
    staged.setCrossovers(stale, true);
    if (enterOffline) {
        staged.setCrossovers(target, false);
    }
    staged.setCrossovers(target, true);
    direct.setCrossovers(target, true);
    juce::AudioBuffer<float> actualRest(
        actual.getArrayOfWritePointers(), 2, change + partialPartition, samples - change - partialPartition
    );
    juce::AudioBuffer<float> referenceRest(
        reference.getArrayOfWritePointers(), 2, change + partialPartition, samples - change - partialPartition
    );
    processInBlocks(staged, actualRest);
    processInBlocks(direct, referenceRest);
    EXPECT_TRUE(sameCrossovers(staged.getActiveCrossovers(), target));
    for (int channel = 0; channel < 2; ++channel) {
        std::vector<double> expected(samples, 0.0);
        for (const auto& impulse : impulses) {
            const auto previous = handoffImpulseResponse(DEFAULT_CROSSOVERS, recoveries, impulse);
            const auto replacement = handoffImpulseResponse(target, recoveries, impulse);
            for (std::size_t tap = 0; tap < previous[0].size(); ++tap) {
                const int output = impulse.sample + PARTITION_SIZE + static_cast<int>(tap);
                const double fade
                    = std::clamp(static_cast<double>(output - change - PARTITION_SIZE + 1) / PARTITION_SIZE, 0.0, 1.0);
                const auto index = static_cast<std::size_t>(channel);
                expected[static_cast<std::size_t>(output)]
                    += previous[index][tap] + (replacement[index][tap] - previous[index][tap]) * fade;
            }
        }
        for (int sample = 0; sample < samples; ++sample) {
            ASSERT_EQ(actual.getSample(channel, sample), reference.getSample(channel, sample)) << sample;
            ASSERT_NEAR(actual.getSample(channel, sample), expected[static_cast<std::size_t>(sample)], 2.0e-6)
                << "channel " << channel << " sample " << sample;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    PendingSlotScenarios,
    OfflineHandoff,
    testing::Combine(testing::Bool(), testing::Bool(), testing::Bool())
);

TEST(Imager, DistinctAdjacentFloatCrossoverRequestsReachEveryKernelPublication)
{
    const Crossovers original {100.0F, 1000.0F, 5000.0F};
    auto moved = original;
    for (auto& frequency : moved) {
        frequency = std::nextafter(frequency, std::numeric_limits<float>::infinity());
    }
    ASSERT_FALSE(exactCrossoverValues(original, moved));
    for (const bool offline : {false, true}) {
        SCOPED_TRACE(offline);
        MultibandImager imager;
        imager.prepare(TEST_SAMPLE_RATE, 2, original);
        imager.setCrossovers(moved, offline);
        juce::AudioBuffer<float> block(2, PARTITION_SIZE);
        for (int attempt = 0; attempt < 400 && !exactCrossoverValues(imager.getActiveCrossovers(), moved); ++attempt) {
            block.clear();
            imager.process(block);
            if (!offline) {
                juce::Thread::sleep(5);
            }
        }
        const auto active = imager.getActiveCrossovers();
        for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(active[crossover]), std::bit_cast<std::uint32_t>(moved[crossover]));
        }
    }
}

TEST(Imager, SideOffMakesBothChannelsEqual)
{
    auto buffer = makeBuffer({makeNoise(0.8F, 16'384, 12), makeNoise(0.8F, 16'384, 13)});
    MultibandImager imager;
    imager.setGains(uniformGains(1.0F), uniformGains(0.0F), 1.0F);
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    processInBlocks(imager, buffer);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
        ASSERT_EQ(buffer.getSample(0, sample), buffer.getSample(1, sample)) << "sample " << sample;
    }
    EXPECT_GT(rms(channelOf(buffer, 0).subspan(LATENCY)), 0.1);
}

TEST(Imager, MidOffMakesChannelsOpposite)
{
    auto buffer = makeBuffer({makeNoise(0.8F, 16'384, 14), makeNoise(0.8F, 16'384, 15)});
    MultibandImager imager;
    imager.setGains(uniformGains(0.0F), uniformGains(1.0F), 1.0F);
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    processInBlocks(imager, buffer);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
        ASSERT_EQ(buffer.getSample(0, sample), -buffer.getSample(1, sample)) << "sample " << sample;
    }
    EXPECT_GT(rms(channelOf(buffer, 0).subspan(LATENCY)), 0.1);
}

TEST(Imager, SideGainActsOnlyInsideItsBand)
{
    BandGains sideGains = uniformGains(1.0F);
    sideGains[1] = 2.0F;
    for (const double frequency : {40.0, 250.0, 18'000.0}) {
        SCOPED_TRACE(frequency);
        auto buffer = makeBuffer({makeSine(frequency, 0.5F, 24'000), std::vector<float>(24'000, 0.0F)});
        const double inputSide = sideRms(buffer, 8000);
        const double inputMid = midRms(buffer, 8000);
        MultibandImager imager;
        imager.setGains(uniformGains(1.0F), sideGains, 1.0F);
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        processInBlocks(imager, buffer);

        const double expectedSideRatio = frequency > 150.0 && frequency < 350.0 ? 2.0 : 1.0;
        EXPECT_NEAR(sideRms(buffer, 8000) / inputSide, expectedSideRatio, 0.01);
        EXPECT_NEAR(midRms(buffer, 8000) / inputMid, 1.0, 0.01);
    }
}

TEST(Imager, MonoInputIsProcessedAsMid)
{
    const auto input = makeNoise(0.8F, 12'000, 16);
    auto neutral = makeBuffer({input});
    MultibandImager imager;
    imager.prepare(TEST_SAMPLE_RATE, 1, DEFAULT_CROSSOVERS);
    processInBlocks(imager, neutral);
    for (int sample = LATENCY; sample < neutral.getNumSamples(); ++sample) {
        ASSERT_EQ(neutral.getSample(0, sample), input[static_cast<std::size_t>(sample - LATENCY)]);
    }

    auto halved = makeBuffer({input});
    imager.setGains(uniformGains(0.5F), uniformGains(0.0F), 1.0F);
    imager.prepare(TEST_SAMPLE_RATE, 1, DEFAULT_CROSSOVERS);
    processInBlocks(imager, halved);
    for (int sample = LATENCY; sample < halved.getNumSamples(); ++sample) {
        ASSERT_NEAR(halved.getSample(0, sample), input[static_cast<std::size_t>(sample - LATENCY)] * 0.5F, 1.0e-5F);
    }
}

TEST(Imager, HardRightPanMovesEveryBandIntoTheRightChannel)
{
    const auto left = makeNoise(0.4F, 16'384, 30);
    const auto right = makeNoise(0.4F, 16'384, 31);
    auto buffer = makeBuffer({left, right});
    MultibandImager imager;
    imager.setPans(uniformPans(1.0F));
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    processInBlocks(imager, buffer);
    for (int sample = LATENCY; sample < buffer.getNumSamples(); ++sample) {
        const auto source = static_cast<std::size_t>(sample - LATENCY);
        ASSERT_EQ(buffer.getSample(0, sample), 0.0F) << "sample " << sample;
        ASSERT_NEAR(buffer.getSample(1, sample), left[source] + right[source], 1.0e-5F) << "sample " << sample;
    }
}

TEST(Imager, PanComesAfterTheMidAndSideGains)
{
    // With the side off, every band is mono.
    // Panning it right by half leaves half the mid in the left channel
    // and puts one and a half times the mid in the right.
    const auto left = makeNoise(0.4F, 16'384, 32);
    const auto right = makeNoise(0.4F, 16'384, 33);
    auto buffer = makeBuffer({left, right});
    MultibandImager imager;
    imager.setGains(uniformGains(1.0F), uniformGains(0.0F), 1.0F);
    imager.setPans(uniformPans(0.5F));
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    processInBlocks(imager, buffer);
    for (int sample = LATENCY; sample < buffer.getNumSamples(); ++sample) {
        const auto source = static_cast<std::size_t>(sample - LATENCY);
        const float mid = (left[source] + right[source]) * 0.5F;
        ASSERT_NEAR(buffer.getSample(0, sample), mid * 0.5F, 1.0e-5F) << "sample " << sample;
        ASSERT_NEAR(buffer.getSample(1, sample), mid * 1.5F, 1.0e-5F) << "sample " << sample;
    }
}

TEST(Imager, PanActsOnlyInsideItsBand)
{
    BandPans pans = uniformPans(0.0F);
    pans[1] = -0.5F;
    for (const double frequency : {40.0, 250.0, 18'000.0}) {
        SCOPED_TRACE(frequency);
        const auto tone = makeSine(frequency, 0.5F, 24'000);
        auto buffer = makeBuffer({tone, tone});
        const double input = rms(std::span(tone).subspan(8000));
        MultibandImager imager;
        imager.setPans(pans);
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        processInBlocks(imager, buffer);

        const bool inBand = frequency > 150.0 && frequency < 350.0;
        EXPECT_NEAR(rms(channelOf(buffer, 0).subspan(8000)) / input, inBand ? 1.5 : 1.0, 0.01);
        EXPECT_NEAR(rms(channelOf(buffer, 1).subspan(8000)) / input, inBand ? 0.5 : 1.0, 0.01);
    }
}

TEST(Imager, CentringThePansAgainRestoresTheBitExactPassThrough)
{
    constexpr int samples = 20'000;
    constexpr int panned = 8000;
    const auto left = makeNoise(0.8F, samples, 34);
    const auto right = makeNoise(0.8F, samples, 35);
    auto buffer = makeBuffer({left, right});
    MultibandImager imager;
    imager.setPans(uniformPans(0.5F));
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    juce::AudioBuffer<float> first(buffer.getArrayOfWritePointers(), 2, 0, panned);
    juce::AudioBuffer<float> second(buffer.getArrayOfWritePointers(), 2, panned, samples - panned);
    processInBlocks(imager, first);
    imager.setPans(uniformPans(0.0F));
    processInBlocks(imager, second);

    // The pans glide back over GAIN_SMOOTHING_SECONDS, and the imager mixes whole partitions,
    // so the pass-through starts at most a glide and two partitions after the change.
    const int settled = panned + static_cast<int>(GAIN_SMOOTHING_SECONDS * TEST_SAMPLE_RATE) + 2 * PARTITION_SIZE;
    EXPECT_NEAR(rms(channelOf(buffer, 0).subspan(LATENCY, 2000)) / rms(std::span(left).first(2000)), 0.5, 0.01);
    for (int sample = settled; sample < samples; ++sample) {
        const auto source = static_cast<std::size_t>(sample - LATENCY);
        ASSERT_EQ(buffer.getSample(0, sample), left[source]) << "sample " << sample;
        ASSERT_EQ(buffer.getSample(1, sample), right[source]) << "sample " << sample;
    }
}

TEST(Imager, MonoIgnoredStereoControlsPreserveExactNeutralAndDeltaAcrossAutomationAndReset)
{
    constexpr int samples {12'000};
    constexpr int change {5037};
    const auto input = makeNoise(0.8F, samples, 36);
    for (const bool delta : {false, true}) {
        for (const bool metered : {false, true}) {
            SCOPED_TRACE(delta);
            SCOPED_TRACE(metered);
            MultibandImager imager;
            imager.setGains(uniformGains(1.0F), {0.0F, 0.25F, 2.0F, 0.5F}, 1.0F);
            imager.setPans({1.0F, -1.0F, 0.25F, -0.5F});
            imager.setWidths({0.0F, 0.25F, 1.5F, 2.0F});
            imager.setRecoveries({2.0F, 0.5F, 1.0F, 0.0F});
            imager.setDelta(delta);
            imager.prepare(TEST_SAMPLE_RATE, 1, DEFAULT_CROSSOVERS);
            imager.setCorrelationEnabled(metered);
            for (const bool resetting : {false, true}) {
                SCOPED_TRACE(resetting);
                if (resetting) {
                    imager.reset();
                }
                auto buffer = makeBuffer({input});
                juce::AudioBuffer<float> first(buffer.getArrayOfWritePointers(), 1, 0, change);
                processInBlocks(imager, first);
                imager.setGains(uniformGains(1.0F), {1.5F, 0.0F, 0.5F, 1.25F}, 1.0F);
                imager.setPans({-0.75F, 0.5F, -1.0F, 1.0F});
                imager.setWidths({2.0F, 1.0F, 0.0F, 0.5F});
                imager.setRecoveries({0.25F, 2.0F, 0.75F, 1.5F});
                imager.setCrossovers({80.0F, 1200.0F, 8000.0F}, true);
                juce::AudioBuffer<float> rest(buffer.getArrayOfWritePointers(), 1, change, samples - change);
                processInBlocks(imager, rest);
                for (int sample = 0; sample < samples; ++sample) {
                    const float expected
                        = delta || sample < LATENCY ? 0.0F : input[static_cast<std::size_t>(sample - LATENCY)];
                    ASSERT_EQ(buffer.getSample(0, sample), expected) << sample;
                }
            }
        }
    }
}

TEST(Imager, MonoNeutralShortcutAdvancesIgnoredControlRampsBeforeActiveProcessing)
{
    constexpr int change {12'288};
    constexpr int ignoredRampSamples {2 * PARTITION_SIZE};
    constexpr int gainRampSamples {1024};
    constexpr int samples {change + 4096};
    // At this rate the 20 ms ramp is 1024 samples, making every half-gain step exactly representable.
    // The expectation measures control timing without accumulated non-dyadic ramp rounding.
    constexpr double sampleRate {51'200.0};
    constexpr int latency {8192 + PARTITION_SIZE};
    const auto input = makeNoise(0.5F, samples, 174);
    auto buffer = makeBuffer({input});
    MultibandImager imager;
    imager.prepare(sampleRate, 1, DEFAULT_CROSSOVERS);
    juce::AudioBuffer<float> warm(buffer.getArrayOfWritePointers(), 1, 0, change - ignoredRampSamples);
    processInBlocks(imager, warm);
    imager.setGains(uniformGains(1.0F), {0.0F, 0.5F, 2.0F, 0.25F}, 1.0F);
    imager.setPans({1.0F, -1.0F, 0.5F, -0.25F});
    imager.setWidths({0.0F, 2.0F, 0.5F, 1.5F});
    imager.setRecoveries({2.0F, 0.5F, 1.0F, 1.5F});
    juce::AudioBuffer<float> ignored(
        buffer.getArrayOfWritePointers(), 1, change - ignoredRampSamples, ignoredRampSamples
    );
    processInBlocks(imager, ignored);
    imager.setGains(uniformGains(0.5F), {0.0F, 0.5F, 2.0F, 0.25F}, 1.0F);
    juce::AudioBuffer<float> active(buffer.getArrayOfWritePointers(), 1, change, samples - change);
    processInBlocks(imager, active);
    for (int sample = 0; sample < samples; ++sample) {
        const float progress
            = std::clamp(static_cast<float>(sample - change - PARTITION_SIZE + 1) / gainRampSamples, 0.0F, 1.0F);
        const float expected
            = sample < latency ? 0.0F : input[static_cast<std::size_t>(sample - latency)] * (1.0F - progress * 0.5F);
        ASSERT_NEAR(buffer.getSample(0, sample), expected, 2.0e-6F) << sample;
        if (sample < change + PARTITION_SIZE && sample >= latency) {
            ASSERT_EQ(buffer.getSample(0, sample), expected) << sample;
        }
    }
}

TEST(Imager, CrossoverMoveSwitchesKernelsWithoutAClick)
{
    constexpr int samples = 48'000;
    const auto tone = makeSine(700.0, 0.5F, samples);
    auto buffer = makeBuffer({tone, std::vector<float>(samples, 0.0F)});

    MultibandImager imager;
    BandGains sideGains = uniformGains(1.0F);
    sideGains[1] = 0.25F;
    sideGains[2] = 2.0F;
    imager.setGains(uniformGains(1.0F), sideGains, 1.0F);
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);

    const Crossovers moved {100.0F, 1500.0F, 5000.0F};
    juce::AudioBuffer<float> first(buffer.getArrayOfWritePointers(), 2, 0, samples / 2);
    juce::AudioBuffer<float> second(buffer.getArrayOfWritePointers(), 2, samples / 2, samples / 2);
    processInBlocks(imager, first);
    imager.setCrossovers(moved, true);
    processInBlocks(imager, second);

    const auto active = imager.getActiveCrossovers();
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        EXPECT_FLOAT_EQ(active.at(crossover), moved.at(crossover));
    }

    float largestInputStep = 0.0F;
    for (std::size_t sample = 1; sample < tone.size(); ++sample) {
        largestInputStep = std::max(largestInputStep, std::abs(tone[sample] - tone[sample - 1]));
    }
    for (int channel = 0; channel < 2; ++channel) {
        const auto output = channelOf(buffer, channel);
        for (std::size_t sample = static_cast<std::size_t>(LATENCY) + 1; sample < output.size(); ++sample) {
            ASSERT_LT(std::abs(output[sample] - output[sample - 1]), largestInputStep * 2.0F)
                << "channel " << channel << " sample " << sample;
        }
    }
    // The tone moves from the High Mid band, with twice the side, to the Low Mid band, with a quarter of it.
    EXPECT_GT(sideRms(buffer, samples / 2 - 8000, samples / 2), sideRms(buffer, samples - 8000) * 4.0);
}

TEST(Imager, RealTimeCrossoverMoveIsDesignedInTheBackground)
{
    MultibandImager imager;
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    const Crossovers moved {80.0F, 3000.0F, 12'000.0F};
    imager.setCrossovers(moved, false);

    juce::AudioBuffer<float> block(2, PARTITION_SIZE);
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (juce::approximatelyEqual(imager.getActiveCrossovers()[1], moved[1])) {
            break;
        }
        block.clear();
        imager.process(block);
        juce::Thread::sleep(5);
    }
    const auto active = imager.getActiveCrossovers();
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        EXPECT_FLOAT_EQ(active.at(crossover), moved.at(crossover));
    }
    imager.release();
}
TEST(Imager, SoloKeepsOnlyTheSoloedBand)
{
    BandSolos soloed {};
    soloed[1] = true;
    for (const double frequency : {40.0, 250.0, 2000.0, 18'000.0}) {
        SCOPED_TRACE(frequency);
        const auto sine = makeSine(frequency, 0.5F, 24'000);
        auto buffer = makeBuffer({sine, sine});
        MultibandImager imager;
        imager.setSolos(soloGains(soloed));
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        processInBlocks(imager, buffer);

        const double ratio = midRms(buffer, 8000) / rms(std::span(sine).subspan(8000));
        if (frequency > 150.0 && frequency < 350.0) {
            EXPECT_NEAR(ratio, 1.0, 0.01);
        } else {
            EXPECT_LT(ratio, 0.01);
        }
    }
}

TEST(Imager, UnsoloingRestoresTheBitExactPassThrough)
{
    constexpr int samples = 20'000;
    constexpr int soloed = 8000;
    const auto left = makeNoise(0.8F, samples, 40);
    const auto right = makeNoise(0.8F, samples, 41);
    auto buffer = makeBuffer({left, right});
    BandSolos solos {};
    solos[2] = true;
    MultibandImager imager;
    imager.setSolos(soloGains(solos));
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    juce::AudioBuffer<float> first(buffer.getArrayOfWritePointers(), 2, 0, soloed);
    juce::AudioBuffer<float> second(buffer.getArrayOfWritePointers(), 2, soloed, samples - soloed);
    processInBlocks(imager, first);
    // Soloing every band is the same as soloing none.
    imager.setSolos(soloGains({true, true, true, true}));
    processInBlocks(imager, second);

    const int settled = soloed + static_cast<int>(GAIN_SMOOTHING_SECONDS * TEST_SAMPLE_RATE) + 2 * PARTITION_SIZE;
    EXPECT_LT(rms(channelOf(buffer, 0).subspan(LATENCY, 2000)), rms(std::span(left).first(2000)) * 0.9);
    for (int sample = settled; sample < samples; ++sample) {
        const auto source = static_cast<std::size_t>(sample - LATENCY);
        ASSERT_EQ(buffer.getSample(0, sample), left[source]) << "sample " << sample;
        ASSERT_EQ(buffer.getSample(1, sample), right[source]) << "sample " << sample;
    }
}

TEST(Imager, DeltaWithASoloedBandIsSilentAtNeutralGains)
{
    auto buffer = makeBuffer({makeNoise(0.8F, 16'384, 42), makeNoise(0.8F, 16'384, 43)});
    BandSolos solos {};
    solos[1] = true;
    MultibandImager imager;
    imager.setSolos(soloGains(solos));
    imager.setDelta(true);
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    processInBlocks(imager, buffer);
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
        ASSERT_NEAR(buffer.getSample(0, sample), 0.0F, 1e-6F) << "sample " << sample;
        ASSERT_NEAR(buffer.getSample(1, sample), 0.0F, 1e-6F) << "sample " << sample;
    }
}

TEST(Imager, DeltaWithASoloedBandCarriesOnlyThatBandsChange)
{
    BandSolos solos {};
    solos[1] = true;
    for (const double frequency : {250.0, 2000.0}) {
        SCOPED_TRACE(frequency);
        // A side-only sine, which turning every band's side off removes in full.
        const auto sine = makeSine(frequency, 0.5F, 24'000);
        std::vector<float> inverted(sine.size());
        std::ranges::transform(sine, inverted.begin(), [](const float value) { return -value; });
        auto buffer = makeBuffer({sine, inverted});
        MultibandImager imager;
        imager.setGains(uniformGains(1.0F), uniformGains(0.0F), 1.0F);
        imager.setSolos(soloGains(solos));
        imager.setDelta(true);
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        processInBlocks(imager, buffer);

        const double ratio = sideRms(buffer, 8000) / rms(std::span(sine).subspan(8000));
        EXPECT_NEAR(ratio, frequency < 500.0 ? 1.0 : 0.0, 0.01);
    }
}
}  // namespace esgrove::image_control::test
