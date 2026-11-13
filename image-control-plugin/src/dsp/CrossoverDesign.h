/// Declares the design of the linear-phase lowpass and quadrature kernels that split ImageControl's bands.
/// Ordinary lowpasses are symmetric, and their quadrature partners are antisymmetric with the same centre and support.
/// Their targets fall from one to zero over a raised-cosine step one octave wide on a logarithmic frequency axis.
/// Differences of the lowpasses and a fullband kernel form complementary bands in either signal path.

#pragma once

#include "Parameters.h"

#include <juce_dsp/juce_dsp.h>

#include <memory>
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

/// Precomputes the shared design FFT, Blackman-Harris window,
/// and crossover-independent fullband quadrature kernel for one prepared sample rate
/// so real-time background and offline crossover designs reuse invariant state.
class CrossoverDesigner
{
public:
    [[nodiscard]] std::vector<std::vector<float>> designKernelSet(const Crossovers& crossovers);
    void prepare(double newSampleRate, int newHalfLength);

private:
    /// Shared inverse transform of size halfLength * 4, created in prepare.
    std::unique_ptr<juce::dsp::FFT> fft;
    /// Reusable interleaved FFT spectrum buffers for one ordinary/quadrature pair.
    std::vector<float> lowpassSpectrum;
    /// Reusable interleaved FFT spectrum buffer for the quadrature partner.
    std::vector<float> quadratureSpectrum;
    /// Precomputed fullband quadrature kernel for the prepared sample rate and half length.
    std::vector<float> fullQuadratureKernel;
    /// Reusable unnormalized double-precision taps for ordinary lowpass DC normalization.
    std::vector<double> lowpassTaps;
    /// Precomputed symmetric four-term Blackman-Harris window of length 2 * halfLength + 1.
    std::vector<double> window;
    /// Sample rate the designer was prepared for.
    double sampleRate {REFERENCE_SAMPLE_RATE};
    /// Kernel half length, in samples.
    int halfLength {REFERENCE_HALF_LENGTH};
};

[[nodiscard]] std::vector<float> designLowpassKernel(double crossover, double sampleRate, int halfLength);
[[nodiscard]] std::vector<float> designQuadratureKernel(
    double sampleRate,
    int halfLength,
    std::optional<double> crossover = std::nullopt
);

[[nodiscard]] double crossoverLowpassMagnitude(double frequency, double crossover);

[[nodiscard]] int kernelHalfLength(double sampleRate);
}  // namespace esgrove::image_control
