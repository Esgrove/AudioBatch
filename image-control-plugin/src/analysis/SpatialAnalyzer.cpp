/// Implements SpatialAnalyzer: the tier layout, the ring of recent samples,
/// the zero-padded Hann-windowed transforms, and the per-bin position, level, and coherence.

#include "analysis/SpatialAnalyzer.h"

#include "Parameters.h"

#include <algorithm>
#include <array>
#include <cmath>
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
/// The long windows run less often, since their content changes slowly anyway.
constexpr std::array TIER_LAYOUTS {
    TierLayout {.windowOrder = 15, .minFrequency = 0.0, .maxFrequency = 80.0, .hop = 2048},
    TierLayout {.windowOrder = 13, .minFrequency = 80.0, .maxFrequency = 500.0, .hop = 512},
    TierLayout {.windowOrder = 11, .minFrequency = 500.0, .maxFrequency = 4000.0, .hop = 512},
    TierLayout {.windowOrder = 10, .minFrequency = 4000.0, .maxFrequency = 1.0e9, .hop = 512},
};
/// Sample rate up to which the tier layouts apply. Each doubling above it doubles the windows and hops.
constexpr double TIER_REFERENCE_SAMPLE_RATE {48'000.0};
/// Samples pushed at a time, so every tier keeps its hop even when large blocks arrive.
constexpr std::size_t PUSH_CHUNK {256};
/// Bins whose normalized level is below this, about −120 dB, are not reported.
constexpr float SILENCE_LEVEL {1.0e-12F};

/// Returns how many times the sample rate doubles above the reference rate.
int rateDoublings(const double sampleRate)
{
    return std::max(0, static_cast<int>(std::ceil(std::log2(sampleRate / TIER_REFERENCE_SAMPLE_RATE))));
}

/// Builds a tier for a layout at a sample rate: its FFT, window, spectra, and level scale.
AnalysisTier makeTier(const TierLayout& layout, const int doublings)
{
    AnalysisTier tier;
    const int windowOrder = layout.windowOrder + doublings;
    const std::size_t windowLength = std::size_t {1} << static_cast<unsigned>(windowOrder);
    // The transform is twice the window, so the zero padding interpolates between the window's own bins.
    tier.fft = std::make_unique<juce::dsp::FFT>(windowOrder + 1);
    const auto fftSize = static_cast<std::size_t>(tier.fft->getSize());
    tier.window.assign(windowLength, 0.0F);
    juce::dsp::WindowingFunction<float>::fillWindowingTables(
        tier.window.data(), windowLength, juce::dsp::WindowingFunction<float>::hann, false
    );
    tier.leftSpectrum.assign(fftSize * 2, 0.0F);
    tier.rightSpectrum.assign(fftSize * 2, 0.0F);
    tier.minFrequency = layout.minFrequency;
    tier.maxFrequency = layout.maxFrequency;
    tier.hop = layout.hop << static_cast<unsigned>(doublings);

    double windowSum = 0.0;
    for (const float value : tier.window) {
        windowSum += static_cast<double>(value);
    }
    // A sine of amplitude A peaks at A times half the window sum in its bin.
    const double fullScaleMagnitude = windowSum * 0.5;
    tier.levelScale = static_cast<float>(1.0 / (fullScaleMagnitude * fullScaleMagnitude));
    return tier;
}
}  // namespace

/// Creates an analyzer for 48 kHz audio with the default focus.
SpatialAnalyzer::SpatialAnalyzer()
{
    prepare(ANALYZER_DEFAULT_SAMPLE_RATE);
}

