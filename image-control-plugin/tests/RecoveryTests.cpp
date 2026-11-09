/// Tests independent Width and Recover signal laws, including the sign and latency of recovered quadrature.
/// Analytical sinusoids and ideal Hilbert impulse taps are independent of the production filter designer.
/// Metamorphic checks additionally cover exact neutrality, arbitrary blocks, automation, history, and finite tails.

#include "dsp/MultibandImager.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Controls are set before preparation unless a test explicitly exercises automation.
struct RecoveryControls {
    BandGains mid {1.0F, 1.0F, 1.0F, 1.0F};
    BandGains side {1.0F, 1.0F, 1.0F, 1.0F};
    BandGains audible {1.0F, 1.0F, 1.0F, 1.0F};
    BandPans pans {};
    BandWidths widths {1.0F, 1.0F, 1.0F, 1.0F};
    BandRecoveries recoveries {};
    float output {1.0F};
    bool delta {false};
};

/// Blocks cross partition boundaries and include both tiny and larger-than-partition callbacks.
constexpr std::array<int, 7> IRREGULAR_BLOCKS {1, 37, 257, 511, 4097, 19, 64};
/// Every tone is well inside one default band, not inside a crossover's transition.
constexpr std::array<double, BAND_COUNT> BAND_TONES {50.0, 250.0, 1800.0, 12'000.0};
/// Absolute sample error allowed for the finite FIR approximation to ideal quadrature on interior tones.
constexpr double QUADRATURE_ERROR {0.003};
/// Carrier safely inside the High Mid band at every tested sample rate.
constexpr double CARRIER {1500.0};
/// Enough settled samples to distinguish sine, cosine, and silence independently.
constexpr int OBSERVATION_SAMPLES {2048};

/// Computes the documented rate-scaled half length without consulting the production designer.
int expectedHalfLength(const double rate)
{
    int halfLength = 4096;
    for (int ceiling = 48'000; rate > static_cast<double>(ceiling); ceiling *= 2) {
        halfLength *= 2;
    }
    return halfLength;
}

/// Makes a rate-correct sine through the shared deterministic signal helper.
std::vector<float> rateSine(const double frequency, const float amplitude, const int samples, const double rate)
{
    return makeSine(frequency * TEST_SAMPLE_RATE / rate, amplitude, static_cast<std::size_t>(samples));
}

/// Encodes independent raw mid and side signals as stereo, without invoking production DSP.
juce::AudioBuffer<float> encodeMidSide(const std::vector<float>& mid, const std::vector<float>& side)
{
    auto buffer = makeBuffer({mid, side});
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
        const auto index = static_cast<std::size_t>(sample);
        buffer.setSample(0, sample, mid[index] + side[index]);
        buffer.setSample(1, sample, mid[index] - side[index]);
    }
    return buffer;
}

/// Installs independent source trims, width, recovery, pan, solo, and global processing controls.
void setControls(MultibandImager& imager, const RecoveryControls& controls)
{
    imager.setGains(controls.mid, controls.side, controls.output);
    imager.setWidths(controls.widths);
    imager.setRecoveries(controls.recoveries);
    imager.setPans(controls.pans);
    imager.setSolos(controls.audible);
    imager.setDelta(controls.delta);
}

/// Processes a slice without resetting history or aligning the caller's blocks to convolution partitions.
void renderSlice(
    MultibandImager& imager,
    juce::AudioBuffer<float>& buffer,
    const int start,
    const int stop,
    const std::span<const int> sizes = IRREGULAR_BLOCKS
)
{
    std::size_t block = 0;
    for (int offset = start; offset < stop; ++block) {
        const int count = std::min(sizes[block % sizes.size()], stop - offset);
        juce::AudioBuffer<float> section(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), offset, count);
        imager.process(section);
        offset += count;
    }
}

/// Renders a fresh imager while preserving the input buffer for independent comparisons.
juce::AudioBuffer<float> renderRecovery(
    const juce::AudioBuffer<float>& input,
    const RecoveryControls& controls,
    const double rate = TEST_SAMPLE_RATE,
    const std::span<const int> sizes = IRREGULAR_BLOCKS
)
{
    juce::AudioBuffer<float> result;
    result.makeCopyOf(input);
    MultibandImager imager;
    setControls(imager, controls);
    imager.prepare(rate, input.getNumChannels(), DEFAULT_CROSSOVERS);
    renderSlice(imager, result, 0, result.getNumSamples(), sizes);
    return result;
}

