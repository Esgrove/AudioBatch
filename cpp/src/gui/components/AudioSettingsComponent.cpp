/// Audio device and preview settings layout, preference serialization, and dialog-local tooltip lookup.
/// All preference entry points are synchronized by the main component rather than storing independent UI state.

#include "gui/components/AudioSettingsComponent.h"

#include "gui/AudioBatchComponentState.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoTagTypes.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"
#include <initializer_list>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <ios>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>

namespace audiobatch
{
namespace
{
/// Keys of the preview preferences in the application settings.
constexpr auto pluginPreviewKey {"previewPluginChain"};
constexpr auto compensationKey {"previewLatencyCompensation"};
constexpr auto continuationKey {"previewContinuePlayback"};
constexpr auto playFromFirstCuePointKey {"playFromFirstCuePoint"};
constexpr auto waveformColoursKey {"seratoWaveformColours"};
constexpr auto loudnessGraphKey {"loudnessGraphEnabled"};
constexpr auto loudnessMinimumKey {"loudnessGraphMinimum"};
constexpr auto loudnessMaximumKey {"loudnessGraphMaximum"};
/// Most digits a loudness axis limit can have, enough for the whole LUFS values from -60 to +6.
constexpr int maximumLoudnessLimitDigits {2};

/// Captions identify JUCE-owned device controls through their public label attachments.
constexpr std::array deviceTooltipDescriptions {
    std::pair {"Audio device type:", "Choose the audio driver used for preview playback."},
    std::pair {"Output:", "Choose the audio interface or speakers used for preview playback."},
    std::pair {"Device:", "Choose the audio interface or speakers used for preview playback."},
    std::pair {"Active output channels:", "Choose the output channels used for preview playback."},
    std::pair {"Sample rate:", "Choose the audio device's playback sample rate. This does not change source files."},
    std::pair {
        "Audio buffer size:",
        "Smaller buffers reduce preview latency. Larger buffers give plugins more time and can prevent dropouts."
    },
};

/// Supplies missing device descriptions on demand so backend-created controls keep their tooltips.
class AudioSettingsTooltipWindow final : public juce::TooltipWindow
{
public:
    /// Keeps tooltip rendering in the settings dialog's own native window.
    explicit AudioSettingsTooltipWindow(juce::Component& parent) :
        juce::TooltipWindow(&parent, layout::tooltipDelayMilliseconds)
    {}

