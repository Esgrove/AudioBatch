/// Implementation of the utils namespace helpers.
/// Covers logger creation, moving files to the OS trash with a native fallback on Windows,
/// file deletion with error logging,
/// collecting and formatting system and build information for logs and the About dialog,
/// and printing to the parent console from the GUI executable.

#include "utils.h"

#include <cstdio>

#if JUCE_WINDOWS
#include <juce_core/native/juce_BasicNativeHeaders.h>
#endif

namespace utils
{
std::unique_ptr<juce::FileLogger> createDefaultLogger(const juce::String& appName, const juce::String& logName)
{
    const auto name = logName.isNotEmpty() ? logName : appName;
    return std::unique_ptr<juce::FileLogger>(
        juce::FileLogger::createDefaultAppLogger(appName, format("{}.log", name), name, 32768)
    );
}

juce::PropertiesFile::Options createApplicationSettingsOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "AudioBatch";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "AudioBatch";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    return options;
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

juce::StringArray systemInfo()
{
    const auto compileTime = juce::Time::getCompilationDate();
    const auto compileTimeInUtc = compileTime - juce::RelativeTime(compileTime.getUTCOffsetSeconds());
    return juce::StringArray {
        juce::String(version::VERSION_INFO),
        format("{} {}", juce::SystemStats::getJUCEVersion(), compileTimeInUtc.toString(true, true, false, true)),
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

juce::String aboutMessage(const juce::String& appName)
{
    // The message is only shown inside the app, so plain '\n' line breaks are fine.
    return format(
        "{} {}\n{}\n\n"
        "Batch audio analysis, normalization, and processing.\n\n"
        "Date: {}\nCommit: {}\nBranch: {}\n\n"
        "OS: {} ({})\nDevice: {}\nCPU: {} ({}C/{}T)\nMemory: {} MB",
        appName,
        version::VERSION_NUMBER,
        juce::SystemStats::getJUCEVersion(),
        version::DATE,
        version::COMMIT,
        version::BRANCH,
        juce::SystemStats::getOperatingSystemName(),
        juce::SystemStats::isOperatingSystem64Bit() ? "64 bit" : "32 bit",
        juce::SystemStats::getDeviceDescription(),
        juce::SystemStats::getCpuModel(),
        juce::SystemStats::getNumPhysicalCpus(),
        juce::SystemStats::getNumCpus(),
        juce::SystemStats::getMemorySizeInMegabytes()
    );
}

juce::String versionLine(const juce::String& appName)
{
    return format("{} {} {}", appName, version::VERSION_STRING, juce::SystemStats::getJUCEVersion());
}

void printLineToConsole(const juce::String& text)
{
#if JUCE_WINDOWS
    if (auto* const outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        outputHandle == nullptr || outputHandle == INVALID_HANDLE_VALUE)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS) != 0) {
            FILE* stream = nullptr;
            freopen_s(&stream, "CONOUT$", "w", stdout);
        }
    }
#endif
    fmt::print("{}\n", text);
    std::fflush(stdout);
}
}  // namespace utils
