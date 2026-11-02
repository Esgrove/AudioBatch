/// Shared plugin scan helpers and the inter-process scan lock.
/// Registers plugin formats, filters built-in Apple Audio Units, and persists the known-plugins list.

#pragma once

#include <JuceHeader.h>

#include <chrono>

namespace audiobatch
{
/// Unique id that marks the command line of a plugin scan worker process.
constexpr auto pluginScanWorkerProcessUid {"audiobatch-plugin-scan-worker"};

/// Longest wait for the worker to scan one plugin, the same as in the Rust version.
/// Without it, a plugin that hangs in the worker would block the scan for good:
/// the CLI rescan cannot be cancelled, and the GUI scan would wait until the user cancels it.
/// Generous, because a copy-protected plugin can show a licensing prompt in the worker while it is scanned.
constexpr auto pluginScanTimeout {std::chrono::seconds(180)};

/// Name of the inter-process lock that the GUI and the CLI take around a plugin scan.
constexpr auto pluginScanLockName {"AudioBatchPluginScan"};

/// Inter-process lock that serializes plugin scans between the GUI and the CLI.
/// Each scan loads the known-plugins list, adds to it, and saves it,
/// so two overlapping scans would overwrite each other's results.
/// Holding this lock from loading the list to saving it prevents that.
class PluginScanLock
{
public:
    explicit PluginScanLock(const juce::String& lockName = pluginScanLockName);

    ~PluginScanLock();

    PluginScanLock(const PluginScanLock&) = delete;
    PluginScanLock& operator=(const PluginScanLock&) = delete;
    PluginScanLock(PluginScanLock&&) = delete;
    PluginScanLock& operator=(PluginScanLock&&) = delete;

    [[nodiscard]] bool tryAcquire();

    void release();

private:
    /// The lock shared with the other processes.
    juce::InterProcessLock lock;
    /// True while this object holds the lock.
    bool held {false};

    JUCE_LEAK_DETECTOR(PluginScanLock)
};

[[nodiscard]] juce::File getKnownPluginListFile(const juce::PropertiesFile& settings);
[[nodiscard]] juce::FileSearchPath buildVst3SearchPath(
    const juce::FileSearchPath& juceSearchPath,
    const juce::String& variableValue,
    const juce::String& listSeparator
);

[[nodiscard]] bool isBuiltInApplePlugin(const juce::String& fileOrIdentifier);
[[nodiscard]] bool isPluginScanWorkerCommandLine(const juce::String& commandLine);
[[nodiscard]] bool saveKnownPluginList(const juce::KnownPluginList& knownPluginList, const juce::File& file);
int removeMissingPlugins(
    juce::KnownPluginList& knownPluginList,
    const juce::AudioPluginFormatManager& formatManager,
    juce::PropertiesFile& settings
);

void addPluginFormats(juce::AudioPluginFormatManager& formatManager);
void loadKnownPluginList(const juce::PropertiesFile& settings, juce::KnownPluginList& knownPluginList);
void migrateKnownPluginList(juce::PropertiesFile& settings);
void removeBuiltInApplePlugins(juce::KnownPluginList& knownPluginList);
}  // namespace audiobatch
