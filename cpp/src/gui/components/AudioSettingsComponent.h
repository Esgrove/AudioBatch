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

    /// Receives the preferences whenever the user changes one of them.
    std::function<void(const AudioPreviewPreferences&)> onPreferencesChanged;

    /// Scrolls the content when the dialog is shorter than the settings.
    juce::Viewport viewport;
    /// The device and preview groups inside the viewport.
    juce::Component content;
    /// JUCE's selector of the output device, its sample rate, and its buffer size.
    juce::AudioDeviceSelectorComponent deviceSelector;
    /// Frames the device selector.
    juce::GroupComponent deviceGroup {"deviceSettings", "Audio interface and playback"};
    /// Frames the preview preferences.
    juce::GroupComponent previewGroup {"previewSettings", "Preview preferences"};
    /// Turns the plugin chain in the preview on or off.
    juce::ToggleButton pluginToggle {"Preview plugin chain"};
    /// Turns the latency compensation of the preview on or off.
    juce::ToggleButton compensationToggle {"Compensate plugin latency"};
    /// Keeps playing when another file is selected.
    juce::ToggleButton continuationToggle {"Continue playback when changing files"};
    /// Starts a newly selected file at its first Serato cue point.
    juce::ToggleButton playFromFirstCuePointToggle {"Play from first cue point"};
    /// Colours the waveform with the Serato overview.
    juce::ToggleButton coloursToggle {"Serato waveform colours"};
    /// Shows the short-term loudness curve over the waveform.
    juce::ToggleButton loudnessGraphToggle {"Show loudness graph"};
    /// Caption of the bottom of the loudness axis.
    juce::Label loudnessMinimumLabel {"loudnessMinimum", "Min (LUFS)"};
    /// Caption of the top of the loudness axis.
    juce::Label loudnessMaximumLabel {"loudnessMaximum", "Max (LUFS)"};
    /// Explains an invalid loudness axis range, empty while the range is valid.
    juce::Label loudnessRangeError;
    /// Edits the bottom of the loudness axis in whole LUFS.
    juce::TextEditor loudnessMinimum;
    /// Edits the top of the loudness axis in whole LUFS.
    juce::TextEditor loudnessMaximum;
    /// The preferences the controls show, which the edits change.
    AudioPreviewPreferences displayedPreferences;
    /// True while resized lays the controls out, so the resize of the device selector it causes is ignored.
    bool layingOut {false};

    /// Serves tooltips in the settings dialog's native window independently of the main view.
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
};
}  // namespace audiobatch
