/// Per-file activity state shown in the GUI results table.
/// Declares FileActivityTracker, which holds the transient activity label of each busy file,
/// such as "Analyzing" or "Normalizing", the outcome message of each file's last operation,
/// and the timer that animates the activity indicators while any file is busy.

#pragma once

#include "gui/AudioFileTableModel.h"

#include <JuceHeader.h>

#include <functional>
#include <map>

namespace audiobatch
{
/// Tracks which files are busy and what their last operation reported, keyed by full path.
/// Runs a repaint timer while any file is busy and stops it once all files are idle.
/// Must only be used on the message thread.
class FileActivityTracker final : juce::Timer
{
public:
    explicit FileActivityTracker(std::function<void()> repaintCallback);

    ~FileActivityTracker() override;

    FileActivityTracker(const FileActivityTracker&) = delete;
    FileActivityTracker& operator=(const FileActivityTracker&) = delete;
    FileActivityTracker(FileActivityTracker&&) = delete;
    FileActivityTracker& operator=(FileActivityTracker&&) = delete;

    void clearActive(const juce::String& fullPath);
    void clearActiveWithLabels(const juce::StringArray& activityLabels);
    void clearAllActive();
    void clearAllMessages();
    void clearMessage(const juce::String& fullPath);
    void markActive(const juce::Array<juce::File>& files, const juce::String& activityLabel);
    void setActive(const juce::String& fullPath, const juce::String& activityLabel);
    void setMessage(const juce::String& fullPath, const OperationMessage& message);

    [[nodiscard]] OperationMessage getMessage(const juce::String& fullPath) const;
    [[nodiscard]] bool isAnyActive() const;
    [[nodiscard]] juce::String getActiveLabel(const juce::String& fullPath) const;
    [[nodiscard]] static float getAnimationPhase();

private:
    void activityChanged();
    void timerCallback() override;

    std::function<void()> repaint;
    std::map<juce::String, OperationMessage> messages;
    std::map<juce::String, juce::String> activeLabels;
};
}  // namespace audiobatch
