/// Display labels for the shared per-file activity states.

#include "gui/FileActivity.h"

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
}  // namespace audiobatch
