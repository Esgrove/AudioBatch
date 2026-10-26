/// Audio settings content combining JUCE device selection with the application's preview preferences.
/// The main view remains the preference authority, while this panel forwards checkbox and loudness-axis changes
/// and reflects updates made through the Options menu.

#pragma once

#include "gui/AudioBatchComponentState.h"
#include "serato/SeratoTagTypes.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>

namespace audiobatch
{
[[nodiscard]] AudioPreviewPreferences loadAudioPreviewPreferences(const juce::PropertiesFile& settings);
[[nodiscard]] bool isValidLoudnessGraphRange(double minimum, double maximum);
[[nodiscard]] bool isValidLoudnessGraphLimitInput(const juce::String& text);
bool saveAudioPreviewPreferences(juce::PropertiesFile& settings, const AudioPreviewPreferences& preferences);
[[nodiscard]] double getPreviewStartPositionSeconds(
    const AudioPreviewPreferences& preferences,
    const serato::Markers* markers,
    double durationSeconds
);
[[nodiscard]] bool shouldContinuePreviewPlayback(
    const AudioPreviewPreferences& preferences,
    bool wasPlaying,
    bool userSelection
);

/// Scrollable settings content whose device controls can change height when the interface changes.
class AudioSettingsComponent final : public juce::Component, private juce::ComponentListener
{
public:
    explicit AudioSettingsComponent(juce::AudioDeviceManager& manager);
    ~AudioSettingsComponent() override;

    AudioSettingsComponent(const AudioSettingsComponent&) = delete;
    AudioSettingsComponent& operator=(const AudioSettingsComponent&) = delete;
    AudioSettingsComponent(AudioSettingsComponent&&) = delete;
    AudioSettingsComponent& operator=(AudioSettingsComponent&&) = delete;

    void resized() override;
    void setPreferences(const AudioPreviewPreferences& preferences);

    void setPreferencesChangedCallback(std::function<void(const AudioPreviewPreferences&)> callback);

private:
    void preferencesChanged();
    void componentMovedOrResized(juce::Component& component, bool moved, bool resized) override;

    std::function<void(const AudioPreviewPreferences&)> onPreferencesChanged;

    juce::Viewport viewport;
    juce::Component content;
    juce::AudioDeviceSelectorComponent deviceSelector;
    juce::GroupComponent deviceGroup {"deviceSettings", "Audio interface and playback"};
    juce::GroupComponent previewGroup {"previewSettings", "Preview preferences"};
    juce::ToggleButton pluginToggle {"Preview plugin chain"};
    juce::ToggleButton compensationToggle {"Compensate plugin latency"};
    juce::ToggleButton continuationToggle {"Continue playback when changing files"};
    juce::ToggleButton playFromFirstCuePointToggle {"Play from first cue point"};
    juce::ToggleButton coloursToggle {"Serato waveform colours"};
    juce::ToggleButton loudnessGraphToggle {"Show loudness graph"};
    juce::Label loudnessMinimumLabel {"loudnessMinimum", "Min (LUFS)"};
    juce::Label loudnessMaximumLabel {"loudnessMaximum", "Max (LUFS)"};
    juce::Label loudnessRangeError;
    juce::TextEditor loudnessMinimum;
    juce::TextEditor loudnessMaximum;
    AudioPreviewPreferences displayedPreferences;
    bool layingOut {false};

    /// Serves tooltips in the settings dialog's native window independently of the main view.
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
};
}  // namespace audiobatch
