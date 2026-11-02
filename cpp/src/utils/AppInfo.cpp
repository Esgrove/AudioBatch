/// Implementation of the application information helpers:
/// startup flag queries, settings file options, system information, the About dialog message, and the version line.

#include "utils/AppInfo.h"

#include "utils/StringFormat.h"
#include "utils/version.h"

#include <filesystem>
#if JUCE_WINDOWS
#include <cstdlib>
#endif
#include <optional>
#include <stdexcept>

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

/// Validates an optional absolute storage root before JUCE can resolve relative paths against the working directory.
/// Invalid nonempty values throw instead of falling back to any production storage.
std::optional<juce::File> parseDataRootOverride(const juce::String& value)
{
    if (value.isEmpty()) {
        return std::nullopt;
    }
#if JUCE_WINDOWS
    const std::filesystem::path path(value.toWideCharPointer());
#else
    const std::filesystem::path path(value.toStdString());
#endif
    if (!path.is_absolute() || value.containsChar('\n') || value.containsChar('\r')) {
        throw std::invalid_argument("AUDIOBATCH_DATA_ROOT must be an absolute path");
    }
    juce::File root(value);
    if (root.existsAsFile()) {
        throw std::invalid_argument("AUDIOBATCH_DATA_ROOT must name a directory, not a file");
    }
    return root;
}

/// Reads the process storage override without caching it or changing the environment.
std::optional<juce::File> getDataRootOverride()
{
    // JUCE's Windows environment helper treats an empty value as absent, so check presence separately.
#if JUCE_WINDOWS
    std::size_t valueLength = 0;
    if (_wgetenv_s(&valueLength, nullptr, 0, L"AUDIOBATCH_DATA_ROOT") != 0) {
        throw std::invalid_argument("Could not read AUDIOBATCH_DATA_ROOT");
    }
    if (valueLength == 0) {
        return std::nullopt;
    }
#endif
    const auto value = juce::SystemStats::getEnvironmentVariable("AUDIOBATCH_DATA_ROOT", {});
#if !JUCE_WINDOWS
    // On POSIX, a present empty value stays empty even when the default is nonempty.
    if (value.isEmpty() && juce::SystemStats::getEnvironmentVariable("AUDIOBATCH_DATA_ROOT", "unset").isNotEmpty()) {
        return std::nullopt;
    }
#endif
    if (value.isEmpty()) {
        throw std::invalid_argument("AUDIOBATCH_DATA_ROOT must be an absolute path, not empty");
    }
    return parseDataRootOverride(value);
}

/// Keeps the analysis database shared with Rust while isolating all default cache connections when requested.
juce::File defaultAnalysisDatabaseFile()
{
    return defaultAnalysisDatabaseFile(getDataRootOverride());
}

/// Resolves the shared cache for an explicitly supplied root without reading the process environment.
juce::File defaultAnalysisDatabaseFile(const std::optional<juce::File>& dataRoot)
{
    const auto root = dataRoot.value_or(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory));
    return root.getChildFile("AudioBatch/analysis.db");
}

/// Gives isolated applications their own scanner lock, while preserving the native lock name when unset.
juce::String scopedPluginScanLockName(const juce::String& name)
{
    return scopedPluginScanLockName(name, getDataRootOverride());
}

/// Derives a stable lock namespace without mutating global state or sharing the production scanner lock.
juce::String scopedPluginScanLockName(const juce::String& name, const std::optional<juce::File>& dataRoot)
{
    if (dataRoot) {
        return format("{}-{}", name, juce::String::toHexString(dataRoot->getFullPathName().hashCode64()));
    }
    return name;
}

/// Returns the process-default settings location, failing closed on an invalid storage override.
juce::PropertiesFile::Options createApplicationSettingsOptions()
{
    return createApplicationSettingsOptions(getDataRootOverride());
}

/// Resolves settings and the adjacent plugin files beneath an explicitly supplied isolation root.
/// An absolute folderName overrides JUCE's platform settings directory without changing native unset defaults.
juce::PropertiesFile::Options createApplicationSettingsOptions(const std::optional<juce::File>& dataRoot)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "AudioBatch";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = dataRoot ? dataRoot->getChildFile("AudioBatch/settings").getFullPathName() : "AudioBatch";
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
