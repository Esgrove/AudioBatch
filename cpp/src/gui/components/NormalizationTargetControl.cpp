/// Implementation of NormalizationTargetControl:
/// configuring the mode buttons, the slider, and the Normalize button,
/// switching the slider range by mode, and persisting the settings.

#include "gui/components/NormalizationTargetControl.h"

#include "gui/GuiLayout.h"
#include "utils/StringFormat.h"

namespace audiobatch
{
/// Slider steps, the radio group of the mode buttons, and setting keys of the normalization target control.
/// The target ranges come from AudioNormalizationOptions, which the CLI validates against too.
namespace normalize_target
{
/// Step of the target sample peak, in dB.
constexpr double peakStepDecibels {0.1};
/// Step of the target loudness, in LU.
constexpr double loudnessStepDecibels {0.5};
/// Radio group of the Peak and Loudness buttons, which must be non-zero.
constexpr int modeRadioGroupId {1};
/// Share of the slider row the Normalize button takes.
constexpr float normalizeButtonProportion {0.4f};
constexpr auto modeSettingKey {"normalizeMode"};
constexpr auto targetPeakSettingKey {"normalizeTargetPeakDbfs"};
constexpr auto targetLufsSettingKey {"normalizeTargetLufs"};
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
        button->setRadioGroupId(normalize_target::modeRadioGroupId);
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

    targetSlider.setSliderStyle(juce::Slider::LinearBar);
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
        AudioNormalizationOptions::minimumTargetPeakDbfs,
        AudioNormalizationOptions::maximumTargetPeakDbfs,
        settings->getDoubleValue(
            normalize_target::targetPeakSettingKey, AudioNormalizationOptions::defaultTargetPeakDbfs
        )
    );
    targetLufs = juce::jlimit(
        AudioNormalizationOptions::minimumTargetLufs,
        AudioNormalizationOptions::maximumTargetLufs,
        settings->getDoubleValue(normalize_target::targetLufsSettingKey, AudioNormalizationOptions::defaultTargetLufs)
    );

    const auto loudness = settings->getValue(normalize_target::modeSettingKey) == "loudness";
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
/// and the target slider with the Normalize button on its right in the bottom row.
void NormalizationTargetControl::resized()
{
    auto bounds = getLocalBounds();
    auto modeRow = bounds.removeFromTop(layout::controlButtonHeight);
    peakButton.setBounds(modeRow.removeFromLeft(modeRow.getWidth() / 2));
    loudnessButton.setBounds(modeRow);
    bounds.removeFromTop(layout::normalizeTargetSpacing);
    auto sliderRow = bounds.removeFromTop(layout::controlButtonHeight);
    normalizeButton.setBounds(sliderRow.removeFromRight(
        juce::roundToInt(static_cast<float>(sliderRow.getWidth()) * normalize_target::normalizeButtonProportion)
    ));
    sliderRow.removeFromRight(layout::sectionSpacing);
    targetSlider.setBounds(sliderRow);
}

void NormalizationTargetControl::setNormalizeEnabled(const bool enabled)
{
    normalizeButton.setEnabled(enabled);
}

void NormalizationTargetControl::setTargetEnabled(const bool enabled)
{
    peakButton.setEnabled(enabled);
    loudnessButton.setEnabled(enabled);
    targetSlider.setEnabled(enabled);
}

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
    settings->setValue(normalize_target::modeSettingKey, isLoudnessMode() ? "loudness" : "peak");
    settings->setValue(normalize_target::targetPeakSettingKey, targetPeakDbfs);
    settings->setValue(normalize_target::targetLufsSettingKey, targetLufs);
}

/// Switches the slider between the peak and loudness ranges for the selected mode,
/// showing the remembered target of that mode.
void NormalizationTargetControl::updateSlider()
{
    if (isLoudnessMode()) {
        targetSlider.setRange(
            AudioNormalizationOptions::minimumTargetLufs,
            AudioNormalizationOptions::maximumTargetLufs,
            normalize_target::loudnessStepDecibels
        );
        targetSlider.setTextValueSuffix(" LUFS");
        targetSlider.setValue(targetLufs, juce::dontSendNotification);
    } else {
        targetSlider.setRange(
            AudioNormalizationOptions::minimumTargetPeakDbfs,
            AudioNormalizationOptions::maximumTargetPeakDbfs,
            normalize_target::peakStepDecibels
        );
        targetSlider.setTextValueSuffix(" dBFS");
        targetSlider.setValue(targetPeakDbfs, juce::dontSendNotification);
    }
}
}  // namespace audiobatch
