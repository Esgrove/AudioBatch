/// Out-of-process plugin scanning, so a plugin that crashes while it is being scanned
/// cannot take down the application.
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

/// Unique id that marks the command line of a plugin scan worker process.
constexpr auto pluginScanWorkerProcessUid = "audiobatch-plugin-scan-worker";

/// Returns true when the command line launches this executable as a plugin scan worker.
[[nodiscard]] bool isPluginScanWorkerCommandLine(const juce::String& commandLine);

/// Child side of out-of-process plugin scanning.
/// Receives scan requests from the main process, loads the plugin file on the message thread,
/// and replies with the plugin descriptions found in it.
class PluginScanWorker final : juce::ChildProcessWorker, juce::AsyncUpdater
{
public:
    /// Creates the worker with the default plugin formats.
    /// The connection-lost callback runs when the main process goes away or finishes scanning,
    /// and should shut the worker process down.
    explicit PluginScanWorker(std::function<void()> connectionLostCallback);

    /// Cancels any pending scan dispatch before the worker is destroyed.
    ~PluginScanWorker() override;

    /// Connects to the main process when the command line identifies a scan worker.
    /// Returns false for any other command line, in which case the worker does nothing.
    bool initialiseFromCommandLine(const juce::String& commandLine);

private:
    /// Queues a scan request and schedules it on the message thread.
    /// Called on the connection thread.
    void handleMessageFromCoordinator(const juce::MemoryBlock& message) override;

    /// Invokes the connection-lost callback so the worker process can exit.
    void handleConnectionLost() override;

    /// Runs queued scan requests on the message thread and sends the results back.
    /// Scanning must happen on the message thread,
    /// because copy-protected plugins (for example PACE / iLok wrapped VST3s)
    /// dispatch licensing work to the main thread while their factory is being enumerated.
    void handleAsyncUpdate() override;

    /// Loads the requested plugin file and returns the plugin descriptions it contains.
    [[nodiscard]] juce::OwnedArray<juce::PluginDescription> scan(const juce::MemoryBlock& request);

    /// Serializes the descriptions as XML and sends them to the main process.
    void sendResults(const juce::OwnedArray<juce::PluginDescription>& results);

    juce::AudioPluginFormatManager formatManager;
    std::function<void()> onConnectionLost;
    std::mutex pendingRequestsMutex;
    std::queue<juce::MemoryBlock> pendingRequests;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginScanWorker)
};

/// Main-process side of out-of-process plugin scanning.
/// Install it with KnownPluginList::setCustomScanner.
/// Each plugin file is sent to a worker process launched from this executable,
/// and the worker is relaunched after a crash so the scan can continue with the next plugin.
class OutOfProcessPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    OutOfProcessPluginScanner();

    /// Stops the worker process, if one is running.
    ~OutOfProcessPluginScanner() override;

    /// Scans one plugin file in the worker process and adds the descriptions it found.
    /// Blocks until the worker replies, the worker process dies, or the scan is cancelled.
    /// Returns false only when the worker crashed, which makes KnownPluginList blacklist the plugin.
    bool findPluginTypesFor(
        juce::AudioPluginFormat& format,
        juce::OwnedArray<juce::PluginDescription>& result,
        const juce::String& fileOrIdentifier
    ) override;

    /// Stops the worker process once the scan has finished.
    void scanFinished() override;

private:
    class WorkerProcess;

    std::unique_ptr<WorkerProcess> worker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutOfProcessPluginScanner)
};