/// Compares both channels with an independent sample-domain expectation after startup has drained.
template<typename Expected>
void expectAnalytical(
    const juce::AudioBuffer<float>& buffer,
    const int start,
    const int stop,
    Expected expected,
    const double tolerance = QUADRATURE_ERROR
)
{
    for (int sample = start; sample < stop; ++sample) {
        const auto [left, right] = expected(sample);
        ASSERT_NEAR(buffer.getSample(0, sample), left, tolerance) << "left sample " << sample;
        ASSERT_NEAR(buffer.getSample(1, sample), right, tolerance) << "right sample " << sample;
    }
}

/// Applies the documented post-recovery pan law independently of the imager.
std::pair<double, double> expectedPan(const double mid, const double side, const double pan)
{
    const double left = mid + side;
    const double right = mid - side;
    const double toRight = std::max(0.0, pan);
    const double toLeft = std::max(0.0, -pan);
    return {left * (1.0 - toRight) + right * toLeft, right * (1.0 - toLeft) + left * toRight};
}

/// Parameterizes the expensive filter preparation rather than putting every rate in one long test case.
class RecoveryRates : public testing::TestWithParam<double>
{};
/// Each case independently exercises one band's solo, width, recovery, and post-recovery pan.
class RecoveryBands : public testing::TestWithParam<std::size_t>
{};
}  // namespace

TEST_P(RecoveryRates, PureSideBecomesNegativeCosineAtTheExistingLatency)
{
    const double rate = GetParam();
    const int halfLength = expectedHalfLength(rate);
    const int latency = halfLength + PARTITION_SIZE;
    const int settled = 2 * halfLength + PARTITION_SIZE;
    const int samples = settled + OBSERVATION_SAMPLES;
    const auto side = rateSine(CARRIER, 0.3F, samples, rate);
    const auto input = encodeMidSide(std::vector<float>(side.size(), 0.0F), side);
    RecoveryControls controls;
    controls.widths.fill(0.0F);
    controls.recoveries.fill(1.0F);
    const auto output = renderRecovery(input, controls, rate);
    expectAnalytical(output, settled, samples, [rate, latency](const int sample) {
        const double recovered = -0.3 * std::cos(2.0 * std::numbers::pi * CARRIER * (sample - latency) / rate);
        return std::pair {recovered, recovered};
    });
    EXPECT_GT(rms(channelOf(output, 0).subspan(static_cast<std::size_t>(settled))), 0.19);
}

TEST_P(RecoveryRates, PureMidIgnoresWidthAndRecoveryIncludingMonoInput)
{
    const double rate = GetParam();
    const int halfLength = expectedHalfLength(rate);
    const int latency = halfLength + PARTITION_SIZE;
    const int samples = 2 * halfLength + PARTITION_SIZE + OBSERVATION_SAMPLES;
    const auto tone = rateSine(CARRIER, 0.3F, samples, rate);
    RecoveryControls controls;
    controls.mid.fill(0.6F);
    controls.side.fill(1.7F);
    controls.widths = {0.0F, 0.25F, 1.0F, 2.0F};
    controls.recoveries.fill(1.0F);
    for (const int channels : {1, 2}) {
        const auto input = channels == 1 ? makeBuffer({tone}) : makeBuffer({tone, tone});
        const auto output = renderRecovery(input, controls, rate);
        for (int channel = 0; channel < channels; ++channel) {
            for (int sample = latency; sample < samples; ++sample) {
                ASSERT_NEAR(
                    output.getSample(channel, sample), tone[static_cast<std::size_t>(sample - latency)] * 0.6F, 2.0e-6F
                ) << sample;
            }
        }
    }
}

