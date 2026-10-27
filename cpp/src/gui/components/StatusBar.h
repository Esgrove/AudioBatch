/// Classic desktop status bar along the bottom of the main window.
/// Declares StatusBar, which shows the root folders, the background activity or the list summary,
/// and the preview audio output device, with the overall progress of a running pass along its top edge.

#pragma once

#include "gui/AudioPreviewPlayer.h"

#include <JuceHeader.h>

namespace audiobatch
{
[[nodiscard]] juce::String formatAudioOutputStatus(const AudioOutputStatus& status);

/// Bottom bar of the main window.
/// The root path on the left and the status text share the width left of the output device,
/// with the larger part for the status text,
/// and the audio output device sits at the right end, dimmed while the device is closed.
/// The overall progress of a running pass is drawn as a thin line along the top edge.
class StatusBar final : public juce::Component
{
public:
    StatusBar();

    void setPath(const juce::String& text, const juce::String& tooltip);
    void setStatusText(const juce::String& text);
    void setOutputStatus(const AudioOutputStatus& status);
    void setProgress(int permille);

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    [[nodiscard]] juce::Rectangle<int> getProgressBounds() const;

    /// The root folders of the list.
    juce::Label pathLabel {"Root folders"};
    /// What the application is doing, or a summary of the list.
    juce::Label statusLabel {"Status"};
    /// The preview output device and its configuration.
    juce::Label outputLabel {"Audio output"};

    /// Overall progress in tenths of a percent, or -1 when no pass of known size runs.
    int progressPermille {-1};
};
}  // namespace audiobatch
