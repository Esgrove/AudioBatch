/// Implements ordinary and quadrature FIR design with frequency sampling and a shared Blackman-Harris window.
/// Ordinary lowpasses sample a real, even spectrum and normalize to unity DC gain.
/// Quadrature kernels sample an imaginary, odd spectrum and retain zero DC gain without normalization.

#include "dsp/CrossoverDesign.h"

#include "Parameters.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// The design FFT is this many times the kernel half length, keeping periodic impulse copies outside its support.
/// Four meets the measured fullband gain-error bound of 0.00055 at 20 Hz to 20 kHz with the Blackman-Harris window.
/// The analytical cotangent impulse test also pins its finite-grid endpoint behaviour.
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

/// Builds the full symmetric Blackman-Harris window of 2 * halfLength + 1 taps.
std::vector<double> buildWindow(const int length)
{
    std::vector<double> result(static_cast<std::size_t>(length));
    for (int tap = 0; tap < length; ++tap) {
        result[static_cast<std::size_t>(tap)] = blackmanHarris(tap, length);
    }
    return result;
}

/// Windows and normalizes an inverse-transformed real-even lowpass impulse response to unity DC gain.
std::vector<float> finishLowpassKernel(
    const std::span<const float> spectrum,
    const std::span<const double> window,
    std::span<double> taps,
    const int halfLength,
    const int fftSize
)
{
    double sum = 0.0;
    for (int offset = -halfLength; offset <= halfLength; ++offset) {
        const auto circularIndex = static_cast<std::size_t>((offset + fftSize) % fftSize);
        const auto tap = static_cast<std::size_t>(offset) + static_cast<std::size_t>(halfLength);
        const double value = static_cast<double>(spectrum[circularIndex]) * window[tap];
        taps[tap] = value;
        sum += value;
    }
    std::vector<float> kernel(taps.size());
    for (std::size_t tap = 0; tap < taps.size(); ++tap) {
        kernel[tap] = static_cast<float>(taps[tap] / sum);
    }
    return kernel;
}

/// Windows an inverse-transformed imaginary-odd quadrature impulse response onto exact antisymmetric tap pairs.
std::vector<float> finishQuadratureKernel(
    const std::span<const float> spectrum,
    const std::span<const double> window,
    const int halfLength,
    const int fftSize
)
{
    std::vector<float> kernel(window.size(), 0.0F);
    const auto centre = static_cast<std::size_t>(halfLength);
    for (int offset = 1; offset <= halfLength; ++offset) {
        const auto positive = static_cast<std::size_t>(offset);
        const auto negative = static_cast<std::size_t>(fftSize - offset);
        // Project FFT rounding back onto the odd impulse response before rounding each shared tap to float.
        const double value = (static_cast<double>(spectrum[positive]) - static_cast<double>(spectrum[negative])) * 0.5
            * window[centre + positive];
        const auto tap = static_cast<float>(value);
        kernel[centre + positive] = tap;
        kernel[centre - positive] = -tap;
    }
    return kernel;
}
}  // namespace

/// Precomputes the design FFT, Blackman-Harris window, workspaces, and constant fullband quadrature kernel.
void CrossoverDesigner::prepare(const double newSampleRate, const int newHalfLength)
{
    sampleRate = newSampleRate;
    halfLength = newHalfLength;
    const int length = 2 * halfLength + 1;
    const int fftOrder = juce::roundToInt(std::log2(static_cast<double>(halfLength * DESIGN_OVERSAMPLING)));
    fft = std::make_unique<juce::dsp::FFT>(fftOrder);
    const int fftSize = fft->getSize();
    window = buildWindow(length);
    lowpassTaps.assign(static_cast<std::size_t>(length), 0.0);
    lowpassSpectrum.assign(static_cast<std::size_t>(fftSize) * 2, 0.0F);
    quadratureSpectrum.assign(static_cast<std::size_t>(fftSize) * 2, 0.0F);

    std::ranges::fill(quadratureSpectrum, 0.0F);
    for (int bin = 1; bin < fftSize / 2; ++bin) {
        quadratureSpectrum[static_cast<std::size_t>(bin) * 2 + 1] = -1.0F;
        quadratureSpectrum[static_cast<std::size_t>(fftSize - bin) * 2 + 1] = 1.0F;
    }
    fft->performRealOnlyInverseTransform(quadratureSpectrum.data());
    fullQuadratureKernel = finishQuadratureKernel(quadratureSpectrum, window, halfLength, fftSize);
}

