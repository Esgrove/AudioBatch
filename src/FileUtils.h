/// File system helpers shared by the services and both front ends.
/// Declares moving files to the system trash and deleting files with error logging.

#pragma once

#include <JuceHeader.h>

namespace utils
{
/// Moves a file or folder to the OS trash or recycle bin.
/// Returns true when the file did not exist or was moved.
bool moveToTrash(const juce::File& file);

/// Deletes a file and logs an error if the deletion fails.
/// Returns true if the file did not exist or was deleted successfully.
bool deleteFile(const juce::File& file);
}  // namespace utils
