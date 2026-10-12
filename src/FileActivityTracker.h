/// Per-file activity state shown in the GUI results table.
/// Declares FileActivityTracker, which holds the transient activity label of each busy file,
/// such as "Analyzing" or "Normalizing", the outcome message of each file's last operation,
/// and the timer that animates the activity indicators while any file is busy.

#pragma once

#include "AudioFileTableModel.h"

#include <JuceHeader.h>

#include <functional>
#include <map>

/// Tracks which files are busy and what their last operation reported, keyed by full path.
/// Runs a repaint timer while any file is busy and stops it once all files are idle.
/// Must only be used on the message thread.
class FileActivityTracker final : private juce::Timer
{
public:
    /// Creates the tracker with the callback that repaints the view showing the activity.
    explicit FileActivityTracker(std::function<void()> repaintCallback);

    /// Stops the animation timer.
    ~FileActivityTracker() override;

    FileActivityTracker(const FileActivityTracker&) = delete;
    FileActivityTracker& operator=(const FileActivityTracker&) = delete;
    FileActivityTracker(FileActivityTracker&&) = delete;
    FileActivityTracker& operator=(FileActivityTracker&&) = delete;

    /// Marks the files busy with the activity label and clears their previous operation messages,
    /// since a new operation replaces the outcome of the last one.
    void markActive(const juce::Array<juce::File>& files, const juce::String& activityLabel);

    /// Sets the activity label of one file, for example when its analysis starts.
    void setActive(const juce::String& fullPath, const juce::String& activityLabel);

    /// Marks one file idle.
    void clearActive(const juce::String& fullPath);

    /// Marks every file idle.
    void clearAllActive();

    /// Marks idle every file whose activity label is one of the given labels.
    void clearActiveWithLabels(const juce::StringArray& activityLabels);

    /// Returns the activity label of a file, or an empty string when it is idle.
    [[nodiscard]] juce::String getActiveLabel(const juce::String& fullPath) const;

    /// Returns true while any file is busy.
    [[nodiscard]] bool isAnyActive() const;

    /// Stores the outcome message of a file's last operation, such as an error or a clipping notice.
    void setMessage(const juce::String& fullPath, const OperationMessage& message);

    /// Forgets the outcome message of one file, for example when its row leaves the table.
    void clearMessage(const juce::String& fullPath);

    /// Forgets the outcome messages of all files.
    void clearAllMessages();

    /// Returns the outcome message of a file's last operation, or an empty message.
    [[nodiscard]] OperationMessage getMessage(const juce::String& fullPath) const;

    /// Returns the shared animation phase in the range 0 to 1 that drives the spinning activity indicators.
    [[nodiscard]] static float getAnimationPhase();

private:
    /// Starts or stops the repaint timer depending on whether any file is busy, and repaints once.
    void activityChanged();

    /// Repaints to animate the activity indicators, and stops itself once all files are idle.
    void timerCallback() override;

    std::function<void()> repaint;
    std::map<juce::String, juce::String> activeLabels;
    std::map<juce::String, OperationMessage> messages;
};
