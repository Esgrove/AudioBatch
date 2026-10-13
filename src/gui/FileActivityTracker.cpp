/// Implementation of FileActivityTracker: the activity and message maps, the progress sum, and the animation timer.

#include "gui/FileActivityTracker.h"

#include <algorithm>
#include <utility>

namespace audiobatch
{
namespace
{
/// Rate in hertz at which the activity indicators are repainted while files are busy.
constexpr int animationTimerHz = 24;
}  // namespace

/// Returns the text shown in the status column for a file busy with the activity.
juce::String getFileActivityLabel(const FileActivity activity)
{
    switch (activity) {
        case FileActivity::waiting:
            return "Waiting";
        case FileActivity::analyzing:
            return "Analyzing";
        case FileActivity::normalizing:
            return "Normalizing";
        case FileActivity::processing:
            return "Processing";
    }

    return {};
}

/// Creates the tracker with the callback that repaints the view showing the activity.
/// The callback also runs on every animation tick while any file is busy.
FileActivityTracker::FileActivityTracker(std::function<void()> repaintCallback) : repaint(std::move(repaintCallback)) {}

/// Stops the animation timer.
FileActivityTracker::~FileActivityTracker()
{
    stopTimer();
}

/// Marks the files busy with the activity and clears their previous operation messages,
/// since a new operation replaces the outcome of the last one.
void FileActivityTracker::markActive(const juce::Array<juce::File>& files, const FileActivity activity)
{
    for (const auto& file : files) {
        activeStates[file.getFullPathName()] = {.activity = activity};
        messages.erase(file.getFullPathName());
    }

    activityChanged();
}

/// Sets the activity of one file, for example when its analysis starts.
/// The progress is unknown until the first progress report for the new activity arrives.
void FileActivityTracker::setActive(const juce::String& fullPath, const FileActivity activity)
{
    activeStates[fullPath] = {.activity = activity};
    activityChanged();
}

/// Stores the completed fraction of a busy file.
/// Ignored for an idle or waiting file, since a late report from a finished or cancelled file must not revive it.
/// The animation timer repaints the change.
void FileActivityTracker::setProgress(const juce::String& fullPath, const float progress)
{
    const auto iterator = activeStates.find(fullPath);

    if (iterator == activeStates.end() || iterator->second.activity == FileActivity::waiting) {
        return;
    }

    iterator->second.progress = juce::jlimit(0.0f, 1.0f, progress);
}

/// Marks one file idle.
void FileActivityTracker::clearActive(const juce::String& fullPath)
{
    activeStates.erase(fullPath);
    activityChanged();
}

/// Marks every file idle.
void FileActivityTracker::clearAllActive()
{
    activeStates.clear();
    activityChanged();
}

/// Marks idle every file whose activity is one of the given activities.
void FileActivityTracker::clearActiveWithActivities(const std::initializer_list<FileActivity> activities)
{
    std::erase_if(activeStates, [&activities](const auto& entry) {
        return std::ranges::find(activities, entry.second.activity) != activities.end();
    });
    activityChanged();
}

/// Returns the display label of a file's activity, or an empty string when it is idle.
juce::String FileActivityTracker::getActiveLabel(const juce::String& fullPath) const
{
    const auto activity = getActivity(fullPath);
    return activity.has_value() ? getFileActivityLabel(*activity) : juce::String();
}

/// Returns the activity of a file, or an empty optional when it is idle.
std::optional<FileActivity> FileActivityTracker::getActivity(const juce::String& fullPath) const
{
    const auto state = getActivityState(fullPath);
    return state.has_value() ? std::optional(state->activity) : std::nullopt;
}

/// Returns the activity and progress of a file, or an empty optional when it is idle.
std::optional<FileActivityState> FileActivityTracker::getActivityState(const juce::String& fullPath) const
{
    const auto iterator = activeStates.find(fullPath);
    return iterator != activeStates.end() ? std::optional(iterator->second) : std::nullopt;
}

/// Returns the sum of the completed fractions of the busy files,
/// which added to the number of finished files gives the progress of the whole run in files.
float FileActivityTracker::getProgressTotal() const
{
    float total = 0.0f;

    for (const auto& [path, state] : activeStates) {
        juce::ignoreUnused(path);
        total += state.progress.value_or(0.0f);
    }

    return total;
}

/// Returns true while any file is busy.
bool FileActivityTracker::isAnyActive() const
{
    return !activeStates.empty();
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
    if (!isAnyActive()) {
        stopTimer();
    } else if (!isTimerRunning()) {
        startTimerHz(animationTimerHz);
    }

    if (repaint != nullptr) {
        repaint();
    }
}

/// Repaints to animate the activity indicators and show progress, and stops itself once all files are idle.
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
}  // namespace audiobatch