/// Designs the three ordinary lowpasses, three quadrature lowpasses, and cached fullband quadrature kernel.
/// Evaluates crossoverLowpassMagnitude once per bin for each crossover pair.
std::vector<std::vector<float>> CrossoverDesigner::designKernelSet(const Crossovers& crossovers)
{
    if (fft == nullptr) {
        prepare(sampleRate, halfLength);
    }
    const int fftSize = fft->getSize();
    const double binWidth = sampleRate / static_cast<double>(fftSize);

    std::vector<std::vector<float>> ordinary;
    std::vector<std::vector<float>> quadrature;
    ordinary.reserve(CROSSOVER_COUNT);
    quadrature.reserve(CROSSOVER_COUNT);

    for (const float crossover : crossovers) {
        const auto target = static_cast<double>(crossover);
        std::ranges::fill(lowpassSpectrum, 0.0F);
        std::ranges::fill(quadratureSpectrum, 0.0F);
        lowpassSpectrum[0] = 1.0F;
        for (int bin = 1; bin < fftSize / 2; ++bin) {
            const auto magnitude
                = static_cast<float>(crossoverLowpassMagnitude(static_cast<double>(bin) * binWidth, target));
            lowpassSpectrum[static_cast<std::size_t>(bin) * 2] = magnitude;
            quadratureSpectrum[static_cast<std::size_t>(bin) * 2 + 1] = -magnitude;
            quadratureSpectrum[static_cast<std::size_t>(fftSize - bin) * 2 + 1] = magnitude;
        }
        const int nyquistBin = fftSize / 2;
        lowpassSpectrum[static_cast<std::size_t>(nyquistBin) * 2]
            = static_cast<float>(crossoverLowpassMagnitude(static_cast<double>(nyquistBin) * binWidth, target));

        fft->performRealOnlyInverseTransform(lowpassSpectrum.data());
        fft->performRealOnlyInverseTransform(quadratureSpectrum.data());
        ordinary.push_back(finishLowpassKernel(lowpassSpectrum, window, lowpassTaps, halfLength, fftSize));
        quadrature.push_back(finishQuadratureKernel(quadratureSpectrum, window, halfLength, fftSize));
    }

    std::vector<std::vector<float>> kernels;
    kernels.reserve(CROSSOVER_COUNT * 2 + 1);
    for (auto& kernel : ordinary) {
        kernels.push_back(std::move(kernel));
    }
    for (auto& kernel : quadrature) {
        kernels.push_back(std::move(kernel));
    }
    kernels.push_back(fullQuadratureKernel);
    return kernels;
}

/// Designs a lowpass kernel of length 2 × halfLength + 1 for a crossover at the given frequency.
/// The kernel is symmetric around its centre tap, so it delays every frequency by halfLength samples,
/// and its taps are normalized to unity DC gain in double precision before rounding to float.
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

/// Designs a Type III quadrature FIR of length 2 × halfLength + 1, with the ordinary lowpass's support and delay.
/// halfLength must be a positive power of two, as returned by kernelHalfLength.
/// Positive frequencies have a -90 degree phase relative to the input delayed by halfLength samples:
/// a cosine becomes a sine. An absent crossover selects fullband, otherwise it selects the same lowpass target.
/// The imaginary spectrum is -target on positive bins and +target on their conjugate partners.
/// DC and Nyquist are zero, and mirrored tap pairs are exact negatives with an exactly zero centre.
/// No DC or passband normalization is applied. Normalizing its zero sum would destroy the quadrature response.
/// Fullband gain stays within 0.00055 of unity on independently measured 20 Hz to 20 kHz grids.
/// Those probes cover 44.1/48/88.2/96/176.4/192 kHz with kernelHalfLength(sampleRate).
/// At 20 Hz the loss is below 0.0048 dB, but every finite Type III kernel must roll off to zero at both endpoints.
/// This is a finite approximation, not an exact Hilbert transform arbitrarily close to DC or Nyquist.
/// Allocates and transforms during preparation or kernel design, never as a second cascaded processing stage.
std::vector<float> designQuadratureKernel(
    const double sampleRate,
    const int halfLength,
    const std::optional<double> crossover
)
{
    const int length = 2 * halfLength + 1;
    const int fftOrder = juce::roundToInt(std::log2(static_cast<double>(halfLength * DESIGN_OVERSAMPLING)));
    const juce::dsp::FFT fft(fftOrder);
    const int fftSize = fft.getSize();

    std::vector<float> spectrum(static_cast<std::size_t>(fftSize) * 2, 0.0F);
    const double binWidth = sampleRate / static_cast<double>(fftSize);
    for (int bin = 1; bin < fftSize / 2; ++bin) {
        const float magnitude = crossover.has_value()
            ? static_cast<float>(crossoverLowpassMagnitude(static_cast<double>(bin) * binWidth, *crossover))
            : 1.0F;
        spectrum[static_cast<std::size_t>(bin) * 2 + 1] = -magnitude;
        spectrum[static_cast<std::size_t>(fftSize - bin) * 2 + 1] = magnitude;
    }
    fft.performRealOnlyInverseTransform(spectrum.data());

    std::vector<float> kernel(static_cast<std::size_t>(length), 0.0F);
    const auto centre = static_cast<std::size_t>(halfLength);
    for (int offset = 1; offset <= halfLength; ++offset) {
        const auto positive = static_cast<std::size_t>(offset);
        const auto negative = static_cast<std::size_t>(fftSize - offset);
        // Project FFT rounding back onto the odd impulse response before rounding each shared tap to float.
        const double value = (static_cast<double>(spectrum[positive]) - static_cast<double>(spectrum[negative])) * 0.5
            * blackmanHarris(halfLength + offset, length);
        const auto tap = static_cast<float>(value);
        kernel[centre + positive] = tap;
        kernel[centre - positive] = -tap;
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
