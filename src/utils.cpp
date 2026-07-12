#include "utils.h"

#if JUCE_WINDOWS
#include <juce_core/native/juce_BasicNativeHeaders.h>
#endif

namespace utils
{
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName)
{
    return std::unique_ptr<juce::FileLogger>(
        juce::FileLogger::createDefaultAppLogger(appName, appName + ".log", appName, 32768)
    );
}

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

    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted && !file.exists();
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
        logError("Failed to delete file: " + file.getFullPathName().quoted());
        return false;
    }

    return true;
}

juce::StringArray systemInfo()
{
    const auto compile_time = juce::Time::getCompilationDate();
    const auto compile_time_in_utc = compile_time - juce::RelativeTime(compile_time.getUTCOffsetSeconds());
    return juce::StringArray {
        juce::String(version::VERSION_INFO),
        juce::SystemStats::getJUCEVersion() + " " + compile_time_in_utc.toString(true, true, false, true),
        "Branch:       " + juce::String(version::BRANCH),
        "OS:           " + juce::SystemStats::getOperatingSystemName()
            + (juce::SystemStats::isOperatingSystem64Bit() ? " 64 bit" : " 32 bit"),
        "Device:       " + juce::SystemStats::getDeviceDescription(),
        "Manufacturer: " + juce::SystemStats::getDeviceManufacturer(),
        "CPU model:    " + juce::SystemStats::getCpuModel(),
        "CPU cores:    " + juce::String(juce::SystemStats::getNumPhysicalCpus()) + "C/"
            + juce::String(juce::SystemStats::getNumCpus()) + "T",
        "Memory size:  " + juce::String(juce::SystemStats::getMemorySizeInMegabytes()) + " MB",
        "User region:  " + juce::SystemStats::getUserRegion(),
        "User lang:    " + juce::SystemStats::getUserLanguage(),
        "Display lang: " + juce::SystemStats::getDisplayLanguage()
    };
}

juce::String formattedSystemInfo()
{
    juce::String info {juce::String(version::APP_NAME) + " " + juce::String(version::VERSION_NUMBER) + juce::newLine};
    for (const auto& line : utils::systemInfo()) {
        for (auto tokens = juce::StringArray::fromTokens(line, " ", "\""); const auto& token : tokens) {
            if (token.containsNonWhitespaceChars()) {
                info += token + " ";
            }
        }
        info += juce::newLine;
    }
    return info;
}

juce::String aboutMessage(const juce::String& appName)
{
    juce::String message;
    message << appName << " " << version::VERSION_NUMBER << juce::newLine << juce::SystemStats::getJUCEVersion()
            << juce::newLine << juce::newLine << "Batch audio analysis, normalization, and processing." << juce::newLine
            << juce::newLine << "Date: " << version::DATE << juce::newLine << "Commit: " << version::COMMIT
            << juce::newLine << "Branch: " << version::BRANCH << juce::newLine << juce::newLine
            << "OS: " << juce::SystemStats::getOperatingSystemName()
            << (juce::SystemStats::isOperatingSystem64Bit() ? " (64 bit)" : " (32 bit)") << juce::newLine
            << "Device: " << juce::SystemStats::getDeviceDescription() << juce::newLine
            << "CPU: " << juce::SystemStats::getCpuModel() << " ("
            << juce::String(juce::SystemStats::getNumPhysicalCpus()) << "C/"
            << juce::String(juce::SystemStats::getNumCpus()) << "T)" << juce::newLine
            << "Memory: " << juce::String(juce::SystemStats::getMemorySizeInMegabytes()) << " MB";
    return message;
}
}  // namespace utils
