/// File system helpers shared by the services and both front ends.
/// Declares moving files to the system trash, the file manager label for revealing a file,
/// deleting files with error logging, and extension checks for the audio file types the services treat specially.

#pragma once

#include <JuceHeader.h>

namespace utils
{
/// Moves a file or folder to the OS trash or recycle bin.
/// Returns true when the file did not exist or was moved.
bool moveToTrash(const juce::File& file);

/// Returns the platform-appropriate label for the menu item that reveals a file in the file manager.
juce::String revealInFileManagerLabel();

/// Deletes a file and logs an error if the deletion fails.
/// Returns true if the file did not exist or was deleted successfully.
bool deleteFile(const juce::File& file);

/// Returns the user-facing error for an output path that is already taken by a file other than the source.
juce::String existingOutputMessage(const juce::File& outputFile);

/// Moves a finished temporary output into place next to its source and disposes of the source.
/// When the output path is the source itself, the source is replaced in place.
/// Otherwise a different file already at the output path fails with existingOutputMessage,
/// unless overwriteExisting is set, in which case that file is moved to the system trash first.
/// After the output is in place, a source with a different path is moved to the system trash,
/// and when that fails the output is removed again, so no duplicate is left next to the source.
/// Returns an empty string on success, or a user-facing error message.
/// The temporary file is deleted by its own destructor on failure.
juce::String commitTemporaryOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    bool overwriteExisting
);

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
