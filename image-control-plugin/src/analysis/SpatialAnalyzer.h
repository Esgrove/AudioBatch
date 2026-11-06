/// Declares SpatialAnalyzer, the analysis behind ImageControl's spatial spectrogram.
/// It runs short-time Fourier transforms of several lengths on the left and right channels,
/// long ones for the low frequencies and short ones for the high frequencies,
/// and reports the frequency, stereo position, level, and phase coherence of every bin to the display.

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// Sample rate the analyzer assumes until it is prepared for another.
constexpr double ANALYZER_DEFAULT_SAMPLE_RATE {48'000.0};
/// Default level difference between the channels that reaches the edge of the display, in dB.
constexpr double DEFAULT_FOCUS_DB {18.0};
/// Smallest focus, in dB.
constexpr double MIN_FOCUS_DB {6.0};
/// Largest focus, in dB.
constexpr double MAX_FOCUS_DB {40.0};

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
    std::unique_ptr<juce::dsp::FFT> fft;
    /// Hann window of the tier's length.
    std::vector<float> window;
    std::vector<float> leftSpectrum;
    std::vector<float> rightSpectrum;
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

/// Multi-resolution spatial analysis of stereo audio.
///
/// Each tier transforms the newest samples with a Hann window, zero padded to twice its length
/// so neighbouring bins interpolate the spectrum.
/// The longest tier resolves the lowest octaves to under a hertz,
/// and the shortest keeps the high frequencies sharp in time.
///
/// A bin's position follows the level difference between the channels, as in common spatial analyzers:
/// the difference in dB divided by the focus, clamped to the edges.
/// Its coherence is Re(L · conj(R)) / (|L| · |R|), which tells anti-phase content from mono content,
/// since both sit in the centre by level alone.
///
/// Not thread safe. The editor owns it and uses it on the message thread.
class SpatialAnalyzer
{
public:
    SpatialAnalyzer();

    [[nodiscard]] static float positionFromMagnitudes(float leftMagnitude, float rightMagnitude, float focusDecibels);

    void clear();
    void prepare(double newSampleRate);
    void pushSamples(std::span<const float> left, std::span<const float> right);
    void setFocus(double decibels);
    void setFrameCallback(SpectralFrameCallback callback);

private:
    void analyzeTier(AnalysisTier& tier);
    void copyNewest(std::span<const float> ring, std::span<float> destination) const;

    std::vector<AnalysisTier> tiers;
    /// Rings of the newest samples of each channel, as long as the longest window.
    std::vector<float> leftHistory;
    std::vector<float> rightHistory;
    std::vector<SpectralPoint> points;
    SpectralFrameCallback frameCallback;

    double sampleRate {ANALYZER_DEFAULT_SAMPLE_RATE};
    float focusDecibels {static_cast<float>(DEFAULT_FOCUS_DB)};
    /// Position in the rings where the next sample goes.
    std::size_t writePosition {0};
};
}  // namespace esgrove::image_control
