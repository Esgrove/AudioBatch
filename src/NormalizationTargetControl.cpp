/// Implementation of NormalizationTargetControl:
/// configuring the mode selector and slider, switching the slider range by mode, and persisting the settings.

#include "NormalizationTargetControl.h"

#include "StringFormat.h"

/// Slider steps, combo box ids, and setting keys of the normalization target control.
/// The target ranges come from AudioNormalizationOptions, which the CLI validates against too.
namespace normalize_target
{
/// Step of the target sample peak, in dB.
constexpr double peakStepDecibels = 0.1;
/// Step of the target loudness, in LU.
constexpr double loudnessStepDecibels = 0.5;
/// Combo box item id of the peak mode, which must be non-zero.
constexpr int peakModeItemId = 1;
/// Combo box item id of the loudness mode.
constexpr int loudnessModeItemId = 2;
/// Gap in pixels between the mode selector and the slider.
constexpr int controlGap = 6;
constexpr auto modeSettingKey = "normalizeMode";
constexpr auto targetPeakSettingKey = "normalizeTargetPeakDbfs";
constexpr auto targetLufsSettingKey = "normalizeTargetLufs";
}  // namespace normalize_target

NormalizationTargetControl::NormalizationTargetControl(juce::ApplicationProperties& applicationProperties) :
    properties(applicationProperties)
{
    modeBox.addItem("Peak", normalize_target::peakModeItemId);
    modeBox.addItem("Loudness", normalize_target::loudnessModeItemId);
    modeBox.setSelectedId(normalize_target::peakModeItemId, juce::dontSendNotification);
    modeBox.setTooltip(
        "Normalize to a sample peak level, or to an integrated loudness limited to a -0.3 dBTP true peak."
    );
    modeBox.onChange = [this] {
        updateSlider();
        saveSettings();
    };
    addAndMakeVisible(modeBox);

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
    modeBox.setSelectedId(
        loudness ? normalize_target::loudnessModeItemId : normalize_target::peakModeItemId, juce::dontSendNotification
    );
    updateSlider();
}

AudioNormalizationOptions NormalizationTargetControl::getOptions() const
{
    AudioNormalizationOptions options;
    options.mode = isLoudnessMode() ? AudioNormalizationMode::loudness : AudioNormalizationMode::peak;
    options.targetPeakDbfs = targetPeakDbfs;
    options.targetLufs = targetLufs;
    return options;
}

juce::String NormalizationTargetControl::describeTarget() const
{
    if (isLoudnessMode()) {
        return utils::format("{:.1f} LUFS", targetLufs);
    }

    return utils::format("{:.1f} dBFS peak", targetPeakDbfs);
}

void NormalizationTargetControl::resized()
{
    auto bounds = getLocalBounds();
    modeBox.setBounds(bounds.removeFromLeft((bounds.getWidth() - normalize_target::controlGap) / 2));
    bounds.removeFromLeft(normalize_target::controlGap);
    targetSlider.setBounds(bounds);
}

bool NormalizationTargetControl::isLoudnessMode() const
{
    return modeBox.getSelectedId() == normalize_target::loudnessModeItemId;
}

void NormalizationTargetControl::saveSettings()
{
    auto* settings = properties.getUserSettings();
    settings->setValue(normalize_target::modeSettingKey, isLoudnessMode() ? "loudness" : "peak");
    settings->setValue(normalize_target::targetPeakSettingKey, targetPeakDbfs);
    settings->setValue(normalize_target::targetLufsSettingKey, targetLufs);
}

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
