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

/// Renames the backup of a replaced output back to the output path after a failed commit.
/// Logs where the previous output is when the rename fails, so it can still be recovered by hand.
static void restoreOutputBackup(const juce::File& backupFile, const juce::File& outputFile)
{
    if (backupFile == juce::File() || !backupFile.existsAsFile()) {
        return;
    }

    if (!backupFile.moveFileTo(outputFile)) {
        logError(
            "Could not restore the previous output {} from its backup {}",
            outputFile.getFullPathName().quoted(),
            backupFile.getFullPathName().quoted()
        );
    }
}

juce::String commitTemporaryOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    const bool overwriteExisting,
    const FileDisposer& disposeFile
)
{
    const bool replacingSource = outputFile == sourceFile;
    juce::File backupFile;

    if (!replacingSource && outputFile.exists()) {
        if (!overwriteExisting) {
            return existingOutputMessage(outputFile);
        }

        if (!outputFile.existsAsFile()) {
            return format("Output path {} is not a file and cannot be replaced.", outputFile.getFileName());
        }

        // A rename within the folder is atomic and can be undone,
        // unlike moving the previous output to the trash before the new one is in place.
        // The dot prefix keeps the backup out of folder scans.
        backupFile = outputFile.getSiblingFile("." + outputFile.getFileName() + ".backup").getNonexistentSibling(false);

        if (!outputFile.moveFileTo(backupFile)) {
            return "Could not move the existing output file aside";
        }
    }

    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        restoreOutputBackup(backupFile, outputFile);
        return replacingSource ? "Could not replace the original file with the new audio"
                               : "Could not move the new audio file into place";
    }

    if (!replacingSource && sourceFile.existsAsFile() && !disposeFile(sourceFile)) {
        deleteFile(outputFile);
        restoreOutputBackup(backupFile, outputFile);
        return "Could not move the original audio file to the system trash";
    }

    if (backupFile.existsAsFile() && !disposeFile(backupFile)) {
        // The new output and the source are already settled, so only the leftover backup is reported.
        logWarn(
            "Could not move the replaced output to the system trash, it was kept as {}",
            backupFile.getFullPathName().quoted()
        );
    }

    return {};
}
}  // namespace utils
