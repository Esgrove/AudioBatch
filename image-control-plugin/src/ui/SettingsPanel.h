/// Declares SettingsPanel, the panel of display settings that the editor's Settings button opens.
/// Keeping these here leaves the main editor to the processing controls and the spectrogram.

#pragma once

#include "ui/ControlSlider.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control
{
/// The display settings: the colours, the frequency scale, the solid layers, the frame rate readout,
/// bypassing or hiding the spectrogram, band correlation visibility and number smoothing,
/// persistence, contrast, and the stereo position curve.
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
    juce::ToggleButton phaseColoursButton {"Colour by phase instead of frequency"};
    juce::ToggleButton melScaleButton {"Mel frequency scale"};
    juce::ToggleButton solidRenderingButton {"Solid layers instead of particles"};
    juce::ToggleButton showFpsButton {"Show the frame rate"};
    juce::ToggleButton showBandCorrelationButton {"Show band correlation"};
    juce::ToggleButton smoothCorrelationReadoutButton {"Smooth correlation numbers"};
    juce::ToggleButton bypassRenderingButton {"Bypass rendering"};
    juce::ToggleButton hideSpectrogramButton {"Hide the spectrogram"};
    juce::Label persistenceLabel;
    ControlSlider persistenceSlider;
    juce::Label contrastLabel;
    ControlSlider contrastSlider;
    juce::Label panMidpointLabel;
    ControlSlider panMidpointSlider;
    juce::Label panEdgeLabel;
    ControlSlider panEdgeSlider;
};
}  // namespace esgrove::image_control
