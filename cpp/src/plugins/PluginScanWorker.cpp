/// Receives plugin scan requests in the child process, dispatches them on the message thread,
/// and serializes the resulting descriptions back to the main process.

#include "plugins/PluginScanWorker.h"

#include "plugins/PluginScanner.h"
#include "utils/Log.h"

#include <functional>
#include <mutex>
#include <utility>

namespace audiobatch
{
namespace
{
/// Tag of the element that holds the plugin descriptions of a worker reply.
constexpr auto resultsXmlTag {"PLUGINS"};
}  // namespace

/// Creates the worker with the plugin formats of the main process.
/// The connection-lost callback runs when the main process goes away or finishes scanning,
/// and should shut the worker process down.
PluginScanWorker::PluginScanWorker(std::function<void()> connectionLostCallback) :
    onConnectionLost(std::move(connectionLostCallback))
{
    addPluginFormats(formatManager);
}

/// Cancels any pending scan dispatch before the worker is destroyed.
PluginScanWorker::~PluginScanWorker()
{
    cancelPendingUpdate();
}

/// Connects to the main process when the command line identifies a scan worker.
/// Returns false for any other command line, in which case the worker does nothing.
bool PluginScanWorker::initialiseFromCommandLine(const juce::String& commandLine)
{
    return ChildProcessWorker::initialiseFromCommandLine(commandLine, pluginScanWorkerProcessUid);
}

/// Queues a scan request and schedules it on the message thread.
/// Called on the connection thread.
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

/// Invokes the connection-lost callback so the worker process can exit.
void PluginScanWorker::handleConnectionLost()
{
    if (onConnectionLost != nullptr) {
        onConnectionLost();
    }
}

/// Runs queued scan requests on the message thread and sends the results back.
/// Scanning must happen on the message thread, because copy-protected plugins (for example PACE / iLok wrapped VST3s)
/// dispatch licensing work to the main thread while their factory is being enumerated.
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

/// Loads the requested plugin file and returns the plugin descriptions it contains.
juce::OwnedArray<juce::PluginDescription> PluginScanWorker::scan(const juce::MemoryBlock& request) const
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

/// Serializes the descriptions as XML and sends them to the main process.
void PluginScanWorker::sendResults(const juce::OwnedArray<juce::PluginDescription>& results)
{
    juce::XmlElement xml(resultsXmlTag);

    for (const auto* description : results) {
        xml.addChildElement(description->createXml().release());
    }

    const auto text = xml.toString();
    sendMessageToCoordinator({text.toRawUTF8(), text.getNumBytesAsUTF8()});
}
}  // namespace audiobatch
