/// Implementation of the utils namespace helpers.
/// Covers logger creation, moving files to the OS trash with a native fallback on Windows,
/// file deletion with error logging,
/// and collecting and formatting system and build information for logs and the About dialog.

#include "utils.h"

#if JUCE_WINDOWS
#include <juce_core/native/juce_BasicNativeHeaders.h>
#endif

namespace utils
{
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName)
{
    return std::unique_ptr<juce::FileLogger>(
        juce::FileLogger::createDefaultAppLogger(appName, format("{}.log", appName), appName, 32768)
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
        logError("Failed to delete file: {}", file.getFullPathName().quoted());
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
        format("{} {}", juce::SystemStats::getJUCEVersion(), compile_time_in_utc.toString(true, true, false, true)),
        format("Branch:       {}", version::BRANCH),
        format(
            "OS:           {}{}",
            juce::SystemStats::getOperatingSystemName(),
            juce::SystemStats::isOperatingSystem64Bit() ? " 64 bit" : " 32 bit"
        ),
        format("Device:       {}", juce::SystemStats::getDeviceDescription()),
        format("Manufacturer: {}", juce::SystemStats::getDeviceManufacturer()),
        format("CPU model:    {}", juce::SystemStats::getCpuModel()),
        format("CPU cores:    {}C/{}T", juce::SystemStats::getNumPhysicalCpus(), juce::SystemStats::getNumCpus()),
        format("Memory size:  {} MB", juce::SystemStats::getMemorySizeInMegabytes()),
        format("User region:  {}", juce::SystemStats::getUserRegion()),
        format("User lang:    {}", juce::SystemStats::getUserLanguage()),
        format("Display lang: {}", juce::SystemStats::getDisplayLanguage())
    };
}

juce::String formattedSystemInfo()
{
    juce::String info {format("{} {}", version::APP_NAME, version::VERSION_NUMBER) + juce::newLine};
    for (const auto& line : utils::systemInfo()) {
        for (auto tokens = juce::StringArray::fromTokens(line, " ", "\""); const auto& token : tokens) {
            if (token.containsNonWhitespaceChars()) {
                info += format("{} ", token);
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
