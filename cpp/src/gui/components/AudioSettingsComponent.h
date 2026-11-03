/// Audio settings content combining JUCE device selection with the application's preview preferences.
/// The main view remains the preference authority, while this panel forwards view, checkbox, and loudness-axis changes
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

[[nodiscard]] bool isValidLoudnessGraphLimitInput(const juce::String& text);
[[nodiscard]] bool isValidLoudnessGraphRange(double minimum, double maximum);
bool saveAudioPreviewPreferences(juce::PropertiesFile& settings, const AudioPreviewPreferences& preferences);
[[nodiscard]] bool shouldContinuePreviewPlayback(
    const AudioPreviewPreferences& preferences,
    bool wasPlaying,
    bool userSelection
);

[[nodiscard]] double getPreviewStartPositionSeconds(
    const AudioPreviewPreferences& preferences,
    const serato::Markers* markers,
    double durationSeconds
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

    void setPreferences(const AudioPreviewPreferences& preferences);
    void setPreferencesChangedCallback(std::function<void(const AudioPreviewPreferences&)> callback);

    // juce::Component
    void resized() override;

private:
    void preferencesChanged();

    // juce::ComponentListener
    void componentMovedOrResized(juce::Component& component, bool moved, bool resized) override;

    /// Preferences reflected by the controls.
    AudioPreviewPreferences displayedPreferences;

    /// Output device, rate and buffer size selector.
    juce::AudioDeviceSelectorComponent deviceSelector;
    /// Parent content outlives its borrowing viewport.
    juce::Component content;
    juce::GroupComponent deviceGroup {"deviceSettings", "Audio interface and playback"};
    juce::GroupComponent previewGroup {"previewSettings", "Preview preferences"};

    /// Source visualization and arrangement, synchronized with the Options menu.
    juce::ComboBox previewView;
    /// Caption of the source visualization selector.
    juce::Label previewViewLabel {"previewView", "Preview view"};

    /// Top and bottom loudness axis captions.
    juce::Label loudnessMaximumLabel {"loudnessMaximum", "Max (LUFS)"};
    juce::Label loudnessMinimumLabel {"loudnessMinimum", "Min (LUFS)"};
    /// Invalid-range explanation, empty while valid.
    juce::Label loudnessRangeError;
    /// Whole-LUFS axis limits.
    juce::TextEditor loudnessMaximum;
    juce::TextEditor loudnessMinimum;

    juce::ToggleButton coloursToggle {"Serato waveform colours"};
    juce::ToggleButton compensationToggle {"Compensate plugin latency"};
    juce::ToggleButton continuationToggle {"Continue playback when changing files"};
    juce::ToggleButton loudnessGraphToggle {"Show loudness graph"};
    juce::ToggleButton playFromFirstCuePointToggle {"Play from first cue point"};
    juce::ToggleButton pluginToggle {"Preview plugin chain"};

    /// Independent source overload signals and their optional vertical lines.
    juce::ToggleButton overloadLinesToggle {"Overload vertical lines"};
    juce::ToggleButton sampleOverloadsToggle {"Peak overloads"};
    juce::ToggleButton truePeakOverloadsToggle {"True-peak overloads"};

    /// Borrows the content while the dialog is shorter than its settings.
    juce::Viewport viewport;
    /// Receives user changes to the preferences.
    std::function<void(const AudioPreviewPreferences&)> onPreferencesChanged;
    /// Independent native-window tooltips.
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    /// Suppresses recursive layout notifications from the selector.
    bool layingOut {false};
};
}  // namespace audiobatch
