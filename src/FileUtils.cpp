/// Implementation of the file system helpers.
/// Moving to the trash uses the shell file operation on Windows,
/// which supports undo through the recycle bin, and juce::File::moveToTrash elsewhere.

#include "FileUtils.h"

#include "Log.h"
#include "StringFormat.h"

#include <string>

#if JUCE_WINDOWS
#include <juce_core/native/juce_BasicNativeHeaders.h>
#endif

namespace utils
{
bool moveToTrash(const juce::File& file)
{
    if (!file.exists()) {
        return true;
    }

#if JUCE_WINDOWS
    auto sourcePath = file.getFullPathName().replaceCharacter('/', '\\');
    std::wstring fromPath(sourcePath.toWideCharPointer());
    fromPath.push_back(L'\0');

    SHFILEOPSTRUCTW operation = {};
    operation.wFunc = FO_DELETE;
    operation.pFrom = fromPath.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOERRORUI | FOF_SILENT | FOF_NOCONFIRMATION;

    return SHFileOperationW(&operation) == 0 && operation.fAnyOperationsAborted == FALSE && !file.exists();
#else
    return file.moveToTrash();
#endif
}

juce::String revealInFileManagerLabel()
{
#if JUCE_MAC
    return "Reveal in Finder";
#elif JUCE_WINDOWS
    return "Reveal in Explorer";
#else
    return "Reveal in File Manager";
#endif
}

bool deleteFile(const juce::File& file)
{
    if (!file.exists()) {
        return true;
    }

    if (!file.deleteFile()) {
        logError("Failed to delete file: {}", file.getFullPathName().quoted());
        return false;
    }

    return true;
}

juce::String normalizeExtension(const juce::String& extension)
{
    auto value = extension.trim().toLowerCase();

    if (value.startsWithChar('.')) {
        value = value.substring(1);
    }

    return value;
}

juce::String fileExtension(const juce::File& file)
{
    return normalizeExtension(file.getFileExtension());
}

bool isMp3File(const juce::File& file)
{
    return fileExtension(file) == "mp3";
}

bool isAiffFile(const juce::File& file)
{
    const auto extension = fileExtension(file);
    return extension == "aif" || extension == "aiff" || extension == "aifc";
}

bool isWavFile(const juce::File& file)
{
    const auto extension = fileExtension(file);
    return extension == "wav" || extension == "wave";
}

juce::String existingOutputMessage(const juce::File& outputFile)
{
    return format("Output file {} already exists. Use force overwrite to replace it.", outputFile.getFileName());
}

juce::String commitTemporaryOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    const bool overwriteExisting
)
{
    const bool replacingSource = outputFile == sourceFile;

    if (!replacingSource && outputFile.exists()) {
        if (!overwriteExisting) {
            return existingOutputMessage(outputFile);
        }

        if (!moveToTrash(outputFile)) {
            return "Could not move the existing output file to the system trash";
        }
    }

    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        return replacingSource ? "Could not replace the original file with the new audio"
                               : "Could not move the new audio file into place";
    }

    if (!replacingSource && sourceFile.existsAsFile() && !moveToTrash(sourceFile)) {
        deleteFile(outputFile);
        return "Could not move the original audio file to the system trash";
    }

    return {};
}
}  // namespace utils
