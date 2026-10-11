/// Implementation of out-of-process plugin scanning.
/// Covers the request and reply protocol between the main process and the scan worker:
/// a request carries the format name and the plugin file or identifier,
/// and a reply is an XML list of plugin descriptions.
/// Also covers the worker process lifetime in the main process,
/// which launches a worker on demand and drops it after a crash, a cancel, or the end of a scan.

#include "PluginScanner.h"

#include "utils.h"
#include <condition_variable>

#include <chrono>

namespace audiobatch::plugin_scanner
{
constexpr auto resultsXmlTag = "PLUGINS";
constexpr auto responsePollInterval = std::chrono::milliseconds(50);

/// Builds a scan request for one plugin file or identifier.
static juce::MemoryBlock createScanRequest(const juce::String& formatName, const juce::String& fileOrIdentifier)
{
    juce::MemoryBlock request;
    juce::MemoryOutputStream stream(request, false);
    stream.writeString(formatName);
    stream.writeString(fileOrIdentifier);
    return request;
}
}  // namespace audiobatch::plugin_scanner

using namespace audiobatch::plugin_scanner;

bool isPluginScanWorkerCommandLine(const juce::String& commandLine)
{
    // ChildProcessCoordinator passes "--<uid>:<pipe name>" to the worker process.
    return commandLine.contains(utils::format("--{}:", pluginScanWorkerProcessUid));
}

//==============================================================================

PluginScanWorker::PluginScanWorker(std::function<void()> connectionLostCallback) :
    onConnectionLost(std::move(connectionLostCallback))
{
    juce::addDefaultFormatsToManager(formatManager);
}

PluginScanWorker::~PluginScanWorker()
{
    cancelPendingUpdate();
}

bool PluginScanWorker::initialiseFromCommandLine(const juce::String& commandLine)
{
    return ChildProcessWorker::initialiseFromCommandLine(commandLine, pluginScanWorkerProcessUid);
}

void PluginScanWorker::handleMessageFromCoordinator(const juce::MemoryBlock& message)
{
    if (message.isEmpty()) {
        return;
    }

    {
        const std::scoped_lock lock(pendingRequestsMutex);
        pendingRequests.push(message);
    }

    triggerAsyncUpdate();
}

void PluginScanWorker::handleConnectionLost()
{
    if (onConnectionLost != nullptr) {
        onConnectionLost();
    }
}

void PluginScanWorker::handleAsyncUpdate()
{
    for (;;) {
        juce::MemoryBlock request;

        {
            const std::scoped_lock lock(pendingRequestsMutex);
            if (pendingRequests.empty()) {
                return;
            }

            request = std::move(pendingRequests.front());
            pendingRequests.pop();
        }

        sendResults(scan(request));
    }
}

juce::OwnedArray<juce::PluginDescription> PluginScanWorker::scan(const juce::MemoryBlock& request)
{
    juce::MemoryInputStream stream(request, false);
    const auto formatName = stream.readString();
    const auto fileOrIdentifier = stream.readString();

    juce::OwnedArray<juce::PluginDescription> results;

    for (auto* format : formatManager.getFormats()) {
        if (format->getName() == formatName) {
            utils::logInfo("Scanning {} plugin: {}", formatName, fileOrIdentifier.quoted());
            format->findAllTypesForFile(results, fileOrIdentifier);
            utils::logInfo("Found {} plugin types in {}", results.size(), fileOrIdentifier.quoted());
            return results;
        }
    }

    utils::logWarn("Unknown plugin format {} for {}", formatName.quoted(), fileOrIdentifier.quoted());
    return results;
}

void PluginScanWorker::sendResults(const juce::OwnedArray<juce::PluginDescription>& results)
{
    juce::XmlElement xml(resultsXmlTag);

    for (const auto* description : results) {
        xml.addChildElement(description->createXml().release());
    }

    const auto text = xml.toString();
    sendMessageToCoordinator({text.toRawUTF8(), text.getNumBytesAsUTF8()});
}

//==============================================================================

/// Owns one launched worker process and hands its replies to the waiting scan call.
/// Replies and connection loss arrive on the connection thread,
/// while the scan call waits on whichever thread KnownPluginList scans from.
class OutOfProcessPluginScanner::WorkerProcess final : juce::ChildProcessCoordinator
{
public:
    /// Outcome of waiting for the reply to one scan request.
    enum class ResponseState { timeout, gotResult, connectionLost };

