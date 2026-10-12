/// Implementation of FileActivityTracker: the label and message maps and the animation timer.

#include "FileActivityTracker.h"

#include <cmath>
#include <utility>

/// Rate in hertz at which the activity indicators are repainted while files are busy.
static constexpr int animationTimerHz = 24;

FileActivityTracker::FileActivityTracker(std::function<void()> repaintCallback) :
    repaint(std::move(repaintCallback)) { }

FileActivityTracker::~FileActivityTracker()
{
    stopTimer();
}

void FileActivityTracker::markActive(const juce::Array<juce::File>& files, const juce::String& activityLabel)
{
    for (const auto& file : files) {
        activeLabels[file.getFullPathName()] = activityLabel;
        messages.erase(file.getFullPathName());
    }

    activityChanged();
}

void FileActivityTracker::setActive(const juce::String& fullPath, const juce::String& activityLabel)
{
    activeLabels[fullPath] = activityLabel;
    activityChanged();
}

void FileActivityTracker::clearActive(const juce::String& fullPath)
{
    activeLabels.erase(fullPath);
    activityChanged();
}

void FileActivityTracker::clearAllActive()
{
    activeLabels.clear();
    activityChanged();
}

void FileActivityTracker::clearActiveWithLabels(const juce::StringArray& activityLabels)
{
    std::erase_if(activeLabels, [&activityLabels](const auto& entry) { return activityLabels.contains(entry.second); });
    activityChanged();
}

juce::String FileActivityTracker::getActiveLabel(const juce::String& fullPath) const
{
    const auto iterator = activeLabels.find(fullPath);
    return iterator != activeLabels.end() ? iterator->second : juce::String();
}

bool FileActivityTracker::isAnyActive() const
{
    return !activeLabels.empty();
}

void FileActivityTracker::setMessage(const juce::String& fullPath, const OperationMessage& message)
{
    messages[fullPath] = message;

    if (repaint != nullptr) {
        repaint();
    }
}

void FileActivityTracker::clearMessage(const juce::String& fullPath)
{
    if (messages.erase(fullPath) > 0 && repaint != nullptr) {
        repaint();
    }
}

void FileActivityTracker::clearAllMessages()
{
    messages.clear();

    if (repaint != nullptr) {
        repaint();
    }
}

OperationMessage FileActivityTracker::getMessage(const juce::String& fullPath) const
{
    const auto iterator = messages.find(fullPath);
    return iterator != messages.end() ? iterator->second : OperationMessage {};
}

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