TEST(Recovery, HardPannedPairHasOppositeRecoveredQuadratureAndTheSameOriginalMid)
{
    constexpr int samples = 12'000;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto tone = makeSine(CARRIER, 0.5F, samples);
    const std::vector<float> silence(tone.size(), 0.0F);
    RecoveryControls controls;
    controls.widths.fill(0.4F);
    controls.recoveries.fill(0.75F);
    for (const bool hardRight : {false, true}) {
        const auto input = hardRight ? makeBuffer({silence, tone}) : makeBuffer({tone, silence});
        const auto output = renderRecovery(input, controls);
        expectAnalytical(output, 9000, samples, [hardRight](const int sample) {
            const double phase = 2.0 * std::numbers::pi * CARRIER * (sample - latency) / TEST_SAMPLE_RATE;
            const double sign = hardRight ? -1.0 : 1.0;
            const double mid = 0.25 * std::sin(phase) - sign * 0.75 * 0.6 * 0.25 * std::cos(phase);
            const double side = sign * 0.4 * 0.25 * std::sin(phase);
            return std::pair {mid + side, mid - side};
        });
    }
}

TEST(Recovery, SourceSideTrimControlsRecoveryButSourceMidTrimDoesNot)
{
    constexpr int samples = 12'000;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto side = makeSine(CARRIER, 0.3F, samples);
    const auto input = encodeMidSide(std::vector<float>(side.size(), 0.0F), side);
    for (const float sideGain : {0.0F, 0.5F, 1.0F}) {
        RecoveryControls controls;
        controls.mid.fill(0.0F);
        controls.side.fill(sideGain);
        controls.widths.fill(0.0F);
        controls.recoveries.fill(1.0F);
        controls.output = 0.7F;
        const auto output = renderRecovery(input, controls);
        expectAnalytical(output, 9000, samples, [sideGain](const int sample) {
            const double recovered = -0.7 * static_cast<double>(sideGain) * 0.3
                * std::cos(2.0 * std::numbers::pi * CARRIER * (sample - latency) / TEST_SAMPLE_RATE);
            return std::pair {recovered, recovered};
        });
        const double level = rms(channelOf(output, 0).subspan(9000));
        if (sideGain > 0.0F) {
            EXPECT_GT(level, static_cast<double>(sideGain) * 0.14);
        } else {
            EXPECT_LT(level, 1.0e-7);
        }
    }
}

TEST(Recovery, RecoverOffIsStandardWidthAsAnIndependentSideScale)
{
    const auto input = makeBuffer({makeNoise(0.4F, 12'345, 101), makeNoise(0.4F, 12'345, 102)});
    RecoveryControls controls;
    controls.mid = {0.7F, 1.2F, 0.0F, 0.9F};
    controls.side = {0.5F, 0.8F, 1.1F, 1.3F};
    controls.widths = {0.0F, 0.35F, 1.0F, 2.0F};
    controls.pans = {-0.3F, 0.2F, 0.0F, 0.6F};
    const auto actual = renderRecovery(input, controls);
    auto equivalent = controls;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        equivalent.side[band] *= controls.widths[band];
    }
    equivalent.widths.fill(1.0F);
    const auto expected = renderRecovery(input, equivalent);
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < actual.getNumSamples(); ++sample) {
            ASSERT_NEAR(actual.getSample(channel, sample), expected.getSample(channel, sample), 2.0e-6F) << sample;
        }
    }
}

TEST(Recovery, RecoverOffMatchesTheRawMidSideEquationAcrossTheEntireWidthRange)
{
    constexpr int samples = 12'009;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto left = makeNoise(0.4F, samples, 111);
    const auto right = makeNoise(0.4F, samples, 112);
    const auto input = makeBuffer({left, right});
    RecoveryControls controls;
    controls.mid.fill(0.7F);
    controls.side.fill(1.2F);
    controls.output = 0.8F;
    for (const float width : {0.0F, 0.35F, 1.0F, 2.0F}) {
        SCOPED_TRACE(width);
        controls.widths.fill(width);
        const auto output = renderRecovery(input, controls);
        expectAnalytical(
            output,
            latency,
            samples,
            [&left, &right, width](const int sample) {
                const auto source = static_cast<std::size_t>(sample - latency);
                const double mid = (static_cast<double>(left[source]) + static_cast<double>(right[source])) * 0.5;
                const double side = (static_cast<double>(left[source]) - static_cast<double>(right[source])) * 0.5;
                return std::pair {
                    0.8 * (0.7 * mid + 1.2 * static_cast<double>(width) * side),
                    0.8 * (0.7 * mid - 1.2 * static_cast<double>(width) * side)
                };
            },
            2.0e-6
        );
    }
}

