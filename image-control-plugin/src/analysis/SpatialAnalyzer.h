/// Declares SpatialAnalyzer, which builds the spatial spectrogram of ImageControl's display.
/// It runs a short-time Fourier transform on the left and right channels,
/// finds the stereo position, energy, and phase coherence of every frequency bin,
/// and accumulates them into a decaying image of frequency columns by stereo position rows.

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cstdint>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// FFT order of the analysis, 4096 samples, about 12 Hz per bin at 48 kHz.
constexpr int ANALYSIS_FFT_ORDER {12};
/// Samples between analysis frames, a quarter of the FFT size.
constexpr int ANALYSIS_HOP {1024};
/// Number of logarithmic frequency columns of the spectrogram, from MIN_FREQUENCY to MAX_FREQUENCY.
constexpr int SPECTROGRAM_COLUMNS {1024};
/// Number of stereo position rows of the spectrogram, from full left at the top to full right at the bottom.
constexpr int SPECTROGRAM_ROWS {256};
/// Shortest time constant of the decay of the spectrogram, in seconds.
constexpr double MIN_PERSISTENCE_SECONDS {0.1};
/// Longest time constant of the decay of the spectrogram, in seconds.
constexpr double MAX_PERSISTENCE_SECONDS {5.0};
/// Default time constant of the decay of the spectrogram, in seconds.
constexpr double DEFAULT_PERSISTENCE_SECONDS {1.0};
/// Sample rate the analyzer assumes until it is prepared for another.
constexpr double ANALYZER_DEFAULT_SAMPLE_RATE {48'000.0};

/// Accumulates a spatial spectrogram from stereo audio.
///
/// Each bin's stereo position is p = (4 / π) · atan2(|L|, |R|) − 1:
/// +1 for content only in the left channel, 0 for equal levels, and −1 for content only in the right.
/// Its coherence is Re(L · conj(R)) / (|L| · |R|),
/// +1 for in-phase content and −1 for anti-phase content, which sits at the centre by level alone.
/// Energies are normalized so a full-scale sine reads 1 once the image has settled.
///
/// Not thread safe. The editor owns it and uses it on the message thread.
class SpatialAnalyzer
{
public:
    SpatialAnalyzer();

    [[nodiscard]] static double columnToFrequency(double column);
    [[nodiscard]] static double frequencyToColumn(double frequency);
    [[nodiscard]] static double positionToRow(double position);
    [[nodiscard]] static double rowToPosition(double row);

    [[nodiscard]] std::span<const float> getCoherentEnergy() const;
    [[nodiscard]] std::span<const float> getEnergy() const;

    [[nodiscard]] std::uint64_t getFrameCount() const;

    void clear();
    void prepare(double newSampleRate);
    void pushSamples(std::span<const float> left, std::span<const float> right);
    void setPersistence(double seconds);

private:
    void accumulateBin(int bin, float binEnergy, float position, float coherence);
    void analyzeFrame();

    juce::dsp::FFT fft;

    /// Accumulated energy per cell, column by column within each row.
    std::vector<float> energy;
    /// Accumulated energy times coherence per cell, so dividing by energy gives the mean coherence.
    std::vector<float> coherentEnergy;
    /// Share of a bin's energy each column receives, so wide columns average their bins instead of summing them.
    std::vector<float> columnBinShare;
    /// The newest FFT size samples of each channel.
    std::vector<float> leftFrame;
    std::vector<float> rightFrame;
    std::vector<float> leftSpectrum;
    std::vector<float> rightSpectrum;
    std::vector<float> window;

    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
    double sampleRate {ANALYZER_DEFAULT_SAMPLE_RATE};
    float decay {0.0F};
    /// Number of frames analyzed, and of clears, so the view knows when the image has changed.
    std::uint64_t frameCount {0};
    float energyScale {1.0F};
    int framePosition {0};
};
}  // namespace esgrove::image_control
