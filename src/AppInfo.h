/// Application identity and environment information.
/// Declares the helpers that describe the build and the machine for the logs, the About dialog,
/// and the --version flag, and the storage options of the shared settings file.

#pragma once

#include <JuceHeader.h>

namespace utils
{
juce::PropertiesFile::Options createApplicationSettingsOptions();

juce::StringArray systemInfo();

juce::String aboutMessage(const juce::String& appName);

juce::String versionLine(const juce::String& appName);
}  // namespace utils
