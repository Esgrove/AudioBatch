/// Declares ImageControlEditor, the editor of the ImageControl plugin.
/// A header holds the bypass, the settings button, and the output gain.
/// The spatial spectrogram fills the middle, with the four band strips below it.

#pragma once

#include "analysis/SpatialAnalyzer.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SpatialSpectrogramView.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace esgrove::image_control
{
class ImageControlProcessor;

/// The ImageControl editor.
/// A VBlankAttachment drains the processor's analyzer ring into the analyzer on every display refresh,
/// and advances the spectrogram by the time that passed,
/// so the particles fade smoothly at the native refresh rate of the display, such as 120 or 144 Hz.
/// The display settings live in a panel that the Settings button opens,
/// and the editor applies them from the editor settings on every refresh.
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
    void applyViewSettings();
    void drainAnalyzerFifo(bool analyze);
    void measureFramesPerSecond(double elapsed);
    void openSettings();
    void refreshDisplay(double timestamp);
    void updateBandRanges();

    ImageControlProcessor& imageControlProcessor;

    ImageControlLookAndFeel lookAndFeel;
    SpatialAnalyzer analyzer;
    SpatialSpectrogramView spectrogram;

    juce::Label titleLabel;
    juce::TextButton bypassButton {"Bypass"};
    juce::TextButton settingsButton {"Settings"};
    juce::Label outputLabel;
    ControlSlider outputSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;
    std::array<std::unique_ptr<BandStrip>, BAND_COUNT> bandStrips;

    /// Scratch buffers the analyzer ring is drained into.
    std::vector<float> leftScratch;
    std::vector<float> rightScratch;

    /// Crossovers the band ranges were last shown with.
    Crossovers displayedCrossovers {};

    /// Bypass state the band strips were last dimmed for, empty before the first update.
    std::optional<bool> shownBypassed;
    /// Colour mode the spectrogram was last set to, empty before the first update.
    std::optional<bool> shownPhaseColours;
    /// Analyzer tap the spectrogram last showed, empty before the first update.
    std::optional<bool> shownTapOutput;

    /// Sample rate the analyzer was last prepared for.
    double analyzedSampleRate {0.0};
    /// Time of the previous display refresh, in seconds, or 0 before the first.
    double lastRefreshTimestamp {0.0};
    /// Time since the frame rate was last measured, in seconds.
    double frameRateClock {0.0};
    /// Display refreshes since the frame rate was last measured.
    int frameRateCount {0};
    /// The most recently measured display refresh rate.
    double measuredFramesPerSecond {0.0};

    /// Calls refreshDisplay on every display refresh while the editor is on screen.
    /// Declared last, so it is created after and destroyed before everything its callback uses.
    juce::VBlankAttachment vblankAttachment;
};
}  // namespace esgrove::image_control
