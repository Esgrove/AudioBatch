/// Tests MultibandImager end to end:
/// the latency, the bit-exact pass-through at neutral settings in any block size,
/// mono and anti-phase output with the side or mid off, gains acting only inside their band,
/// mono processing, and crossover moves that switch kernels without a click.

#include "dsp/MultibandImager.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Latency of the imager at the test sample rate.
constexpr int LATENCY {4096 + PARTITION_SIZE};
/// Block sizes cycled through while processing, including ones that do not divide the partition size.
constexpr std::array<int, 6> BLOCK_SIZES {1, 37, 256, 511, 4097, 64};

/// Returns gains of the same value for every band.
BandGains uniformGains(const float gain)
{
    BandGains gains {};
    gains.fill(gain);
    return gains;
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
    for (const double frequency : {40.0, 400.0, 18'000.0}) {
        SCOPED_TRACE(frequency);
        auto buffer = makeBuffer({makeSine(frequency, 0.5F, 24'000), std::vector<float>(24'000, 0.0F)});
        const double inputSide = sideRms(buffer, 8000);
        const double inputMid = midRms(buffer, 8000);
        MultibandImager imager;
        imager.setGains(uniformGains(1.0F), sideGains, 1.0F);
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        processInBlocks(imager, buffer);

        const double expectedSideRatio = frequency > 150.0 && frequency < 800.0 ? 2.0 : 1.0;
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

    const Crossovers moved {120.0F, 500.0F, 10'000.0F};
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
    EXPECT_GT(sideRms(buffer, samples - 8000), sideRms(buffer, samples / 2 - 8000, samples / 2) * 4.0);
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
}  // namespace esgrove::image_control::test
