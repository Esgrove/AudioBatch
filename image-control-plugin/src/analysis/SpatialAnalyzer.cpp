/// Implements SpatialAnalyzer: the tier layout, the ring of recent samples, the packed transform of both channels,
/// the zero-padded Hann-windowed transforms, and the per-bin position, level, and coherence.

#include "analysis/SpatialAnalyzer.h"

#include "Parameters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace esgrove::image_control
{
namespace
{
/// The window order, frequency range, and hop of one tier at sample rates up to 48 kHz.
struct TierLayout {
    int windowOrder {0};
    double minFrequency {0.0};
    double maxFrequency {0.0};
    std::size_t hop {0};
};

/// The tiers from the lowest frequencies to the highest.
/// 32768 samples resolve the lowest octaves to about 0.7 Hz with zero padding,
/// and 1024 samples keep the high frequencies within about 20 ms.
/// Up to 640 Hz each tier covers one octave and starts where its window length times the frequency is the same,
/// so a sine spreads over about the same share of an octave at every tier boundary
/// instead of widening several times where a much shorter window takes over.
/// The long windows run less often, since their content changes slowly anyway.
constexpr std::array TIER_LAYOUTS {
    TierLayout {.windowOrder = 15, .minFrequency = 0.0, .maxFrequency = 80.0, .hop = 2048},
    TierLayout {.windowOrder = 14, .minFrequency = 80.0, .maxFrequency = 160.0, .hop = 1024},
    TierLayout {.windowOrder = 13, .minFrequency = 160.0, .maxFrequency = 320.0, .hop = 512},
    TierLayout {.windowOrder = 12, .minFrequency = 320.0, .maxFrequency = 640.0, .hop = 512},
    TierLayout {.windowOrder = 11, .minFrequency = 640.0, .maxFrequency = 4000.0, .hop = 512},
    TierLayout {.windowOrder = 10, .minFrequency = 4000.0, .maxFrequency = 1.0e9, .hop = 512},
};
/// Sample rate up to which the tier layouts apply. Each doubling above it doubles the windows and hops.
constexpr double TIER_REFERENCE_SAMPLE_RATE {48'000.0};
/// Samples pushed at a time, so every tier keeps its hop even when large blocks arrive.
constexpr std::size_t PUSH_CHUNK {256};
/// Bins whose normalized level is below this, about −120 dB, are not reported.
constexpr float SILENCE_LEVEL {1.0e-12F};
/// Share of the way to the edge where the stereo position curve draws its midpoint.
constexpr double CURVE_MIDPOINT_POSITION {0.5};
/// Share of the way to the edge where the stereo position curve draws its edge setting.
/// Just below 1, so the curve can reach it with a finite level difference.
constexpr double CURVE_EDGE_POSITION {0.99};
/// Decibels per decade of an amplitude ratio.
constexpr float DECIBELS_PER_AMPLITUDE_DECADE {20.0F};
/// Weight of the spectrum two bins to each side of a bin in the periodic Hann window,
/// applied to a transform zero padded to twice the window length: H[k] = X[k] / 2 − (X[k − 2] + X[k + 2]) / 4.
constexpr float HANN_SIDE_WEIGHT {0.25F};

/// Builds a tier for a layout at a sample rate: its FFT, spectra, and level scale.
AnalysisTier makeTier(const TierLayout& layout, const int doublings)
{
    AnalysisTier tier;
    const int windowOrder = layout.windowOrder + doublings;
    tier.windowLength = std::size_t {1} << static_cast<unsigned>(windowOrder);
    // The transform is twice the window, so the zero padding interpolates between the window's own bins.
    tier.fft = std::make_unique<juce::dsp::FFT>(windowOrder + 1);
    const auto fftSize = static_cast<std::size_t>(tier.fft->getSize());
    tier.samples.assign(fftSize, {});
    tier.spectrum.assign(fftSize, {});
    tier.minFrequency = layout.minFrequency;
    tier.maxFrequency = layout.maxFrequency;
    tier.hop = layout.hop << static_cast<unsigned>(doublings);

    // The periodic Hann window sums to half its length,
    // and a sine of amplitude A peaks at A times half the window sum in its bin.
    const double windowSum = static_cast<double>(tier.windowLength) * 0.5;
    const double fullScaleMagnitude = windowSum * 0.5;
    tier.levelScale = static_cast<float>(1.0 / (fullScaleMagnitude * fullScaleMagnitude));
    return tier;
}

/// Separates the left and right spectra at a bin from the packed transform of both channels,
/// wrapping bins below 0 and above the transform length, which the spectrum filters reach at its edges.
/// Since both inputs are real, the packed spectrum X separates into
/// L[k] = (X[k] + conj(X[N − k])) / 2 and R[k] = (X[k] − conj(X[N − k])) / 2i.
std::pair<std::complex<float>, std::complex<float>> separateChannels(
    const std::span<const std::complex<float>> spectrum,
    const int bin
)
{
    const auto size = static_cast<int>(spectrum.size());
    const int wrapped = ((bin % size) + size) % size;
    const std::complex<float> packed = spectrum[static_cast<std::size_t>(wrapped)];
    const std::complex<float> mirrored = std::conj(spectrum[static_cast<std::size_t>((size - wrapped) % size)]);
    const std::complex<float> left = (packed + mirrored) * 0.5F;
    const std::complex<float> difference = (packed - mirrored) * 0.5F;
    // Dividing by i turns a + bi into b − ai.
    return {left, {difference.imag(), -difference.real()}};
}

/// Returns a spectrum value divided by 2i: (a + bi) / 2i = (b − ai) / 2.
std::complex<float> divideByTwoI(const std::complex<float> value)
{
    return {value.imag() * 0.5F, -value.real() * 0.5F};
}
}  // namespace

/// Creates an analyzer for 48 kHz audio with the default stereo position curve.
SpatialAnalyzer::SpatialAnalyzer()
{
    prepare(ANALYZER_DEFAULT_SAMPLE_RATE);
}

/// Creates the curve of the default pan midpoint and edge.
StereoPositionCurve::StereoPositionCurve() : StereoPositionCurve(DEFAULT_PAN_MIDPOINT_DB, DEFAULT_PAN_EDGE_DB) {}

/// Creates the curve that draws the midpoint setting half way to the edge, and the edge setting 99% of the way there.
/// Both are clamped to their setting ranges, and the edge to at least MIN_PAN_CURVE_GAP_DB above the midpoint.
/// From tanh((m / s)^p) = 1/2 and tanh((e / s)^p) = 0.99 follow
/// p = ln(atanh(0.99) / atanh(1/2)) / ln(e / m) and s = m / atanh(1/2)^(1/p).
StereoPositionCurve::StereoPositionCurve(const double midpointSetting, const double edgeSetting)
{
    const double midpoint = std::clamp(midpointSetting, MIN_PAN_MIDPOINT_DB, MAX_PAN_MIDPOINT_DB);
    const double edge
        = std::clamp(edgeSetting, std::max(MIN_PAN_EDGE_DB, midpoint + MIN_PAN_CURVE_GAP_DB), MAX_PAN_EDGE_DB);
    const double midpointArgument = std::atanh(CURVE_MIDPOINT_POSITION);
    const double curveExponent
        = std::log(std::atanh(CURVE_EDGE_POSITION) / midpointArgument) / std::log(edge / midpoint);
    midpointDecibels = static_cast<float>(midpoint);
    edgeDecibels = static_cast<float>(edge);
    exponent = static_cast<float>(curveExponent);
    scaleDecibels = static_cast<float>(midpoint / std::pow(midpointArgument, 1.0 / curveExponent));
}

/// Compares the curve settings and their derived coefficients within floating-point rounding precision.
bool StereoPositionCurve::operator==(const StereoPositionCurve& other) const
{
    return juce::approximatelyEqual(edgeDecibels, other.edgeDecibels)
        && juce::approximatelyEqual(exponent, other.exponent)
        && juce::approximatelyEqual(midpointDecibels, other.midpointDecibels)
        && juce::approximatelyEqual(scaleDecibels, other.scaleDecibels);
}

/// Returns the level difference drawn 99% of the way to the edge, in dB.
float StereoPositionCurve::edge() const noexcept
{
    return edgeDecibels;
}

/// Returns the level difference drawn half way to the edge, in dB.
float StereoPositionCurve::midpoint() const noexcept
{
    return midpointDecibels;
}

/// Returns the share of the way from the centre to the edge, from 0 to 1,
/// where a level difference between the channels in dB is drawn. The sign of the difference is ignored.
float StereoPositionCurve::position(const float differenceDecibels) const
{
    return std::tanh(std::pow(std::abs(differenceDecibels) / scaleDecibels, exponent));
}

/// Returns the stereo position of a bin from its channel magnitudes:
/// the level difference in dB through the curve, towards the louder channel.
/// Content in only one channel sits at that channel's edge, and silence in the centre.
float positionFromMagnitudes(const float leftMagnitude, const float rightMagnitude, const StereoPositionCurve& curve)
{
    if (rightMagnitude <= 0.0F) {
        return leftMagnitude > 0.0F ? 1.0F : 0.0F;
    }
    if (leftMagnitude <= 0.0F) {
        return -1.0F;
    }
    const float difference = DECIBELS_PER_AMPLITUDE_DECADE * std::log10(leftMagnitude / rightMagnitude);
    const float share = curve.position(difference);
    return difference < 0.0F ? -share : share;
}

/// Forgets the recent samples, as if the input had been silent.
void SpatialAnalyzer::clear()
{
    std::ranges::fill(leftHistory, 0.0F);
    std::ranges::fill(rightHistory, 0.0F);
    for (auto& tier : tiers) {
        tier.samplesSinceFrame = 0;
    }
    writePosition = 0;
}

/// Builds the tiers for a sample rate and clears the recent samples.
/// Allocates, so it runs when the editor opens or the sample rate changes.
void SpatialAnalyzer::prepare(const double newSampleRate)
{
    sampleRate = newSampleRate;
    const int doublings = sampleRateDoublings(sampleRate, TIER_REFERENCE_SAMPLE_RATE);
    tiers.clear();
    std::size_t longestWindow = 0;
    std::size_t mostPoints = 0;
    for (const auto& layout : TIER_LAYOUTS) {
        tiers.push_back(makeTier(layout, doublings));
        longestWindow = std::max(longestWindow, tiers.back().windowLength);
        mostPoints = std::max(mostPoints, tiers.back().windowLength);
    }
    leftHistory.assign(longestWindow, 0.0F);
    rightHistory.assign(longestWindow, 0.0F);
    // A tier reports at most half its transform, its window length, and the spectrum filters reach two bins further.
    points.reserve(mostPoints);
    rawLeft.reserve(mostPoints + 4);
    rawRight.reserve(mostPoints + 4);
    hannLeft.reserve(mostPoints);
    hannRight.reserve(mostPoints);
    reassignedFrequencies.reserve(mostPoints);
    clear();
}

/// Adds samples of both channels, which must be the same length,
/// and analyzes each tier whenever its hop has passed, reporting each frame to the callback.
void SpatialAnalyzer::pushSamples(const std::span<const float> left, const std::span<const float> right)
{
    jassert(left.size() == right.size());
    const std::size_t ringMask = leftHistory.size() - 1;
    std::size_t consumed = 0;
    while (consumed < left.size()) {
        const std::size_t count = std::min(PUSH_CHUNK, left.size() - consumed);
        for (std::size_t sample = 0; sample < count; ++sample) {
            leftHistory[writePosition] = left[consumed + sample];
            rightHistory[writePosition] = right[consumed + sample];
            writePosition = (writePosition + 1) & ringMask;
        }
        consumed += count;
        for (auto& tier : tiers) {
            tier.samplesSinceFrame += count;
            if (tier.samplesSinceFrame >= tier.hop) {
                tier.samplesSinceFrame %= tier.hop;
                analyzeTier(tier);
            }
        }
    }
}

/// Sets the function that receives every analyzed frame.
void SpatialAnalyzer::setFrameCallback(SpectralFrameCallback callback)
{
    frameCallback = std::move(callback);
}

/// Sets the curve that maps the level difference between the channels to the stereo position.
void SpatialAnalyzer::setPositionCurve(const StereoPositionCurve& curve)
{
    positionCurve = curve;
}

/// Transforms the newest window of both channels for a tier,
/// and reports the bins in the tier's frequency range that are not silent.
/// Both channels go through one complex transform, the left as the real part and the right as the imaginary part,
/// which costs about half of two real transforms with JUCE's fallback engine.
///
/// The frame is transformed unwindowed, and the periodic Hann window h(n) = 1/2 − cos(2πn/W)/2 is applied
/// to the spectrum: with the transform twice the window length W, its cosine shifts the spectrum by two bins,
/// so H[k] = X[k]/2 − (X[k − 2] + X[k + 2])/4.
/// Its time derivative h'(n) = (π/W) sin(2πn/W) gives D[k] = (π/W) (X[k − 2] − X[k + 2]) / 2i the same way.
/// Each bin is reassigned to the frequency its energy comes from, f − fs/2π · Im(D · conj(H)) / |H|²,
/// summed over both channels, and keeps its own frequency when that lies outside the window's main lobe.
///
/// The stereo position and coherence are measured over the bins within POSITION_SMOOTHING_OCTAVES,
/// which draws decorrelated content at its average position, while the level is the bin's own.
void SpatialAnalyzer::analyzeTier(AnalysisTier& tier)
{
    const std::size_t windowLength = tier.windowLength;
    const std::size_t ringMask = leftHistory.size() - 1;
    const std::size_t start = (writePosition + leftHistory.size() - windowLength) & ringMask;
    for (std::size_t sample = 0; sample < windowLength; ++sample) {
        const std::size_t index = (start + sample) & ringMask;
        tier.samples[sample] = {leftHistory[index], rightHistory[index]};
    }
    // The zero padding after the window stays zero, since the transform does not write its input.
    tier.fft->perform(tier.samples.data(), tier.spectrum.data(), false);

    const int fftSize = tier.fft->getSize();
    const double binWidth = sampleRate / static_cast<double>(fftSize);
    const double lowest = std::max(tier.minFrequency, static_cast<double>(MIN_FREQUENCY));
    const double highest = std::min(tier.maxFrequency, static_cast<double>(MAX_FREQUENCY));
    const int firstBin = std::max(1, static_cast<int>(std::ceil(lowest / binWidth)));
    const int lastBin = std::min(fftSize / 2 - 1, static_cast<int>(std::floor(highest / binWidth)));

    points.clear();
    if (lastBin >= firstBin) {
        const auto binCount = static_cast<std::size_t>(lastBin - firstBin) + 1;
        rawLeft.resize(binCount + 4);
        rawRight.resize(binCount + 4);
        for (std::size_t offset = 0; offset < binCount + 4; ++offset) {
            const auto [left, right] = separateChannels(tier.spectrum, firstBin - 2 + static_cast<int>(offset));
            rawLeft[offset] = left;
            rawRight[offset] = right;
        }

        hannLeft.resize(binCount);
        hannRight.resize(binCount);
        reassignedFrequencies.resize(binCount);
        const auto derivativeScale
            = static_cast<float>(juce::MathConstants<double>::pi / static_cast<double>(windowLength));
        const double radiansToHertz = sampleRate / juce::MathConstants<double>::twoPi;
        // The main lobe of the Hann window reaches two of its own bins, four transform bins, to each side.
        const double lobeHalfWidth = 4.0 * binWidth;
        for (std::size_t index = 0; index < binCount; ++index) {
            const std::size_t centre = index + 2;
            hannLeft[index] = rawLeft[centre] * 0.5F - (rawLeft[centre - 2] + rawLeft[centre + 2]) * HANN_SIDE_WEIGHT;
            hannRight[index]
                = rawRight[centre] * 0.5F - (rawRight[centre - 2] + rawRight[centre + 2]) * HANN_SIDE_WEIGHT;
            const std::complex<float> derivativeLeft
                = divideByTwoI(rawLeft[centre - 2] - rawLeft[centre + 2]) * derivativeScale;
            const std::complex<float> derivativeRight
                = divideByTwoI(rawRight[centre - 2] - rawRight[centre + 2]) * derivativeScale;
            const float power = std::norm(hannLeft[index]) + std::norm(hannRight[index]);
            const double frequency = static_cast<double>(firstBin + static_cast<int>(index)) * binWidth;
            double reassigned = frequency;
            if (power > 0.0F) {
                const float shift = (derivativeLeft * std::conj(hannLeft[index])).imag()
                    + (derivativeRight * std::conj(hannRight[index])).imag();
                const double correction = radiansToHertz * static_cast<double>(shift / power);
                if (std::abs(correction) <= lobeHalfWidth) {
                    reassigned = frequency - correction;
                }
            }
            reassignedFrequencies[index] = static_cast<float>(
                std::clamp(reassigned, static_cast<double>(MIN_FREQUENCY), static_cast<double>(MAX_FREQUENCY))
            );
        }

        const double smoothingRatio = std::exp2(POSITION_SMOOTHING_OCTAVES * 0.5) - 1.0;
        for (std::size_t index = 0; index < binCount; ++index) {
            const float level = (std::norm(hannLeft[index]) + std::norm(hannRight[index])) * tier.levelScale;
            if (level < SILENCE_LEVEL) {
                continue;
            }
            const double frequency = static_cast<double>(firstBin + static_cast<int>(index)) * binWidth;
            const auto reach = static_cast<std::size_t>(std::floor(frequency * smoothingRatio / binWidth));
            const std::size_t firstNeighbour = index - std::min(index, reach);
            const std::size_t lastNeighbour = std::min(binCount - 1, index + reach);
            float leftPower = 0.0F;
            float rightPower = 0.0F;
            float crossPower = 0.0F;
            for (std::size_t neighbour = firstNeighbour; neighbour <= lastNeighbour; ++neighbour) {
                leftPower += std::norm(hannLeft[neighbour]);
                rightPower += std::norm(hannRight[neighbour]);
                crossPower += (hannLeft[neighbour] * std::conj(hannRight[neighbour])).real();
            }
            const float powerProduct = leftPower * rightPower;
            points.push_back({
                .frequency = static_cast<float>(frequency),
                .reassignedFrequency = reassignedFrequencies[index],
                .spacing = static_cast<float>(binWidth),
                .position = positionFromMagnitudes(std::sqrt(leftPower), std::sqrt(rightPower), positionCurve),
                .level = level,
                .coherence = powerProduct > 0.0F ? crossPower / std::sqrt(powerProduct) : 1.0F,
            });
        }
    }
    if (frameCallback) {
        frameCallback(points, static_cast<double>(tier.hop) / sampleRate);
    }
}

}  // namespace esgrove::image_control
