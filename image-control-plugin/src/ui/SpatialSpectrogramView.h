/// Declares SpatialSpectrogramView, the horizontal spatial spectrogram of the ImageControl editor.
/// Frequency runs left to right on a logarithmic axis
/// and stereo position from full left at the top to full right at the bottom.
/// Every analyzed bin is drawn as a soft particle into a colour buffer that fades and blurs over time.
/// The crossovers are vertical lines that can be dragged, and the mouse wheel over a band changes its width.

#pragma once

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace esgrove::image_control
{
/// Number of frequency columns of the particle buffer, which the view scales smoothly to its size.
constexpr int SPECTROGRAM_COLUMNS {768};
/// Number of stereo position rows of the particle buffer.
constexpr int SPECTROGRAM_ROWS {192};

/// How the particles are coloured.
/// Frequency colours run from red in the bass to blue in the highs,
/// and phase colours from blue for in-phase content to red for anti-phase content.
enum class SpectrogramColours : std::uint8_t {
    frequency,
    phase,
};

/// How frequency maps onto the horizontal axis.
/// The logarithmic scale gives every octave the same width,
/// and the mel scale, as in AudioBatch's spectrogram, gives the bass less room and the high frequencies more.
enum class FrequencyScale : std::uint8_t {
    logarithmic,
    mel,
};

/// A colour with float channels from 0 to 1, for the additive particle buffer.
struct LinearColour {
    float red {0.0F};
    float green {0.0F};
    float blue {0.0F};
};

[[nodiscard]] double frequencyToShare(double frequency, FrequencyScale scale);
[[nodiscard]] double shareToFrequency(double share, FrequencyScale scale);
[[nodiscard]] double frequencyToMel(double frequency);
[[nodiscard]] double melToFrequency(double mel);

/// The spatial spectrogram with the band regions and the crossover lines.
/// The editor feeds it analyzed frames and advances it once per display refresh.
class SpatialSpectrogramView final : public juce::Component
{
public:
    explicit SpatialSpectrogramView(juce::AudioProcessorValueTreeState& parameters);

    [[nodiscard]] juce::Rectangle<float> getPlotBounds() const;

    void addFrame(std::span<const SpectralPoint> points, double frameSeconds);
    void advance(double seconds);
    void clear();
    void setColours(SpectrogramColours newColours);
    void setFramesPerSecond(std::optional<double> rate);
    void setFrequencyScale(FrequencyScale newScale);
    void setPersistence(double seconds);

    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;
    void paint(juce::Graphics& graphics) override;

private:
    [[nodiscard]] Crossovers currentCrossovers() const;
    [[nodiscard]] LinearColour pointColour(const SpectralPoint& point) const;

    [[nodiscard]] std::optional<std::size_t> bandAt(float x) const;
    [[nodiscard]] std::optional<std::size_t> crossoverNear(float x) const;

    [[nodiscard]] double bandWidth(std::size_t band) const;
    [[nodiscard]] float frequencyToX(double frequency) const;
    [[nodiscard]] float positionToY(double position) const;
    [[nodiscard]] double xToFrequency(float x) const;

    void blurBuffer();
    void paintAxisLabels(juce::Graphics& graphics) const;
    void paintBands(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintFramesPerSecond(juce::Graphics& graphics) const;
    void paintGrid(juce::Graphics& graphics) const;
    void renderImage();
    void updateHover(juce::Point<float> position);

    /// The rendered particles, scaled into the plot when painting.
    juce::Image image;

    /// The additive particle buffer, one plane per colour channel, row by row.
    std::array<std::vector<float>, 3> buffer;
    /// Scratch plane for the blur.
    std::vector<float> blurScratch;
    /// Frequency colour of every column.
    std::vector<LinearColour> columnColours;
    /// Display value for every step of exposed buffer energy, which saturates smoothly towards white.
    std::vector<juce::uint8> toneCurve;

    /// Crossover parameters, owned by the parameter tree, which outlives the editor. Never null.
    std::array<juce::RangedAudioParameter*, CROSSOVER_COUNT> crossoverParameters {};
    /// Mid gain parameters, owned by the parameter tree. Never null.
    std::array<juce::RangedAudioParameter*, BAND_COUNT> midParameters {};
    /// Side gain parameters, owned by the parameter tree. Never null.
    std::array<juce::RangedAudioParameter*, BAND_COUNT> sideParameters {};
    /// Animated hover highlight of each band, from 0 to 1.
    std::array<float, BAND_COUNT> bandHighlights {};
    /// Animated hover highlight of each crossover, from 0 to 1.
    std::array<float, CROSSOVER_COUNT> crossoverHighlights {};

    /// Crossover being dragged, if any.
    std::optional<std::size_t> draggedCrossover;
    /// Band under the pointer, if any.
    std::optional<std::size_t> hoveredBand;
    /// Crossover under the pointer, if any.
    std::optional<std::size_t> hoveredCrossover;
    /// Display refresh rate shown in the top-left corner, or empty when the readout is off.
    std::optional<double> framesPerSecond;

    /// How new particles are coloured.
    SpectrogramColours colours {SpectrogramColours::frequency};
    /// How frequency maps onto the horizontal axis.
    FrequencyScale scale {FrequencyScale::logarithmic};
    /// Time constant of the particle fade, in seconds.
    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
    /// Time since the last blur step, in seconds.
    double blurClock {0.0};
};
}  // namespace esgrove::image_control