    /// Reply to one scan request.
    struct Response {
        ResponseState state = ResponseState::timeout;
        std::unique_ptr<juce::XmlElement> xml;
    };

    /// Launches this executable as a scan worker.
    /// Check isRunning afterwards, since the launch can fail.
    WorkerProcess()
    {
        // The worker's stdout and stderr are not read, so do not capture them.
        // An undrained pipe would block the worker once it fills up.
        running = launchWorkerProcess(
            juce::File::getSpecialLocation(juce::File::currentExecutableFile), pluginScanWorkerProcessUid, 0, 0
        );
    }

    /// Kills the worker process.
    ~WorkerProcess() override
    {
        killWorkerProcess();
    }

    /// True when the worker process was launched and connected.
    [[nodiscard]] bool isRunning() const noexcept
    {
        return running;
    }

    /// Sends a scan request to the worker. Returns false when the worker is no longer reachable.
    bool sendRequest(const juce::MemoryBlock& request)
    {
        return sendMessageToWorker(request);
    }

    /// Waits a short while for the reply to the last request.
    /// Returns a timeout response when nothing arrived yet, so the caller can check for cancellation.
    Response waitForResponse()
    {
        std::unique_lock lock(mutex);

        if (!condition.wait_for(lock, responsePollInterval, [this] { return gotResult || connectionLost; })) {
            return {};
        }

        Response response;
        response.state = connectionLost ? ResponseState::connectionLost : ResponseState::gotResult;
        response.xml = std::move(resultXml);
        gotResult = false;
        return response;
    }

private:
    /// Stores the worker's reply and wakes the waiting scan call.
    void handleMessageFromWorker(const juce::MemoryBlock& message) override
    {
        const std::scoped_lock lock(mutex);
        resultXml = juce::parseXML(message.toString());
        gotResult = true;
        condition.notify_one();
    }

    /// Records that the worker died, usually because the plugin crashed it, and wakes the waiting scan call.
    void handleConnectionLost() override
    {
        const std::scoped_lock lock(mutex);
        connectionLost = true;
        condition.notify_one();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::unique_ptr<juce::XmlElement> resultXml;
    bool gotResult = false;
    bool connectionLost = false;
    bool running = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WorkerProcess)
};

OutOfProcessPluginScanner::OutOfProcessPluginScanner() = default;

OutOfProcessPluginScanner::~OutOfProcessPluginScanner() = default;

bool OutOfProcessPluginScanner::findPluginTypesFor(
    juce::AudioPluginFormat& format,
    juce::OwnedArray<juce::PluginDescription>& result,
    const juce::String& fileOrIdentifier
)
{
    if (worker == nullptr) {
        worker = std::make_unique<WorkerProcess>();

        if (!worker->isRunning()) {
            // Without a worker there is nothing to isolate the plugin with.
            // Skip it instead of blacklisting, so a later scan can try again.
            utils::logError("Could not launch the plugin scan worker process");
            worker.reset();
            return true;
        }
    }

    if (!worker->sendRequest(createScanRequest(format.getName(), fileOrIdentifier))) {
        utils::logWarn("Plugin scan worker is not reachable, skipping {}", fileOrIdentifier.quoted());
        worker.reset();
        return true;
    }

    for (;;) {
        if (shouldExit()) {
            // The scan was cancelled. Drop the worker, which may be stuck inside the plugin.
            worker.reset();
            return true;
        }

        auto response = worker->waitForResponse();

        switch (response.state) {
            case WorkerProcess::ResponseState::timeout:
                continue;

            case WorkerProcess::ResponseState::connectionLost:
                utils::logWarn("Plugin scan worker crashed while scanning {}", fileOrIdentifier.quoted());
                worker.reset();
                return false;

            case WorkerProcess::ResponseState::gotResult:
                if (response.xml != nullptr) {
                    for (const auto* element : response.xml->getChildIterator()) {
                        auto description = std::make_unique<juce::PluginDescription>();

                        if (description->loadFromXml(*element)) {
                            result.add(std::move(description));
                        }
                    }
                }

                return true;
        }
    }
}

void OutOfProcessPluginScanner::scanFinished()
{
    worker.reset();
}