TEST(Recovery, UnityWidthIsBitExactNeutralEvenWithRecoveryAndMovingCrossovers)
{
    constexpr int samples = 15'013;
    constexpr int change = 7037;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto left = makeNoise(0.4F, samples, 103);
    const auto right = makeNoise(0.4F, samples, 104);
    for (const bool delta : {false, true}) {
        auto output = makeBuffer({left, right});
        MultibandImager imager;
        RecoveryControls controls;
        controls.recoveries = {1.0F, 0.25F, 0.75F, 1.0F};
        controls.delta = delta;
        setControls(imager, controls);
        imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
        renderSlice(imager, output, 0, change);
        imager.setRecoveries({0.0F, 1.0F, 0.5F, 0.25F});
        imager.setCrossovers({80.0F, 1800.0F, 8000.0F}, true);
        renderSlice(imager, output, change, samples);
        for (int sample = 0; sample < samples; ++sample) {
            const auto source = static_cast<std::size_t>(std::max(0, sample - latency));
            const bool silent = delta || sample < latency;
            ASSERT_EQ(output.getSample(0, sample), silent ? 0.0F : left[source]) << sample;
            ASSERT_EQ(output.getSample(1, sample), silent ? 0.0F : right[source]) << sample;
        }
    }
}

TEST(Recovery, RecoveryCannotAddSignalWhenWidthIsUnityOrWider)
{
    const auto input = makeBuffer({makeNoise(0.4F, 12'009, 105), makeNoise(0.4F, 12'009, 106)});
    RecoveryControls controls;
    controls.mid = {0.4F, 0.8F, 1.2F, 0.0F};
    controls.widths = {1.0F, 1.25F, 1.7F, 2.0F};
    controls.recoveries.fill(1.0F);
    const auto recovered = renderRecovery(input, controls);
    controls.recoveries.fill(0.0F);
    const auto plain = renderRecovery(input, controls);
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < plain.getNumSamples(); ++sample) {
            ASSERT_EQ(recovered.getSample(channel, sample), plain.getSample(channel, sample)) << sample;
        }
    }
}

TEST_P(RecoveryBands, SoloAndPanApplyAfterRecoveryAndDeltaSubtractsOnlyTheDrySolo)
{
    const std::size_t band = GetParam();
    constexpr int samples = 12'000;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto mid = makeComb(BAND_TONES, 0.08F, samples);
    const auto side = makeComb(BAND_TONES, 0.12F, samples);
    const auto input = encodeMidSide(mid, side);
    RecoveryControls controls;
    controls.mid = {0.7F, 0.8F, 0.9F, 1.1F};
    controls.side = {1.2F, 0.6F, 1.1F, 0.8F};
    controls.widths = {0.0F, 0.25F, 0.6F, 0.35F};
    controls.recoveries = {1.0F, 0.75F, 0.4F, 0.6F};
    controls.pans = {-0.5F, 0.25F, 1.0F, -1.0F};
    controls.audible.fill(0.0F);
    controls.audible[band] = 1.0F;
    controls.output = 0.8F;
    for (const bool delta : {false, true}) {
        controls.delta = delta;
        const auto output = renderRecovery(input, controls);
        expectAnalytical(output, 9000, samples, [band, controls, delta](const int sample) {
            const double phase = 2.0 * std::numbers::pi * BAND_TONES[band] * (sample - latency) / TEST_SAMPLE_RATE;
            const double sine = std::sin(phase);
            const double midValue = static_cast<double>(controls.mid[band]) * 0.08 * sine
                - static_cast<double>(controls.recoveries[band])
                    * std::max(0.0, 1.0 - static_cast<double>(controls.widths[band]))
                    * static_cast<double>(controls.side[band]) * 0.12 * std::cos(phase);
            const double sideValue
                = static_cast<double>(controls.widths[band]) * static_cast<double>(controls.side[band]) * 0.12 * sine;
            const auto [left, right] = expectedPan(midValue, sideValue, static_cast<double>(controls.pans[band]));
            return std::pair {
                static_cast<double>(controls.output) * left - (delta ? (0.08 + 0.12) * sine : 0.0),
                static_cast<double>(controls.output) * right - (delta ? (0.08 - 0.12) * sine : 0.0)
            };
        });
    }
}

