/// Declares ImageControlEditor, the editor of the ImageControl plugin.
/// A header holds the settings button and the analyzer tap on the left,
/// and mono, delta, bypass, and output gain on the right.
/// A second row holds local undo/redo, A/B comparisons, and user presets.
/// The spatial spectrogram fills the middle, with the correlation row and four band strips below it,
/// and a divider between them resizes the two.

#pragma once

#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/HistoryAttachments.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/ProcessingToolbar.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"
#include "ui/SplitDivider.h"
#include "ui/TooltipOverlay.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace esgrove::image_control
{
class ImageControlProcessor;
namespace test
{
struct ImageControlEditorTestAccess;
}

/// The ImageControl editor.
/// A VBlankAttachment wakes the analysis worker on every display refresh,
/// which drains the processor's analyzer ring and builds the particles on its own thread,
/// advances the spectrogram's overlays and the fade by the time that passed, and asks the OpenGL context for a frame,
/// so the particles fade smoothly at the native refresh rate of the display, such as 120 or 144 Hz.
/// The context is attached to the whole editor: the renderer draws the particles,
/// and JUCE paints the components over them, repainting only the regions that changed.
/// The display settings live in a panel that the Settings button opens,
/// and the editor applies them from the editor settings on every refresh.
class ImageControlEditor final : public juce::AudioProcessorEditor, private juce::ComponentMovementWatcher
{
public:
    explicit ImageControlEditor(ImageControlProcessor& owner);

    ~ImageControlEditor() override;

    ImageControlEditor(const ImageControlEditor&) = delete;
    ImageControlEditor(ImageControlEditor&&) = delete;
    ImageControlEditor& operator=(const ImageControlEditor&) = delete;
    ImageControlEditor& operator=(ImageControlEditor&&) = delete;

    bool keyPressed(const juce::KeyPress& key) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    friend struct test::ImageControlEditorTestAccess;

    [[nodiscard]] SpectrogramRenderMode renderMode() const;
    [[nodiscard]] std::optional<std::size_t> bandUnderPointer() const;
    [[nodiscard]] int compactHeight() const;
    [[nodiscard]] double persistenceSeconds() const;

    void applySpectrogramHidden(bool hidden);
    void applyViewSettings();
    void measureFramesPerSecond(double elapsed);
    void openSettings();
    void refreshDisplay(double timestamp);
    void setAnalyzeOutput(bool output);
    void updateBandRanges();
    void updateCorrelationMonitoring();
    void updateSpectrogramMonitoring();

    void componentMovedOrResized(bool wasMoved, bool wasResized) override;
    void componentPeerChanged() override;
    void componentVisibilityChanged() override;

    /// The processor that owns this editor and outlives it.
    ImageControlProcessor& imageControlProcessor;

    /// The look and feel of every control, set on the editor and cleared before it is destroyed.
    ImageControlLookAndFeel lookAndFeel;
    /// Local editing commands and the plugin-owned preset library.
    ProcessingToolbar processingToolbar;
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
    /// Toggles the delta parameter, which outputs only what the processing changes.
    juce::TextButton deltaButton {"Delta"};
    /// Toggles the mono parameter, which folds the final processed output equally into both channels.
    juce::TextButton monoButton {"Mono"};
    /// Opens the display settings.
    juce::TextButton settingsButton {"Settings"};
    /// Shows the input of the plugin in the spectrogram, the left half of the analyzer tap switch.
    juce::TextButton preButton {"Pre"};
    /// Shows the output of the plugin in the spectrogram, the right half of the analyzer tap switch.
    juce::TextButton postButton {"Post"};
    /// Label of the output gain.
    juce::Label outputLabel;
    /// The output gain.
    ControlSlider outputSlider;

    /// Binds the bypass button to its parameter.
    std::unique_ptr<HistoryButtonAttachment> bypassAttachment;
    /// Binds the delta button to its parameter.
    std::unique_ptr<HistoryButtonAttachment> deltaAttachment;
    /// Binds the mono button to its parameter.
    std::unique_ptr<HistoryButtonAttachment> monoAttachment;
    /// Binds the output gain slider to its parameter.
    std::unique_ptr<HistorySliderAttachment> outputAttachment;
    /// The controls of each band.
    std::array<std::unique_ptr<BandStrip>, BAND_COUNT> bandStrips;
    /// Independent read-only meter row aligned with the four band strips.
    std::array<std::unique_ptr<BandCorrelationMeter>, BAND_COUNT> correlationMeters;

    /// Latest worker settings, reused when visibility changes without a display refresh.
    SpectrogramAnalysisSettings analysisSettings;
    /// Raw pan settings the cached worker position curve was derived from.
    std::optional<std::array<double, 2>> shownPositionSettings;
    /// Processor lifecycle last accepted by the correlation display history.
    std::optional<std::uint32_t> shownCorrelationGeneration;

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

    /// Whether the saved layout has been restored, so layout changes are saved from now on.
    /// Until then, sizes JUCE applies while the editor is built, such as the minimum size, are not saved.
    bool layoutRestored {false};
    /// Suppresses intermediate layout writes while changing resize limits between compact and expanded layouts.
    bool layoutTransition {false};
    /// Whether the correlation row is shown, independently of the spectrogram settings.
    bool correlationShown {true};
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
