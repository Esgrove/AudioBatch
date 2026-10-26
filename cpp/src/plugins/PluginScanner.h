/// Out-of-process plugin scanning, so a plugin that crashes while it is being scanned cannot take down the application.
/// Declares PluginScanWorker, which runs in a child process launched from the application's own executable
/// and loads plugins to describe them,
/// and OutOfProcessPluginScanner, the KnownPluginList::CustomScanner the main process installs
/// to send each plugin file to the worker and collect the descriptions it reports back.
/// A worker crash is reported as a failed scan, which makes KnownPluginList blacklist that plugin.

#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <mutex>
#include <queue>

namespace audiobatch
{
/// Unique id that marks the command line of a plugin scan worker process.
constexpr auto pluginScanWorkerProcessUid {"audiobatch-plugin-scan-worker"};

[[nodiscard]] bool isPluginScanWorkerCommandLine(const juce::String& commandLine);
[[nodiscard]] bool saveKnownPluginList(const juce::KnownPluginList& knownPluginList);
[[nodiscard]] juce::File getKnownPluginListFile();
[[nodiscard]] juce::File getPluginScanCrashLogFile();

void loadKnownPluginList(const juce::PropertiesFile& settings, juce::KnownPluginList& knownPluginList);
void migrateKnownPluginList(juce::PropertiesFile& settings);

/// Child side of out-of-process plugin scanning.
/// Receives scan requests from the main process, loads the plugin file on the message thread,
/// and replies with the plugin descriptions found in it.
class PluginScanWorker final : juce::ChildProcessWorker, juce::AsyncUpdater
{
public:
    explicit PluginScanWorker(std::function<void()> connectionLostCallback);

    ~PluginScanWorker() override;

    PluginScanWorker(const PluginScanWorker&) = delete;
    PluginScanWorker& operator=(const PluginScanWorker&) = delete;
    PluginScanWorker(PluginScanWorker&&) = delete;
    PluginScanWorker& operator=(PluginScanWorker&&) = delete;

    bool initialiseFromCommandLine(const juce::String& commandLine);

private:
    void handleMessageFromCoordinator(const juce::MemoryBlock& message) override;

    void handleConnectionLost() override;

    void handleAsyncUpdate() override;

    void sendResults(const juce::OwnedArray<juce::PluginDescription>& results);

    [[nodiscard]] juce::OwnedArray<juce::PluginDescription> scan(const juce::MemoryBlock& request) const;

    juce::AudioPluginFormatManager formatManager;
    std::function<void()> onConnectionLost;
    std::mutex pendingRequestsMutex;
    std::queue<juce::MemoryBlock> pendingRequests;

    JUCE_LEAK_DETECTOR(PluginScanWorker)
};

class PluginScanWorkerProcess;

/// Main-process side of out-of-process plugin scanning.
/// Install it with KnownPluginList::setCustomScanner.
/// Each plugin file is sent to a worker process launched from this executable,
/// and the worker is relaunched after a crash so the scan can continue with the next plugin.
class OutOfProcessPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    /// Called before the first plugin of a scan is loaded.
    /// Returning false skips every plugin of that scan without blacklisting any of them.
    using ScanStartingCallback = std::function<bool()>;

    /// Called once a scan has finished, including scans that were skipped or cancelled.
    using ScanFinishedCallback = std::function<void()>;

    explicit OutOfProcessPluginScanner(
        ScanStartingCallback scanStartingCallback = nullptr,
        ScanFinishedCallback scanFinishedCallback = nullptr
    );

    ~OutOfProcessPluginScanner() override;

    OutOfProcessPluginScanner(const OutOfProcessPluginScanner&) = delete;
    OutOfProcessPluginScanner& operator=(const OutOfProcessPluginScanner&) = delete;
    OutOfProcessPluginScanner(OutOfProcessPluginScanner&&) = delete;
    OutOfProcessPluginScanner& operator=(OutOfProcessPluginScanner&&) = delete;

    bool findPluginTypesFor(
        juce::AudioPluginFormat& format,
        juce::OwnedArray<juce::PluginDescription>& result,
        const juce::String& fileOrIdentifier
    ) override;

    void scanFinished() override;

private:
    std::unique_ptr<PluginScanWorkerProcess> worker;
    ScanStartingCallback onScanStarting;
    ScanFinishedCallback onScanFinished;
    bool scanStarted {false};
    bool scanSkipped {false};

    JUCE_LEAK_DETECTOR(OutOfProcessPluginScanner)
};

/// Inter-process lock that serializes plugin scans between the GUI and the CLI.
/// Each scan loads the known-plugins list, adds to it, and saves it,
/// so two overlapping scans would overwrite each other's results.
/// Holding this lock from loading the list to saving it prevents that.
class PluginScanLock
{
public:
    PluginScanLock() = default;

    ~PluginScanLock();

    PluginScanLock(const PluginScanLock&) = delete;
    PluginScanLock& operator=(const PluginScanLock&) = delete;
    PluginScanLock(PluginScanLock&&) = delete;
    PluginScanLock& operator=(PluginScanLock&&) = delete;

    [[nodiscard]] bool tryAcquire();

    void release();

private:
    juce::InterProcessLock lock {"AudioBatchPluginScan"};
    bool held {false};

    JUCE_LEAK_DETECTOR(PluginScanLock)
};
}  // namespace audiobatch
