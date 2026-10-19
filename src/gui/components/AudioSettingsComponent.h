/// Audio settings content combining JUCE device selection with the application's preview preferences.
/// The main view remains the preference authority, while this panel forwards checkbox changes
/// and reflects updates made through the Options menu.

#pragma once

#include "gui/AudioBatchComponentState.h"

#include <JuceHeader.h>

#include <functional>

namespace audiobatch
{
[[nodiscard]] AudioPreviewPreferences loadAudioPreviewPreferences(const juce::PropertiesFile& settings);
bool saveAudioPreviewPreferences(juce::PropertiesFile& settings, const AudioPreviewPreferences& preferences);
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

    void resized() override;
    void setPreferences(const AudioPreviewPreferences& preferences);

    std::function<void(const AudioPreviewPreferences&)> onPreferencesChanged;

private:
    void preferencesChanged();
    void componentMovedOrResized(juce::Component& component, bool moved, bool resized) override;

    juce::Viewport viewport;
    juce::Component content;
    juce::AudioDeviceSelectorComponent deviceSelector;
    juce::GroupComponent deviceGroup {"deviceSettings", "Audio interface and playback"};
    juce::GroupComponent previewGroup {"previewSettings", "Preview preferences"};
    juce::ToggleButton pluginToggle {"Preview plugin chain"};
    juce::ToggleButton compensationToggle {"Compensate plugin latency"};
    juce::ToggleButton continuationToggle {"Continue playback when changing files"};
    juce::ToggleButton coloursToggle {"Serato waveform colours"};
    bool layingOut {false};
};
}  // namespace audiobatch
