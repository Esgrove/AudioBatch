/// Application identity, startup options, and environment information.
/// Declares shared launch-flag queries, settings-file options,
/// and build and machine descriptions for logs, the About dialog, and version-only output.

#pragma once

#include <JuceHeader.h>

namespace audiobatch::utils
{
bool isVersionRequest(const juce::String& commandLine);
bool isHeadlessRequest(const juce::String& commandLine);
juce::StringArray getPathArguments(const juce::String& commandLine);

juce::PropertiesFile::Options createApplicationSettingsOptions();

juce::String aboutMessage(const juce::String& appName);
juce::String versionLine(const juce::String& appName);
juce::StringArray systemInfo();
}  // namespace audiobatch::utils
