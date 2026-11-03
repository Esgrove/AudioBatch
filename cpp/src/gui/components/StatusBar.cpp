/// Implementation of StatusBar: the label styling, the section layout with the output device sized to its text,
/// the separators and the progress line, and the audio output summary text.

#include "gui/components/StatusBar.h"

#include "gui/AudioPreviewPlayer.h"
#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "utils/GuiUtils.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace audiobatch
{
namespace
{
/// Converts a sample rate in hertz to kilohertz.
constexpr double hertzPerKilohertz {1000.0};
/// Converts the stored tenths of a percent to a fraction.
constexpr float permillePerUnit {1000.0f};
/// Opacity of the unfilled track behind the progress line.
constexpr float progressTrackAlpha {0.25f};
/// Extra width for the output device text, so rounding never makes the label truncate it.
constexpr int outputTextSlack {2};
}  // namespace

/// Returns the device name with the sample rate and buffer size, for example "Speakers, 48 kHz, 512 samples".
/// Values that are not known yet are left out, and a missing name falls back to the system default output,
/// which is what the preview opens before a device has been chosen.
juce::String formatAudioOutputStatus(const AudioOutputStatus& status)
{
    juce::StringArray parts;
    parts.add(status.deviceName.isNotEmpty() ? status.deviceName : juce::String("Default output"));

    if (status.sampleRate > 0.0) {
        parts.add(utils::format("{:g} kHz", status.sampleRate / hertzPerKilohertz));
    }

    if (status.bufferSize > 0) {
        parts.add(utils::format("{} samples", status.bufferSize));
    }

    return parts.joinIntoString(", ");
}

/// Formats both source times with hours when the total reaches one hour, otherwise as minutes and seconds.
juce::String formatPlaybackTime(const double positionSeconds, const double durationSeconds)
{
    constexpr auto MAXIMUM_DISPLAY_SECONDS = 999'999'999.0;
    const auto duration
        = std::isfinite(durationSeconds) ? std::clamp(durationSeconds, 0.0, MAXIMUM_DISPLAY_SECONDS) : 0.0;
    const auto position = std::isfinite(positionSeconds) ? std::clamp(positionSeconds, 0.0, duration) : 0.0;
    constexpr std::int64_t SECONDS_PER_MINUTE {60};
    const auto current = static_cast<std::int64_t>(std::floor(position));
    const auto total = static_cast<std::int64_t>(std::floor(duration));
    constexpr std::int64_t SECONDS_PER_HOUR {3600};
    if (total >= SECONDS_PER_HOUR) {
        return utils::format(
            "{}:{:02}:{:02} / {}:{:02}:{:02}",
            current / SECONDS_PER_HOUR,
            (current / SECONDS_PER_MINUTE) % SECONDS_PER_MINUTE,
            current % SECONDS_PER_MINUTE,
            total / SECONDS_PER_HOUR,
            (total / SECONDS_PER_MINUTE) % SECONDS_PER_MINUTE,
            total % SECONDS_PER_MINUTE
        );
    }
    return utils::format(
        "{:02}:{:02} / {:02}:{:02}",
        current / SECONDS_PER_MINUTE,
        current % SECONDS_PER_MINUTE,
        total / SECONDS_PER_MINUTE,
        total % SECONDS_PER_MINUTE
    );
}

/// Creates the four section labels with the status bar font.
/// The output device starts as the closed default output until the owner reports the real device.
StatusBar::StatusBar()
{
    const auto font = CustomLookAndFeel::getTextFont().withHeight(layout::statusBar::fontHeight);

    for (auto* label : {&pathLabel, &statusLabel, &playbackLabel, &outputLabel}) {
        utils::styleLabel(*label, font, CustomLookAndFeel::greyMiddleLight());
        label->setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(*label);
    }

    statusLabel.setTooltip("Background analysis and normalization status.");
    playbackLabel.setVisible(false);
    setOutputStatus({});
}

/// Shows the root folder text on the left, with the tooltip listing every root.
void StatusBar::setPath(const juce::String& text, const juce::String& tooltip)
{
    pathLabel.setText(text, juce::dontSendNotification);
    pathLabel.setTooltip(tooltip);
}

/// Updates the independently visible clock only when its displayed second or visibility changes.
void StatusBar::setPlaybackTime(const double positionSeconds, const double durationSeconds, const bool visible)
{
    const auto text = visible ? formatPlaybackTime(positionSeconds, durationSeconds) : juce::String();
    if (playbackLabel.getText() == text && playbackLabel.isVisible() == visible) {
        return;
    }
    playbackLabel.setText(text, juce::dontSendNotification);
    playbackLabel.setVisible(visible);
    resized();
    repaint();
}

/// Shows the progress or summary of the background activity, or a transient message.
void StatusBar::setStatusText(const juce::String& text)
{
    statusLabel.setText(text, juce::dontSendNotification);
}

/// Shows the output device at the right end, dimmed while it is closed,
/// and resizes its section to fit the new text.
void StatusBar::setOutputStatus(const AudioOutputStatus& status)
{
    const auto text = formatAudioOutputStatus(status);
    outputLabel.setText(text, juce::dontSendNotification);
    outputLabel.setColour(
        juce::Label::textColourId, status.open ? CustomLookAndFeel::greyMiddleLight() : CustomLookAndFeel::greyMiddle()
    );
    outputLabel.setTooltip(
        status.open ? utils::format("Preview audio output: {}", text)
                    : utils::format("Preview audio output: {}\nThe device opens when preview playback starts.", text)
    );
    resized();
}

/// Stores the overall progress in tenths of a percent, or -1 to hide the line.
/// Only the progress strip is repainted, and only when the value changes.
void StatusBar::setProgress(const int permille)
{
    if (permille == progressPermille) {
        return;
    }

    progressPermille = permille;
    repaint(getProgressBounds());
}

/// Fills the bar a step lighter than the window, draws the top border and the section separators,
/// and draws the progress line over the top border while a pass of known size runs.
void StatusBar::paint(juce::Graphics& graphics)
{
    graphics.fillAll(CustomLookAndFeel::greySemiDark());

    graphics.setColour(CustomLookAndFeel::greyMedium());
    graphics.fillRect(getLocalBounds().withHeight(1));

    const auto separatorHeight = getHeight() - layout::statusBar::separatorInset * 2;
    for (const auto* label : {&statusLabel, &playbackLabel, &outputLabel}) {
        if (!label->isVisible()) {
            continue;
        }
        graphics.fillRect(
            label->getX() - layout::statusBar::sectionSpacing / 2, layout::statusBar::separatorInset, 1, separatorHeight
        );
    }

    if (progressPermille < 0) {
        return;
    }

    const auto bar = getProgressBounds().toFloat();
    graphics.setColour(CustomLookAndFeel::greyMiddle().withAlpha(progressTrackAlpha));
    graphics.fillRect(bar);
    graphics.setColour(CustomLookAndFeel::blue());
    graphics.fillRect(bar.withWidth(bar.getWidth() * static_cast<float>(progressPermille) / permillePerUnit));
}

/// Gives the output device the width of its text up to a maximum,
/// and splits the rest between the root path and the larger status text section.
/// Both truncate their text with an ellipsis when it does not fit.
void StatusBar::resized()
{
    auto bounds = getLocalBounds().reduced(layout::statusBar::horizontalPadding, 0);

    const auto outputTextWidth
        = juce::roundToInt(std::ceil(juce::TextLayout::getStringWidth(outputLabel.getFont(), outputLabel.getText())));
    outputLabel.setBounds(bounds.removeFromRight(
        juce::jmin(layout::statusBar::outputStatusMaximumWidth, outputTextWidth + outputTextSlack)
    ));
    bounds.removeFromRight(layout::statusBar::sectionSpacing);
    if (playbackLabel.isVisible()) {
        const auto timeWidth = juce::roundToInt(
            std::ceil(juce::TextLayout::getStringWidth(playbackLabel.getFont(), playbackLabel.getText()))
        );
        playbackLabel.setBounds(bounds.removeFromRight(std::min(bounds.getWidth(), timeWidth + outputTextSlack)));
        bounds.removeFromRight(std::min(layout::statusBar::sectionSpacing, bounds.getWidth()));
    }
    const auto pathWidth = juce::roundToInt(bounds.getWidth() * layout::statusBar::pathProportion);
    pathLabel.setBounds(bounds.removeFromLeft(pathWidth));
    bounds.removeFromLeft(layout::statusBar::sectionSpacing);
    statusLabel.setBounds(bounds);
}

/// Returns the strip along the top edge where the progress line is drawn.
juce::Rectangle<int> StatusBar::getProgressBounds() const
{
    return getLocalBounds().withHeight(layout::statusBar::progressHeight);
}
}  // namespace audiobatch
