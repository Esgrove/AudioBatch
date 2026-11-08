/// Declares SpatialAnalyzer, the analysis behind ImageControl's spatial spectrogram.
/// It runs short-time Fourier transforms of several lengths on the left and right channels,
/// long ones for the low frequencies and short ones for the high frequencies,
/// and reports the frequency, stereo position, level, and phase coherence of every bin to the display.

#pragma once

#include "Parameters.h"

#include <juce_dsp/juce_dsp.h>

#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// Sample rate the analyzer assumes until it is prepared for another.
constexpr double ANALYZER_DEFAULT_SAMPLE_RATE {48'000.0};

/// One analyzed frequency bin of one channel pair.
struct SpectralPoint {
    /// Centre frequency of the bin, in Hz.
    float frequency {0.0F};
    /// Distance to the neighbouring bins, in Hz, which sets how wide the display draws the point.
    float spacing {0.0F};
    /// Stereo position: +1 at full left, 0 in the centre, and −1 at full right.
    float position {0.0F};
    /// Power of both channels, normalized so a full-scale sine in one channel reads 1.
    float level {0.0F};
    /// Phase coherence: +1 for in-phase content and −1 for anti-phase content.
    float coherence {1.0F};
};

/// Receives the points of one analyzed frame, and the time in seconds the frame stands for.
using SpectralFrameCallback = std::function<void(std::span<const SpectralPoint> points, double frameSeconds)>;

/// One resolution of the analysis: a window length, the frequency range it reports, and how often it runs.
struct AnalysisTier {
    /// Complex transform of twice the window length.
    std::unique_ptr<juce::dsp::FFT> fft;
    /// Hann window of the tier's length.
    std::vector<float> window;
    /// Windowed samples, the left channel as the real part and the right channel as the imaginary part,
    /// zero padded to the transform length.
    std::vector<std::complex<float>> samples;
    /// Spectrum of the packed samples, from which the spectra of both channels are separated.
    std::vector<std::complex<float>> spectrum;
    /// Lowest frequency the tier reports, in Hz.
    double minFrequency {0.0};
    /// Highest frequency the tier reports, in Hz.
    double maxFrequency {0.0};
    /// Scale from squared bin magnitudes to levels where a full-scale sine reads 1.
    float levelScale {1.0F};
    /// Samples between frames.
    std::size_t hop {0};
    /// Samples pushed since the tier's last frame.
    std::size_t samplesSinceFrame {0};
};

/// The S-curve from the level difference between the channels to the stereo position.
///
/// A difference of d dB draws at tanh((d / scale)^exponent) of the way from the centre to the edge,
/// towards the louder channel.
/// The scale and exponent follow in closed form from two settings:
/// the midpoint, the difference drawn half way to the edge,
/// and the edge, the difference drawn 99% of the way there,
/// so only nearly fully panned content reaches the edge.
/// Below the midpoint the curve rises gently, and above it it flattens towards the edge,
/// the steeper the closer the two settings are.
/// The edge always stays at least MIN_PAN_CURVE_GAP_DB above the midpoint.
class StereoPositionCurve
{
public:
    StereoPositionCurve();
    StereoPositionCurve(double midpointSetting, double edgeSetting);

    [[nodiscard]] float edge() const noexcept;
    [[nodiscard]] float midpoint() const noexcept;
    [[nodiscard]] float position(float differenceDecibels) const;

    [[nodiscard]] bool operator==(const StereoPositionCurve&) const = default;

private:
    /// Level difference drawn 99% of the way to the edge, in dB.
    float edgeDecibels {0.0F};
    /// Exponent of the curve, which sets how sharply it turns at the midpoint.
    float exponent {1.0F};
    /// Level difference drawn half way to the edge, in dB.
    float midpointDecibels {0.0F};
    /// Level difference the exponent applies to, in dB.
    float scaleDecibels {1.0F};
};

[[nodiscard]] float positionFromMagnitudes(float leftMagnitude, float rightMagnitude, const StereoPositionCurve& curve);

/// Multi-resolution spatial analysis of stereo audio.
///
/// Each tier transforms the newest samples with a Hann window, zero padded to twice its length
/// so neighbouring bins interpolate the spectrum.
/// The longest tier resolves the lowest octaves to under a hertz,
/// and the shortest keeps the high frequencies sharp in time.
///
/// A bin's position follows the level difference between the channels, as in common spatial analyzers:
/// the difference in dB through the stereo position curve, so the edges hold only nearly fully panned content.
/// Its coherence is Re(L · conj(R)) / (|L| · |R|), which tells anti-phase content from mono content,
/// since both sit in the centre by level alone.
///
/// Not thread safe. The editor's analysis worker owns it and uses it on the worker thread only.
class SpatialAnalyzer
{
public:
    SpatialAnalyzer();

    void clear();
    void prepare(double newSampleRate);
    void pushSamples(std::span<const float> left, std::span<const float> right);
    void setFrameCallback(SpectralFrameCallback callback);
    void setPositionCurve(const StereoPositionCurve& curve);

private:
    void analyzeTier(AnalysisTier& tier);

    /// Maps the level difference between the channels to the stereo position.
    StereoPositionCurve positionCurve;
    /// The tiers from the lowest frequencies to the highest.
    std::vector<AnalysisTier> tiers;
    /// Ring of the newest left samples, as long as the longest window.
    std::vector<float> leftHistory;
    /// Ring of the newest right samples, as long as the longest window.
    std::vector<float> rightHistory;
    /// The points of the frame being reported, reused between frames.
    std::vector<SpectralPoint> points;
    /// Receives every analyzed frame.
    SpectralFrameCallback frameCallback;

    /// Sample rate the tiers were built for.
    double sampleRate {ANALYZER_DEFAULT_SAMPLE_RATE};
    /// Position in the rings where the next sample goes.
    std::size_t writePosition {0};
};
}  // namespace esgrove::image_control
