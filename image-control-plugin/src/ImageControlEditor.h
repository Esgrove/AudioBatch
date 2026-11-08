/// Declares ImageControlEditor, the editor of the ImageControl plugin.
/// A header holds the bypass, the settings button, and the output gain.
/// The spatial spectrogram fills the middle, with the four band strips below it,
/// and a divider between them resizes the two.

#pragma once

#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"
#include "ui/SplitDivider.h"
#include "ui/TooltipOverlay.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <array>
#include <memory>
#include <optional>

namespace esgrove::image_control
{
class ImageControlProcessor;

/// The ImageControl editor.
/// A VBlankAttachment wakes the analysis worker on every display refresh,
/// which drains the processor's analyzer ring and builds the particles on its own thread,
/// advances the spectrogram's overlays and the fade by the time that passed, and asks the OpenGL context for a frame,
/// so the particles fade smoothly at the native refresh rate of the display, such as 120 or 144 Hz.
/// The context is attached to the whole editor: the renderer draws the particles,
/// and JUCE paints the components over them, repainting only the regions that changed.
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
    [[nodiscard]] int compactHeight() const;
    [[nodiscard]] double persistenceSeconds() const;
    [[nodiscard]] SpectrogramRenderMode renderMode() const;

    void applySpectrogramHidden(bool hidden);
    void applyViewSettings();
    void measureFramesPerSecond(double elapsed);
    void openSettings();
    void refreshDisplay(double timestamp);
    void updateBandRanges();

    /// The processor that owns this editor and outlives it.
    ImageControlProcessor& imageControlProcessor;

    /// The look and feel of every control, set on the editor and cleared before it is destroyed.
    ImageControlLookAndFeel lookAndFeel;
    /// The spatial spectrogram's box, axes, and overlays.
    SpatialSpectrogramView spectrogram;
    /// The OpenGL context attached to the editor, detached when the editor goes away.
    juce::OpenGLContext openGLContext;
    /// Draws the spectrogram's particles in the OpenGL context.
    SpectrogramRenderer spectrogramRenderer {openGLContext};
    /// Analyzes the audio the processor queues and builds the particles on its own thread.
    /// Stopped first when the editor goes away.
    SpectrogramWorker spectrogramWorker;
    /// The draggable divider between the spectrogram and the band strips.
    SplitDivider divider;
    /// Height of the band strip section, in pixels, which the divider changes.
    int bandSectionHeight {0};

    /// The plugin name in the header.
    juce::Label titleLabel;
    /// Toggles the bypass parameter.
    juce::TextButton bypassButton {"Bypass"};
    /// Opens the display settings.
    juce::TextButton settingsButton {"Settings"};
    /// Label of the output gain.
    juce::Label outputLabel;
    /// The output gain.
    ControlSlider outputSlider;

    /// Binds the bypass button to its parameter.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment;
    /// Binds the output gain slider to its parameter.
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttachment;
    /// The controls of each band.
    std::array<std::unique_ptr<BandStrip>, BAND_COUNT> bandStrips;

    /// Crossovers the band ranges were last shown with.
    Crossovers displayedCrossovers {};

    /// Bypass state the band strips were last dimmed for, empty before the first update.
    std::optional<bool> shownBypassed;
    /// Colour mode the spectrogram was last set to, empty before the first update.
    std::optional<bool> shownPhaseColours;
    /// Analyzer tap the spectrogram last showed, empty before the first update.
    std::optional<bool> shownTapOutput;
    /// Frequency scale the spectrogram was last set to, empty before the first update.
    std::optional<FrequencyScale> shownScale;
    /// Rendering bypass the spectrogram was last set to, empty before the first update.
    std::optional<bool> shownRenderingBypassed;
    /// Whether the spectrogram was last hidden, empty before the first update.
    std::optional<bool> shownHidden;

    /// Whether the spectrogram is hidden, which shrinks the editor to the header and the band strips.
    bool spectrogramHidden {false};
    /// Whether the particles are not analyzed or drawn, while the audio is still processed.
    bool renderingBypassed {false};

    /// Time of the previous display refresh, in seconds, or 0 before the first.
    double lastRefreshTimestamp {0.0};
    /// Time since the frame rate was last measured, in seconds.
    double frameRateClock {0.0};
    /// The most recently measured rate of drawn frames.
    double measuredFramesPerSecond {0.0};
    /// The renderer's frame count when the frame rate was last measured.
    int measuredFrameCount {0};

    /// The tooltip bubble, kept above the other components.
    TooltipOverlay tooltips;

    /// Calls refreshDisplay on every display refresh while the editor is on screen.
    /// Declared last, so it is created after and destroyed before everything its callback uses.
    juce::VBlankAttachment vblankAttachment;
};
}  // namespace esgrove::image_control
