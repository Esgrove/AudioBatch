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

/// Registers the default plugin formats, installs the out-of-process scanner,
/// and restores the known-plugins list from its file.
PluginLibrary::PluginLibrary(juce::ApplicationProperties& applicationProperties) : appProperties(applicationProperties)
{
    juce::addDefaultFormatsToManager(formatManager);
    knownPluginList.addChangeListener(this);

    // Load plugins in a separate worker process while scanning,
    // so a plugin that crashes during the scan is blacklisted instead of taking down the app.
    // The plugin scan lock is held for the whole scan, so a CLI rescan cannot overwrite its results.
    knownPluginList.setCustomScanner(
        std::make_unique<OutOfProcessPluginScanner>(
            [this] { return beginPluginScan(); }, [this] { pluginScanLock.release(); }
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
    constexpr bool allowAsynchronousInstantiation = false;
    auto component = std::make_unique<juce::PluginListComponent>(
        formatManager,
        knownPluginList,
        getPluginScanCrashLogFile(),
        appProperties.getUserSettings(),
        allowAsynchronousInstantiation
    );

    // Without a scan thread the component scans on the message thread,
    // which then blocks while the worker process loads each plugin.
    component->setNumberOfThreadsForScanning(pluginScanThreadCount);
    return component;
}

/// Restores the known-plugins list from its KnownPlugins.xml file,
/// first moving a list stored in the settings by older versions into that file.
void PluginLibrary::reload()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    migrateKnownPluginList(*settings);
    loadKnownPluginList(*settings, knownPluginList);
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

/// Saves the known-plugins list to its KnownPlugins.xml file after every change.
void PluginLibrary::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &knownPluginList) {
        // Failures are logged by saveKnownPluginList, and the in-memory list stays usable.
        [[maybe_unused]] const auto saved = saveKnownPluginList(knownPluginList);
    }
}

/// Prepares a scan from the plugin manager before its first plugin is loaded.
/// Runs on the scan thread of the plugin manager.
/// Takes the plugin scan lock and reloads the known-plugins list, so the scan builds on the latest saved list.
/// Returns false, and tells the user, when another process such as a CLI rescan is scanning.
bool PluginLibrary::beginPluginScan()
{
    if (!pluginScanLock.tryAcquire()) {
        utils::logWarn("Skipped a plugin scan because another plugin scan is running");
        juce::MessageManager::callAsync([] {
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions::makeOptionsOk(
                    juce::MessageBoxIconType::InfoIcon,
                    "Plugin Scan Skipped",
                    "Another plugin scan is running, for example audiobatch --rescan in a terminal.\n\n"
                    "Scan again once it has finished.",
                    "OK"
                ),
                nullptr
            );
        });
        return false;
    }

    // Another process may have saved a newer list since this one was loaded.
    reload();
    return true;
}
}  // namespace audiobatch
