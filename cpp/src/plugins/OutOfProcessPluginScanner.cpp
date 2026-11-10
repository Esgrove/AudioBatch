/// Launches isolated plugin scan workers, waits for their replies, and handles crashes, cancellation, and timeouts.

#include "plugins/OutOfProcessPluginScanner.h"

#include "plugins/PluginScanner.h"
#include "utils/Log.h"
#include <condition_variable>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace audiobatch
{
namespace
{
/// Outcome of waiting for the reply to one scan request.
enum class PluginScanResponseState : std::uint8_t { timeout, gotResult, connectionLost };

/// Reply to one scan request.
struct PluginScanResponse {
    /// Whether a reply arrived, the wait timed out, or the worker went away.
    PluginScanResponseState state {PluginScanResponseState::timeout};
    /// The plugin descriptions the worker replied with, or null without a reply.
    std::unique_ptr<juce::XmlElement> xml;
};

/// How often waiting for a worker reply checks whether the scan was cancelled.
constexpr auto RESPONSE_POLL_INTERVAL {std::chrono::milliseconds(50)};

/// Adds the plugin descriptions in a worker reply to the scan results, skipping entries that do not parse.
void addDescriptions(const juce::XmlElement& reply, juce::OwnedArray<juce::PluginDescription>& result)
{
    for (const auto* element : reply.getChildIterator()) {
        if (auto description = std::make_unique<juce::PluginDescription>(); description->loadFromXml(*element)) {
            result.add(std::move(description));
        }
    }
}

/// Builds a scan request for one plugin file or identifier.
juce::MemoryBlock createScanRequest(const juce::String& formatName, const juce::String& fileOrIdentifier)
{
    juce::MemoryBlock request;
    juce::MemoryOutputStream stream(request, false);
    stream.writeString(formatName);
    stream.writeString(fileOrIdentifier);
    return request;
}
}  // namespace

/// Owns one launched worker process and hands its replies to the waiting scan call.
/// Replies and connection loss arrive on the connection thread,
/// while the scan call waits on whichever thread KnownPluginList scans from.
class PluginScanWorkerProcess final : juce::ChildProcessCoordinator
{
public:
    /// Launches this executable as a scan worker.
    /// Check isRunning afterwards, since the launch can fail.
    PluginScanWorkerProcess()
    {
        // The worker's stdout and stderr are not read, so do not capture them.
        // An undrained pipe would block the worker once it fills up.
        running = launchWorkerProcess(
            juce::File::getSpecialLocation(juce::File::currentExecutableFile), PLUGIN_SCAN_WORKER_PROCESS_UID, 0, 0
        );
    }

    /// Kills the worker process.
    ~PluginScanWorkerProcess() override
    {
        killWorkerProcess();
    }

    PluginScanWorkerProcess(const PluginScanWorkerProcess&) = delete;
    PluginScanWorkerProcess& operator=(const PluginScanWorkerProcess&) = delete;
    PluginScanWorkerProcess(PluginScanWorkerProcess&&) = delete;
    PluginScanWorkerProcess& operator=(PluginScanWorkerProcess&&) = delete;

    /// Waits a short while for the reply to the last request.
    /// Returns a timeout response when nothing arrived yet, so the caller can check for cancellation.
    /// A reply wins over a lost connection that arrived with it,
    /// since the worker finished the plugin before it went away and the plugin must not be blacklisted.
    PluginScanResponse waitForResponse()
    {
        std::unique_lock lock(mutex);

        if (!condition.wait_for(lock, RESPONSE_POLL_INTERVAL, [this] { return gotResult || connectionLost; })) {
            return {};
        }

        PluginScanResponse response;
        response.state = gotResult ? PluginScanResponseState::gotResult : PluginScanResponseState::connectionLost;
        response.xml = std::move(resultXml);
        gotResult = false;
        return response;
    }

    /// True once the worker process has gone away.
    [[nodiscard]] bool hasLostConnection()
    {
        const std::scoped_lock lock(mutex);
        return connectionLost;
    }

    /// True when the worker process was launched and connected.
    [[nodiscard]] bool isRunning() const noexcept
    {
        return running;
    }

    /// Sends a scan request to the worker. Returns false when the worker is unreachable.
    bool sendRequest(const juce::MemoryBlock& request)
    {
        return sendMessageToWorker(request);
    }

private:
    /// Records that the worker died, usually because the plugin crashed it, and wakes the waiting scan call.
    void handleConnectionLost() override
    {
        const std::scoped_lock lock(mutex);
        connectionLost = true;
        condition.notify_one();
    }

    /// Stores the worker's reply and wakes the waiting scan call.
    void handleMessageFromWorker(const juce::MemoryBlock& message) override
    {
        const std::scoped_lock lock(mutex);
        resultXml = juce::parseXML(message.toString());
        gotResult = true;
        condition.notify_one();
    }

    /// Guards the reply state, which the coordinator thread writes and the scan call reads.
    /// Keep the mutex alive until the condition variable has been destroyed.
    std::mutex mutex;
    /// Wakes the scan call when a reply arrives or the worker goes away.
    std::condition_variable condition;
    /// The plugin descriptions of the last reply.
    std::unique_ptr<juce::XmlElement> resultXml;

    /// True once the worker process went away.
    bool connectionLost {false};
    /// True once a reply to the current request arrived.
    bool gotResult {false};
    /// True when the worker process was launched and connected.
    bool running {false};

    JUCE_LEAK_DETECTOR(PluginScanWorkerProcess)
};

/// Creates the scanner without starting a worker.
/// The worker process is launched on the first scan request, and relaunched after a crash or a cancelled scan.
/// The optional callbacks let the owner coordinate the scan with other processes,
/// for example through a PluginScanLock.
OutOfProcessPluginScanner::OutOfProcessPluginScanner(
    ScanStartingCallback scanStartingCallback,
    ScanFinishedCallback scanFinishedCallback
) :
    onScanStarting(std::move(scanStartingCallback)),
    onScanFinished(std::move(scanFinishedCallback))
{}

/// Stops the worker process, if one is running.
OutOfProcessPluginScanner::~OutOfProcessPluginScanner() = default;

/// Scans one plugin file in the worker process and adds the descriptions it found.
/// Blocks until the worker replies, the worker process dies, the scan is cancelled, or the per-plugin timeout passes.
/// A worker that times out is dropped, since it may be stuck inside the plugin.
/// Returns false only when the worker crashed, which makes KnownPluginList blacklist the plugin.
bool OutOfProcessPluginScanner::findPluginTypesFor(
    juce::AudioPluginFormat& format,
    juce::OwnedArray<juce::PluginDescription>& result,
    const juce::String& fileOrIdentifier
)
{
    if (!scanStarted) {
        scanStarted = true;
        scanSkipped = onScanStarting != nullptr && !onScanStarting([this] { return shouldExit(); });
        if (scanSkipped) {
            // Ends the scan after this plugin, without listing the plugins it did not load as failed.
            abandonScan();
        }
    }

    if (scanSkipped) {
        // Report the plugin as scanned without loading it, so it is not blacklisted.
        return true;
    }

    if (worker == nullptr) {
        worker = std::make_unique<PluginScanWorkerProcess>();

        if (!worker->isRunning()) {
            // Without a worker there is nothing to isolate the plugin with.
            // Skip it instead of blacklisting, so a later scan can try again.
            utils::logErrorAlways("Could not launch the plugin scan worker process");
            worker.reset();
            return true;
        }
    }

    if (!worker->sendRequest(createScanRequest(format.getName(), fileOrIdentifier))) {
        utils::logWarn("Plugin scan worker is not reachable, skipping {}", fileOrIdentifier.quoted());
        worker.reset();
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + PLUGIN_SCAN_TIMEOUT;

    for (;;) {
        if (shouldExit()) {
            // The scan was cancelled. Drop the worker, which may be stuck inside the plugin.
            worker.reset();
            return true;
        }

        auto [state, xml] = worker->waitForResponse();

        switch (state) {
            case PluginScanResponseState::timeout:
                if (std::chrono::steady_clock::now() >= deadline) {
                    // Skipped instead of blacklisted, so a plugin that was only slow is scanned again next time.
                    utils::logWarn(
                        "Plugin scan timed out after {} seconds, skipping {}",
                        PLUGIN_SCAN_TIMEOUT.count(),
                        fileOrIdentifier.quoted()
                    );
                    timedOutFiles.addIfNotAlreadyThere(fileOrIdentifier);
                    worker.reset();
                    return true;
                }
                continue;

            case PluginScanResponseState::connectionLost:
                utils::logWarn("Plugin scan worker crashed while scanning {}", fileOrIdentifier.quoted());
                worker.reset();
                return false;

            case PluginScanResponseState::gotResult:
                if (xml != nullptr) {
                    addDescriptions(*xml, result);
                }
                // A worker that went away after replying is relaunched for the next plugin.
                if (worker->hasLostConnection()) {
                    worker.reset();
                }
                return true;
        }
    }
}

/// Stops the worker process once the scan has finished and invokes the scan-finished callback.
/// KnownPluginList clears the abandon request only after this call, so shouldExit still tells a cancelled scan apart.
void OutOfProcessPluginScanner::scanFinished()
{
    worker.reset();

    if (scanStarted && onScanFinished != nullptr) {
        onScanFinished(!scanSkipped && !shouldExit());
    }

    scanStarted = false;
    scanSkipped = false;
}
}  // namespace audiobatch