TEST(Recovery, IrregularAndSingleSampleBlocksHaveIdenticalActiveRecovery)
{
    const auto input = makeBuffer({makeNoise(0.4F, 12'345, 107), makeNoise(0.4F, 12'345, 108)});
    RecoveryControls controls;
    controls.widths = {0.0F, 0.25F, 0.6F, 1.5F};
    controls.recoveries = {1.0F, 0.75F, 0.4F, 1.0F};
    controls.pans = {-0.4F, 0.2F, 0.0F, 0.3F};
    constexpr std::array<int, 1> SINGLE_SAMPLE {1};
    const auto reference = renderRecovery(input, controls, TEST_SAMPLE_RATE, SINGLE_SAMPLE);
    const auto irregular = renderRecovery(input, controls);
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < input.getNumSamples(); ++sample) {
            ASSERT_EQ(irregular.getSample(channel, sample), reference.getSample(channel, sample)) << sample;
        }
    }
}

TEST(Recovery, ResetClearsQuadratureHistoryAndRestartsWithEnabledTargets)
{
    const auto input = makeBuffer({makeNoise(0.4F, 12'345, 109), makeNoise(0.4F, 12'345, 110)});
    RecoveryControls controls;
    controls.mid.fill(0.0F);
    controls.widths.fill(0.0F);
    controls.recoveries.fill(1.0F);
    MultibandImager imager;
    setControls(imager, controls);
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    juce::AudioBuffer<float> first;
    first.makeCopyOf(input);
    renderSlice(imager, first, 0, first.getNumSamples());
    EXPECT_GT(rms(channelOf(first, 0).subspan(9000)), 0.1);
    imager.setWidths({0.5F, 0.5F, 0.5F, 0.5F});
    imager.setRecoveries({0.8F, 0.8F, 0.8F, 0.8F});
    imager.reset();
    juce::AudioBuffer<float> silence(2, 12'345);
    silence.clear();
    renderSlice(imager, silence, 0, silence.getNumSamples());
    for (int channel = 0; channel < 2; ++channel) {
        for (const float value : channelOf(silence, channel)) {
            ASSERT_EQ(value, 0.0F);
        }
    }
    controls.widths.fill(0.5F);
    controls.recoveries.fill(0.8F);
    imager.reset();
    juce::AudioBuffer<float> restarted;
    restarted.makeCopyOf(input);
    renderSlice(imager, restarted, 0, restarted.getNumSamples());
    const auto fresh = renderRecovery(input, controls);
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < fresh.getNumSamples(); ++sample) {
            ASSERT_EQ(restarted.getSample(channel, sample), fresh.getSample(channel, sample)) << sample;
        }
    }
}

TEST(Recovery, AutomationUsesLinearWidthAndRecoveryGlidesAndReturnsToExactNeutral)
{
    constexpr int activation = 10'240;
    constexpr int neutral = 15'360;
    constexpr int samples = 19'000;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto side = makeSine(CARRIER, 0.3F, samples);
    auto output = encodeMidSide(std::vector<float>(side.size(), 0.0F), side);
    MultibandImager imager;
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    renderSlice(imager, output, 0, activation);
    imager.setWidths({0.25F, 0.25F, 0.25F, 0.25F});
    imager.setRecoveries({0.8F, 0.8F, 0.8F, 0.8F});
    renderSlice(imager, output, activation, neutral);
    imager.setWidths({1.0F, 1.0F, 1.0F, 1.0F});
    imager.setRecoveries({0.0F, 0.0F, 0.0F, 0.0F});
    renderSlice(imager, output, neutral, samples);
    const int glide = static_cast<int>(GAIN_SMOOTHING_SECONDS * TEST_SAMPLE_RATE);
    expectAnalytical(output, activation + PARTITION_SIZE, samples, [](const int sample) {
        const bool returning = sample >= neutral + PARTITION_SIZE;
        const int change = returning ? neutral : activation;
        const double progress = std::clamp(static_cast<double>(sample - change - PARTITION_SIZE + 1) / glide, 0.0, 1.0);
        const double width = returning ? 0.25 + 0.75 * progress : 1.0 - 0.75 * progress;
        const double recovery = returning ? 0.8 * (1.0 - progress) : 0.8 * progress;
        const double phase = 2.0 * std::numbers::pi * CARRIER * (sample - latency) / TEST_SAMPLE_RATE;
        const double mid = -recovery * (1.0 - width) * 0.3 * std::cos(phase);
        const double sideValue = width * 0.3 * std::sin(phase);
        return std::pair {mid + sideValue, mid - sideValue};
    });
    for (int sample = neutral + glide + 2 * PARTITION_SIZE; sample < samples; ++sample) {
        const float expected = side[static_cast<std::size_t>(sample - latency)];
        ASSERT_EQ(output.getSample(0, sample), expected) << sample;
        ASSERT_EQ(output.getSample(1, sample), -expected) << sample;
    }
}

