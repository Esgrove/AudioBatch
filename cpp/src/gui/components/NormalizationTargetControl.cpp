/// Implementation of NormalizationTargetControl:
/// configuring the mode buttons, the slider, and the Normalize button,
/// switching the slider range by mode, and persisting the settings.

#include "gui/components/NormalizationTargetControl.h"

#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiControlIds.h"
#include "gui/GuiLayout.h"
#include "normalization/AudioNormalization.h"
#include "utils/StringFormat.h"

#include <functional>
#include <utility>

namespace audiobatch
{
/// Slider steps, the radio group of the mode buttons, and setting keys of the normalization target control.
/// The target ranges come from AudioNormalizationOptions, which the CLI validates against too.
namespace normalize_target
{
/// Step of the target sample peak, in dB.
constexpr double PEAK_STEP_DECIBELS {0.1};
/// Step of the target loudness, in LU.
constexpr double LOUDNESS_STEP_DECIBELS {0.5};
/// Radio group of the Peak and Loudness buttons, which must be non-zero.
constexpr int MODE_RADIO_GROUP_ID {1};
/// Opacity of the grey filled part of the target slider.
/// The fill is neutral, so it does not compete with the blue of the chosen mode right above it.
constexpr float TARGET_FILL_ALPHA {0.7f};
constexpr auto MODE_SETTING_KEY {"normalizeMode"};
constexpr auto TARGET_PEAK_SETTING_KEY {"normalizeTargetPeakDbfs"};
constexpr auto TARGET_LUFS_SETTING_KEY {"normalizeTargetLufs"};
}  // namespace normalize_target

/// Creates the controls. The settings are read later by loadSettings,
/// so the properties can be configured after this control is constructed.
/// The mode buttons do not toggle themselves on click.
/// A self-toggling radio button sends a click to the button it turns off,
/// whose handler would turn that button back on and leave both buttons on.
/// selectMode sets the toggle state instead, without notifying the other button.
NormalizationTargetControl::NormalizationTargetControl(juce::ApplicationProperties& applicationProperties) :
    properties(applicationProperties)
{
    for (auto* button : {&peakButton, &loudnessButton}) {
        button->setRadioGroupId(normalize_target::MODE_RADIO_GROUP_ID);
        addAndMakeVisible(*button);
    }
    peakButton.setConnectedEdges(juce::Button::ConnectedOnRight);
    loudnessButton.setConnectedEdges(juce::Button::ConnectedOnLeft);
    peakButton.setTooltip("Normalize to a sample peak level.");
    loudnessButton.setTooltip("Normalize to an integrated loudness limited to a -0.3 dBTP true peak.");
    peakButton.setToggleState(true, juce::dontSendNotification);
    peakButton.onClick = [this] { selectMode(false); };
    loudnessButton.onClick = [this] { selectMode(true); };

    normalizeButton.setTooltip("Normalize the selected files to the target.");
    addAndMakeVisible(normalizeButton);

    peakButton.setComponentID(control_ids::NORMALIZE_PEAK);
    loudnessButton.setComponentID(control_ids::NORMALIZE_LOUDNESS);
    normalizeButton.setComponentID(control_ids::NORMALIZE_SELECTED);
    targetSlider.setComponentID(control_ids::NORMALIZE_TARGET);
    targetSlider.setTitle(control_titles::NORMALIZE_TARGET);

    targetSlider.setSliderStyle(juce::Slider::LinearBar);
    targetSlider.setColour(
        juce::Slider::trackColourId, CustomLookAndFeel::greyMiddle().withAlpha(normalize_target::TARGET_FILL_ALPHA)
    );
    targetSlider.setNumDecimalPlacesToDisplay(1);
    targetSlider.setTooltip("Target level for normalization.");
    targetSlider.onValueChange = [this] {
        if (isLoudnessMode()) {
            targetLufs = targetSlider.getValue();
        } else {
            targetPeakDbfs = targetSlider.getValue();
        }
        saveSettings();
    };
    addAndMakeVisible(targetSlider);
    updateSlider();
}

/// Reads the mode and both targets from the user settings into the controls.
void NormalizationTargetControl::loadSettings()
{
    const auto* settings = properties.getUserSettings();
    targetPeakDbfs = juce::jlimit(
        AudioNormalizationOptions::MINIMUM_TARGET_PEAK_DBFS,
        AudioNormalizationOptions::MAXIMUM_TARGET_PEAK_DBFS,
        settings->getDoubleValue(
            normalize_target::TARGET_PEAK_SETTING_KEY, AudioNormalizationOptions::DEFAULT_TARGET_PEAK_DBFS
        )
    );
    targetLufs = juce::jlimit(
        AudioNormalizationOptions::MINIMUM_TARGET_LUFS,
        AudioNormalizationOptions::MAXIMUM_TARGET_LUFS,
        settings->getDoubleValue(
            normalize_target::TARGET_LUFS_SETTING_KEY, AudioNormalizationOptions::DEFAULT_TARGET_LUFS
        )
    );

    const auto loudness = settings->getValue(normalize_target::MODE_SETTING_KEY) == "loudness";
    (loudness ? loudnessButton : peakButton).setToggleState(true, juce::dontSendNotification);
    updateSlider();
}

/// Returns the normalization mode and target selected in the controls.
/// The caller adds the overwrite setting.
AudioNormalizationOptions NormalizationTargetControl::getOptions() const
{
    AudioNormalizationOptions options;
    options.mode = isLoudnessMode() ? AudioNormalizationMode::loudness : AudioNormalizationMode::peak;
    options.targetPeakDbfs = targetPeakDbfs;
    options.targetLufs = targetLufs;
    return options;
}

/// Returns the selected target as display text, for example "-14.0 LUFS" or "0.0 dBFS peak".
juce::String NormalizationTargetControl::describeTarget() const
{
    if (isLoudnessMode()) {
        return utils::format("{:.1f} LUFS", targetLufs);
    }

    return utils::format("{:.1f} dBFS peak", targetPeakDbfs);
}

/// Lays out the mode buttons in two equal halves of the top row,
/// and the target slider and the Normalize button in two equal halves of the bottom row,
/// with the gap between them centred under the seam of the mode buttons.
void NormalizationTargetControl::resized()
{
    auto bounds = getLocalBounds();
    auto modeRow = bounds.removeFromTop(layout::CONTROL_BUTTON_HEIGHT);
    peakButton.setBounds(modeRow.removeFromLeft(modeRow.getWidth() / 2));
    loudnessButton.setBounds(modeRow);
    bounds.removeFromTop(layout::NORMALIZE_TARGET_SPACING);
    auto sliderRow = bounds.removeFromTop(layout::CONTROL_BUTTON_HEIGHT);
    targetSlider.setBounds(sliderRow.removeFromLeft((sliderRow.getWidth() - layout::SECTION_SPACING) / 2));
    sliderRow.removeFromLeft(layout::SECTION_SPACING);
    normalizeButton.setBounds(sliderRow);
}

/// Enables or disables the Normalize button, which the owner keeps in step with the selection.
void NormalizationTargetControl::setNormalizeEnabled(const bool enabled)
{
    normalizeButton.setEnabled(enabled);
}

/// Enables or disables the mode buttons and the target slider, but not the Normalize button,
/// which the owner keeps in step with the option that normalizes before the plugins.
void NormalizationTargetControl::setTargetEnabled(const bool enabled)
{
    peakButton.setEnabled(enabled);
    loudnessButton.setEnabled(enabled);
    targetSlider.setEnabled(enabled);
}

/// Sets the callback the Normalize button calls to normalize the selected files to the target.
void NormalizationTargetControl::setNormalizeCallback(std::function<void()> callback)
{
    normalizeButton.onClick = std::move(callback);
}

/// Returns true when loudness mode is selected.
bool NormalizationTargetControl::isLoudnessMode() const
{
    return loudnessButton.getToggleState();
}

/// Switches to the clicked mode, showing that mode's remembered target.
void NormalizationTargetControl::selectMode(const bool loudness)
{
    (loudness ? loudnessButton : peakButton).setToggleState(true, juce::dontSendNotification);
    updateSlider();
    saveSettings();
}

/// Writes the mode and both targets to the user settings.
void NormalizationTargetControl::saveSettings() const
{
    auto* settings = properties.getUserSettings();
    settings->setValue(normalize_target::MODE_SETTING_KEY, isLoudnessMode() ? "loudness" : "peak");
    settings->setValue(normalize_target::TARGET_PEAK_SETTING_KEY, targetPeakDbfs);
    settings->setValue(normalize_target::TARGET_LUFS_SETTING_KEY, targetLufs);
}

/// Switches the slider between the peak and loudness ranges for the selected mode,
/// showing the remembered target of that mode.
void NormalizationTargetControl::updateSlider()
{
    if (isLoudnessMode()) {
        targetSlider.setRange(
            AudioNormalizationOptions::MINIMUM_TARGET_LUFS,
            AudioNormalizationOptions::MAXIMUM_TARGET_LUFS,
            normalize_target::LOUDNESS_STEP_DECIBELS
        );
        targetSlider.setTextValueSuffix(" LUFS");
        targetSlider.setValue(targetLufs, juce::dontSendNotification);
    } else {
        targetSlider.setRange(
            AudioNormalizationOptions::MINIMUM_TARGET_PEAK_DBFS,
            AudioNormalizationOptions::MAXIMUM_TARGET_PEAK_DBFS,
            normalize_target::PEAK_STEP_DECIBELS
        );
        targetSlider.setTextValueSuffix(" dBFS");
        targetSlider.setValue(targetPeakDbfs, juce::dontSendNotification);
    }
}
}  // namespace audiobatch
