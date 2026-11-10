/// Audio device and preview settings layout, preference serialization, and dialog-local tooltip lookup.
/// All preference entry points are synchronized by the main component rather than storing independent UI state.

#include "gui/components/AudioSettingsComponent.h"

#include "gui/AudioBatchComponentState.h"
#include "gui/GuiLayout.h"
#include "serato/SeratoTagTypes.h"
#include "utils/Log.h"
#include "utils/OptionalRef.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
constexpr auto PLUGIN_PREVIEW_KEY {"previewPluginChain"};
constexpr auto COMPENSATION_KEY {"previewLatencyCompensation"};
constexpr auto CONTINUATION_KEY {"previewContinuePlayback"};
constexpr auto PLAY_FROM_FIRST_CUE_POINT_KEY {"playFromFirstCuePoint"};
constexpr auto WAVEFORM_COLOURS_KEY {"seratoWaveformColours"};
constexpr auto LOUDNESS_GRAPH_KEY {"loudnessGraphEnabled"};
constexpr auto LOUDNESS_MINIMUM_KEY {"loudnessGraphMinimum"};
constexpr auto LOUDNESS_MAXIMUM_KEY {"loudnessGraphMaximum"};
constexpr auto ALBUM_ARTWORK_KEY {"showAlbumArtwork"};
constexpr auto PREVIEW_VIEW_MODE_KEY {"previewViewMode"};
constexpr auto SHOW_SAMPLE_OVERLOADS_KEY {"showSampleOverloads"};
constexpr auto SHOW_TRUE_PEAK_OVERLOADS_KEY {"showTruePeakOverloads"};
constexpr auto SHOW_OVERLOAD_LINES_KEY {"showOverloadLines"};
constexpr auto SHOW_GAIN_ADJUSTED_METRICS_KEY {"showGainAdjustedMetrics"};

/// Stable storage names and canonical labels in the settings selector's display order.
constexpr std::array PREVIEW_VIEW_MODES {
    PreviewViewMode::waveform,
    PreviewViewMode::spectrogram,
    PreviewViewMode::sideBySide,
    PreviewViewMode::stacked,
};
constexpr std::array PREVIEW_VIEW_NAMES {"waveform", "spectrogram", "side_by_side", "stacked"};
constexpr std::array PREVIEW_VIEW_LABELS {"Waveform", "Spectrogram", "Both: side by side", "Both: stacked"};
/// Most digits a loudness axis limit can have, enough for the whole LUFS values from -60 to +6.
constexpr int MAXIMUM_LOUDNESS_LIMIT_DIGITS {2};

