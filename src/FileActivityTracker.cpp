/// Implementation of FileActivityTracker: the label and message maps and the animation timer.

#include "FileActivityTracker.h"

#include <utility>

namespace
{
/// Rate in hertz at which the activity indicators are repainted while files are busy.
constexpr int animationTimerHz = 24;
}  // namespace

/// Creates the tracker with the callback that repaints the view showing the activity.
FileActivityTracker::FileActivityTracker(std::function<void()> repaintCallback) : repaint(std::move(repaintCallback)) {}

/// Stops the animation timer.
FileActivityTracker::~FileActivityTracker()
{
    stopTimer();
}

/// Marks the files busy with the activity label and clears their previous operation messages,
/// since a new operation replaces the outcome of the last one.
void FileActivityTracker::markActive(const juce::Array<juce::File>& files, const juce::String& activityLabel)
{
    for (const auto& file : files) {
        activeLabels[file.getFullPathName()] = activityLabel;
        messages.erase(file.getFullPathName());
    }

    activityChanged();
}

/// Sets the activity label of one file, for example when its analysis starts.
void FileActivityTracker::setActive(const juce::String& fullPath, const juce::String& activityLabel)
{
    activeLabels[fullPath] = activityLabel;
    activityChanged();
}

/// Marks one file idle.
void FileActivityTracker::clearActive(const juce::String& fullPath)
{
    activeLabels.erase(fullPath);
    activityChanged();
}

/// Marks every file idle.
void FileActivityTracker::clearAllActive()
{
    activeLabels.clear();
    activityChanged();
}

/// Marks idle every file whose activity label is one of the given labels.
void FileActivityTracker::clearActiveWithLabels(const juce::StringArray& activityLabels)
{
    std::erase_if(activeLabels, [&activityLabels](const auto& entry) { return activityLabels.contains(entry.second); });
    activityChanged();
}

/// Returns the activity label of a file, or an empty string when it is idle.
juce::String FileActivityTracker::getActiveLabel(const juce::String& fullPath) const
{
    const auto iterator = activeLabels.find(fullPath);
    return iterator != activeLabels.end() ? iterator->second : juce::String();
}

/// Returns true while any file is busy.
bool FileActivityTracker::isAnyActive() const
{
    return !activeLabels.empty();
}

/// Stores the outcome message of a file's last operation, such as an error or a clipping notice.
void FileActivityTracker::setMessage(const juce::String& fullPath, const OperationMessage& message)
{
    messages[fullPath] = message;

    if (repaint != nullptr) {
        repaint();
    }
}

/// Forgets the outcome message of one file, for example when its row leaves the table.
void FileActivityTracker::clearMessage(const juce::String& fullPath)
{
    if (messages.erase(fullPath) > 0 && repaint != nullptr) {
        repaint();
    }
}

/// Forgets the outcome messages of all files.
void FileActivityTracker::clearAllMessages()
{
    messages.clear();

    if (repaint != nullptr) {
        repaint();
    }
}

/// Returns the outcome message of a file's last operation, or an empty message.
OperationMessage FileActivityTracker::getMessage(const juce::String& fullPath) const
{
    const auto iterator = messages.find(fullPath);
    return iterator != messages.end() ? iterator->second : OperationMessage {};
}

/// Returns the shared animation phase in the range 0 to 1 that drives the spinning activity indicators.
float FileActivityTracker::getAnimationPhase()
{
    /// Full turns per second of the spinning indicator.
    constexpr auto rotationsPerSecond = 0.85;
    /// Converts the millisecond counter to seconds.
    constexpr auto secondsPerMillisecond = 0.001;
    const auto phase
        = std::fmod(juce::Time::getMillisecondCounterHiRes() * secondsPerMillisecond * rotationsPerSecond, 1.0);
    return static_cast<float>(phase);
}

/// Starts or stops the repaint timer depending on whether any file is busy, and repaints once.
void FileActivityTracker::activityChanged()
{
    if (isAnyActive()) {
        startTimerHz(animationTimerHz);
    } else {
        stopTimer();
    }

    if (repaint != nullptr) {
        repaint();
    }
}

/// Repaints to animate the activity indicators, and stops itself once all files are idle.
void FileActivityTracker::timerCallback()
{
    if (!isAnyActive()) {
        stopTimer();
        return;
    }

    if (repaint != nullptr) {
        repaint();
    }
}
