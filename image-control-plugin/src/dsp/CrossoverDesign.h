/// Declares the design of the linear-phase lowpass kernels that split ImageControl's bands.
/// Each kernel is a symmetric FIR filter whose magnitude falls from one to zero over a
/// raised-cosine step one octave wide on a logarithmic frequency axis, centred on its crossover.
/// The bands are differences between these kernels and a pure delay, so they always sum back to the delayed input.

#pragma once

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

[[nodiscard]] double crossoverLowpassMagnitude(double frequency, double crossover);

[[nodiscard]] int kernelHalfLength(double sampleRate);
}  // namespace esgrove::image_control
