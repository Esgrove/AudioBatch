/// Implements the lowpass kernel design with the frequency sampling method:
/// the target magnitude is sampled on a dense FFT grid, inverse transformed to a zero-phase impulse response,
/// centred, shortened with a Blackman-Harris window, and normalized to unity gain at DC.

#include "dsp/CrossoverDesign.h"

#include "Parameters.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// The design FFT is this many times the kernel half length,
/// so the sampled response is dense and the time aliasing of the zero-phase response stays far from the kernel.
constexpr int DESIGN_OVERSAMPLING {4};
/// Coefficients of the four-term Blackman-Harris window.
constexpr std::array<double, 4> BLACKMAN_HARRIS {0.358'75, 0.488'29, 0.141'28, 0.011'68};

/// Returns the Blackman-Harris window value at an index of a window with the given length:
/// the sum of the coefficients times the cosines of the phase multiples, with alternating signs.
double blackmanHarris(const int index, const int length)
{
    const double phase = 2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(length - 1);
    double value = 0.0;
    double sign = 1.0;
    for (std::size_t term = 0; term < BLACKMAN_HARRIS.size(); ++term) {
        value += sign * BLACKMAN_HARRIS.at(term) * std::cos(static_cast<double>(term) * phase);
        sign = -sign;
    }
    return value;
}
}  // namespace

/// Designs a lowpass kernel of length 2 × halfLength + 1 for a crossover at the given frequency.
/// The kernel is symmetric around its centre tap, so it delays every frequency by halfLength samples,
/// and its taps sum to exactly one in double precision before rounding to float.
/// Allocates, so it runs during preparation or on the kernel design thread during real-time playback.
/// An explicitly non-real-time render may design kernels synchronously in its processing callback.
std::vector<float> designLowpassKernel(const double crossover, const double sampleRate, const int halfLength)
{
    const int length = 2 * halfLength + 1;
    const int fftOrder = juce::roundToInt(std::log2(static_cast<double>(halfLength * DESIGN_OVERSAMPLING)));
    const juce::dsp::FFT fft(fftOrder);
    const int fftSize = fft.getSize();

    std::vector<float> spectrum(static_cast<std::size_t>(fftSize) * 2, 0.0F);
    const double binWidth = sampleRate / static_cast<double>(fftSize);
    for (int bin = 0; bin <= fftSize / 2; ++bin) {
        spectrum[static_cast<std::size_t>(bin) * 2]
            = static_cast<float>(crossoverLowpassMagnitude(static_cast<double>(bin) * binWidth, crossover));
    }
    fft.performRealOnlyInverseTransform(spectrum.data());

    std::vector<double> taps(static_cast<std::size_t>(length));
    double sum = 0.0;
    for (int offset = -halfLength; offset <= halfLength; ++offset) {
        const auto circularIndex = static_cast<std::size_t>((offset + fftSize) % fftSize);
        const int tap = offset + halfLength;
        const double value = static_cast<double>(spectrum[circularIndex]) * blackmanHarris(tap, length);
        taps[static_cast<std::size_t>(tap)] = value;
        sum += value;
    }

    std::vector<float> kernel(taps.size());
    for (std::size_t tap = 0; tap < taps.size(); ++tap) {
        kernel[tap] = static_cast<float>(taps[tap] / sum);
    }
    return kernel;
}

/// Returns the target magnitude of a crossover lowpass at a frequency.
/// It is one below the transition, zero above it,
/// and a raised-cosine step on a log2 frequency axis in between, crossing one half at the crossover.
/// The matching highpass is one minus this, so each crossover is complementary in amplitude.
double crossoverLowpassMagnitude(const double frequency, const double crossover)
{
    if (frequency <= 0.0) {
        return 1.0;
    }
    const double position = std::log2(frequency / crossover) / TRANSITION_OCTAVES;
    if (position <= -0.5) {
        return 1.0;
    }
    if (position >= 0.5) {
        return 0.0;
    }
    return 0.5 * (1.0 - std::sin(std::numbers::pi * position));
}

/// Returns the kernel half length for a sample rate, which is also the latency of the crossovers in samples.
/// The length doubles with each doubling of the sample rate above REFERENCE_SAMPLE_RATE,
/// so the frequency resolution stays the same, and it is always a power of two.
int kernelHalfLength(const double sampleRate)
{
    const int doublings = sampleRateDoublings(sampleRate, REFERENCE_SAMPLE_RATE);
    int halfLength = REFERENCE_HALF_LENGTH;
    for (int doubling = 0; doubling < doublings; ++doubling) {
        halfLength *= 2;
    }
    return halfLength;
}
}  // namespace esgrove::image_control
