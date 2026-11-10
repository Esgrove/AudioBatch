/// Declares SpatialSpectrogramView, the horizontal spatial spectrogram of the ImageControl editor.
/// Frequency runs left to right on a logarithmic axis
/// and stereo position from full left at the top to full right at the bottom.
/// The SpectrogramRenderer draws the particles on the GPU,
/// and the view paints the grid, the bands, and the crossovers over them.
/// The crossovers are vertical lines that can be dragged, a band can be dragged sideways to move it,
/// and Left/Right moves the hovered boundary or band in frequency-relative steps, with Shift for fine adjustment,
/// a vertical drag or the mouse wheel over a band with Ctrl or Cmd changes its mid gain,
/// and with Alt or Option, or the plain mouse wheel, its side gain.
/// Bands that the solos mute are veiled.

#pragma once

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"
#include "ui/SpectrogramParticles.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

namespace esgrove::image_control
{
class ParameterHistory;

namespace test
{
struct SpectrogramViewTestAccess;
}

/// Cached response geometry and colours, independent of hover opacity and the band's processing controls.
struct SpectrogramBandResponses {
    /// Plot bounds the response paths were built for.
    juce::Rectangle<float> plot;
    /// Closed shapes under each band's response curve.
    std::array<juce::Path, BAND_COUNT> fills;
    /// Filled outlines of the response strokes, including their fixed thickness.
    std::array<juce::Path, BAND_COUNT> strokes;
    /// Frequency-derived band colours shared by the regions and response paths.
    std::array<juce::Colour, BAND_COUNT> colours;
    /// Ordered crossover frequencies used to derive the four responses.
    Crossovers crossovers {};
    /// Frequency projection used to sample the response curves.
    FrequencyScale scale {FrequencyScale::logarithmic};
};

/// Corner radius of the plot inside the spectrogram box, in logical pixels.
/// The view cuts it out of the box and the renderer rounds the picture to it.
constexpr float SPECTROGRAM_PLOT_CORNER_RADIUS {4.0F};

/// What a mouse drag on the spectrogram changes, chosen when the button goes down.
/// A drag on a crossover line moves that crossover, a drag on a band moves the band,
/// and a vertical drag on a band changes its mid gain with Ctrl or Cmd held, and its side gain with Alt or Option.
enum class SpectrogramDrag : std::uint8_t {
    none,
    crossover,
    band,
    midGain,
    sideGain,
};

/// What the overlays show, compared on every display refresh, so the view repaints only when it changes.
struct SpectrogramOverlayState {
    [[nodiscard]] bool operator==(const SpectrogramOverlayState& other) const;

    StereoPositionCurve positionCurve;
    std::array<float, BAND_COUNT> bandHighlights {};
    std::array<float, CROSSOVER_COUNT> crossoverHighlights {};
    Crossovers crossovers {};
    std::array<double, BAND_COUNT> bandWidths {};
    std::array<bool, BAND_COUNT> mutedBands {};
    float positionCurveOpacity {0.0F};
    bool renderingBypassed {false};
    bool renderingUnavailable {false};
};

/// The spatial spectrogram's box, axes, band regions, and crossover lines.
/// The editor advances it once per display refresh, which animates the hover highlights.
/// The plot inside the box is left unpainted, so the particles the GPU draws under the view show through.
class SpatialSpectrogramView final : public juce::Component, public juce::TooltipClient
{
public:
    SpatialSpectrogramView(
        juce::AudioProcessorValueTreeState& parameters,
        juce::ValueTree editorState,
        ParameterHistory& history
    );

    ~SpatialSpectrogramView() override;

    SpatialSpectrogramView(const SpatialSpectrogramView&) = delete;
    SpatialSpectrogramView(SpatialSpectrogramView&&) = delete;
    SpatialSpectrogramView& operator=(const SpatialSpectrogramView&) = delete;
    SpatialSpectrogramView& operator=(SpatialSpectrogramView&&) = delete;

    [[nodiscard]] juce::Rectangle<float> getPlotBounds() const;
    [[nodiscard]] juce::Rectangle<float> crossoverLabelBounds(std::size_t crossover) const;
    [[nodiscard]] std::optional<std::size_t> getHoveredBand() const;
    [[nodiscard]] float positionCurveOpacity() const;

    void advance(double seconds);
    void endDrag();
    void setFrameStatistics(std::optional<double> rate, std::optional<double> gpuMilliseconds);
    void setFrequencyScale(FrequencyScale newScale);
    void setPositionCurve(const StereoPositionCurve& curve);
    void setRenderingBypassed(bool bypassed);
    void setRenderingUnavailable(bool unavailable);

    bool keyPressed(const juce::KeyPress& key) override;
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
    friend struct test::SpectrogramViewTestAccess;

    [[nodiscard]] const SpectrogramBandResponses& bandResponses(const Crossovers& crossovers);
    [[nodiscard]] Crossovers currentCrossovers() const;
    [[nodiscard]] SpectrogramOverlayState currentOverlayState() const;
    [[nodiscard]] juce::Rectangle<float> framesPerSecondArea() const;
    [[nodiscard]] juce::Rectangle<float> positionCurveLabelBounds(float difference, bool leftChannel) const;
    [[nodiscard]] juce::Point<float> positionCurvePoint(float difference, bool leftChannel) const;
    [[nodiscard]] std::array<bool, BAND_COUNT> mutedBands() const;

