/// File system helpers shared by the services and both front ends.
/// Declares moving files to the system trash, the file manager label for revealing a file,
/// deleting files with error logging, and extension checks for the audio file types the services treat specially.

#pragma once

#include <JuceHeader.h>

#include <functional>

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

/// Returns the user-facing error when the output path cannot take the output of the given source,
/// or an empty string when it can.
/// A folder at the output path is never replaced, even with overwriteExisting,
/// and a different file there needs overwriteExisting.
/// The services check this before rendering, so a doomed file fails before any work is done.
juce::String checkOutputPath(const juce::File& sourceFile, const juce::File& outputFile, bool overwriteExisting);

/// Disposes of a file that a committed output replaces, and returns true when the file is gone.
using FileDisposer = std::function<bool(const juce::File&)>;

/// Moves a finished temporary output into place next to its source and disposes of the source.
/// When the output path is the source itself, the source is replaced in place.
/// Otherwise an output path that fails checkOutputPath fails the commit,
/// and a file allowed to be overwritten is renamed to a hidden backup next to it first.
/// After the output is in place, a source with a different path is disposed of,
/// and when that fails the output is removed again and the backup is renamed back,
/// so the folder is left as it was.
/// The backup is disposed of last, once everything else succeeded.
/// When an in-place replace fails after the original was already removed,
/// the new audio is kept next to it under a "(recovered)" name instead of being deleted with the temporary file.
/// `disposeFile` moves files to the system trash unless a caller such as a test passes another function.
/// Returns an empty string on success, or a user-facing error message.
/// The temporary file is deleted by its own destructor on failure.
juce::String commitTemporaryOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    bool overwriteExisting,
    const FileDisposer& disposeFile = moveToTrash
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
