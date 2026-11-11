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
    juce::Viewport viewport;

    juce::Label appearanceLabel;
    juce::Label colourLabel;
    juce::Label contrastLabel;
    juce::Label correlationLabel;
    juce::Label displayLabel;
    juce::Label frequencyLabel;
    juce::Label panEdgeLabel;
    juce::Label panMidpointLabel;
    juce::Label persistenceLabel;
    juce::Label renderingLabel;
    juce::Label stereoPositionLabel;
    juce::ComboBox colourChoice;
    juce::ComboBox frequencyChoice;
    juce::ComboBox renderingChoice;

    ControlSlider contrastSlider;
    ControlSlider panEdgeSlider;
    ControlSlider panMidpointSlider;
    ControlSlider persistenceSlider;

    juce::TextButton keyboardShortcutsButton {"Keyboard shortcuts"};
    juce::TextButton resetAppearanceButton {"Reset display appearance"};

    juce::ToggleButton bypassRenderingButton {"Stop spectrogram analysis"};
    juce::ToggleButton hideSpectrogramButton {"Hide spectrogram"};
    juce::ToggleButton showBandCorrelationButton {"Show band correlation"};
    juce::ToggleButton showFpsButton {"Show frame rate"};
    juce::ToggleButton smoothCorrelationReadoutButton {"Smooth correlation numbers"};

    /// Nested reference replaces preferences within the same callout, without another window or modal layer.
    KeyboardShortcutsPanel shortcutsPanel;
    juce::TextButton backButton {"Back to Settings"};
    bool showingShortcuts {false};
};
}  // namespace esgrove::image_control
