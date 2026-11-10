/// Tests ordinary and quadrature FIRs against analytical impulses and frequency responses.
/// Checks delay, symmetry, endpoint zeros, finite passband bounds, complementary bands, and crossover ordering.

#include "dsp/CrossoverDesign.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <optional>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Rates covering both standard audio clock families and every half-length up to 192 kHz.
constexpr std::array<double, 6> QUADRATURE_SAMPLE_RATES {44'100.0, 48'000.0, 88'200.0, 96'000.0, 176'400.0, 192'000.0};

/// Returns the complex FIR response with its centre delay removed, without assuming either tap symmetry.
std::complex<double> centredKernelResponse(
    const std::vector<float>& kernel,
    const double frequency,
    const double sampleRate
)
{
    const double centre = (static_cast<double>(kernel.size()) - 1.0) * 0.5;
    const double step = -2.0 * std::numbers::pi * frequency / sampleRate;
    std::complex<double> response {0.0, 0.0};
    for (std::size_t tap = 0; tap < kernel.size(); ++tap) {
        const double phase = step * (static_cast<double>(tap) - centre);
        response += static_cast<double>(kernel[tap]) * std::complex<double> {std::cos(phase), std::sin(phase)};
    }
    return response;
}

/// Evaluates the four-term window independently in centred coordinates, where all cosine terms have positive signs.
double analyticalWindow(const int offset, const int halfLength)
{
    const double phase = std::numbers::pi * static_cast<double>(offset) / static_cast<double>(halfLength);
    return 0.35875 + 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase) + 0.01168 * std::cos(3.0 * phase);
}

