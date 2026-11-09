/// Declares the design of the linear-phase lowpass and quadrature kernels that split ImageControl's bands.
/// Ordinary lowpasses are symmetric, and their quadrature partners are antisymmetric with the same centre and support.
/// Their targets fall from one to zero over a raised-cosine step one octave wide on a logarithmic frequency axis.
/// Differences of the lowpasses and a fullband kernel form complementary bands in either signal path.

#pragma once

#include <optional>
#include <vector>

namespace esgrove::image_control
{
/// Width of each crossover's transition from pass to stop, in octaves.
constexpr double TRANSITION_OCTAVES {1.0};
/// Kernel half length at sample rates up to REFERENCE_SAMPLE_RATE, in samples.
/// It sets the frequency resolution, which must resolve a one-octave step at the lowest crossovers.
constexpr int REFERENCE_HALF_LENGTH {4096};
/// Sample rate up to which REFERENCE_HALF_LENGTH applies. Higher rates double it per doubling of the rate.
constexpr double REFERENCE_SAMPLE_RATE {48'000.0};

[[nodiscard]] std::vector<float> designLowpassKernel(double crossover, double sampleRate, int halfLength);
[[nodiscard]] std::vector<float> designQuadratureKernel(
    double sampleRate,
    int halfLength,
    std::optional<double> crossover = std::nullopt
);

[[nodiscard]] double crossoverLowpassMagnitude(double frequency, double crossover);

[[nodiscard]] int kernelHalfLength(double sampleRate);
}  // namespace esgrove::image_control