TEST(Recovery, EnablingRecoveryUsesRawSideHistoryBeforeAnotherFilterLatencyCanElapse)
{
    constexpr int activation = 10'240;
    constexpr int samples = activation + 3500;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto side = makeSine(CARRIER, 0.3F, samples);
    auto output = encodeMidSide(std::vector<float>(side.size(), 0.0F), side);
    MultibandImager imager;
    imager.setWidths({0.0F, 0.0F, 0.0F, 0.0F});
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    renderSlice(imager, output, 0, activation);
    EXPECT_LT(rms(channelOf(output, 0).subspan(9000, 1000)), 1.0e-7);
    imager.setRecoveries({1.0F, 1.0F, 1.0F, 1.0F});
    renderSlice(imager, output, activation, samples);
    const int settled = activation + static_cast<int>(GAIN_SMOOTHING_SECONDS * TEST_SAMPLE_RATE) + 2 * PARTITION_SIZE;
    ASSERT_LT(samples - activation, latency);
    expectAnalytical(output, settled, samples, [](const int sample) {
        const double value = -0.3 * std::cos(2.0 * std::numbers::pi * CARRIER * (sample - latency) / TEST_SAMPLE_RATE);
        return std::pair {value, value};
    });
}

TEST(Recovery, RecoveryAutomationSettlesWhileWidthIsNeutralBeforeNarrowing)
{
    constexpr int recoverChange = 10'240;
    constexpr int widthChange = 12'288;
    constexpr int samples = 15'500;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto side = makeSine(CARRIER, 0.3F, samples);
    auto output = encodeMidSide(std::vector<float>(side.size(), 0.0F), side);
    MultibandImager imager;
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    renderSlice(imager, output, 0, recoverChange);
    imager.setRecoveries({1.0F, 1.0F, 1.0F, 1.0F});
    renderSlice(imager, output, recoverChange, widthChange);
    for (int sample = recoverChange; sample < widthChange; ++sample) {
        const float expected = side[static_cast<std::size_t>(sample - latency)];
        ASSERT_EQ(output.getSample(0, sample), expected) << sample;
        ASSERT_EQ(output.getSample(1, sample), -expected) << sample;
    }
    imager.setWidths({0.0F, 0.0F, 0.0F, 0.0F});
    renderSlice(imager, output, widthChange, samples);
    const int glide = static_cast<int>(GAIN_SMOOTHING_SECONDS * TEST_SAMPLE_RATE);
    expectAnalytical(output, widthChange + PARTITION_SIZE, samples, [](const int sample) {
        const double progress
            = std::clamp(static_cast<double>(sample - widthChange - PARTITION_SIZE + 1) / glide, 0.0, 1.0);
        const double phase = 2.0 * std::numbers::pi * CARRIER * (sample - latency) / TEST_SAMPLE_RATE;
        const double mid = -progress * 0.3 * std::cos(phase);
        const double sideValue = (1.0 - progress) * 0.3 * std::sin(phase);
        return std::pair {mid + sideValue, mid - sideValue};
    });
}

