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
/// Uses the same check as ChildProcessWorker: the worker prefix must start the command line.
[[nodiscard]] bool isPluginScanWorkerCommandLine(const juce::String& commandLine);

/// Returns the KnownPlugins.xml file next to the AudioBatch settings file.
/// The known-plugins list lives in its own file instead of the settings,
/// so the GUI and the CLI can each update it without overwriting the other one's settings.
[[nodiscard]] juce::File getKnownPluginListFile();

/// Restores the known-plugins list from KnownPlugins.xml.
/// When that file does not exist yet, reads the list stored in the settings by older versions instead.
/// Leaves the list unchanged when nothing has been stored.
void loadKnownPluginList(const juce::PropertiesFile& settings, juce::KnownPluginList& knownPluginList);

/// Writes the known-plugins list, including blacklisted plugins, to KnownPlugins.xml.
/// The file is replaced atomically, so a concurrent reader never sees a partial list.
/// Returns false and logs an error when the file could not be written.
[[nodiscard]] bool saveKnownPluginList(const juce::KnownPluginList& knownPluginList);

/// Moves a known-plugins list stored in the settings by older versions into KnownPlugins.xml,
/// and removes it from the settings.
/// Only the GUI calls this, since it owns the settings file.
void migrateKnownPluginList(juce::PropertiesFile& settings);

/// Returns the dead man's pedal file that records the plugin being scanned,
/// so a plugin that crashed a scan is blacklisted on the next one.
[[nodiscard]] juce::File getPluginScanCrashLogFile();

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
    /// Creates the scanner without starting a worker.
    /// The worker process is launched on the first scan request,
    /// and relaunched after a crash or a cancelled scan.
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
