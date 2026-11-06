/// Declares ImageControlEditor, the editor of the ImageControl plugin.
/// The spatial spectrogram fills the top, with the four band strips below it
/// and a header row holding the analyzer tap, the persistence, and the output gain.

#pragma once

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/SpatialSpectrogramView.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace esgrove::image_control
{
class ImageControlProcessor;

/// The ImageControl editor.
/// A VBlankAttachment drains the processor's analyzer ring into the analyzer on every display refresh,
/// so the editor follows the native refresh rate of the display, such as 120 or 144 Hz.
/// The spectrogram image is only rendered again when the analyzer has new frames,
/// and the crossover lines and band ranges only repaint when the crossovers move.
class ImageControlEditor final : public juce::AudioProcessorEditor
{
public:
    explicit ImageControlEditor(ImageControlProcessor& owner);

    ~ImageControlEditor() override;

    ImageControlEditor(const ImageControlEditor&) = delete;
    ImageControlEditor(ImageControlEditor&&) = delete;
    ImageControlEditor& operator=(const ImageControlEditor&) = delete;
    ImageControlEditor& operator=(ImageControlEditor&&) = delete;

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void drainAnalyzerFifo(bool analyze);
    void refreshDisplay();
    void updateBandRanges();

    ImageControlProcessor& imageControlProcessor;

    juce::LookAndFeel_V4 lookAndFeel;
    SpatialAnalyzer analyzer;
    SpatialSpectrogramView spectrogram;

    juce::Label titleLabel;
    juce::TextButton tapButton;
    juce::Label persistenceLabel;
    juce::Slider persistenceSlider;
    juce::Label outputLabel;
    juce::Slider outputSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;
    std::array<std::unique_ptr<BandStrip>, BAND_COUNT> bandStrips;

    /// Scratch buffers the analyzer ring is drained into.
    std::vector<float> leftScratch;
    std::vector<float> rightScratch;

    /// Crossovers the spectrogram lines and band ranges were last drawn with.
    Crossovers displayedCrossovers {};

    /// Analyzer frame count the spectrogram image was last rendered at.
    std::uint64_t renderedFrameCount {0};
    /// Sample rate the analyzer was last prepared for.
    double analyzedSampleRate {0.0};

    /// Calls refreshDisplay on every display refresh while the editor is on screen.
    /// Declared last, so it is created after and destroyed before everything its callback uses.
    juce::VBlankAttachment vblankAttachment;
};
}  // namespace esgrove::image_control