/// Returns the stereo position of a bin from its channel magnitudes:
/// the level difference in dB divided by the focus, clamped to ±1.
/// Content in only one channel sits at that channel's edge, and silence in the centre.
float SpatialAnalyzer::positionFromMagnitudes(
    const float leftMagnitude,
    const float rightMagnitude,
    const float focusDecibels
)
{
    if (rightMagnitude <= 0.0F) {
        return leftMagnitude > 0.0F ? 1.0F : 0.0F;
    }
    if (leftMagnitude <= 0.0F) {
        return -1.0F;
    }
    const float difference = 20.0F * std::log10(leftMagnitude / rightMagnitude);
    return std::clamp(difference / focusDecibels, -1.0F, 1.0F);
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
    const int doublings = rateDoublings(sampleRate);
    tiers.clear();
    std::size_t longestWindow = 0;
    std::size_t mostPoints = 0;
    for (const auto& layout : TIER_LAYOUTS) {
        tiers.push_back(makeTier(layout, doublings));
        longestWindow = std::max(longestWindow, tiers.back().window.size());
        mostPoints += tiers.back().window.size();
    }
    leftHistory.assign(longestWindow, 0.0F);
    rightHistory.assign(longestWindow, 0.0F);
    points.reserve(mostPoints);
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

/// Sets the level difference in dB that reaches the edges of the display, clamped to the focus range.
void SpatialAnalyzer::setFocus(const double decibels)
{
    focusDecibels = static_cast<float>(std::clamp(decibels, MIN_FOCUS_DB, MAX_FOCUS_DB));
}

/// Sets the function that receives every analyzed frame.
void SpatialAnalyzer::setFrameCallback(SpectralFrameCallback callback)
{
    frameCallback = std::move(callback);
}

/// Transforms the newest window of both channels for a tier,
/// and reports the bins in the tier's frequency range that are not silent.
void SpatialAnalyzer::analyzeTier(AnalysisTier& tier)
{
    const std::size_t windowLength = tier.window.size();
    std::ranges::fill(tier.leftSpectrum, 0.0F);
    std::ranges::fill(tier.rightSpectrum, 0.0F);
    const auto leftWindow = std::span<float>(tier.leftSpectrum).first(windowLength);
    const auto rightWindow = std::span<float>(tier.rightSpectrum).first(windowLength);
    copyNewest(leftHistory, leftWindow);
    copyNewest(rightHistory, rightWindow);
    for (std::size_t sample = 0; sample < windowLength; ++sample) {
        leftWindow[sample] *= tier.window[sample];
        rightWindow[sample] *= tier.window[sample];
    }
    tier.fft->performRealOnlyForwardTransform(tier.leftSpectrum.data(), true);
    tier.fft->performRealOnlyForwardTransform(tier.rightSpectrum.data(), true);

    const int fftSize = tier.fft->getSize();
    const double binWidth = sampleRate / static_cast<double>(fftSize);
    const double lowest = std::max(tier.minFrequency, static_cast<double>(MIN_FREQUENCY));
    const double highest = std::min(tier.maxFrequency, static_cast<double>(MAX_FREQUENCY));
    const int firstBin = std::max(1, static_cast<int>(std::ceil(lowest / binWidth)));
    const int lastBin = std::min(fftSize / 2 - 1, static_cast<int>(std::floor(highest / binWidth)));

    points.clear();
    for (int bin = firstBin; bin <= lastBin; ++bin) {
        const auto index = static_cast<std::size_t>(bin) * 2;
        const float leftReal = tier.leftSpectrum[index];
        const float leftImaginary = tier.leftSpectrum[index + 1];
        const float rightReal = tier.rightSpectrum[index];
        const float rightImaginary = tier.rightSpectrum[index + 1];
        const float leftMagnitude = std::hypot(leftReal, leftImaginary);
        const float rightMagnitude = std::hypot(rightReal, rightImaginary);
        const float level = (leftMagnitude * leftMagnitude + rightMagnitude * rightMagnitude) * tier.levelScale;
        if (level < SILENCE_LEVEL) {
            continue;
        }
        const float magnitudeProduct = leftMagnitude * rightMagnitude;
        points.push_back({
            .frequency = static_cast<float>(static_cast<double>(bin) * binWidth),
            .spacing = static_cast<float>(binWidth),
            .position = positionFromMagnitudes(leftMagnitude, rightMagnitude, focusDecibels),
            .level = level,
            .coherence = magnitudeProduct > 0.0F
                ? (leftReal * rightReal + leftImaginary * rightImaginary) / magnitudeProduct
                : 1.0F,
        });
    }
    if (frameCallback) {
        frameCallback(points, static_cast<double>(tier.hop) / sampleRate);
    }
}

/// Copies the newest samples of a ring, oldest first, filling the destination.
void SpatialAnalyzer::copyNewest(const std::span<const float> ring, const std::span<float> destination) const
{
    const std::size_t length = destination.size();
    const std::size_t start = (writePosition + ring.size() - length) & (ring.size() - 1);
    const std::size_t firstPart = std::min(length, ring.size() - start);
    std::ranges::copy(ring.subspan(start, firstPart), destination.begin());
    std::ranges::copy(ring.first(length - firstPart), destination.subspan(firstPart).begin());
}
}  // namespace esgrove::image_control
