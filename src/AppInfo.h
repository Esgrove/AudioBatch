/// Application identity and environment information.
/// Declares the helpers that describe the build and the machine for the logs, the About dialog,
/// and the --version flag, and the storage options of the shared settings file.

#pragma once

#include <JuceHeader.h>

namespace utils
{
/// Returns the storage options for the AudioBatch.settings file that holds the GUI settings and plugin chain.
/// The CLI only reads this file, for example the plugin search folders of the last GUI scan.
juce::PropertiesFile::Options createApplicationSettingsOptions();

/// Collects build and runtime environment details for logs and diagnostics.
juce::StringArray systemInfo();

/// Builds the multi-line About dialog message with application, build, and system information.
juce::String aboutMessage(const juce::String& appName);

/// Returns the line printed by the --version flag of both executables:
/// the application name, the build version string from CMake, and the JUCE version.
juce::String versionLine(const juce::String& appName);
}  // namespace utils
