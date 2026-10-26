/// Implementation of the application information helpers:
/// startup flag queries, settings file options, system information, the About dialog message, and the version line.

#include "utils/AppInfo.h"

#include "utils/StringFormat.h"
#include "utils/version.h"

namespace audiobatch::utils
{
/// Recognizes a version-only launch before logging, single-instance forwarding, or GUI initialization.
bool isVersionRequest(const juce::String& commandLine)
{
    const juce::ArgumentList arguments {"AudioBatch", commandLine};
    return arguments.containsOption("--version|-V");
}

/// Recognizes deliberate hidden launches consistently during startup and single-instance forwarding.
bool isHeadlessRequest(const juce::String& commandLine)
{
    const juce::ArgumentList arguments {"AudioBatch", commandLine};
    return arguments.containsOption("--headless|-H");
}

/// Returns the full paths of the arguments that are not options, such as the folders and files to open on launch.
/// Relative paths resolve against the current working directory, and quoted paths may contain spaces.
juce::StringArray getPathArguments(const juce::String& commandLine)
{
    const juce::ArgumentList arguments {"AudioBatch", commandLine};
    juce::StringArray paths;

    for (const auto& argument : arguments.arguments) {
        if (!argument.isOption()) {
            paths.addIfNotAlreadyThere(argument.resolveAsFile().getFullPathName());
        }
    }

    return paths;
}

/// Returns the storage options for the AudioBatch.settings file that holds the GUI settings and plugin chain.
/// The CLI only reads this file, for example the plugin search folders of the last GUI scan.
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

/// Collects build and runtime environment details for logs and diagnostics.
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

/// Builds the multi-line About dialog message with application, build, and system information.
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

/// Returns the line printed by the --version flag of both executables:
/// the application name, the build version string from CMake, and the JUCE version.
juce::String versionLine(const juce::String& appName)
{
    return format("{} {} {}", appName, version::VERSION_STRING, juce::SystemStats::getJUCEVersion());
}
}  // namespace audiobatch::utils
