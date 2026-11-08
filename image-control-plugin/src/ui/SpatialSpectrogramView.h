/// Declares SpatialSpectrogramView, the horizontal spatial spectrogram of the ImageControl editor.
/// Frequency runs left to right on a logarithmic axis
/// and stereo position from full left at the top to full right at the bottom.
/// Every analyzed bin becomes a particle that the SpectrogramRenderer draws on the GPU,
/// and the view paints the grid, the bands, and the crossovers over it.
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
/// Corner radius of the plot inside the spectrogram box, in logical pixels.
/// The view cuts it out of the box and the renderer rounds the picture to it.
constexpr float SPECTROGRAM_PLOT_CORNER_RADIUS {4.0F};

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

/// One analyzed bin, ready for the GPU.
/// The position is in shares of the plot, so it does not depend on the plot size or the display scale,
/// and the renderer turns it into physical pixels.
/// The layout is the instance layout of the renderer's particle shader.
struct SpectrogramParticle {
    /// Centre on the frequency axis, from 0 at the left edge to 1 at the right edge.
    float x {0.0F};
    /// Centre on the position axis, from 0 at the top (full left) to 1 at the bottom (full right).
    float y {0.0F};
    /// Horizontal spread that the distance to the neighbouring bins asks for, as a share of the plot width.
    /// The renderer never draws a particle narrower than its sharp core.
    float spread {0.0F};
    /// Colour multiplied by the particle's weight.
    float red {0.0F};
    float green {0.0F};
    float blue {0.0F};
};

/// What the renderer needs from the view since its last batch:
/// the new particles, how much time the old ones fade by, and whether to clear the picture first.
struct SpectrogramRenderBatch {
    /// Particles of the frames analyzed since the last batch.
    std::vector<SpectrogramParticle> particles;
    /// Time the existing particles fade by, in seconds.
    double fadeSeconds {0.0};
    /// Time constant of the fade, in seconds.
    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
    /// Whether to clear the picture before drawing the new particles.
    bool clear {false};
};

[[nodiscard]] double frequencyToShare(double frequency, FrequencyScale scale);
[[nodiscard]] double shareToFrequency(double share, FrequencyScale scale);
[[nodiscard]] double frequencyToMel(double frequency);
[[nodiscard]] double melToFrequency(double mel);

/// What the overlays show, compared on every display refresh, so the view repaints only when it changes.
struct SpectrogramOverlayState {
    std::array<float, BAND_COUNT> bandHighlights {};
    std::array<float, CROSSOVER_COUNT> crossoverHighlights {};
    Crossovers crossovers {};
    std::array<double, BAND_COUNT> bandWidths {};
    bool renderingBypassed {false};
    bool renderingUnavailable {false};

    [[nodiscard]] bool operator==(const SpectrogramOverlayState&) const = default;
};

/// The spatial spectrogram with the band regions and the crossover lines.
/// The editor feeds it analyzed frames, advances it once per display refresh,
/// and hands its render batches to the SpectrogramRenderer, which draws the particles under the view's overlays.
/// The plot inside the box is left unpainted, so the GPU picture shows through.
class SpatialSpectrogramView final : public juce::Component, public juce::TooltipClient
{
public:
    explicit SpatialSpectrogramView(juce::AudioProcessorValueTreeState& parameters);

    [[nodiscard]] juce::Rectangle<float> getPlotBounds() const;
    [[nodiscard]] SpectrogramRenderBatch takeRenderBatch();

    void addFrame(std::span<const SpectralPoint> points, double frameSeconds);
    void advance(double seconds);
    void clear();
    void setColours(SpectrogramColours newColours);
    void setFrameStatistics(std::optional<double> rate, std::optional<double> gpuMilliseconds);
    void setFrequencyScale(FrequencyScale newScale);
    void setPersistence(double seconds);
    void setRenderingBypassed(bool bypassed);
    void setRenderingUnavailable(bool unavailable);

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
    [[nodiscard]] SpectrogramOverlayState currentOverlayState() const;
    [[nodiscard]] juce::Rectangle<float> framesPerSecondArea() const;
    [[nodiscard]] LinearColour pointColour(const SpectralPoint& point) const;

    [[nodiscard]] std::optional<std::size_t> bandAt(float x) const;
    [[nodiscard]] std::optional<std::size_t> crossoverNear(float x) const;

    [[nodiscard]] double bandWidth(std::size_t band) const;
    [[nodiscard]] float frequencyToX(double frequency) const;
    [[nodiscard]] float positionToY(double position) const;
    [[nodiscard]] double xToFrequency(float x) const;

    void changeBandWidth(const juce::MouseEvent& event);
    void moveBand(const juce::MouseEvent& event);
    void moveCrossover(const juce::MouseEvent& event);
    void paintAxisLabels(juce::Graphics& graphics) const;
    void paintBandResponses(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintBands(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintCrossovers(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintFramesPerSecond(juce::Graphics& graphics) const;
    void paintGrid(juce::Graphics& graphics) const;
    void paintRenderingStatus(juce::Graphics& graphics) const;
    void updateCursor(const juce::ModifierKeys& modifiers);
    void updateHover(juce::Point<float> position);

    /// The particles and the fade since the renderer last took a batch.
    SpectrogramRenderBatch pendingBatch;
    /// Frequency colour of evenly spaced shares of the logarithmic frequency axis.
    std::vector<LinearColour> frequencyColours;
    /// What the overlays showed when the view last repainted.
    SpectrogramOverlayState shownOverlay;

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
    /// GPU time of one frame shown next to the refresh rate, in milliseconds, or empty when it is not measured.
    std::optional<double> gpuFrameMilliseconds;

    /// What the current drag changes.
    SpectrogramDrag drag {SpectrogramDrag::none};
    /// How new particles are coloured.
    SpectrogramColours colours {SpectrogramColours::frequency};
    /// How frequency maps onto the horizontal axis.
    FrequencyScale scale {FrequencyScale::logarithmic};
    /// Time constant of the particle fade, in seconds.
    double persistenceSeconds {DEFAULT_PERSISTENCE_SECONDS};
    /// Side gain in dB of the dragged band when its width drag started.
    float dragStartSide {0.0F};
    /// Whether the particles are not drawn, while the audio keeps being processed.
    bool renderingBypassed {false};
    /// Whether the GPU renderer could not start, such as without OpenGL 3.3.
    bool renderingUnavailable {false};
};
}  // namespace esgrove::image_control
