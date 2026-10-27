/// Per-file activity state shown in the GUI results table.
/// Declares FileActivityTracker, which holds the activity of each busy file,
/// such as waiting, analyzing, or normalizing, together with its progress,
/// the outcome message of each file's last operation,
/// and the display-synchronized updates that animate the indicators while any file is busy.

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
/// Requests display-synchronized repaints while any file is busy and stops once all files are idle.
/// Must only be used on the message thread.
class FileActivityTracker final
{
public:
    FileActivityTracker(juce::Component& component, std::function<void()> repaintCallback);

    ~FileActivityTracker();

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
    std::map<juce::String, FileActivityState>::iterator eraseState(
        std::map<juce::String, FileActivityState>::iterator iterator
    );
    void activityChanged();
    void storeState(const juce::String& fullPath, FileActivityState state);

    /// Component whose display drives the animation, owned by the caller, which outlives the tracker.
    juce::Component& animationComponent;
    /// Repaints the view that shows the activities, and runs on every animation frame while files are busy.
    std::function<void()> repaint;
    /// Outcome message of the last operation of each file, by full path.
    std::map<juce::String, OperationMessage> messages;
    /// Activity and progress of each busy file, by full path.
    std::map<juce::String, FileActivityState> activeStates;
    /// Sum of the progress of the busy files, updated with every change of activeStates.
    double progressTotal {0.0};
    /// Display callback that animates the indicators, attached only while a file is busy.
    juce::VBlankAttachment animationAttachment;
};

juce::String getFileActivityLabel(FileActivity activity);
}  // namespace audiobatch
