/// GUI control for choosing the normalization target.
/// Declares NormalizationTargetControl, a mode selector for peak
/// or loudness normalization next to a slider for the target level, with both targets persisted in the user settings.

#pragma once

#include "normalization/AudioNormalization.h"

#include <JuceHeader.h>

namespace audiobatch
{
/// Mode selector and target slider for normalization, laid out side by side.
/// Each mode remembers its own target, and the mode and both targets are saved in the user settings.
class NormalizationTargetControl final : public juce::Component
{
public:
    explicit NormalizationTargetControl(juce::ApplicationProperties& applicationProperties);

    void loadSettings();

    [[nodiscard]] AudioNormalizationOptions getOptions() const;

    [[nodiscard]] juce::String describeTarget() const;

    void resized() override;

private:
    [[nodiscard]] bool isLoudnessMode() const;

    void saveSettings() const;

    void updateSlider();

    juce::ApplicationProperties& properties;
    juce::ComboBox modeBox;
    juce::Slider targetSlider {"NormalizeTarget"};
    /// Remembered peak mode target, in dBFS, kept while loudness mode is selected.
    double targetPeakDbfs {AudioNormalizationOptions::defaultTargetPeakDbfs};
    /// Remembered loudness mode target, in LUFS, kept while peak mode is selected.
    double targetLufs {AudioNormalizationOptions::defaultTargetLufs};
};
}  // namespace audiobatch
