/// GUI control for choosing the normalization target.
/// Declares NormalizationTargetControl, Peak and Loudness mode buttons above a slider for the target level
/// and the Normalize button, with the mode and both targets persisted in the user settings.

#pragma once

#include "normalization/AudioNormalization.h"

#include <JuceHeader.h>

namespace audiobatch
{
/// Peak and Loudness mode buttons on one row,
/// and the target slider and the Normalize button in equal halves below them.
/// The slider fills in a neutral grey, so it does not compete with the blue of the chosen mode.
/// Each mode remembers its own target, and the mode and both targets are saved in the user settings.
class NormalizationTargetControl final : public juce::Component
{
public:
    explicit NormalizationTargetControl(juce::ApplicationProperties& applicationProperties);

    void loadSettings();

    [[nodiscard]] AudioNormalizationOptions getOptions() const;

    [[nodiscard]] juce::String describeTarget() const;

    void resized() override;

    void setNormalizeEnabled(bool enabled);

    void setTargetEnabled(bool enabled);

    void setNormalizeCallback(std::function<void()> callback);

private:
    [[nodiscard]] bool isLoudnessMode() const;

    void saveSettings() const;

    void selectMode(bool loudness);

    void updateSlider();

    /// The application settings the mode and the targets are kept in.
    juce::ApplicationProperties& properties;
    /// Selects normalizing to a sample peak.
    juce::TextButton peakButton {"Peak"};
    /// Selects normalizing to an integrated loudness.
    juce::TextButton loudnessButton {"Loudness"};
    /// Normalizes the selected files.
    juce::TextButton normalizeButton {"Normalize"};
    /// The target of the selected mode, in dBFS or LUFS.
    juce::Slider targetSlider {"NormalizeTarget"};
    /// Remembered peak mode target, in dBFS, kept while loudness mode is selected.
    double targetPeakDbfs {AudioNormalizationOptions::defaultTargetPeakDbfs};
    /// Remembered loudness mode target, in LUFS, kept while peak mode is selected.
    double targetLufs {AudioNormalizationOptions::defaultTargetLufs};
};
}  // namespace audiobatch