/// Captions identify JUCE-owned device controls through their public label attachments.
constexpr std::array DEVICE_TOOLTIP_DESCRIPTIONS {
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
        juce::TooltipWindow(&parent, layout::TOOLTIP_DELAY_MILLISECONDS)
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
        for (const auto& [caption, description] : DEVICE_TOOLTIP_DESCRIPTIONS) {
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

/// Returns the canonical display label used by settings and menus.
juce::String previewViewModeLabel(const PreviewViewMode mode)
{
    for (std::size_t index = 0; index < PREVIEW_VIEW_MODES.size(); ++index) {
        if (PREVIEW_VIEW_MODES[index] == mode) {
            return PREVIEW_VIEW_LABELS[index];
        }
    }
    return PREVIEW_VIEW_LABELS.front();
}

/// Reads an exact stable storage name, falling back only this preference to waveform for unknown values.
PreviewViewMode previewViewModeFromName(const juce::String& name)
{
    for (std::size_t index = 0; index < PREVIEW_VIEW_NAMES.size(); ++index) {
        if (name == PREVIEW_VIEW_NAMES[index]) {
            return PREVIEW_VIEW_MODES[index];
        }
    }
    return PreviewViewMode::waveform;
}

/// Returns a stable settings value independent of the label presented to the user.
juce::String previewViewModeName(const PreviewViewMode mode)
{
    for (std::size_t index = 0; index < PREVIEW_VIEW_MODES.size(); ++index) {
        if (PREVIEW_VIEW_MODES[index] == mode) {
            return PREVIEW_VIEW_NAMES[index];
        }
    }
    return PREVIEW_VIEW_NAMES.front();
}

/// Accepts provisional integer editor text, including empty text and a leading minus before any digits.
bool isValidLoudnessGraphLimitInput(const juce::String& text)
{
    const auto digits = text.startsWithChar('-') ? text.substring(1) : text;
    return digits.length() <= MAXIMUM_LOUDNESS_LIMIT_DIGITS && (digits.isEmpty() || digits.containsOnly("0123456789"));
}

/// Requires whole-LU axis limits within the supported range and keeps at least one LU between them.
bool isValidLoudnessGraphRange(const double minimum, const double maximum)
{
    return std::isfinite(minimum) && std::isfinite(maximum)
        && minimum >= AudioPreviewPreferences::MINIMUM_LOUDNESS_GRAPH_LIMIT
        && maximum <= AudioPreviewPreferences::MAXIMUM_LOUDNESS_GRAPH_LIMIT && maximum - minimum >= 1.0
        && std::floor(minimum) >= minimum && std::floor(maximum) >= maximum;
}

/// Reads only preview-owned keys, leaving existing plugin and normalization properties intact.
AudioPreviewPreferences loadAudioPreviewPreferences(const juce::PropertiesFile& settings)
{
    AudioPreviewPreferences preferences {
        .pluginChainEnabled = settings.getBoolValue(PLUGIN_PREVIEW_KEY, false),
        .latencyCompensated = settings.getBoolValue(COMPENSATION_KEY, true),
        .continuePlayback = settings.getBoolValue(CONTINUATION_KEY, false),
        .playFromFirstCuePoint = settings.getBoolValue(PLAY_FROM_FIRST_CUE_POINT_KEY, false),
        .seratoWaveformColours = settings.getBoolValue(WAVEFORM_COLOURS_KEY, true),
        .loudnessGraphEnabled = settings.getBoolValue(LOUDNESS_GRAPH_KEY, false),
        .showAlbumArtwork = settings.getBoolValue(ALBUM_ARTWORK_KEY, true),
        .viewMode = previewViewModeFromName(settings.getValue(PREVIEW_VIEW_MODE_KEY)),
        .showSampleOverloads = settings.getBoolValue(SHOW_SAMPLE_OVERLOADS_KEY, true),
        .showTruePeakOverloads = settings.getBoolValue(SHOW_TRUE_PEAK_OVERLOADS_KEY, true),
        .showOverloadLines = settings.getBoolValue(SHOW_OVERLOAD_LINES_KEY, true),
        .showGainAdjustedMetrics = settings.getBoolValue(SHOW_GAIN_ADJUSTED_METRICS_KEY, true),
    };
    // PropertiesFile's numeric conversion accepts a numeric prefix, so parse the complete saved text.
    const auto minimum = readLoudnessLimit(settings.getValue(
        LOUDNESS_MINIMUM_KEY, utils::format("{}", AudioPreviewPreferences::DEFAULT_LOUDNESS_GRAPH_MINIMUM)
    ));
    const auto maximum = readLoudnessLimit(settings.getValue(
        LOUDNESS_MAXIMUM_KEY, utils::format("{}", AudioPreviewPreferences::DEFAULT_LOUDNESS_GRAPH_MAXIMUM)
    ));

    if (minimum.has_value() && maximum.has_value() && isValidLoudnessGraphRange(*minimum, *maximum)) {
        preferences.loudnessGraphMinimum = *minimum;
        preferences.loudnessGraphMaximum = *maximum;
    } else {
        utils::logWarn(
            "Invalid saved loudness graph range, restoring {} to {} LUFS",
            AudioPreviewPreferences::DEFAULT_LOUDNESS_GRAPH_MINIMUM,
            AudioPreviewPreferences::DEFAULT_LOUDNESS_GRAPH_MAXIMUM
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
    settings.setValue(PLUGIN_PREVIEW_KEY, preferences.pluginChainEnabled);
    settings.setValue(COMPENSATION_KEY, preferences.latencyCompensated);
    settings.setValue(CONTINUATION_KEY, preferences.continuePlayback);
    settings.setValue(PLAY_FROM_FIRST_CUE_POINT_KEY, preferences.playFromFirstCuePoint);
    settings.setValue(WAVEFORM_COLOURS_KEY, preferences.seratoWaveformColours);
    settings.setValue(LOUDNESS_GRAPH_KEY, preferences.loudnessGraphEnabled);
    settings.setValue(LOUDNESS_MINIMUM_KEY, preferences.loudnessGraphMinimum);
    settings.setValue(LOUDNESS_MAXIMUM_KEY, preferences.loudnessGraphMaximum);
    settings.setValue(ALBUM_ARTWORK_KEY, preferences.showAlbumArtwork);
    settings.setValue(PREVIEW_VIEW_MODE_KEY, previewViewModeName(preferences.viewMode));
    settings.setValue(SHOW_SAMPLE_OVERLOADS_KEY, preferences.showSampleOverloads);
    settings.setValue(SHOW_TRUE_PEAK_OVERLOADS_KEY, preferences.showTruePeakOverloads);
    settings.setValue(SHOW_OVERLOAD_LINES_KEY, preferences.showOverloadLines);
    settings.setValue(SHOW_GAIN_ADJUSTED_METRICS_KEY, preferences.showGainAdjustedMetrics);
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
/// The preview owner retains the optional markers throughout this call.
double getPreviewStartPositionSeconds(
    const AudioPreviewPreferences& preferences,
    const utils::OptionalRef<const serato::Markers> markers,
    const double durationSeconds
)
{
    if (!preferences.playFromFirstCuePoint || !markers || markers->get().cues.empty() || !std::isfinite(durationSeconds)
        || durationSeconds <= 0.0)
    {
        return 0.0;
    }
    const auto firstCue = std::ranges::min_element(markers->get().cues, {}, &serato::CuePoint::positionMilliseconds);
    const auto position = static_cast<double>(firstCue->positionMilliseconds) / utils::MILLISECONDS_PER_SECOND;
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
    viewport.setScrollBarThickness(layout::SCROLL_BAR_THICKNESS);
    addAndMakeVisible(viewport);
    content.addAndMakeVisible(deviceGroup);
    content.addAndMakeVisible(deviceSelector);
    content.addAndMakeVisible(previewGroup);
    content.addAndMakeVisible(overloadGroup);
    content.addAndMakeVisible(previewView);
    content.addAndMakeVisible(previewViewLabel);
    previewView.setName("Preview view");
    for (std::size_t index = 0; index < PREVIEW_VIEW_MODES.size(); ++index) {
        previewView.addItem(previewViewModeLabel(PREVIEW_VIEW_MODES[index]), static_cast<int>(index) + 1);
    }
    previewView.setSelectedItemIndex(0, juce::dontSendNotification);
    previewView.setTooltip("Choose the source visualization and its arrangement in the preview.");
    previewViewLabel.setTooltip(previewView.getTooltip());
    previewView.onChange = [this] { preferencesChanged(); };
    pluginToggle.setTooltip(
        "Listen through enabled plugins and the file's custom input gain. Does not normalize or write files."
    );
    compensationToggle.setTooltip(
        "Align processed audio with the file position. High-latency chains need a preparation pause after starts or "
        "seeks."
    );
    continuationToggle.setTooltip(
        "Start a newly selected file only if the previous preview was playing, and play the next file when one ends."
    );
    playFromFirstCuePointToggle.setTooltip(
        "When loading a file, start at its first cue point only if it is at or before halfway through the file. "
        "Otherwise, or if there is no cue point, start at zero."
    );
    coloursToggle.setTooltip(
        "Use Serato spectral waveform colours where available, without changing cues or playback."
    );
    gainAdjustedMetricsToggle.setTooltip(
        "Show predicted peaks and estimated loudness after custom gain. "
        "Integrated loudness uses a fitted L2 limiter model with a -0.3 dB ceiling for 0 through +6 dB gain. "
        "Other gains and short-term loudness use hard clipping at 0 dBFS. Does not measure the active plugin chain."
    );
    sampleOverloadsToggle.setTooltip("Show source sample-peak overload indicators independently of true peaks.");
    truePeakOverloadsToggle.setTooltip("Show source true-peak overload indicators independently of sample peaks.");
    overloadLinesToggle.setTooltip("Extend enabled overload indicators as vertical lines across the preview.");
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
             &gainAdjustedMetricsToggle,
             &sampleOverloadsToggle,
             &truePeakOverloadsToggle,
             &overloadLinesToggle,
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
    previewView.setText(previewViewModeLabel(preferences.viewMode), juce::dontSendNotification);
    pluginToggle.setToggleState(preferences.pluginChainEnabled, juce::dontSendNotification);
    compensationToggle.setToggleState(preferences.latencyCompensated, juce::dontSendNotification);
    continuationToggle.setToggleState(preferences.continuePlayback, juce::dontSendNotification);
    playFromFirstCuePointToggle.setToggleState(preferences.playFromFirstCuePoint, juce::dontSendNotification);
    coloursToggle.setToggleState(preferences.seratoWaveformColours, juce::dontSendNotification);
    gainAdjustedMetricsToggle.setToggleState(preferences.showGainAdjustedMetrics, juce::dontSendNotification);
    sampleOverloadsToggle.setToggleState(preferences.showSampleOverloads, juce::dontSendNotification);
    truePeakOverloadsToggle.setToggleState(preferences.showTruePeakOverloads, juce::dontSendNotification);
    overloadLinesToggle.setToggleState(preferences.showOverloadLines, juce::dontSendNotification);
    loudnessGraphToggle.setToggleState(preferences.loudnessGraphEnabled, juce::dontSendNotification);
    loudnessMinimum.setText(utils::format("{:.0f}", preferences.loudnessGraphMinimum), false);
    loudnessMaximum.setText(utils::format("{:.0f}", preferences.loudnessGraphMaximum), false);
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
    resized();
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
            .showAlbumArtwork = displayedPreferences.showAlbumArtwork,
            .viewMode = previewView.getSelectedItemIndex() >= 0
                ? PREVIEW_VIEW_MODES[static_cast<std::size_t>(previewView.getSelectedItemIndex())]
                : PreviewViewMode::waveform,
            .showSampleOverloads = sampleOverloadsToggle.getToggleState(),
            .showTruePeakOverloads = truePeakOverloadsToggle.getToggleState(),
            .showOverloadLines = overloadLinesToggle.getToggleState(),
            .showGainAdjustedMetrics = gainAdjustedMetricsToggle.getToggleState(),
        });
    }
}

/// Lets JUCE choose the device selector height and keeps the lower section reachable at small window sizes.
void AudioSettingsComponent::resized()
{
    const juce::ScopedValueSetter<bool> layoutGuard(layingOut, true);
    viewport.setBounds(getLocalBounds());
    const auto width = juce::jmax(0, getWidth() - layout::SCROLL_BAR_THICKNESS);
    const auto area = juce::Rectangle<int>(0, 0, width, layout::audioSettings::PREFERRED_HEIGHT)
                          .reduced(layout::audioSettings::PADDING);
    deviceSelector.setBounds(
        area.getX() + layout::audioSettings::PADDING,
        area.getY() + layout::audioSettings::GROUP_TOP_INSET,
        juce::jmax(0, area.getWidth() - layout::audioSettings::PADDING * 2),
        juce::jmax(1, deviceSelector.getHeight())
    );
    deviceGroup.setBounds(
        area.getX(),
        area.getY(),
        area.getWidth(),
        deviceSelector.getBottom() - area.getY() + layout::audioSettings::PADDING
    );
    const auto groupTop = deviceGroup.getBottom() + layout::audioSettings::SECTION_SPACING;
    const auto errorHeight = loudnessRangeError.getText().isNotEmpty() ? layout::audioSettings::ROW_HEIGHT : 0;
    previewGroup.setBounds(
        area.getX(),
        groupTop,
        area.getWidth(),
        layout::audioSettings::PREFERENCES_HEIGHT + layout::audioSettings::ROW_HEIGHT + errorHeight
    );
    auto preferences = previewGroup.getBounds().reduced(layout::audioSettings::PADDING, 0);
    preferences.removeFromTop(layout::audioSettings::GROUP_TOP_INSET);
    auto viewRow = preferences.removeFromTop(layout::audioSettings::ROW_HEIGHT);
    previewViewLabel.setBounds(viewRow.removeFromLeft(layout::audioSettings::RANGE_LABEL_WIDTH * 2));
    const auto selectorWidth
        = juce::roundToInt(viewRow.getWidth() * layout::audioSettings::PREVIEW_SELECTOR_WIDTH_PROPORTION);
    previewView.setBounds(viewRow.withWidth(selectorWidth).reduced(0, layout::audioSettings::RANGE_EDITOR_PADDING));
    for (auto* toggle :
         {
             &pluginToggle,
             &compensationToggle,
             &continuationToggle,
             &playFromFirstCuePointToggle,
             &coloursToggle,
             &gainAdjustedMetricsToggle,
         })
    {
        toggle->setBounds(preferences.removeFromTop(layout::audioSettings::ROW_HEIGHT));
    }
    auto loudnessRow = preferences.removeFromTop(layout::audioSettings::ROW_HEIGHT);
    for (const auto& controls :
         {std::pair {&loudnessMaximumLabel, &loudnessMaximum}, std::pair {&loudnessMinimumLabel, &loudnessMinimum}})
    {
        controls.second->setBounds(loudnessRow.removeFromRight(layout::audioSettings::RANGE_EDITOR_WIDTH)
                                       .reduced(0, layout::audioSettings::RANGE_EDITOR_PADDING));
        controls.first->setBounds(loudnessRow.removeFromRight(layout::audioSettings::RANGE_LABEL_WIDTH));
        loudnessRow.removeFromRight(layout::audioSettings::PADDING);
    }
    loudnessGraphToggle.setBounds(loudnessRow);
    loudnessRangeError.setVisible(errorHeight > 0);
    loudnessRangeError.setBounds(preferences.removeFromTop(errorHeight));
    overloadGroup.setBounds(
        area.getX(),
        previewGroup.getBottom() + layout::audioSettings::SECTION_SPACING,
        area.getWidth(),
        layout::audioSettings::OVERLOAD_GROUP_HEIGHT
    );
    auto overloads = overloadGroup.getBounds().reduced(layout::audioSettings::PADDING, 0);
    overloads.removeFromTop(layout::audioSettings::GROUP_TOP_INSET);
    auto overloadRow = overloads.removeFromTop(layout::audioSettings::ROW_HEIGHT);
    sampleOverloadsToggle.setBounds(overloadRow.removeFromLeft(overloadRow.getWidth() / 2));
    truePeakOverloadsToggle.setBounds(overloadRow);
    overloadLinesToggle.setBounds(overloads.removeFromTop(layout::audioSettings::ROW_HEIGHT));
    content.setSize(width, overloadGroup.getBottom() + layout::audioSettings::PADDING);
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
