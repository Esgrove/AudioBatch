/// Implementation of PluginLibrary.
/// Covers loading and saving the known-plugins list, the plugin manager component that scans on one background thread,
/// the plugin scan lock shared with CLI rescans, and creating one chain of plugin instances per worker.

#include "plugins/PluginLibrary.h"

#include "utils/AudioFormatUtils.h"
#include "utils/Log.h"

namespace audiobatch
{
namespace
{
/// Number of background threads the plugin manager scans on.
/// One thread keeps the window responsive while the worker process loads each plugin in turn.
constexpr int pluginScanThreadCount {1};
}  // namespace

/// Registers the plugin formats, installs the out-of-process scanner,
/// and restores the known-plugins list from its file.
PluginLibrary::PluginLibrary(juce::ApplicationProperties& applicationProperties) : appProperties(applicationProperties)
{
    addPluginFormats(formatManager);
    knownPluginList.addChangeListener(this);

    // Load plugins in a separate worker process while scanning,
    // so a plugin that crashes during the scan is blacklisted instead of taking down the app.
    // The plugin scan lock is held for the whole scan, so a CLI rescan cannot overwrite its results.
    knownPluginList.setCustomScanner(
        std::make_unique<OutOfProcessPluginScanner>(
            [this](const utils::CancellationCheck& shouldStop) { return beginPluginScan(shouldStop); },
            [this](const bool completed) { finishPluginScan(completed); }
        )
    );

    reload();
}

/// Stops saving the list on changes.
PluginLibrary::~PluginLibrary()
{
    knownPluginList.removeChangeListener(this);
}

/// Returns a copy of the known plugin descriptions, which is safe to use while a scan adds to the list.
juce::Array<juce::PluginDescription> PluginLibrary::getKnownTypes() const
{
    return knownPluginList.getTypes();
}

/// Returns the known effect plugins, the ones a chain can process audio with.
/// Instruments are left out of the menus that add to a chain, since a chain only processes audio.
juce::Array<juce::PluginDescription> PluginLibrary::getKnownEffectTypes() const
{
    auto types = knownPluginList.getTypes();
    types.removeIf([](const juce::PluginDescription& type) { return type.isInstrument; });
    return types;
}

/// Creates the plugin manager component that lists the known plugins and scans for new ones.
/// Reloads the known-plugins list from its file first, so changes from a CLI rescan are kept.
/// The caller owns the component and must destroy it before this library.
std::unique_ptr<juce::PluginListComponent> PluginLibrary::createScanComponent()
{
    // Pick up changes from a CLI rescan since the list was loaded,
    // so a scan from this window builds on them instead of saving over them.
    reload();

    // The plugins themselves are loaded by the OutOfProcessPluginScanner worker process,
    // which scans on its own message thread.
    // Copy-protected plugins (for example PACE / iLok wrapped VST3s) need that,
    // because they dispatch licensing work to the main thread while their factory is still being enumerated.
    // allowPluginsWhichRequireAsynchronousInstantiation stays false,
    // so asynchronous-only plugins (AUv3) are still left out of the scan.
    // They are not usable by this app anyway,
    // since all instantiation goes through the synchronous createPluginInstance.
    // No crash log file is given, because the worker process already reports plugins that crash.
    // The crash log would only catch this app quitting mid-scan, and would blacklist the plugin being scanned.
    constexpr bool allowAsynchronousInstantiation = false;
    auto component = std::make_unique<juce::PluginListComponent>(
        formatManager, knownPluginList, juce::File(), appProperties.getUserSettings(), allowAsynchronousInstantiation
    );

    // Without a scan thread the component scans on the message thread,
    // which then blocks while the worker process loads each plugin.
    component->setNumberOfThreadsForScanning(pluginScanThreadCount);
    return component;
}

/// Restores the known-plugins list from the KnownPlugins.xml file next to the settings file,
/// first moving a list stored in the settings by older versions into that file.
/// Skips a file that has not changed since this library last loaded or saved it,
/// and does nothing while a scan from the plugin manager adds to the list.
/// Must run on the message thread, which is also the thread that saves the list.
void PluginLibrary::reload()
{
    JUCE_ASSERT_MESSAGE_THREAD

    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr || scanning) {
        return;
    }

    migrateKnownPluginList(*settings);
    const auto modificationTime = getKnownPluginListFile(*settings).getLastModificationTime();
    if (listLoaded && modificationTime == loadedModificationTime) {
        return;
    }

    const juce::ScopedValueSetter reloadingSetter(reloading, true);
    loadKnownPluginList(*settings, knownPluginList);
    // The list now matches its file, so its change messages are delivered while saving is off.
    knownPluginList.dispatchPendingMessages();
    loadedModificationTime = modificationTime;
    listLoaded = true;
}

