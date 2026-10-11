/// File system helpers shared by the services and both front ends.
/// Declares moving files to the system trash, deleting files with error logging,
/// and extension checks for the audio file types the services treat specially.

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

/// Returns the extension text trimmed, lowercased, and without the leading dot, for example "mp3".
juce::String normalizeExtension(const juce::String& extension);

/// Returns the file's extension lowercased and without the leading dot, for example "mp3".
juce::String fileExtension(const juce::File& file);

/// Returns true for an MP3 file.
bool isMp3File(const juce::File& file);

/// Returns true for an AIFF family file: .aif, .aiff, or .aifc.
bool isAiffFile(const juce::File& file);

/// Returns true for a WAV file: .wav or .wave.
bool isWavFile(const juce::File& file);
}  // namespace utils
