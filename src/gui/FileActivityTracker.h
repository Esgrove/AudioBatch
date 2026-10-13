/// Per-file activity state shown in the GUI results table.
/// Declares FileActivityTracker, which holds the activity of each busy file,
/// such as waiting, analyzing, or normalizing, together with its progress,
/// the outcome message of each file's last operation,
/// and the timer that animates the activity indicators while any file is busy.

#pragma once

#include "gui/AudioFileTableModel.h"
#include <initializer_list>

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <optional>

namespace audiobatch
{
/// Tracks which files are busy, how far along they are, and what their last operation reported, keyed by full path.
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
    void clearActiveWithActivities(std::initializer_list<FileActivity> activities);
    void clearAllActive();
    void clearAllMessages();
    void clearMessage(const juce::String& fullPath);
    void markActive(const juce::Array<juce::File>& files, FileActivity activity);
    void setActive(const juce::String& fullPath, FileActivity activity);
    void setMessage(const juce::String& fullPath, const OperationMessage& message);
    void setProgress(const juce::String& fullPath, float progress);

    [[nodiscard]] OperationMessage getMessage(const juce::String& fullPath) const;
    [[nodiscard]] bool isAnyActive() const;
    [[nodiscard]] juce::String getActiveLabel(const juce::String& fullPath) const;
    [[nodiscard]] std::optional<FileActivity> getActivity(const juce::String& fullPath) const;
    [[nodiscard]] std::optional<FileActivityState> getActivityState(const juce::String& fullPath) const;
    [[nodiscard]] float getProgressTotal() const;
    [[nodiscard]] static float getAnimationPhase();

private:
    void activityChanged();
    void timerCallback() override;

    std::function<void()> repaint;
    std::map<juce::String, OperationMessage> messages;
    std::map<juce::String, FileActivityState> activeStates;
};

juce::String getFileActivityLabel(FileActivity activity);
}  // namespace audiobatch
