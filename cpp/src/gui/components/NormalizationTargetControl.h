/// GUI control for choosing the normalization target.
/// Declares NormalizationTargetControl, Peak and Loudness mode buttons above a slider for the target level
/// and the Normalize button, with the mode and both targets persisted in the user settings.

#pragma once

#include "normalization/AudioNormalization.h"

#include <JuceHeader.h>

namespace audiobatch
{
/// Peak and Loudness mode buttons on one row, and the target slider with the Normalize button below them,
/// like the normalization group of the Rust version.
/// Each mode remembers its own target, and the mode and both targets are saved in the user settings.
class NormalizationTargetControl final : public juce::Component
{
public:
    explicit NormalizationTargetControl(juce::ApplicationProperties& applicationProperties);

    void loadSettings();

    [[nodiscard]] AudioNormalizationOptions getOptions() const;

    [[nodiscard]] juce::String describeTarget() const;

    void resized() override;

    /// Enables or disables the Normalize button, which the owner keeps in step with the selection.
    void setNormalizeEnabled(bool enabled);

    /// Sets the callback the Normalize button calls to normalize the selected files to the target.
    void setNormalizeCallback(std::function<void()> callback);

private:
    [[nodiscard]] bool isLoudnessMode() const;

    void saveSettings() const;

    void selectMode(bool loudness);

    void updateSlider();

    juce::ApplicationProperties& properties;
    juce::TextButton peakButton {"Peak"};
    juce::TextButton loudnessButton {"Loudness"};
    juce::TextButton normalizeButton {"Normalize"};
    juce::Slider targetSlider {"NormalizeTarget"};
    /// Remembered peak mode target, in dBFS, kept while loudness mode is selected.
    double targetPeakDbfs {AudioNormalizationOptions::defaultTargetPeakDbfs};
    /// Remembered loudness mode target, in LUFS, kept while peak mode is selected.
    double targetLufs {AudioNormalizationOptions::defaultTargetLufs};
};
}  // namespace audiobatch
