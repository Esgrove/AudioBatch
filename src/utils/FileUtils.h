/// File system helpers shared by the services and both front ends.
/// Declares moving files to the system trash, the file manager label for revealing a file,
/// deleting files with error logging, choosing the default browse folder,
/// and extension checks for supported audio types.

#pragma once

#include <JuceHeader.h>

#include <functional>

namespace audiobatch::utils
{

/// Disposes of a file that a committed output replaces, and returns true when the file is gone.
using FileDisposer = std::function<bool(const juce::File&)>;

juce::File getDefaultBrowseDirectory();

bool deleteFile(const juce::File& file);
bool disposeOfCopy(const juce::File& file, const FileDisposer& disposeFile);
bool isAiffFile(const juce::File& file);
bool isMp3File(const juce::File& file);
bool isWavFile(const juce::File& file);
bool moveToTrash(const juce::File& file);

void revealFileInFileManager(const juce::File& file);
void revealParentDirectory(const juce::File& file);

juce::String checkOutputPath(const juce::File& sourceFile, const juce::File& outputFile, bool overwriteExisting);
juce::String existingOutputMessage(const juce::File& outputFile);
juce::String fileExtension(const juce::File& file);
juce::String normalizeExtension(const juce::String& extension);
juce::String revealInFileManagerLabel();

juce::String commitTemporaryOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    bool overwriteExisting,
    const FileDisposer& disposeFile = moveToTrash
);
}  // namespace audiobatch::utils
