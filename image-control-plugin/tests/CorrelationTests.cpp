/// Tests independent analytical correlation expectations and integration with the processed multiband signals.
/// Metering must not change PCM, even for the exact neutral and Delta paths.

#include "dsp/BandCorrelation.h"
#include "dsp/MultibandImager.h"
#include "Parameters.h"
#include "tests/TestSignals.h"
#include <stop_token>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <thread>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Reads independent published meters until stopped, recording invalid values without assertions on the worker.
void monitorPublishedCorrelations(const MultibandImager& imager, std::atomic<bool>& valid, const std::stop_token& stop)
{
    while (!stop.stop_requested()) {
        for (const float reading : imager.getBandCorrelations()) {
            if (!std::isnan(reading) && (!std::isfinite(reading) || std::abs(reading) > 1.0F)) {
                valid.store(false, std::memory_order_relaxed);
            }
        }
        std::this_thread::yield();
    }
}

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

/// Selects the full, half, or zero Width used by the analytical correlation cases.
float widthForCorrelationMode(const int mode)
{
    if (mode == 0) {
        return 1.0F;
    }
    if (mode == 2 || mode == 4) {
        return 0.5F;
    }
    return 0.0F;
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

TEST(Correlation, UnequalDcOffsetsDoNotBiasSignedCenteredCorrelation)
{
    for (const float sign : {-1.0F, 1.0F}) {
        SCOPED_TRACE(sign);
        BandCorrelation meter;
        meter.prepare(TEST_SAMPLE_RATE);
        // Four seconds lets the initial zero-mean prior decay below the centered-power threshold.
        constexpr int settlingSamples {192'000};
        for (int sample = 0; sample < settlingSamples; ++sample) {
            const float signal = sample % 2 == 0 ? 0.125F : -0.125F;
            meter.push(0, 0.625F + signal, -0.25F + sign * 3.0F * signal);
        }
        meter.publish();
        const float reading = meter.readings()[0];
        ASSERT_TRUE(std::isfinite(reading));
        EXPECT_NEAR(reading, sign, 1.0e-6F);
    }
}

TEST(Correlation, SettledConstantChannelsAreUnavailableEvenWithNonzeroDcPower)
{
    BandCorrelation meter;
    meter.prepare(TEST_SAMPLE_RATE);
    constexpr int settlingSamples {192'000};
    for (int sample = 0; sample < settlingSamples; ++sample) {
        const float signal = sample % 2 == 0 ? 0.25F : -0.25F;
        meter.push(0, 0.5F, 0.5F);
        meter.push(1, 0.75F, -0.25F);
        meter.push(2, 0.5F, signal);
        meter.push(3, signal, -0.25F);
    }
    meter.publish();
    // Correlation requires variance in both channels, not merely nonzero second moments.
    for (const float reading : meter.readings()) {
        EXPECT_TRUE(std::isnan(reading));
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

TEST(Correlation, ProcessedBandMetersFollowActualWidthRecoveryAndPanWithoutChangingPcm)
{
    constexpr std::array<double, BAND_COUNT> frequencies {50.0, 250.0, 1500.0, 12'000.0};
    const auto left = makeComb(frequencies, 0.1F, 28'672);
    std::vector<float> right(left.size());
    std::ranges::transform(left, right.begin(), [](const float value) { return -value; });
    for (const int mode : {0, 1, 2, 3, 4}) {
        SCOPED_TRACE(mode);
        auto metered = makeBuffer({left, right});
        auto unmetered = makeBuffer({left, right});
        MultibandImager first;
        MultibandImager second;
        BandWidths widths {};
        widths.fill(widthForCorrelationMode(mode));
        BandRecoveries recoveries {};
        recoveries.fill(mode == 0 || mode == 3 ? 0.0F : 1.0F);
        BandPans pans {};
        pans.fill(mode == 4 ? 1.0F : 0.0F);
        for (auto* imager : {&first, &second}) {
            imager->setWidths(widths);
            imager->setRecoveries(recoveries);
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
        // Pure Side is anti-correlated. Recovered mono is correlated.
        // At half Width and full Recover, equal-power orthogonal Mid and Side give zero covariance.
        for (const float value : first.getBandCorrelations()) {
            if (mode == 0) {
                EXPECT_NEAR(value, -1.0F, 1.0e-6F);
            } else if (mode == 1) {
                EXPECT_NEAR(value, 1.0F, 1.0e-6F);
            } else if (mode == 2) {
                EXPECT_NEAR(value, 0.0F, 0.04F);
            } else {
                EXPECT_TRUE(std::isnan(value));
            }
        }
        first.setCorrelationEnabled(false);
        for (const float value : first.getBandCorrelations()) {
            EXPECT_TRUE(std::isnan(value));
        }
    }
}

TEST(Correlation, VisibilityAndControlTransitionsNeverAdvanceAudioSmoothersOrFilterHistory)
{
    constexpr int phaseSamples = 8192;
    constexpr int phases = 5;
    constexpr auto totalSamples = static_cast<std::size_t>(phaseSamples) * static_cast<std::size_t>(phases);
    const auto left = makeNoise(0.4F, totalSamples, 97);
    const auto right = makeNoise(0.4F, totalSamples, 98);
    auto metered = makeBuffer({left, right});
    auto unmetered = makeBuffer({left, right});
    MultibandImager first;
    MultibandImager second;
    for (auto* imager : {&first, &second}) {
        imager->prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    }
    constexpr std::array<int, phases> blockSizes {37, 511, 1, 4097, 257};
    for (int phase = 0; phase < phases; ++phase) {
        SCOPED_TRACE(phase);
        first.setCorrelationEnabled(phase != 2);
        for (auto* imager : {&first, &second}) {
            if (phase == 1) {
                imager->setWidths({0.0F, 0.25F, 0.5F, 1.5F});
                imager->setRecoveries({1.0F, 0.75F, 0.5F, 1.0F});
            } else if (phase == 2) {
                imager->setCrossovers({80.0F, 1200.0F, 8000.0F}, true);
                imager->setPans({-0.5F, 0.25F, 0.0F, 1.0F});
                imager->setSolos({0.0F, 1.0F, 1.0F, 0.0F});
                imager->setDelta(true);
            } else if (phase == 3) {
                imager->setWidths({0.75F, 0.0F, 0.25F, 0.5F});
                imager->setRecoveries({0.25F, 1.0F, 0.75F, 1.0F});
                imager->setGains({0.0F, 0.5F, 1.0F, 1.2F}, {1.2F, 0.5F, 0.0F, 1.0F}, 0.8F);
            } else if (phase == 4) {
                imager->setWidths({1.0F, 1.0F, 1.0F, 1.0F});
                imager->setRecoveries({0.0F, 0.0F, 0.0F, 0.0F});
                imager->setGains({1.0F, 1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}, 1.0F);
                imager->setPans({0.0F, 0.0F, 0.0F, 0.0F});
                imager->setSolos({1.0F, 1.0F, 1.0F, 1.0F});
                imager->setDelta(false);
            }
        }
        const int start = phase * phaseSamples;
        juce::AudioBuffer<float> meteredSlice(metered.getArrayOfWritePointers(), 2, start, phaseSamples);
        juce::AudioBuffer<float> unmeteredSlice(unmetered.getArrayOfWritePointers(), 2, start, phaseSamples);
        renderMetered(first, meteredSlice, blockSizes[static_cast<std::size_t>(phase)]);
        renderMetered(second, unmeteredSlice, blockSizes[static_cast<std::size_t>(phase)]);
        for (int channel = 0; channel < 2; ++channel) {
            for (int sample = 0; sample < phaseSamples; ++sample) {
                ASSERT_EQ(meteredSlice.getSample(channel, sample), unmeteredSlice.getSample(channel, sample)) << sample;
            }
        }
        for (const float value : first.getBandCorrelations()) {
            if (phase == 2) {
                EXPECT_TRUE(std::isnan(value));
            } else if (!std::isnan(value)) {
                EXPECT_TRUE(std::isfinite(value));
                EXPECT_LE(std::abs(value), 1.0F);
            }
        }
    }
}

TEST(Correlation, RealtimeRecoveryKernelHandoffsSwitchBothSlotsWithConcurrentMeterReads)
{
    MultibandImager imager;
    imager.setWidths({0.0F, 0.0F, 0.0F, 0.0F});
    imager.setRecoveries({0.25F, 0.5F, 0.75F, 1.0F});
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    imager.setCorrelationEnabled(true);
    constexpr int blockSamples {1024};
    // Every tone repeats exactly in this block and excites a different band at either requested crossover set.
    constexpr std::array<double, BAND_COUNT> frequencies {46.875, 281.25, 1500.0, 12'000.0};
    const auto side = makeComb(frequencies, 0.1F, blockSamples);
    std::vector<float> inverted(side.size());
    std::ranges::transform(side, inverted.begin(), [](const float value) { return -value; });
    auto buffer = makeBuffer({side, inverted});
    bool finitePcm = true;
    double outputPower = 0.0;
    const auto processSide = [&] {
        buffer.copyFrom(0, 0, side.data(), blockSamples);
        buffer.copyFrom(1, 0, inverted.data(), blockSamples);
        imager.process(buffer);
        for (int channel = 0; channel < 2; ++channel) {
            for (int sample = 0; sample < blockSamples; ++sample) {
                const float value = buffer.getSample(channel, sample);
                finitePcm = finitePcm && std::isfinite(value);
                outputPower += static_cast<double>(value) * static_cast<double>(value);
            }
        }
    };
    for (int block = 0; block < 16; ++block) {
        processSide();
    }

    std::atomic<bool> valid {true};
    // RAII joins the reader even if a future assertion adds an early return to this test.
    std::jthread reader([&imager, &valid](const std::stop_token& stop) {
        monitorPublishedCorrelations(imager, valid, stop);
    });
    constexpr std::array<Crossovers, 2> requests {
        {
            {80.0F, 1200.0F, 8000.0F},
            {150.0F, 700.0F, 4000.0F},
        },
    };
    for (const auto& requested : requests) {
        SCOPED_TRACE(requested[1]);
        const auto previous = imager.getActiveCrossovers();
        imager.setCrossovers(requested, false);
        // Allow the instrumented background designer the same bounded handoff window as the RTSan probes.
        constexpr auto handoffTimeout = std::chrono::seconds(5);
        const auto deadline = std::chrono::steady_clock::now() + handoffTimeout;
        // Two distinct, observed transitions necessarily hand off from slot 0 to 1 and back to slot 0.
        while (!juce::approximatelyEqual(imager.getActiveCrossovers()[1], requested[1])
               && std::chrono::steady_clock::now() < deadline)
        {
            processSide();
            juce::Thread::sleep(1);
        }
        const auto active = imager.getActiveCrossovers();
        EXPECT_FALSE(juce::approximatelyEqual(active[1], previous[1]));
        for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
            EXPECT_FLOAT_EQ(active[crossover], requested[crossover]);
        }
        // Drain the crossfaded partition and verify that recovery remains active in each newly published slot.
        processSide();
        for (const float reading : imager.getBandCorrelations()) {
            EXPECT_TRUE(std::isfinite(reading));
            EXPECT_NEAR(reading, 1.0F, 1.0e-6F);
        }
    }
    reader.request_stop();
    reader.join();
    EXPECT_TRUE(valid.load(std::memory_order_relaxed));
    EXPECT_TRUE(finitePcm);
    // Pure Side at zero Width would be silent without the quadrature recovery path.
    EXPECT_GT(outputPower, 1.0);
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
        imager.setWidths({0.0F, 0.5F, 0.25F, 1.5F});
        imager.setRecoveries({1.0F, 1.0F, 0.5F, 1.0F});
        imager.setDelta(size != 1);
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        imager.setCorrelationEnabled(true);
        auto buffer = makeBuffer({left, right});
        renderMetered(imager, buffer, size);
        const auto readings = imager.getBandCorrelations();
        EXPECT_NEAR(readings.front(), 1.0F, 1.0e-6F);
        EXPECT_NEAR(readings.back(), -1.0F, 1.0e-6F);
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            EXPECT_TRUE(std::isfinite(readings[band]));
            if (size != 1) {
                EXPECT_FLOAT_EQ(readings[band], reference[band]);
            }
        }
        reference = readings;
    }
}
}  // namespace esgrove::image_control::test
