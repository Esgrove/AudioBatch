/// Per-file activity and operation outcomes shared by the GUI table and its activity tracker.
/// Keeps the state and display labels independent of the table model.

#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <optional>

namespace audiobatch
{
/// What a busy file is doing, shown with an activity indicator in the status column.
/// Waiting means queued for analysis, normalization, or processing, but not started yet.
enum class FileActivity : std::uint8_t {
    waiting,
    analyzing,
    normalizing,
    processing,
};

/// Activity of a busy file and how far along it is.
struct FileActivityState {
    /// What is happening to the file.
    FileActivity activity {FileActivity::waiting};
    /// Completed fraction in the range 0 to 1, initialized to zero for each activity.
    /// An empty optional represents indeterminate progress.
    std::optional<float> progress;
};

/// Outcome of the last normalize or process run for a file, shown in the status column while the file is idle.
struct OperationMessage {
    /// Message shown in the status column.
    juce::String text;
    /// True for a failure, which is highlighted, and false for an informational notice.
    bool isError {false};
};

juce::String getFileActivityLabel(FileActivity activity);
}  // namespace audiobatch
