/// Classic desktop status bar along the bottom of the main window.
/// Declares StatusBar, which shows the root folders, the background activity or the list summary,
/// the source playback clock, and the audio output device.
/// Overall foreground progress remains a separate strip along the top edge.

#pragma once

#include "gui/AudioPreviewPlayer.h"

#include <JuceHeader.h>

namespace audiobatch
{
[[nodiscard]] juce::String formatAudioOutputStatus(const AudioOutputStatus& status);
[[nodiscard]] juce::String formatPlaybackTime(double positionSeconds, double durationSeconds);

/// Bottom bar of the main window.
/// The root path on the left and the status text share the width left of the output device,
/// with the larger part for the status text,
/// and the audio output device sits at the right end, dimmed while the device is closed.
/// The overall progress of a running pass is drawn as a thin line along the top edge.
class StatusBar final : public juce::Component
{
public:
    StatusBar();

    void setOutputStatus(const AudioOutputStatus& status);
    void setPath(const juce::String& text, const juce::String& tooltip);
    void setPlaybackTime(double positionSeconds, double durationSeconds, bool visible);
    void setProgress(int permille);
    void setStatusText(const juce::String& text);

    // juce::Component
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    [[nodiscard]] juce::Rectangle<int> getProgressBounds() const;

    /// The preview output device and its configuration.
    juce::Label outputLabel {"Audio output"};
    /// The root folders of the list.
    juce::Label pathLabel {"Root folders"};
    /// Current playback time and total source duration, independent of background work text.
    juce::Label playbackLabel {"Playback time"};
    /// What the application is doing, or a summary of the list.
    juce::Label statusLabel {"Status"};

    /// Overall progress in tenths of a percent, or -1 when no pass of known size runs.
    int progressPermille {-1};
};
}  // namespace audiobatch
