/// Implementation of the file system helpers.
/// Moving to the trash uses the shell file operation on Windows,
/// which supports undo through the recycle bin, and juce::File::moveToTrash elsewhere.

#include "FileUtils.h"

#include "Log.h"

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
}  // namespace utils