    [[nodiscard]] std::optional<std::size_t> bandAt(float x) const;
    [[nodiscard]] std::optional<std::size_t> crossoverNear(float x) const;

    [[nodiscard]] double bandWidth(std::size_t band) const;
    [[nodiscard]] float frequencyToX(double frequency) const;
    [[nodiscard]] float positionToY(double position) const;
    [[nodiscard]] double xToFrequency(float x) const;
    [[nodiscard]] bool isLinked(std::size_t band) const;

    void beginHistoryEdit(const juce::String& name);
    void endHistoryEdit();
    void setLocalValue(juce::RangedAudioParameter& parameter, const juce::String& parameterId, float value);

    void applyGainChange(std::size_t band, bool mid, float change);
    void applyKeyboardCrossovers(const Crossovers& before, const Crossovers& after, const juce::String& name);
    void changeBandGain(const juce::MouseEvent& event);
    void moveBand(const juce::MouseEvent& event);
    void moveCrossover(const juce::MouseEvent& event);
    void paintAxisLabels(juce::Graphics& graphics) const;
    void paintBandResponses(juce::Graphics& graphics, const Crossovers& crossovers);
    void paintBands(juce::Graphics& graphics, const Crossovers& crossovers);
    void paintCrossovers(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintFramesPerSecond(juce::Graphics& graphics) const;
    void paintGrid(juce::Graphics& graphics) const;
    void paintPositionCurve(juce::Graphics& graphics) const;
    void paintRenderingStatus(juce::Graphics& graphics) const;
    void updateCursor(const juce::ModifierKeys& modifiers);
    void updateHover(juce::Point<float> position);

    /// Geometry cached by plot bounds, scale, and ordered crossovers, excluding animated opacity.
    std::optional<SpectrogramBandResponses> cachedBandResponses;
    /// Number of cache rebuilds, observable by CPU-only regression tests through the test access friend.
    std::uint64_t bandResponseBuilds {0};
    /// What the overlays showed when the view last repainted.
    SpectrogramOverlayState shownOverlay;
    /// The stereo position curve the particles are placed with, which the curve overlay draws.
    StereoPositionCurve positionCurve;
    /// The editor settings, which hold the Link toggle of each band.
    juce::ValueTree editorSettings;

    /// Processor-owned local history, which outlives this view.
    ParameterHistory& history;
    /// Generation of the host state when the explicit local gesture started.
    std::uint64_t historyGeneration {0};
    /// Whether this view owns a local history frame, independent of its parameter host gestures.
    bool historyEditing {false};

    /// Crossover parameters, owned by the parameter tree, which outlives this view.
    std::array<std::reference_wrapper<juce::RangedAudioParameter>, CROSSOVER_COUNT> crossoverParameters;
    /// Mid gain parameters, owned by the parameter tree, which outlives this view.
    std::array<std::reference_wrapper<juce::RangedAudioParameter>, BAND_COUNT> midParameters;
    /// Side gain parameters, owned by the parameter tree, which outlives this view.
    std::array<std::reference_wrapper<juce::RangedAudioParameter>, BAND_COUNT> sideParameters;
    /// Independent Width parameters, owned by the parameter tree, which outlives this view.
    std::array<std::reference_wrapper<juce::RangedAudioParameter>, BAND_COUNT> widthParameters;
    /// Solo parameters, owned by the parameter tree, which outlives this view.
    std::array<std::reference_wrapper<juce::RangedAudioParameter>, BAND_COUNT> soloParameters;
    /// Animated hover highlight of each band, from 0 to 1.
    std::array<float, BAND_COUNT> bandHighlights {};
    /// Animated hover highlight of each crossover, from 0 to 1.
    std::array<float, CROSSOVER_COUNT> crossoverHighlights {};

    /// Crossovers when the current drag started.
    Crossovers dragStartCrossovers {};

    /// Band being moved or having its gains changed, if any.
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

    /// Seconds since the position curve last changed, which time the curve overlay.
    /// Infinite until the curve changes after it was first set, so opening the editor does not show the overlay.
    double positionCurveAge {std::numeric_limits<double>::infinity()};
    /// Whether the position curve was set at least once.
    bool positionCurveSet {false};

    /// What the current drag changes.
    SpectrogramDrag drag {SpectrogramDrag::none};
    /// How frequency maps onto the horizontal axis.
    FrequencyScale scale {FrequencyScale::logarithmic};
    /// Mid gain in dB of the dragged band when its gain drag started.
    float dragStartMid {0.0F};
    /// Side gain in dB of the dragged band when its gain drag started.
    float dragStartSide {0.0F};
    /// Smooth mouse wheel movement not yet turned into a whole step.
    float wheelRemainder {0.0F};
    /// Whether the current mid gain drag moves Side with Mid, as the band's Link asks.
    bool dragLinked {false};
    /// Whether the particles are not drawn, while the audio keeps being processed.
    bool renderingBypassed {false};
    /// Whether the GPU renderer could not start, such as without OpenGL 3.3.
    bool renderingUnavailable {false};
};
}  // namespace esgrove::image_control
