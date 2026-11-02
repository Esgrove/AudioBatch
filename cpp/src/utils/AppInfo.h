/// Application identity, startup options, and environment information.
/// Declares shared launch-flag queries, settings-file options,
/// and build and machine descriptions for logs, the About dialog, and version-only output.

#pragma once

#include <JuceHeader.h>

#include <optional>

namespace audiobatch::utils
{
bool isVersionRequest(const juce::String& commandLine);
bool isHeadlessRequest(const juce::String& commandLine);
juce::StringArray getPathArguments(const juce::String& commandLine);

[[nodiscard]] std::optional<juce::File> parseDataRootOverride(const juce::String& value);
[[nodiscard]] std::optional<juce::File> getDataRootOverride();
[[nodiscard]] juce::File defaultAnalysisDatabaseFile();
[[nodiscard]] juce::File defaultAnalysisDatabaseFile(const std::optional<juce::File>& dataRoot);
[[nodiscard]] juce::String scopedPluginScanLockName(const juce::String& name);
[[nodiscard]] juce::String scopedPluginScanLockName(
    const juce::String& name,
    const std::optional<juce::File>& dataRoot
);
juce::PropertiesFile::Options createApplicationSettingsOptions();
juce::PropertiesFile::Options createApplicationSettingsOptions(const std::optional<juce::File>& dataRoot);

juce::String aboutMessage(const juce::String& appName);
juce::String versionLine(const juce::String& appName);
juce::StringArray systemInfo();
}  // namespace audiobatch::utils
