/// GUI control for choosing the normalization target.
/// Declares NormalizationTargetControl, a mode selector for peak or loudness normalization
/// next to a slider for the target level, with both targets persisted in the user settings.

#pragma once

#include "AudioNormalizationService.h"

#include <JuceHeader.h>

/// Mode selector and target slider for normalization, laid out side by side.
/// Each mode remembers its own target, and the mode and both targets are saved in the user settings.
class NormalizationTargetControl final : public juce::Component
{
public:
    /// Creates the controls. The settings are read later by loadSettings,
    /// so the properties can be configured after this control is constructed.
    explicit NormalizationTargetControl(juce::ApplicationProperties& applicationProperties);

    /// Reads the mode and both targets from the user settings into the controls.
    void loadSettings();

    /// Returns the normalization mode and target selected in the controls.
    /// The caller adds the overwrite setting.
    [[nodiscard]] AudioNormalizationOptions getOptions() const;

    /// Returns the selected target as display text, for example "-14.0 LUFS" or "0.0 dBFS peak".
    [[nodiscard]] juce::String describeTarget() const;

    /// Lays out the mode selector and the target slider in two equal halves.
    void resized() override;

private:
    /// Returns true when loudness mode is selected.
    [[nodiscard]] bool isLoudnessMode() const;

    /// Writes the mode and both targets to the user settings.
    void saveSettings();

    /// Switches the slider between the peak and loudness ranges for the selected mode,
    /// showing the remembered target of that mode.
    void updateSlider();

    juce::ApplicationProperties& properties;
    juce::ComboBox modeBox;
    juce::Slider targetSlider {"NormalizeTarget"};
    /// Remembered peak mode target, in dBFS, kept while loudness mode is selected.
    double targetPeakDbfs = AudioNormalizationOptions::defaultTargetPeakDbfs;
    /// Remembered loudness mode target, in LUFS, kept while peak mode is selected.
    double targetLufs = AudioNormalizationOptions::defaultTargetLufs;
};
