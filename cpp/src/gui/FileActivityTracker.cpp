/// Implementation of FileActivityTracker: activity and message maps, the progress sum,
/// and display-synchronized repaint scheduling.

#include "gui/FileActivityTracker.h"

#include "gui/AudioFileTableModel.h"
#include <initializer_list>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace audiobatch
{
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
/// The component must outlive the tracker and selects the display used for animation updates.
FileActivityTracker::FileActivityTracker(juce::Component& component, std::function<void()> repaintCallback) :
    animationComponent(component),
    repaint(std::move(repaintCallback))
{}

/// Detaches display callbacks before releasing the activity state.
FileActivityTracker::~FileActivityTracker() = default;

/// Marks the files busy with the activity and clears their previous operation messages,
/// since a new operation replaces the outcome of the last one. Progress starts empty before the first repaint.
void FileActivityTracker::markActive(const juce::Array<juce::File>& files, const FileActivity activity)
{
    for (const auto& file : files) {
        storeState(file.getFullPathName(), {.activity = activity, .progress = 0.0f});
        messages.erase(file.getFullPathName());
    }

    activityChanged();
}

/// Sets the activity of one file, for example when its analysis starts.
/// Resets progress before repainting so no previous fill or indeterminate arc appears at startup.
void FileActivityTracker::setActive(const juce::String& fullPath, const FileActivity activity)
{
    storeState(fullPath, {.activity = activity, .progress = 0.0f});
    activityChanged();
}

/// Stores the completed fraction of a busy file.
/// Ignored for an idle or waiting file, since a late report from a finished or cancelled file must not revive it.
/// The next display refresh repaints the change.
void FileActivityTracker::setProgress(const juce::String& fullPath, const float progress)
{
    const auto iterator = activeStates.find(fullPath);

    if (iterator == activeStates.end() || iterator->second.activity == FileActivity::waiting) {
        return;
    }

    const auto limited = juce::jlimit(0.0f, 1.0f, progress);
    progressTotal += static_cast<double>(limited) - static_cast<double>(iterator->second.progress.value_or(0.0f));
    iterator->second.progress = limited;
}

/// Marks one file idle.
void FileActivityTracker::clearActive(const juce::String& fullPath)
{
    if (const auto iterator = activeStates.find(fullPath); iterator != activeStates.end()) {
        eraseState(iterator);
    }
    activityChanged();
}

/// Marks every file idle.
void FileActivityTracker::clearAllActive()
{
    activeStates.clear();
    progressTotal = 0.0;
    activityChanged();
}

/// Marks idle every file whose activity is one of the given activities.
void FileActivityTracker::clearActiveWithActivities(const std::initializer_list<FileActivity> activities)
{
    for (auto iterator = activeStates.begin(); iterator != activeStates.end();) {
        if (std::ranges::find(activities, iterator->second.activity) != activities.end()) {
            iterator = eraseState(iterator);
        } else {
            ++iterator;
        }
    }
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
/// The sum is kept up to date as the states change, because the progress line reads it on every display frame.
float FileActivityTracker::getProgressTotal() const
{
    return static_cast<float>(juce::jmax(0.0, progressTotal));
}

/// Sets the activity state of a file and keeps the running progress total in step with it.
void FileActivityTracker::storeState(const juce::String& fullPath, const FileActivityState state)
{
    if (const auto [iterator, inserted] = activeStates.try_emplace(fullPath, state); !inserted) {
        progressTotal -= static_cast<double>(iterator->second.progress.value_or(0.0f));
        iterator->second = state;
    }
    progressTotal += static_cast<double>(state.progress.value_or(0.0f));
}

/// Removes the state of a file from the running progress total and the busy files,
/// and returns the iterator to the state after it.
/// The total starts again from zero once no file is busy, so rounding errors do not build up across runs.
std::map<juce::String, FileActivityState>::iterator FileActivityTracker::eraseState(
    const std::map<juce::String, FileActivityState>::iterator& iterator
)
{
    progressTotal -= static_cast<double>(iterator->second.progress.value_or(0.0f));
    const auto next = activeStates.erase(iterator);
    if (activeStates.empty()) {
        progressTotal = 0.0;
    }
    return next;
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

/// Attaches to the component's display only while files are busy, and repaints state changes immediately.
void FileActivityTracker::activityChanged()
{
    if (!isAnyActive()) {
        animationAttachment = {};
    } else if (animationAttachment.isEmpty()) {
        animationAttachment = juce::VBlankAttachment(&animationComponent, [this](double) {
            if (animationComponent.isShowing() && repaint != nullptr) {
                repaint();
            }
        });
    }

    if (repaint != nullptr) {
        repaint();
    }
}

}  // namespace audiobatch