    juce::String getTipFor(juce::Component& component) override;
};

/// Allows at most two integer digits and an optional leading minus, including incomplete edits.
class LoudnessLimitInputFilter final : public juce::TextEditor::InputFilter
{
public:
    /// Rejects invalid input as a whole, so pasting a decimal never silently changes its value.
    juce::String filterNewText(juce::TextEditor& editor, const juce::String& newInput) override
    {
        const auto selection = editor.getHighlightedRegion();
        const auto candidate = editor.getText().replaceSection(selection.getStart(), selection.getLength(), newInput);
        return isValidLoudnessGraphLimitInput(candidate) ? newInput : juce::String();
    }
};

/// Resolves device controls and their captions without depending on JUCE's private selector implementation.
juce::String getDeviceControlTooltip(const juce::Component& component)
{
    const auto* parent = component.getParentComponent();
    if (parent == nullptr) {
        return {};
    }
    for (const auto* sibling : parent->getChildren()) {
        const auto* label = dynamic_cast<const juce::Label*>(sibling);
        if (label == nullptr || label->getAttachedComponent() == nullptr) {
            continue;
        }
        if (label != &component && label->getAttachedComponent() != &component) {
            continue;
        }
        for (const auto& [caption, description] : deviceTooltipDescriptions) {
            if (label->getText() == juce::translate(caption)) {
                return description;
            }
        }
    }
    return {};
}

/// Preserves explicit tooltips and resolves nested device controls through their labeled ancestors.
juce::String AudioSettingsTooltipWindow::getTipFor(juce::Component& component)
{
    if (auto* client = dynamic_cast<juce::SettableTooltipClient*>(&component);
        client != nullptr && client->getTooltip().isEmpty())
    {
        for (const auto* control = &component; control != nullptr && control != getParentComponent();
             control = control->getParentComponent())
        {
            const auto description = getDeviceControlTooltip(*control);
            if (description.isNotEmpty()) {
                client->setTooltip(description);
                break;
            }
        }
    }
    return juce::TooltipWindow::getTipFor(component);
}

/// Parses the complete saved or edited text, or returns an empty optional for partially numeric and non-finite values.
std::optional<double> readLoudnessLimit(const juce::String& input)
{
    std::istringstream stream(input.trim().toStdString());
    stream.imbue(std::locale::classic());
    double value {};
    stream >> std::noskipws >> value;
    return !stream.fail() && stream.eof() && std::isfinite(value) ? std::optional {value} : std::nullopt;
}
}  // namespace

/// Accepts provisional integer editor text, including empty text and a leading minus before any digits.
bool isValidLoudnessGraphLimitInput(const juce::String& text)
{
    const auto digits = text.startsWithChar('-') ? text.substring(1) : text;
    return digits.length() <= maximumLoudnessLimitDigits && (digits.isEmpty() || digits.containsOnly("0123456789"));
}

/// Requires whole-LU axis limits within the supported range and keeps at least one LU between them.
bool isValidLoudnessGraphRange(const double minimum, const double maximum)
{
    return std::isfinite(minimum) && std::isfinite(maximum)
        && minimum >= AudioPreviewPreferences::minimumLoudnessGraphLimit
        && maximum <= AudioPreviewPreferences::maximumLoudnessGraphLimit && maximum - minimum >= 1.0
        && std::floor(minimum) >= minimum && std::floor(maximum) >= maximum;
}

/// Reads only preview-owned keys, leaving existing plugin and normalization properties intact.
AudioPreviewPreferences loadAudioPreviewPreferences(const juce::PropertiesFile& settings)
{
    AudioPreviewPreferences preferences {
        .pluginChainEnabled = settings.getBoolValue(pluginPreviewKey, false),
        .latencyCompensated = settings.getBoolValue(compensationKey, true),
        .continuePlayback = settings.getBoolValue(continuationKey, false),
        .playFromFirstCuePoint = settings.getBoolValue(playFromFirstCuePointKey, false),
        .seratoWaveformColours = settings.getBoolValue(waveformColoursKey, true),
        .loudnessGraphEnabled = settings.getBoolValue(loudnessGraphKey, false),
    };
    // PropertiesFile's numeric conversion accepts a numeric prefix, so parse the complete saved text.
    const auto minimum = readLoudnessLimit(
        settings.getValue(loudnessMinimumKey, utils::format("{}", AudioPreviewPreferences::defaultLoudnessGraphMinimum))
    );
    const auto maximum = readLoudnessLimit(
        settings.getValue(loudnessMaximumKey, utils::format("{}", AudioPreviewPreferences::defaultLoudnessGraphMaximum))
    );

    if (minimum.has_value() && maximum.has_value() && isValidLoudnessGraphRange(*minimum, *maximum)) {
        preferences.loudnessGraphMinimum = *minimum;
        preferences.loudnessGraphMaximum = *maximum;
    } else {
        utils::logWarn(
            "Invalid saved loudness graph range, restoring {} to {} LUFS",
            AudioPreviewPreferences::defaultLoudnessGraphMinimum,
            AudioPreviewPreferences::defaultLoudnessGraphMaximum
        );
    }
    return preferences;
}

/// Saves preview switches and loudness axis limits together, explicitly reporting persistence failures.
bool saveAudioPreviewPreferences(juce::PropertiesFile& settings, const AudioPreviewPreferences& preferences)
{
    if (!isValidLoudnessGraphRange(preferences.loudnessGraphMinimum, preferences.loudnessGraphMaximum)) {
        utils::logError("Cannot save invalid loudness graph range");
        return false;
    }
    settings.setValue(pluginPreviewKey, preferences.pluginChainEnabled);
    settings.setValue(compensationKey, preferences.latencyCompensated);
    settings.setValue(continuationKey, preferences.continuePlayback);
    settings.setValue(playFromFirstCuePointKey, preferences.playFromFirstCuePoint);
    settings.setValue(waveformColoursKey, preferences.seratoWaveformColours);
    settings.setValue(loudnessGraphKey, preferences.loudnessGraphEnabled);
    settings.setValue(loudnessMinimumKey, preferences.loudnessGraphMinimum);
    settings.setValue(loudnessMaximumKey, preferences.loudnessGraphMaximum);
    if (!settings.saveIfNeeded()) {
        utils::logError(
            "Could not save audio preview preferences to {}", settings.getFile().getFullPathName().quoted()
        );
        return false;
    }
    return true;
}

/// Uses the earliest cue in time only when enabled and at or before the track's halfway point.
/// Missing cues and invalid durations fall back to the beginning, independent of playback continuation.
double getPreviewStartPositionSeconds(
    const AudioPreviewPreferences& preferences,
    const serato::Markers* markers,
    const double durationSeconds
)
{
    if (!preferences.playFromFirstCuePoint || markers == nullptr || markers->cues.empty()
        || !std::isfinite(durationSeconds) || durationSeconds <= 0.0)
    {
        return 0.0;
    }
    const auto firstCue = std::ranges::min_element(markers->cues, {}, &serato::CuePoint::positionMilliseconds);
    const auto position = static_cast<double>(firstCue->positionMilliseconds) / utils::millisecondsPerSecond;
    return position <= durationSeconds * 0.5 ? position : 0.0;
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
    playFromFirstCuePointToggle.setTooltip(
        "When loading a file, start at its first cue point only if it is at or before halfway through the file. "
        "Otherwise, or if there is no cue point, start at zero."
    );
    coloursToggle.setTooltip(
        "Use Serato spectral waveform colours where available, without changing cues or playback."
    );
    loudnessGraphToggle.setTooltip(
        "Overlay source-file short-term loudness (the trailing three seconds). "
        "Uses cached analysis, not preview gain or live plugins."
    );
    for (auto* toggle :
         {
             &pluginToggle,
             &compensationToggle,
             &continuationToggle,
             &playFromFirstCuePointToggle,
             &coloursToggle,
             &loudnessGraphToggle,
         })
    {
        content.addAndMakeVisible(toggle);
        toggle->onClick = [this] { preferencesChanged(); };
    }
    for (auto* editor : {&loudnessMinimum, &loudnessMaximum}) {
        content.addAndMakeVisible(editor);
        editor->setInputFilter(new LoudnessLimitInputFilter(), true);
        editor->setTooltip("Whole-number LUFS from -60 to +6. Maximum must be at least 1 LU above minimum.");
        editor->onReturnKey = [this] { preferencesChanged(); };
        editor->onFocusLost = [this] { preferencesChanged(); };
    }
    loudnessMinimumLabel.setTooltip(loudnessMinimum.getTooltip());
    loudnessMaximumLabel.setTooltip(loudnessMaximum.getTooltip());
    for (auto* label : {&loudnessMinimumLabel, &loudnessMaximumLabel, &loudnessRangeError}) {
        content.addAndMakeVisible(label);
    }
    loudnessRangeError.setColour(juce::Label::textColourId, juce::Colours::orange);
    deviceSelector.addComponentListener(this);
    tooltipWindow = std::make_unique<AudioSettingsTooltipWindow>(*this);
}

/// Detaches editor callbacks and the height observer before destroying the controls.
AudioSettingsComponent::~AudioSettingsComponent()
{
    for (auto* editor : {&loudnessMinimum, &loudnessMaximum}) {
        editor->onFocusLost = {};
        editor->onReturnKey = {};
    }
    deviceSelector.removeComponentListener(this);
}

/// Reflects canonical state without sending checkbox callbacks back to the controller.
void AudioSettingsComponent::setPreferences(const AudioPreviewPreferences& preferences)
{
    displayedPreferences = preferences;
    pluginToggle.setToggleState(preferences.pluginChainEnabled, juce::dontSendNotification);
    compensationToggle.setToggleState(preferences.latencyCompensated, juce::dontSendNotification);
    continuationToggle.setToggleState(preferences.continuePlayback, juce::dontSendNotification);
    playFromFirstCuePointToggle.setToggleState(preferences.playFromFirstCuePoint, juce::dontSendNotification);
    coloursToggle.setToggleState(preferences.seratoWaveformColours, juce::dontSendNotification);
    loudnessGraphToggle.setToggleState(preferences.loudnessGraphEnabled, juce::dontSendNotification);
    loudnessMinimum.setText(utils::format("{:.0f}", preferences.loudnessGraphMinimum), false);
    loudnessMaximum.setText(utils::format("{:.0f}", preferences.loudnessGraphMaximum), false);
    for (auto* component :
         std::initializer_list<juce::Component*> {
             &loudnessMinimum,
             &loudnessMaximum,
             &loudnessMinimumLabel,
             &loudnessMaximumLabel,
         })
    {
        component->setEnabled(preferences.loudnessGraphEnabled);
    }
}

/// Installs the message-thread callback that receives user changes to preview preferences.
void AudioSettingsComponent::setPreferencesChangedCallback(std::function<void(const AudioPreviewPreferences&)> callback)
{
    onPreferencesChanged = std::move(callback);
}

/// Forwards a complete preference snapshot so all other surfaces can update together.
void AudioSettingsComponent::preferencesChanged()
{
    const auto editedMinimum = readLoudnessLimit(loudnessMinimum.getText());
    const auto editedMaximum = readLoudnessLimit(loudnessMaximum.getText());
    auto minimum = editedMinimum.value_or(displayedPreferences.loudnessGraphMinimum);
    auto maximum = editedMaximum.value_or(displayedPreferences.loudnessGraphMaximum);
    loudnessRangeError.setText({}, juce::dontSendNotification);
    if (!editedMinimum.has_value() || !editedMaximum.has_value() || !isValidLoudnessGraphRange(minimum, maximum)) {
        minimum = displayedPreferences.loudnessGraphMinimum;
        maximum = displayedPreferences.loudnessGraphMaximum;
        loudnessMinimum.setText(utils::format("{:.0f}", minimum), false);
        loudnessMaximum.setText(utils::format("{:.0f}", maximum), false);
        loudnessRangeError.setText("Use whole LUFS (-60 to +6), with minimum < maximum.", juce::dontSendNotification);
    }
    if (onPreferencesChanged) {
        onPreferencesChanged({
            .pluginChainEnabled = pluginToggle.getToggleState(),
            .latencyCompensated = compensationToggle.getToggleState(),
            .continuePlayback = continuationToggle.getToggleState(),
            .playFromFirstCuePoint = playFromFirstCuePointToggle.getToggleState(),
            .seratoWaveformColours = coloursToggle.getToggleState(),
            .loudnessGraphEnabled = loudnessGraphToggle.getToggleState(),
            .loudnessGraphMinimum = minimum,
            .loudnessGraphMaximum = maximum,
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
    for (auto* toggle :
         {
             &pluginToggle,
             &compensationToggle,
             &continuationToggle,
             &playFromFirstCuePointToggle,
             &coloursToggle,
         })
    {
        toggle->setBounds(preferences.removeFromTop(layout::audioSettings::rowHeight));
    }
    auto loudnessRow = preferences.removeFromTop(layout::audioSettings::rowHeight);
    for (const auto& controls :
         {std::pair {&loudnessMaximumLabel, &loudnessMaximum}, std::pair {&loudnessMinimumLabel, &loudnessMinimum}})
    {
        controls.second->setBounds(loudnessRow.removeFromRight(layout::audioSettings::rangeEditorWidth)
                                       .reduced(0, layout::audioSettings::rangeEditorPadding));
        controls.first->setBounds(loudnessRow.removeFromRight(layout::audioSettings::rangeLabelWidth));
        loudnessRow.removeFromRight(layout::audioSettings::padding);
    }
    loudnessGraphToggle.setBounds(loudnessRow);
    loudnessRangeError.setBounds(preferences.removeFromTop(layout::audioSettings::rowHeight));
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