/// Returns the zero-phase magnitude of a symmetric kernel at a frequency.
double kernelMagnitude(
    const std::vector<float>& kernel,
    const double frequency,
    const double sampleRate = TEST_SAMPLE_RATE
)
{
    const double centre = (static_cast<double>(kernel.size()) - 1.0) * 0.5;
    double sum = 0.0;
    for (std::size_t tap = 0; tap < kernel.size(); ++tap) {
        const double offset = static_cast<double>(tap) - centre;
        sum += static_cast<double>(kernel[tap]) * std::cos(2.0 * std::numbers::pi * frequency * offset / sampleRate);
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

TEST(Crossover, QuadratureHasExactTypeThreeSymmetryAndEndpointZeros)
{
    for (const double sampleRate : QUADRATURE_SAMPLE_RATES) {
        SCOPED_TRACE(sampleRate);
        const int halfLength = kernelHalfLength(sampleRate);
        for (const auto crossover : {std::optional<double> {}, std::optional<double> {100.0}}) {
            const auto kernel = designQuadratureKernel(sampleRate, halfLength, crossover);
            ASSERT_EQ(kernel.size(), static_cast<std::size_t>(2 * halfLength + 1));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(kernel[static_cast<std::size_t>(halfLength)]), 0U);
            for (std::size_t tap = 0; tap < kernel.size() / 2; ++tap) {
                ASSERT_EQ(
                    std::bit_cast<std::uint32_t>(kernel[tap]),
                    std::bit_cast<std::uint32_t>(-kernel[kernel.size() - 1 - tap])
                );
            }
            EXPECT_NEAR(std::abs(centredKernelResponse(kernel, 0.0, sampleRate)), 0.0, 1.0e-12);
            EXPECT_NEAR(std::abs(centredKernelResponse(kernel, sampleRate * 0.5, sampleRate)), 0.0, 1.0e-12);
        }
    }
}

TEST(Crossover, FullbandQuadratureMatchesAnalyticalWindowedCotangentImpulse)
{
    constexpr int halfLength {64};
    constexpr int designSize {256};
    const auto kernel = designQuadratureKernel(TEST_SAMPLE_RATE, halfLength);
    for (int offset = 1; offset <= halfLength; ++offset) {
        SCOPED_TRACE(offset);
        // The inverse sine sum of a flat imaginary spectrum is 2 cot(pi n / N) / N on odd offsets.
        // Even offsets vanish. This closed form tests the finite frequency grid without another FFT oracle.
        const double phase = std::numbers::pi * static_cast<double>(offset) / static_cast<double>(designSize);
        const double impulse = offset % 2 == 0 ? 0.0 : 2.0 / (static_cast<double>(designSize) * std::tan(phase));
        const double expected = impulse * analyticalWindow(offset, halfLength);
        EXPECT_NEAR(kernel[static_cast<std::size_t>(halfLength + offset)], expected, 1.0e-7);
        EXPECT_NEAR(kernel[static_cast<std::size_t>(halfLength - offset)], -expected, 1.0e-7);
    }
}

TEST(Crossover, QuadratureLowpassMatchesIndependentWindowedSineSum)
{
    constexpr int halfLength {64};
    constexpr int designSize {256};
    constexpr double crossover {6000.0};
    const auto kernel = designQuadratureKernel(TEST_SAMPLE_RATE, halfLength, crossover);
    for (int offset = 1; offset <= halfLength; ++offset) {
        SCOPED_TRACE(offset);
        double impulse = 0.0;
        for (int bin = 1; bin < designSize / 2; ++bin) {
            const double frequency = TEST_SAMPLE_RATE * static_cast<double>(bin) / static_cast<double>(designSize);
            const double progress = std::clamp(std::log2(frequency / crossover) + 0.5, 0.0, 1.0);
            const double target = 0.5 * (1.0 + std::cos(std::numbers::pi * progress));
            impulse += target
                * std::sin(2.0 * std::numbers::pi * static_cast<double>(bin * offset)
                           / static_cast<double>(designSize));
        }
        const double expected
            = impulse * (2.0 / static_cast<double>(designSize)) * analyticalWindow(offset, halfLength);
        EXPECT_NEAR(kernel[static_cast<std::size_t>(halfLength + offset)], expected, 1.0e-7);
        EXPECT_NEAR(kernel[static_cast<std::size_t>(halfLength - offset)], -expected, 1.0e-7);
    }
}

TEST(Crossover, FullbandQuadratureBoundsUsefulAudioResponseAtEveryRate)
{
    for (const double sampleRate : QUADRATURE_SAMPLE_RATES) {
        SCOPED_TRACE(sampleRate);
        const auto kernel = designQuadratureKernel(sampleRate, kernelHalfLength(sampleRate));
        // Include 20, 21, 25 and 30 Hz explicitly, where the finite DC transition is most significant.
        for (const double frequency : {20.0, 21.0, 25.0, 30.0}) {
            const auto response = centredKernelResponse(kernel, frequency, sampleRate);
            EXPECT_NEAR(response.real(), 0.0, 1.0e-12);
            EXPECT_NEAR(-response.imag(), 1.0, 0.00055);
        }
        for (int step = 0; step <= 64; ++step) {
            const double frequency = 20.0 * std::pow(1000.0, static_cast<double>(step) / 64.0);
            SCOPED_TRACE(frequency);
            const auto response = centredKernelResponse(kernel, frequency, sampleRate);
            EXPECT_NEAR(response.real(), 0.0, 1.0e-12);
            EXPECT_NEAR(-response.imag(), 1.0, 0.00055);
        }
    }
}

TEST(Crossover, QuadratureLowpassesMatchOrdinaryPassbandsAndAnalyticalTransition)
{
    for (const double sampleRate : QUADRATURE_SAMPLE_RATES) {
        SCOPED_TRACE(sampleRate);
        const int halfLength = kernelHalfLength(sampleRate);
        for (const double crossover : {100.0, 1000.0, 10'000.0}) {
            SCOPED_TRACE(crossover);
            const auto ordinary = designLowpassKernel(crossover, sampleRate, halfLength);
            const auto quadrature = designQuadratureKernel(sampleRate, halfLength, crossover);
            for (int step = 0; step <= 16; ++step) {
                const double frequency = 20.0 * std::pow(1000.0, static_cast<double>(step) / 16.0);
                SCOPED_TRACE(frequency);
                const auto response = centredKernelResponse(quadrature, frequency, sampleRate);
                EXPECT_NEAR(response.real(), 0.0, 1.0e-12);
                EXPECT_NEAR(-response.imag(), kernelMagnitude(ordinary, frequency, sampleRate), 0.00055);
            }
            EXPECT_NEAR(-centredKernelResponse(quadrature, crossover * 0.5, sampleRate).imag(), 1.0, 0.005);
            EXPECT_NEAR(-centredKernelResponse(quadrature, crossover, sampleRate).imag(), 0.5, 0.02);
            EXPECT_NEAR(-centredKernelResponse(quadrature, crossover * 2.0, sampleRate).imag(), 0.0, 0.005);
            const double insideStep = crossover * std::exp2(-0.25);
            // An independent closed-form point on the raised-cosine transition, not the production target helper.
            const double expected = (1.0 + std::numbers::sqrt2 * 0.5) * 0.5;
            EXPECT_NEAR(-centredKernelResponse(quadrature, insideStep, sampleRate).imag(), expected, 0.05);
        }
    }
}

TEST(Crossover, MinimumCrossoverRetainsFiniteOrdinaryResponseWithoutDcNormalization)
{
    for (const double sampleRate : QUADRATURE_SAMPLE_RATES) {
        SCOPED_TRACE(sampleRate);
        const int halfLength = kernelHalfLength(sampleRate);
        const auto ordinary = designLowpassKernel(20.0, sampleRate, halfLength);
        const auto quadrature = designQuadratureKernel(sampleRate, halfLength, 20.0);
        for (const double frequency : {20.0, 21.0, 25.0, 30.0, 40.0, 100.0}) {
            const auto response = centredKernelResponse(quadrature, frequency, sampleRate);
            EXPECT_NEAR(response.real(), 0.0, 1.0e-12);
            // The existing finite lowpass's broad transition at 20 Hz is not an ideal brick-wall response.
            // Its very small DC normalization correction is deliberately absent from the zero-sum quadrature FIR.
            EXPECT_NEAR(-response.imag(), kernelMagnitude(ordinary, frequency, sampleRate), 0.0015);
        }
        const double gain20 = -centredKernelResponse(quadrature, 20.0, sampleRate).imag();
        EXPECT_GT(gain20, 0.510);
        EXPECT_LT(gain20, 0.513);
    }
}

TEST(Crossover, QuadratureBandDifferencesAreComplementaryToFullband)
{
    constexpr int halfLength {4096};
    const auto low = designQuadratureKernel(TEST_SAMPLE_RATE, halfLength, 100.0);
    const auto middle = designQuadratureKernel(TEST_SAMPLE_RATE, halfLength, 500.0);
    const auto high = designQuadratureKernel(TEST_SAMPLE_RATE, halfLength, 5000.0);
    const auto fullband = designQuadratureKernel(TEST_SAMPLE_RATE, halfLength);
    std::array<std::vector<float>, 4> bands;
    for (auto& band : bands) {
        band.resize(fullband.size());
    }
    for (std::size_t tap = 0; tap < fullband.size(); ++tap) {
        bands[0][tap] = low[tap];
        bands[1][tap] = middle[tap] - low[tap];
        bands[2][tap] = high[tap] - middle[tap];
        bands[3][tap] = fullband[tap] - high[tap];
        double sum = 0.0;
        for (const auto& band : bands) {
            sum += static_cast<double>(band[tap]);
        }
        ASSERT_NEAR(sum, fullband[tap], 1.0e-7);
    }
    for (const double frequency : {20.0, 100.0, 500.0, 1000.0, 5000.0, 20'000.0}) {
        std::complex<double> response {0.0, 0.0};
        for (const auto& band : bands) {
            response += centredKernelResponse(band, frequency, TEST_SAMPLE_RATE);
        }
        const auto expected = centredKernelResponse(fullband, frequency, TEST_SAMPLE_RATE);
        EXPECT_NEAR(std::abs(response - expected), 0.0, 2.0e-7);
        EXPECT_NEAR(response.real(), 0.0, 1.0e-12);
        EXPECT_NEAR(-response.imag(), 1.0, 0.00055);
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
