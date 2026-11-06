/// Declares SpatialSpectrogramView, the horizontal spatial spectrogram of the ImageControl editor.
/// Frequency runs left to right on a logarithmic axis
/// and stereo position from full left at the top to full right at the bottom.
/// Every analyzed bin is drawn as a soft particle into a colour buffer that fades and blurs over time.
/// The crossovers are vertical lines that can be dragged, a band can be dragged sideways to move it,
/// and a vertical drag with Ctrl or Cmd, or the mouse wheel, over a band changes its width.

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

/// What a mouse drag on the spectrogram changes, chosen when the button goes down.
/// A drag on a crossover line moves that crossover, a drag on a band moves the band,
/// and a drag on a band with Ctrl or Cmd held changes the band's width.
enum class SpectrogramDrag : std::uint8_t {
    none,
    crossover,
    band,
    width,
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
class SpatialSpectrogramView final : public juce::Component, public juce::TooltipClient
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
    void modifierKeysChanged(const juce::ModifierKeys& modifiers) override;
    void paint(juce::Graphics& graphics) override;

    [[nodiscard]] juce::String getTooltip() override;

private:
    [[nodiscard]] static std::vector<std::size_t> bandEdges(std::size_t band);
    [[nodiscard]] static bool isWidthModifier(const juce::ModifierKeys& modifiers);

    [[nodiscard]] Crossovers currentCrossovers() const;
    [[nodiscard]] LinearColour pointColour(const SpectralPoint& point) const;

    [[nodiscard]] std::optional<std::size_t> bandAt(float x) const;
    [[nodiscard]] std::optional<std::size_t> crossoverNear(float x) const;

    [[nodiscard]] double bandWidth(std::size_t band) const;
    [[nodiscard]] float frequencyToX(double frequency) const;
    [[nodiscard]] float positionToY(double position) const;
    [[nodiscard]] double xToFrequency(float x) const;

    void blurBuffer();
    void changeBandWidth(const juce::MouseEvent& event);
    void moveBand(const juce::MouseEvent& event);
    void moveCrossover(const juce::MouseEvent& event);
    void paintAxisLabels(juce::Graphics& graphics) const;
    void paintBandResponses(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintBands(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintCrossovers(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintFramesPerSecond(juce::Graphics& graphics) const;
    void paintGrid(juce::Graphics& graphics) const;
    void renderImage();
    void updateCursor(const juce::ModifierKeys& modifiers);
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

    /// Crossovers when the current drag started.
    Crossovers dragStartCrossovers {};

    /// Band being moved or having its width changed, if any.
    std::optional<std::size_t> draggedBand;
    /// Crossover being dragged, if any.
    std::optional<std::size_t> draggedCrossover;
    /// Band under the pointer, if any.
    std::optional<std::size_t> hoveredBand;
    /// Crossover under the pointer, if any.
    std::optional<std::size_t> hoveredCrossover;
    /// Display refresh rate shown in the top-left corner, or empty when the readout is off.
    std::optional<double> framesPerSecond;

    /// How new particles are coloured.
    /// What the current drag changes.
    SpectrogramDrag drag {SpectrogramDrag::none};
    SpectrogramColours colours {SpectrogramColours::frequency};
    /// How frequency maps onto the horizontal axis.
    FrequencyScale scale {FrequencyScale::logarithmic};
    /// Time constant of the particle fade, in seconds.
    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
    /// Time since the last blur step, in seconds.
    double blurClock {0.0};
    /// Side gain in dB of the dragged band when its width drag started.
    float dragStartSide {0.0F};
};
}  // namespace esgrove::image_control
