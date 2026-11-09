/// Tests independent analytical correlation expectations and integration with the processed multiband signals.
/// Metering must not change PCM, even for the exact neutral and Delta paths.

#include "dsp/BandCorrelation.h"
#include "dsp/MultibandImager.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <thread>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Processes an arbitrary-sized buffer in the requested block size.
void renderMetered(MultibandImager& imager, juce::AudioBuffer<float>& buffer, const int blockSize)
{
    for (int offset = 0; offset < buffer.getNumSamples(); offset += blockSize) {
        juce::AudioBuffer<float> block(
            buffer.getArrayOfWritePointers(),
            buffer.getNumChannels(),
            offset,
            std::min(blockSize, buffer.getNumSamples() - offset)
        );
        imager.process(block);
    }
}
}  // namespace

TEST(Correlation, KnownSignalsHaveAnalyticalSignedReadings)
{
    for (const double rate : {44'100.0, 48'000.0, 96'000.0, 192'000.0}) {
        for (const float sign : {-1.0F, 1.0F}) {
            BandCorrelation meter;
            meter.prepare(rate);
            for (int sample = 0; sample < static_cast<int>(rate); ++sample) {
                const auto value = static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(sample) / rate)
                );
                meter.push(0, value, sign * value);
            }
            meter.publish();
            EXPECT_NEAR(meter.readings()[0], sign, 1.0e-6F);
            EXPECT_TRUE(std::isnan(meter.readings()[1]));
        }
    }
}

TEST(Correlation, IndependentNoiseIsNearZeroAndSilenceOrOneChannelIsUnavailable)
{
    BandCorrelation meter;
    meter.prepare(TEST_SAMPLE_RATE);
    const auto left = makeNoise(0.5F, 96'000, 91);
    const auto right = makeNoise(0.5F, 96'000, 92);
    for (std::size_t sample = 0; sample < left.size(); ++sample) {
        meter.push(0, left[sample], right[sample]);
        meter.push(1, 0.0F, 0.0F);
        meter.push(2, left[sample], 0.0F);
        meter.push(3, 0.0F, right[sample]);
    }
    meter.publish();
    EXPECT_NEAR(meter.readings()[0], 0.0F, 0.04F);
    for (std::size_t band = 1; band < BAND_COUNT; ++band) {
        EXPECT_TRUE(std::isnan(meter.readings()[band]));
    }
    meter.reset();
    for (const float value : meter.readings()) {
        EXPECT_TRUE(std::isnan(value));
    }
}

TEST(Correlation, SampleRateChangesPreserveThePhysicalIntegrationTime)
{
    float reference = 0.0F;
    for (const double rate : {48'000.0, 96'000.0, 192'000.0}) {
        BandCorrelation meter;
        meter.prepare(rate);
        for (int sample = 0; sample < static_cast<int>(rate); ++sample) {
            const auto signal
                = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(sample) / rate));
            meter.push(0, signal, sample < static_cast<int>(rate * 0.9) ? signal : -signal);
        }
        meter.publish();
        const float value = meter.readings()[0];
        ASSERT_TRUE(std::isfinite(value));
        if (rate > 48'000.0) {
            EXPECT_NEAR(value, reference, 0.001F);
        }
        reference = value;
    }
}

TEST(Correlation, ProcessedBandMetersFollowWidthAndPanWithoutChangingPcm)
{
    const auto left = makeNoise(0.5F, 48'000, 93);
    const auto right = makeNoise(0.5F, 48'000, 94);
    for (const int mode : {0, 1, 2}) {
        auto metered = makeBuffer({left, right});
        auto unmetered = makeBuffer({left, right});
        MultibandImager first;
        MultibandImager second;
        BandGains side {};
        side.fill(mode == 1 ? 0.0F : 1.0F);
        BandGains mid {};
        mid.fill(1.0F);
        BandPans pans {};
        pans.fill(mode == 2 ? 1.0F : 0.0F);
        for (auto* imager : {&first, &second}) {
            imager->setGains(mid, side, 1.0F);
            imager->setPans(pans);
            imager->prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        }
        first.setCorrelationEnabled(true);
        renderMetered(first, metered, 37);
        renderMetered(second, unmetered, 37);
        for (int channel = 0; channel < 2; ++channel) {
            for (int sample = 0; sample < metered.getNumSamples(); ++sample) {
                ASSERT_EQ(metered.getSample(channel, sample), unmetered.getSample(channel, sample));
            }
        }
        for (const float value : first.getBandCorrelations()) {
            if (mode == 1) {
                EXPECT_NEAR(value, 1.0F, 1.0e-6F);
            } else if (mode == 2) {
                EXPECT_TRUE(std::isnan(value));
            } else {
                EXPECT_TRUE(std::isfinite(value));
                EXPECT_LE(std::abs(value), 1.0F);
            }
        }
        first.setCorrelationEnabled(false);
        for (const float value : first.getBandCorrelations()) {
            EXPECT_TRUE(std::isnan(value));
        }
    }
}

TEST(Correlation, RealtimeKernelHandoffsRemainSafeUnderConcurrentRequests)
{
    MultibandImager imager;
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    imager.setCorrelationEnabled(true);
    std::atomic<bool> finished {false};
    std::atomic<bool> valid {true};
    std::thread reader([&imager, &finished, &valid] {
        while (!finished.load(std::memory_order_acquire)) {
            for (const float reading : imager.getBandCorrelations()) {
                if (!std::isnan(reading) && (!std::isfinite(reading) || std::abs(reading) > 1.0F)) {
                    valid.store(false, std::memory_order_relaxed);
                }
            }
            std::this_thread::yield();
        }
    });
    const auto noise = makeNoise(0.5F, 256, 96);
    auto buffer = makeBuffer({noise, noise});
    for (int pass = 0; pass < 300; ++pass) {
        const auto offset = static_cast<float>(pass % 20);
        imager.setCrossovers({80.0F + offset, 500.0F + offset * 20.0F, 5000.0F + offset * 100.0F}, false);
        buffer.copyFrom(0, 0, noise.data(), 256);
        buffer.copyFrom(1, 0, noise.data(), 256);
        imager.process(buffer);
        if (pass % 20 == 0) {
            std::this_thread::yield();
        }
    }
    finished.store(true, std::memory_order_release);
    reader.join();
    EXPECT_TRUE(valid.load(std::memory_order_relaxed));
    imager.release();
}

TEST(Correlation, BandReadingsAreBlockInvariantAndIgnoreDelta)
{
    const auto left = makeNoise(0.5F, 48'128, 95);
    std::vector<float> right(left.size());
    std::ranges::transform(left, right.begin(), [](const float value) { return -value; });
    BandCorrelations reference {};
    for (const int size : {1, 37, 256, 511, 4097}) {
        MultibandImager imager;
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        imager.setCorrelationEnabled(true);
        imager.setDelta(size != 1);
        auto buffer = makeBuffer({left, right});
        renderMetered(imager, buffer, size);
        const auto readings = imager.getBandCorrelations();
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            EXPECT_NEAR(readings[band], -1.0F, 1.0e-6F);
            if (size != 1) {
                EXPECT_FLOAT_EQ(readings[band], reference[band]);
            }
        }
        reference = readings;
    }
}
}  // namespace esgrove::image_control::test