TEST(Recovery, MovingCrossoversCrossfadesKnownWarmQuadratureRatherThanStartingASecondFilter)
{
    constexpr int change = 10'240;
    constexpr int samples = 17'000;
    constexpr int latency = 4096 + PARTITION_SIZE;
    constexpr double frequency = 1000.0;
    const auto side = makeSine(frequency, 0.3F, samples);
    auto output = encodeMidSide(std::vector<float>(side.size(), 0.0F), side);
    RecoveryControls controls;
    controls.widths.fill(0.0F);
    controls.recoveries = {0.0F, 0.25F, 1.0F, 0.0F};
    MultibandImager imager;
    setControls(imager, controls);
    imager.prepare(TEST_SAMPLE_RATE, 2, DEFAULT_CROSSOVERS);
    renderSlice(imager, output, 0, change);
    imager.setCrossovers({100.0F, 1500.0F, 5000.0F}, true);
    renderSlice(imager, output, change, samples);
    for (int sample = 9000; sample < samples; ++sample) {
        double amount = 1.0;
        if (sample >= change + 2 * PARTITION_SIZE) {
            amount = 0.25;
        } else if (sample >= change + PARTITION_SIZE) {
            const double fade = static_cast<double>(sample - change - PARTITION_SIZE + 1) / PARTITION_SIZE;
            amount = 1.0 - 0.75 * fade;
        }
        const double expected
            = -amount * 0.3 * std::cos(2.0 * std::numbers::pi * frequency * (sample - latency) / TEST_SAMPLE_RATE);
        ASSERT_TRUE(std::isfinite(output.getSample(0, sample))) << sample;
        ASSERT_NEAR(output.getSample(0, sample), expected, QUADRATURE_ERROR) << sample;
        ASSERT_NEAR(output.getSample(1, sample), expected, QUADRATURE_ERROR) << sample;
    }
    const auto active = imager.getActiveCrossovers();
    EXPECT_FLOAT_EQ(active[1], 1500.0F);
}

TEST(Recovery, PolarityIsLinearButSwappingChannelsChangesTheRecoveredMid)
{
    constexpr int samples = 12'000;
    constexpr int latency = 4096 + PARTITION_SIZE;
    const auto mid = makeSine(750.0, 0.2F, samples);
    const auto side = makeSine(CARRIER, 0.3F, samples);
    const auto input = encodeMidSide(mid, side);
    RecoveryControls controls;
    controls.widths.fill(0.4F);
    controls.recoveries.fill(0.8F);
    const auto original = renderRecovery(input, controls);
    auto invertedInput = makeBuffer({std::vector<float>(samples), std::vector<float>(samples)});
    auto swappedInput = makeBuffer({std::vector<float>(samples), std::vector<float>(samples)});
    for (int sample = 0; sample < samples; ++sample) {
        invertedInput.setSample(0, sample, -input.getSample(0, sample));
        invertedInput.setSample(1, sample, -input.getSample(1, sample));
        swappedInput.setSample(0, sample, input.getSample(1, sample));
        swappedInput.setSample(1, sample, input.getSample(0, sample));
    }
    const auto inverted = renderRecovery(invertedInput, controls);
    const auto swapped = renderRecovery(swappedInput, controls);
    double swapDifferencePower = 0.0;
    for (int sample = 9000; sample < samples; ++sample) {
        for (int channel = 0; channel < 2; ++channel) {
            ASSERT_NEAR(inverted.getSample(channel, sample), -original.getSample(channel, sample), 1.0e-6F);
        }
        const double difference
            = static_cast<double>(swapped.getSample(0, sample)) - static_cast<double>(original.getSample(1, sample));
        swapDifferencePower += difference * difference;
    }
    EXPECT_GT(std::sqrt(swapDifferencePower / (samples - 9000)), 0.18);
    expectAnalytical(swapped, 9000, samples, [](const int sample) {
        const double phase = 2.0 * std::numbers::pi * CARRIER * (sample - latency) / TEST_SAMPLE_RATE;
        const double midValue = 0.2 * std::sin(phase * 0.5) + 0.8 * 0.6 * 0.3 * std::cos(phase);
        const double sideValue = -0.4 * 0.3 * std::sin(phase);
        return std::pair {midValue + sideValue, midValue - sideValue};
    });
}

