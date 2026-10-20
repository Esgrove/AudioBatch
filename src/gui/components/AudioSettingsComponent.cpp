/// Audio device and preview settings layout, preference serialization, and checkbox forwarding.
/// All preference entry points are synchronized by the main component rather than storing independent UI state.

#include "gui/components/AudioSettingsComponent.h"

#include "gui/GuiLayout.h"
#include "utils/Log.h"

#include <utility>

namespace audiobatch
{
namespace
{
constexpr auto pluginPreviewKey {"previewPluginChain"};
constexpr auto compensationKey {"previewLatencyCompensation"};
constexpr auto continuationKey {"previewContinuePlayback"};
constexpr auto waveformColoursKey {"seratoWaveformColours"};
}  // namespace

/// Reads only preview-owned keys, leaving existing plugin and normalization properties intact.
AudioPreviewPreferences loadAudioPreviewPreferences(const juce::PropertiesFile& settings)
{
    return {
        .pluginChainEnabled = settings.getBoolValue(pluginPreviewKey, false),
        .latencyCompensated = settings.getBoolValue(compensationKey, true),
        .continuePlayback = settings.getBoolValue(continuationKey, false),
        .seratoWaveformColours = settings.getBoolValue(waveformColoursKey, true),
    };
}

/// Saves all four switches together and explicitly reports persistence failures.
bool saveAudioPreviewPreferences(juce::PropertiesFile& settings, const AudioPreviewPreferences& preferences)
{
    settings.setValue(pluginPreviewKey, preferences.pluginChainEnabled);
    settings.setValue(compensationKey, preferences.latencyCompensated);
    settings.setValue(continuationKey, preferences.continuePlayback);
    settings.setValue(waveformColoursKey, preferences.seratoWaveformColours);
    if (!settings.saveIfNeeded()) {
        utils::logError(
            "Could not save audio preview preferences to {}", settings.getFile().getFullPathName().quoted()
        );
        return false;
    }
    return true;
}

/// Restricts continuation to an intentional selection while playback, pre-roll, or a tail was active.
bool shouldContinuePreviewPlayback(
    const AudioPreviewPreferences& preferences,
    const bool wasPlaying,
    const bool userSelection
)
{
    return preferences.continuePlayback && wasPlaying && userSelection;
}

/// Creates native device controls and a separate, visually grouped section of preview preferences.
AudioSettingsComponent::AudioSettingsComponent(juce::AudioDeviceManager& manager) :
    deviceSelector(manager, 0, 0, 0, 2, false, false, true, false)
{
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollBarThickness(layout::scrollBarThickness);
    addAndMakeVisible(viewport);
    content.addAndMakeVisible(deviceGroup);
    content.addAndMakeVisible(deviceSelector);
    content.addAndMakeVisible(previewGroup);
    pluginToggle.setTooltip(
        "Listen through enabled plugins and the file's custom input gain. Does not normalize or write files."
    );
    compensationToggle.setTooltip(
        "Align processed audio with the file position. High-latency chains need a preparation pause after starts or "
        "seeks."
    );
    continuationToggle.setTooltip("Start a newly selected file only if the previous preview was playing.");
    coloursToggle.setTooltip(
        "Use Serato spectral waveform colours where available, without changing cues or playback."
    );
    for (auto* toggle : {&pluginToggle, &compensationToggle, &continuationToggle, &coloursToggle}) {
        content.addAndMakeVisible(toggle);
        toggle->onClick = [this] { preferencesChanged(); };
    }
    deviceSelector.addComponentListener(this);
}

/// Removes the height observer before destroying the device selector.
AudioSettingsComponent::~AudioSettingsComponent()
{
    deviceSelector.removeComponentListener(this);
}

/// Reflects canonical state without sending checkbox callbacks back to the controller.
void AudioSettingsComponent::setPreferences(const AudioPreviewPreferences& preferences)
{
    pluginToggle.setToggleState(preferences.pluginChainEnabled, juce::dontSendNotification);
    compensationToggle.setToggleState(preferences.latencyCompensated, juce::dontSendNotification);
    continuationToggle.setToggleState(preferences.continuePlayback, juce::dontSendNotification);
    coloursToggle.setToggleState(preferences.seratoWaveformColours, juce::dontSendNotification);
}

/// Installs the message-thread callback that receives user changes to preview preferences.
void AudioSettingsComponent::setPreferencesChangedCallback(std::function<void(const AudioPreviewPreferences&)> callback)
{
    onPreferencesChanged = std::move(callback);
}

/// Forwards a complete preference snapshot so all other surfaces can update together.
void AudioSettingsComponent::preferencesChanged()
{
    if (onPreferencesChanged) {
        onPreferencesChanged({
            .pluginChainEnabled = pluginToggle.getToggleState(),
            .latencyCompensated = compensationToggle.getToggleState(),
            .continuePlayback = continuationToggle.getToggleState(),
            .seratoWaveformColours = coloursToggle.getToggleState(),
        });
    }
}

/// Lets JUCE choose the device selector height and keeps the lower section reachable at small window sizes.
void AudioSettingsComponent::resized()
{
    const juce::ScopedValueSetter<bool> layoutGuard(layingOut, true);
    viewport.setBounds(getLocalBounds());
    const auto width = juce::jmax(0, getWidth() - layout::scrollBarThickness);
    const auto area = juce::Rectangle<int>(0, 0, width, layout::audioSettings::preferredHeight)
                          .reduced(layout::audioSettings::padding);
    deviceSelector.setBounds(
        area.getX() + layout::audioSettings::padding,
        area.getY() + layout::audioSettings::padding + layout::audioSettings::headingHeight,
        juce::jmax(0, area.getWidth() - layout::audioSettings::padding * 2),
        juce::jmax(1, deviceSelector.getHeight())
    );
    deviceGroup.setBounds(
        area.getX(),
        area.getY(),
        area.getWidth(),
        deviceSelector.getBottom() - area.getY() + layout::audioSettings::padding
    );
    const auto groupTop = deviceGroup.getBottom() + layout::audioSettings::sectionSpacing;
    previewGroup.setBounds(area.getX(), groupTop, area.getWidth(), layout::audioSettings::preferencesHeight);
    auto preferences = previewGroup.getBounds().reduced(layout::audioSettings::padding);
    preferences.removeFromTop(layout::audioSettings::headingHeight);
    for (auto* toggle : {&pluginToggle, &compensationToggle, &continuationToggle, &coloursToggle}) {
        toggle->setBounds(preferences.removeFromTop(layout::audioSettings::rowHeight));
    }
    content.setSize(width, previewGroup.getBottom() + layout::audioSettings::padding);
}

/// Relayouts when a device/backend change adds or removes native selector rows.
void AudioSettingsComponent::componentMovedOrResized(
    juce::Component& /*component*/,
    bool /*wasMoved*/,
    const bool wasResized
)
{
    if (wasResized && !layingOut) {
        resized();
    }
}
}  // namespace audiobatch
