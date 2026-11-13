/// Declares SettingsPanel, the bounded, scrollable display preferences panel.
/// Controls bind only to editor settings and never change sound parameters or presets.

#pragma once

#include "ui/ControlSlider.h"
#include "ui/KeyboardShortcutsPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace esgrove::image_control
{
/// Groups appearance, stereo display mapping, correlation, and display behavior without sound parameters.
/// The caller owns the message-thread editor state for the lifetime of the panel.
class SettingsPanel final : public juce::Component, private juce::FocusChangeListener, private juce::ValueTree::Listener
{
public:
    explicit SettingsPanel(juce::ValueTree& editorState, std::optional<int> maximumHeight = std::nullopt);

    ~SettingsPanel() override;

    SettingsPanel(const SettingsPanel&) = delete;
    SettingsPanel(SettingsPanel&&) = delete;
    SettingsPanel& operator=(const SettingsPanel&) = delete;
    SettingsPanel& operator=(SettingsPanel&&) = delete;

    [[nodiscard]] int getNaturalHeight() const;

    void resetDisplayAppearance();
    void showKeyboardShortcuts();
    void showPreferences();

    bool keyPressed(const juce::KeyPress& key) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void layoutPreferences(int width);
    void revealControl(juce::Component& control);
    void syncChoices();

    void globalFocusChanged(juce::Component* focusedComponent) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;

    /// Processor-owned message-thread settings tree, which outlives the panel and its bound controls.
    juce::ValueTree& state;
    /// Content must outlive the non-owning viewport, and controls detach before both are destroyed.
    juce::Component content;
    /// Scrollable viewport containing the preference controls.
    juce::Viewport viewport;

    /// Appearance section heading.
    juce::Label appearanceLabel;
    /// Colour choice caption.
    juce::Label colourLabel;
    /// Contrast slider caption.
    juce::Label contrastLabel;
    /// Correlation section heading.
    juce::Label correlationLabel;
    /// Display behavior section heading.
    juce::Label displayLabel;
    /// Frequency-scale choice caption.
    juce::Label frequencyLabel;
    /// Pan-edge slider caption.
    juce::Label panEdgeLabel;
    /// Pan-midpoint slider caption.
    juce::Label panMidpointLabel;
    /// Shared persistence slider caption.
    juce::Label persistenceLabel;
    /// Rendering-mode choice caption.
    juce::Label renderingLabel;
    /// Stereo-position section heading.
    juce::Label stereoPositionLabel;
    /// Selects frequency or phase-coherence colours.
    juce::ComboBox colourChoice;
    /// Selects logarithmic or mel frequency spacing.
    juce::ComboBox frequencyChoice;
    /// Selects particles or solid-layer rendering.
    juce::ComboBox renderingChoice;

    /// Spectrogram contrast slider.
    ControlSlider contrastSlider;
    /// Stereo display edge slider.
    ControlSlider panEdgeSlider;
    /// Stereo display midpoint slider.
    ControlSlider panMidpointSlider;
    /// Shared particle-fade and correlation persistence slider.
    ControlSlider persistenceSlider;

    /// Opens the nested keyboard and pointer shortcuts view.
    juce::TextButton keyboardShortcutsButton {"Keyboard shortcuts"};
    /// Resets visual appearance properties to their defaults.
    juce::TextButton resetAppearanceButton {"Reset display appearance"};

    /// Stops spectrogram analysis and rendering without bypassing audio.
    juce::ToggleButton bypassRenderingButton {"Stop spectrogram analysis"};
    /// Hides the spectrogram section.
    juce::ToggleButton hideSpectrogramButton {"Hide spectrogram"};
    /// Expands the spectrogram plot and draws axes inside it.
    juce::ToggleButton maximizeSpectrogramButton {"Maximize spectrogram"};
    /// Shows or hides the per-band correlation row.
    juce::ToggleButton showBandCorrelationButton {"Show band correlation"};
    /// Shows or hides the diagnostic frame-rate readout.
    juce::ToggleButton showFpsButton {"Show frame rate"};
    /// Selects smoothed or live correlation readout numbers.
    juce::ToggleButton smoothCorrelationReadoutButton {"Smooth correlation numbers"};

    /// Nested reference replaces preferences within the same callout, without another window or modal layer.
    KeyboardShortcutsPanel shortcutsPanel;
    /// Returns from the nested shortcuts view to the preference controls.
    juce::TextButton backButton {"Back to Settings"};
    /// Whether the nested shortcuts view is currently shown instead of preferences.
    bool showingShortcuts {false};
};
}  // namespace esgrove::image_control
