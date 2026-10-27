/// Implementation of out-of-process plugin scanning.
/// Covers the request and reply protocol between the main process and the scan worker:
/// a request carries the format name and the plugin file or identifier,
/// and a reply is an XML list of plugin descriptions.
/// Also covers the worker process lifetime in the main process,
/// which launches a worker on demand and drops it after a crash, a cancel, or the end of a scan,
/// and the known-plugins list file with its migration out of the settings.
/// The plugin formats are registered with an Audio Unit format that leaves out the Audio Units built into macOS,
/// and loading the known-plugins list drops any of them that an older version saved.

#include "plugins/PluginScanner.h"

#include "utils/AppInfo.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include <condition_variable>

#include <chrono>

namespace audiobatch
{
namespace
{
constexpr auto resultsXmlTag {"PLUGINS"};
constexpr auto knownPluginListFileName {"KnownPlugins.xml"};
constexpr auto legacyKnownPluginListPropertyKey {"knownPluginList"};
constexpr auto audioUnitIdentifierPrefix {"AudioUnit:"};
constexpr auto appleManufacturerCode {"appl"};
constexpr auto vst3PathVariable {"VST3_PATH"};
constexpr auto responsePollInterval {std::chrono::milliseconds(50)};

/// Outcome of waiting for the reply to one scan request.
enum class PluginScanResponseState : std::uint8_t { timeout, gotResult, connectionLost };

/// Reply to one scan request.
struct PluginScanResponse {
    PluginScanResponseState state {PluginScanResponseState::timeout};
    std::unique_ptr<juce::XmlElement> xml;
};

/// Adds the plugin descriptions in a worker reply to the scan results, skipping entries that do not parse.
void addDescriptions(const juce::XmlElement* reply, juce::OwnedArray<juce::PluginDescription>& result)
{
    if (reply == nullptr) {
        return;
    }

    for (const auto* element : reply->getChildIterator()) {
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

#if JUCE_PLUGINHOST_AU && JUCE_MAC
/// Audio Unit format that leaves the Audio Units built into macOS out of plugin scans.
/// Only the list of plugins to scan is filtered, so every other Audio Unit scans and loads as usual.
class AudioUnitFormatWithoutBuiltInPlugins final : public juce::AudioUnitPluginFormat
{
public:
    /// Lists the registered Audio Units except the ones built into macOS.
    juce::StringArray searchPathsForPlugins(
        const juce::FileSearchPath& directoriesToSearch,
        const bool recursive,
        const bool allowPluginsWhichRequireAsynchronousInstantiation
    ) override
    {
        juce::StringArray identifiers;

        for (const auto& identifier : AudioUnitPluginFormat::searchPathsForPlugins(
                 directoriesToSearch, recursive, allowPluginsWhichRequireAsynchronousInstantiation
             ))
        {
            if (!isBuiltInApplePlugin(identifier)) {
                identifiers.add(identifier);
            }
        }

        return identifiers;
    }
};
#endif

#if JUCE_PLUGINHOST_VST3
/// VST3 format whose default search path splits VST3_PATH with the separator of the platform's path lists.
/// JUCE replaces every ":" in the variable with ";", which splits a Windows path such as C:\Plugins in two.
class VST3FormatWithPlatformSearchPath final : public juce::VST3PluginFormat
{
public:
    /// Returns the folders of VST3_PATH, then the standard VST3 folders of the platform.
    juce::FileSearchPath getDefaultLocationsToSearch() override
    {
        return buildVst3SearchPath(
            VST3PluginFormat::getDefaultLocationsToSearch(),
            juce::SystemStats::getEnvironmentVariable(vst3PathVariable, {}),
            juce::File::getSeparatorChar() == '\\' ? ";" : ":"
        );
    }
};
#endif
}  // namespace

/// Rebuilds the search path JUCE made from VST3_PATH and the standard folders,
/// with the variable split at `listSeparator`, the separator of the platform's path lists.
/// JUCE splits the variable at both ";" and ":", so the entries it made from the variable are taken out first,
/// leaving the standard folders, which then follow the variable's folders without duplicates.
/// Works on the raw strings, because an entry such as "C" is not a valid juce::File.
juce::FileSearchPath buildVst3SearchPath(
    const juce::FileSearchPath& juceSearchPath,
    const juce::String& variableValue,
    const juce::String& listSeparator
)
{
    auto juceVariableEntries = juce::StringArray::fromTokens(variableValue.replace(":", ";"), ";", "\"");
    juceVariableEntries.trim();
    juceVariableEntries.removeEmptyStrings();

    auto entries = juce::StringArray::fromTokens(variableValue, listSeparator, "\"");
    entries.trim();
    entries.removeEmptyStrings();

    for (int index = 0; index < juceSearchPath.getNumPaths(); ++index) {
        const auto entry = juceSearchPath.getRawString(index);
        if (const auto variableIndex = juceVariableEntries.indexOf(entry); variableIndex >= 0) {
            juceVariableEntries.remove(variableIndex);
            continue;
        }
        entries.add(entry);
    }

    entries.removeDuplicates(!juce::File::areFileNamesCaseSensitive());
    return {entries.joinIntoString(";")};
}

/// Returns true for an Audio Unit built into macOS, such as AUPeakLimiter or AUGraphicEQ.
/// They are recognised by the Apple manufacturer code that ends their identifier,
/// for example "AudioUnit:Effects/aufx,lmtr,appl".
bool isBuiltInApplePlugin(const juce::String& fileOrIdentifier)
{
    return fileOrIdentifier.startsWith(audioUnitIdentifierPrefix)
        && fileOrIdentifier.fromLastOccurrenceOf(",", false, false) == appleManufacturerCode;
}

/// Registers the VST3 and Audio Unit formats that CMakeLists.txt enables, in the order juce::addDefaultFormatsToManager
/// uses, with an Audio Unit format that leaves the Audio Units built into macOS out of plugin scans,
/// and a VST3 format that keeps Windows drive letters in VST3_PATH intact.
void addPluginFormats([[maybe_unused]] juce::AudioPluginFormatManager& formatManager)
{
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    formatManager.addFormat(std::make_unique<AudioUnitFormatWithoutBuiltInPlugins>());
#endif

#if JUCE_PLUGINHOST_VST3
    formatManager.addFormat(std::make_unique<VST3FormatWithPlatformSearchPath>());
#endif
}

/// Removes the Audio Units built into macOS from a known-plugins list, including blacklisted ones,
/// since the scans leave them out and a list saved by an older version may still contain them.
void removeBuiltInApplePlugins(juce::KnownPluginList& knownPluginList)
{
    for (const auto& type : knownPluginList.getTypes()) {
        if (isBuiltInApplePlugin(type.fileOrIdentifier)) {
            knownPluginList.removeType(type);
        }
    }

    // A copy, since removeFromBlacklist changes the array that getBlacklistedFiles refers to.
    const auto blacklistedFiles = knownPluginList.getBlacklistedFiles();
    for (const auto& file : blacklistedFiles) {
        if (isBuiltInApplePlugin(file)) {
            knownPluginList.removeFromBlacklist(file);
        }
    }
}

/// Removes the plugins and blacklist entries whose files are gone from a search folder that still exists,
/// the same rule as in the Rust version.
/// Plugins outside the searched folders, such as Audio Units or plugins on a disconnected drive, are kept.
/// Returns the number of plugins removed.
int removeMissingPlugins(
    juce::KnownPluginList& knownPluginList,
    const juce::AudioPluginFormatManager& formatManager,
    juce::PropertiesFile& settings
)
{
    juce::Array<juce::File> existingFolders;
    for (auto* format : formatManager.getFormats()) {
        const auto searchPath = juce::PluginListComponent::getLastSearchPath(settings, *format);
        for (int index = 0; index < searchPath.getNumPaths(); ++index) {
            if (const auto folder = searchPath[index]; folder.isDirectory()) {
                existingFolders.addIfNotAlreadyThere(folder);
            }
        }
    }

    const auto isMissing = [&existingFolders](const juce::String& fileOrIdentifier) {
        if (!juce::File::isAbsolutePath(fileOrIdentifier)) {
            return false;
        }
        const juce::File file(fileOrIdentifier);
        return !file.exists() && std::ranges::any_of(existingFolders, [&file](const juce::File& folder) {
            return file.isAChildOf(folder);
        });
    };

    int removedCount = 0;
    for (const auto& type : knownPluginList.getTypes()) {
        if (isMissing(type.fileOrIdentifier)) {
            knownPluginList.removeType(type);
            ++removedCount;
        }
    }

    // A copy, since removeFromBlacklist changes the array that getBlacklistedFiles refers to.
    const auto blacklistedFiles = knownPluginList.getBlacklistedFiles();
    for (const auto& file : blacklistedFiles) {
        if (isMissing(file)) {
            knownPluginList.removeFromBlacklist(file);
        }
    }

    if (removedCount > 0) {
        utils::logInfo("Removed {} plugins that no longer exist", removedCount);
    }
    return removedCount;
}

/// Returns true when the command line launches this executable as a plugin scan worker.
/// Uses the same check as ChildProcessWorker: the worker prefix must start the command line.
bool isPluginScanWorkerCommandLine(const juce::String& commandLine)
{
    // ChildProcessCoordinator passes "--<uid>:<pipe name>" as the first argument.
    // Match ChildProcessWorker::initialiseFromCommandLine exactly,
    // so the same text in a later argument of an ordinary launch is not mistaken for a worker.
    return commandLine.trim().startsWith(utils::format("--{}:", pluginScanWorkerProcessUid));
}

/// Returns the KnownPlugins.xml file next to the given settings file.
/// The known-plugins list lives in its own file instead of the settings,
/// so the GUI and the CLI can each update it without overwriting the other one's settings.
juce::File getKnownPluginListFile(const juce::PropertiesFile& settings)
{
    return settings.getFile().getSiblingFile(knownPluginListFileName);
}

/// Restores the known-plugins list from the KnownPlugins.xml file next to the settings file.
/// When that file does not exist yet, reads the list stored in the settings by older versions instead.
/// Leaves the list unchanged when nothing has been stored.
/// Drops the Audio Units built into macOS, which older versions scanned.
void loadKnownPluginList(const juce::PropertiesFile& settings, juce::KnownPluginList& knownPluginList)
{
    if (const auto file = getKnownPluginListFile(settings); file.existsAsFile()) {
        if (const auto element = juce::parseXML(file); element != nullptr) {
            knownPluginList.recreateFromXml(*element);
            removeBuiltInApplePlugins(knownPluginList);
        } else {
            utils::logError("Could not parse the known plugins list: {}", file.getFullPathName().quoted());
        }

        return;
    }

    // Older versions stored the list in the settings.
    const auto legacyXmlText = settings.getValue(legacyKnownPluginListPropertyKey);
    if (const auto element = juce::parseXML(legacyXmlText); element != nullptr) {
        knownPluginList.recreateFromXml(*element);
        removeBuiltInApplePlugins(knownPluginList);
    }
}

/// Writes the known-plugins list, including blacklisted plugins, to the given file.
/// The file is replaced atomically, so a concurrent reader never sees a partial list.
/// Returns false and logs an error when the file could not be written.
bool saveKnownPluginList(const juce::KnownPluginList& knownPluginList, const juce::File& file)
{
    const auto xml = knownPluginList.createXml();

    // writeTo writes a temporary file and then replaces the target,
    // so a concurrent reader never sees a partially written list.
    if (xml == nullptr || !file.getParentDirectory().createDirectory() || !xml->writeTo(file)) {
        utils::logError("Could not save the known plugins list: {}", file.getFullPathName().quoted());
        return false;
    }

    return true;
}

/// Moves a known-plugins list stored in the settings by older versions into KnownPlugins.xml,
/// and removes it from the settings.
/// Only the GUI calls this, since it owns the settings file.
void migrateKnownPluginList(juce::PropertiesFile& settings)
{
    if (!settings.containsKey(legacyKnownPluginListPropertyKey)) {
        return;
    }

    if (const auto file = getKnownPluginListFile(settings); !file.existsAsFile()) {
        juce::KnownPluginList legacyList;
        loadKnownPluginList(settings, legacyList);

        if (!saveKnownPluginList(legacyList, file)) {
            // Keep the legacy value so nothing is lost, and try again on the next start.
            return;
        }

        utils::logInfo("Moved the known plugins list to {}", file.getFullPathName().quoted());
    }

    settings.removeValue(legacyKnownPluginListPropertyKey);
    settings.saveIfNeeded();
}

//==============================================================================

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
            juce::File::getSpecialLocation(juce::File::currentExecutableFile), pluginScanWorkerProcessUid, 0, 0
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

    /// True when the worker process was launched and connected.
    [[nodiscard]] bool isRunning() const noexcept
    {
        return running;
    }

    /// True once the worker process has gone away.
    [[nodiscard]] bool hasLostConnection()
    {
        const std::scoped_lock lock(mutex);
        return connectionLost;
    }

    /// Sends a scan request to the worker. Returns false when the worker is no longer reachable.
    bool sendRequest(const juce::MemoryBlock& request)
    {
        return sendMessageToWorker(request);
    }

    /// Waits a short while for the reply to the last request.
    /// Returns a timeout response when nothing arrived yet, so the caller can check for cancellation.
    /// A reply wins over a lost connection that arrived with it,
    /// since the worker finished the plugin before it went away and the plugin must not be blacklisted.
    PluginScanResponse waitForResponse()
    {
        std::unique_lock lock(mutex);

        if (!condition.wait_for(lock, responsePollInterval, [this] { return gotResult || connectionLost; })) {
            return {};
        }

        PluginScanResponse response;
        response.state = gotResult ? PluginScanResponseState::gotResult : PluginScanResponseState::connectionLost;
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
    bool gotResult {false};
    bool connectionLost {false};
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

    const auto deadline = std::chrono::steady_clock::now() + pluginScanTimeout;

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
                        pluginScanTimeout.count(),
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
                addDescriptions(xml.get(), result);
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

//==============================================================================

/// Creates the lock without taking it.
/// Every process that uses the same name shares the lock, and tests pass their own name to stay apart from the app.
PluginScanLock::PluginScanLock(const juce::String& lockName) : lock(lockName) {}

/// Releases the lock if it is held.
PluginScanLock::~PluginScanLock()
{
    release();
}

/// Tries to take the lock without waiting.
/// Returns false when another process is scanning.
/// Taking a lock that is already held by this object succeeds.
bool PluginScanLock::tryAcquire()
{
    if (!held) {
        held = lock.enter(0);
    }

    return held;
}

/// Releases the lock if it is held.
void PluginScanLock::release()
{
    if (held) {
        lock.exit();
        held = false;
    }
}
}  // namespace audiobatch
