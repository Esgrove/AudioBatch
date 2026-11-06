/// Tests the lowpass kernel design and the crossover ordering rule:
/// kernel length and symmetry, unity gain at DC, the magnitude around each crossover, and the minimum band width.

#include "dsp/CrossoverDesign.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <numeric>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Returns the zero-phase magnitude of a symmetric kernel at a frequency.
double kernelMagnitude(const std::vector<float>& kernel, const double frequency)
{
    const double centre = (static_cast<double>(kernel.size()) - 1.0) * 0.5;
    double sum = 0.0;
    for (std::size_t tap = 0; tap < kernel.size(); ++tap) {
        const double offset = static_cast<double>(tap) - centre;
        sum += static_cast<double>(kernel[tap])
            * std::cos(2.0 * std::numbers::pi * frequency * offset / TEST_SAMPLE_RATE);
    }
    return sum;
}
}  // namespace

TEST(Crossover, HalfLengthScalesWithSampleRate)
{
    EXPECT_EQ(kernelHalfLength(44'100.0), 4096);
    EXPECT_EQ(kernelHalfLength(48'000.0), 4096);
    EXPECT_EQ(kernelHalfLength(88'200.0), 8192);
    EXPECT_EQ(kernelHalfLength(96'000.0), 8192);
    EXPECT_EQ(kernelHalfLength(192'000.0), 16'384);
}

TEST(Crossover, KernelIsSymmetricWithUnityDcGain)
{
    const auto kernel = designLowpassKernel(1000.0, TEST_SAMPLE_RATE, 4096);
    ASSERT_EQ(kernel.size(), 8193U);
    for (std::size_t tap = 0; tap < kernel.size() / 2; ++tap) {
        ASSERT_NEAR(kernel[tap], kernel[kernel.size() - 1 - tap], 1.0e-7F);
    }
    const double sum = std::accumulate(kernel.begin(), kernel.end(), 0.0);
    EXPECT_NEAR(sum, 1.0, 1.0e-5);
}

TEST(Crossover, MagnitudeFollowsTheOneOctaveStep)
{
    for (const double crossover : {120.0, 1000.0, 10'000.0}) {
        SCOPED_TRACE(crossover);
        const auto kernel = designLowpassKernel(crossover, TEST_SAMPLE_RATE, 4096);
        EXPECT_NEAR(kernelMagnitude(kernel, crossover), 0.5, 0.02);
        EXPECT_NEAR(kernelMagnitude(kernel, crossover * 0.5), 1.0, 0.005);
        EXPECT_NEAR(kernelMagnitude(kernel, crossover * 2.0), 0.0, 0.005);
        const double insideStep = crossover * std::pow(2.0, -0.25);
        EXPECT_NEAR(kernelMagnitude(kernel, insideStep), crossoverLowpassMagnitude(insideStep, crossover), 0.05);
    }
}

TEST(Crossover, TargetMagnitudeIsComplementaryStep)
{
    EXPECT_DOUBLE_EQ(crossoverLowpassMagnitude(0.0, 100.0), 1.0);
    EXPECT_DOUBLE_EQ(crossoverLowpassMagnitude(70.0, 100.0), 1.0);
    EXPECT_NEAR(crossoverLowpassMagnitude(100.0, 100.0), 0.5, 1.0e-12);
    EXPECT_DOUBLE_EQ(crossoverLowpassMagnitude(142.0, 100.0), 0.0);
}

TEST(Crossover, OrderingKeepsDefaultsAndSeparatesBands)
{
    const auto defaults = orderCrossovers(DEFAULT_CROSSOVERS);
    for (std::size_t crossover = 0; crossover < CROSSOVER_COUNT; ++crossover) {
        EXPECT_FLOAT_EQ(defaults.at(crossover), DEFAULT_CROSSOVERS.at(crossover));
    }

    const auto reversed = orderCrossovers({5000.0F, 1000.0F, 100.0F});
    EXPECT_FLOAT_EQ(reversed[0], 5000.0F);
    EXPECT_GE(reversed[1] / reversed[0], MIN_CROSSOVER_RATIO * 0.9999F);
    EXPECT_GE(reversed[2] / reversed[1], MIN_CROSSOVER_RATIO * 0.9999F);

    const auto top = orderCrossovers({MAX_FREQUENCY, MAX_FREQUENCY, MAX_FREQUENCY});
    EXPECT_LE(top[2], MAX_FREQUENCY);
    EXPECT_GE(top[2] / top[1], MIN_CROSSOVER_RATIO * 0.9999F);
}
}  // namespace esgrove::image_control::test
