/// Declares SpatialSpectrogramView, the horizontal spatial spectrogram of the ImageControl editor.
/// Frequency runs left to right on a logarithmic axis
/// and stereo position from full left at the top to full right at the bottom.
/// Brightness shows energy and hue shows phase coherence.
/// The crossovers are vertical lines that can be dragged to move them.

#pragma once

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <optional>
#include <vector>

namespace esgrove::image_control
{
/// Number of energy levels in the colour palette of the spectrogram.
constexpr int PALETTE_SIZE {256};
/// Number of coherence steps in the colour palette, from in phase to anti-phase.
constexpr int COHERENCE_STEPS {16};

/// The spatial spectrogram with the crossover lines.
/// Owned by the editor, which calls refresh after feeding the analyzer.
class SpatialSpectrogramView final : public juce::Component
{
public:
    SpatialSpectrogramView(const SpatialAnalyzer& spectrumAnalyzer, juce::AudioProcessorValueTreeState& parameters);

    [[nodiscard]] juce::Rectangle<float> getPlotBounds() const;

    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void paint(juce::Graphics& graphics) override;
    void refresh();

private:
    [[nodiscard]] std::optional<std::size_t> crossoverNear(float x) const;

    [[nodiscard]] Crossovers currentCrossovers() const;

    [[nodiscard]] float frequencyToX(double frequency) const;
    [[nodiscard]] float positionToY(double position) const;
    [[nodiscard]] double xToFrequency(float x) const;

    void paintBands(juce::Graphics& graphics, const Crossovers& crossovers) const;
    void paintGrid(juce::Graphics& graphics) const;
    void renderImage();

    juce::Image image;

    /// The analyzer the editor feeds, which outlives this view.
    const SpatialAnalyzer& analyzer;

    /// Colours by energy level, PALETTE_SIZE per coherence step, from in phase to anti-phase.
    std::vector<juce::Colour> palette;
    /// Crossover parameters, owned by the parameter tree, which outlives the editor.
    std::array<juce::RangedAudioParameter*, CROSSOVER_COUNT> crossoverParameters {};
    /// Crossover being dragged, if any.
    std::optional<std::size_t> draggedCrossover;
};
}  // namespace esgrove::image_control