/// Creates one chain of instances of the enabled plugins per worker, for a processing run.
/// Must run on the message thread.
/// Stops at the first plugin that fails, since a chain missing any plugin must not run.
/// Captures the initial state of each plugin from the first chain.
ChainInstantiation PluginLibrary::instantiateChains(
    const std::vector<EnabledChainPlugin>& enabledPlugins,
    const int workerCount
) const
{
    // Use a reasonable default sample rate. The chain is prepared again per file with the file's rate.
    constexpr int defaultBlockSize = 1024;

    ChainInstantiation instantiation;
    instantiation.chains.reserve(static_cast<std::size_t>(workerCount));

    for (int worker = 0; worker < workerCount; ++worker) {
        std::vector<std::unique_ptr<juce::AudioPluginInstance>> chainInstances;
        chainInstances.reserve(enabledPlugins.size());

        for (const auto& [description, descriptorRef] : enabledPlugins) {
            juce::String error;
            auto instance
                = formatManager.createPluginInstance(description, utils::defaultSampleRate, defaultBlockSize, error);

            if (instance == nullptr) {
                instantiation.errorMessage = error.isNotEmpty() ? error : juce::String("Plugin instantiation failed");
                instantiation.failedPluginName = description.name;
                return instantiation;
            }

            if (worker == 0) {
                instance->getStateInformation(instantiation.initialStates.emplace_back());
            }

            chainInstances.push_back(std::move(instance));
        }

        instantiation.chains.push_back(std::move(chainInstances));
    }

    return instantiation;
}

/// Saves the known-plugins list to its KnownPlugins.xml file after a change on the message thread.
/// Changes during a scan from the plugin manager are saved once when the scan finishes,
/// since the scan thread changes the list while it runs.
/// Outside a scan the plugin scan lock is taken for the save, so it cannot overwrite the result of a CLI rescan.
void PluginLibrary::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source != &knownPluginList || reloading || scanning) {
        return;
    }

    if (!pluginScanLock.tryAcquire()) {
        utils::logWarn("Did not save the known plugins list, because another plugin scan is running");
        return;
    }

    saveList();
    pluginScanLock.release();
}

/// Writes the list to its file and remembers the file's new modification time, so reload skips it.
/// Failures are logged by saveKnownPluginList, and the in-memory list stays usable.
void PluginLibrary::saveList()
{
    const auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    if (const auto file = getKnownPluginListFile(*settings); saveKnownPluginList(knownPluginList, file)) {
        loadedModificationTime = file.getLastModificationTime();
    }
}

/// Prepares a scan from the plugin manager before its first plugin is loaded.
/// Called on the scan thread of the plugin manager, and does its work on the message thread,
/// where the lock, the reload, and the saves all happen, so the list is never reloaded while it is saved.
/// Takes the plugin scan lock and reloads the known-plugins list, so the scan builds on the latest saved list.
/// Returns false, and tells the user, when another process such as a CLI rescan is scanning,
/// or when the scan stops before the message thread got to it.
bool PluginLibrary::beginPluginScan(const utils::CancellationCheck& shouldStop)
{
    bool acquired {false};
    const bool ran = utils::callOnMessageThread(
        [this, &acquired] {
            acquired = pluginScanLock.tryAcquire();
            if (acquired) {
                // Another process may have saved a newer list since this one was loaded.
                reload();
                scanning = true;
            }
        },
        shouldStop
    );

    if (ran && !acquired) {
        utils::logWarn("Skipped a plugin scan because another plugin scan is running");
        juce::MessageManager::callAsync([] {
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions::makeOptionsOk(
                    juce::MessageBoxIconType::InfoIcon,
                    "Plugin Scan Skipped",
                    "Another plugin scan is running, for example audiobatch list --rescan in a terminal.\n\n"
                    "Scan again once it has finished.",
                    "OK"
                ),
                nullptr
            );
        });
    }

    return ran && acquired;
}

/// Saves the list a scan from the plugin manager added to, and releases the plugin scan lock.
/// A scan that ran to its end also removes the plugins whose files are gone from a scanned folder.
/// The plugin manager ends its scans on the message thread.
void PluginLibrary::finishPluginScan(const bool completed)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (!scanning) {
        return;
    }

    scanning = false;
    if (auto* settings = appProperties.getUserSettings(); completed && settings != nullptr) {
        removeMissingPlugins(knownPluginList, formatManager, *settings);
    }
    saveList();
    pluginScanLock.release();
}
}  // namespace audiobatch
