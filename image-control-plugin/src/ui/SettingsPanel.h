/// Declares SettingsPanel, the panel of display settings that the editor's Settings button opens.
/// Keeping these here leaves the main editor to the processing controls and the spectrogram.

#pragma once

#include "ui/ControlSlider.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control
{
/// The display settings: the analyzer tap, the colours, the frequency scale, the frame rate readout,
/// bypassing or hiding the spectrogram, the persistence, and the focus.
/// Every control is bound to an editor setting, so the editor picks changes up on its next display refresh
/// and the settings are saved with the plugin state.
class SettingsPanel final : public juce::Component
{
public:
    explicit SettingsPanel(juce::ValueTree& editorState);

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    juce::Label titleLabel;
    juce::ToggleButton analyzeOutputButton {"Show the output (off: the input)"};
    juce::ToggleButton phaseColoursButton {"Colour by phase instead of frequency"};
    juce::ToggleButton melScaleButton {"Mel frequency scale"};
    juce::ToggleButton showFpsButton {"Show the frame rate"};
    juce::ToggleButton bypassRenderingButton {"Bypass rendering"};
    juce::ToggleButton hideSpectrogramButton {"Hide the spectrogram"};
    juce::Label persistenceLabel;
    ControlSlider persistenceSlider;
    juce::Label focusLabel;
    ControlSlider focusSlider;
};
}  // namespace esgrove::image_control