TEST(Recovery, AdversarialQuadratureMidCanCancelRecoveryRatherThanGuaranteeingEnergy)
{
    constexpr int samples = 12'000;
    const auto side = makeSine(CARRIER, 0.3F, samples);
    std::vector<float> mid(side.size());
    for (std::size_t sample = 0; sample < mid.size(); ++sample) {
        mid[sample] = 0.3F
            * static_cast<float>(
                          std::cos(2.0 * std::numbers::pi * CARRIER * static_cast<double>(sample) / TEST_SAMPLE_RATE)
            );
    }
    const auto input = encodeMidSide(mid, side);
    RecoveryControls controls;
    controls.widths.fill(0.0F);
    controls.recoveries.fill(1.0F);
    const auto recovered = renderRecovery(input, controls);
    controls.recoveries.fill(0.0F);
    const auto plain = renderRecovery(input, controls);
    EXPECT_GT(rms(channelOf(plain, 0).subspan(9000)), 0.2);
    EXPECT_LT(rms(channelOf(recovered, 0).subspan(9000)), QUADRATURE_ERROR);
    EXPECT_LT(rms(channelOf(recovered, 1).subspan(9000)), QUADRATURE_ERROR);
}

TEST_P(RecoveryRates, ImpulseHasIndependentHilbertSignsAndAFiniteExistingTail)
{
    const double rate = GetParam();
    const int halfLength = expectedHalfLength(rate);
    const int latency = halfLength + PARTITION_SIZE;
    const int impulse = 37;
    const int end = impulse + PARTITION_SIZE + 2 * halfLength + 1;
    juce::AudioBuffer<float> input(2, end + 2 * PARTITION_SIZE + 17);
    input.clear();
    input.setSample(0, impulse, 1.0F);
    input.setSample(1, impulse, -1.0F);
    RecoveryControls controls;
    controls.widths.fill(0.0F);
    controls.recoveries.fill(1.0F);
    MultibandImager imager;
    setControls(imager, controls);
    imager.prepare(rate, 2, DEFAULT_CROSSOVERS);
    EXPECT_EQ(imager.getLatencySamples(), latency);
    EXPECT_NEAR(imager.getTailLengthSeconds(), static_cast<double>(halfLength) / rate, 1.0e-12);
    renderSlice(imager, input, 0, input.getNumSamples());
    const int centre = impulse + latency;
    EXPECT_NEAR(input.getSample(0, centre), 0.0F, 1.0e-6F);
    // The ideal -90 degree Hilbert impulse is 2 / (pi * offset) on odd offsets, zero on even offsets.
    // Its near-centre taps are insensitive to the finite window, so no designer output is an oracle here.
    for (const int offset : {-3, -2, -1, 1, 2, 3}) {
        const double expected = offset % 2 == 0 ? 0.0 : 2.0 / (std::numbers::pi * offset);
        EXPECT_NEAR(input.getSample(0, centre + offset), expected, 2.0e-4) << offset;
    }
    for (int offset = 1; offset < halfLength; ++offset) {
        ASSERT_NEAR(input.getSample(0, centre - offset), -input.getSample(0, centre + offset), 2.0e-6F) << offset;
    }
    EXPECT_GT(rms(channelOf(input, 0).subspan(static_cast<std::size_t>(centre + 1), 512)), 0.01);
    for (int sample = 0; sample < input.getNumSamples(); ++sample) {
        ASSERT_TRUE(std::isfinite(input.getSample(0, sample))) << sample;
        ASSERT_EQ(input.getSample(0, sample), input.getSample(1, sample)) << sample;
        if (sample < impulse + PARTITION_SIZE || sample >= end) {
            ASSERT_NEAR(input.getSample(0, sample), 0.0F, 1.0e-7F) << sample;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(SampleRates, RecoveryRates, testing::Values(44'100.0, 48'000.0, 96'000.0, 192'000.0));
INSTANTIATE_TEST_SUITE_P(
    Bands,
    RecoveryBands,
    testing::Values(std::size_t {0}, std::size_t {1}, std::size_t {2}, std::size_t {3})
);
}  // namespace esgrove::image_control::test
