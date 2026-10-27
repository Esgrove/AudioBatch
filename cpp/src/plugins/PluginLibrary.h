/// The plugins the GUI knows about and can create.
/// Declares PluginLibrary, which owns the plugin format manager and the known-plugins list,
/// keeps the list in its KnownPlugins.xml file, scans for plugins out of process,
/// and creates the plugin instances of a processing run.

#pragma once

#include "plugins/PluginProcessing.h"
#include "plugins/PluginScanner.h"

#include <JuceHeader.h>

#include <expected>
#include <memory>
#include <vector>

namespace audiobatch
{
/// Description plus descriptor reference for one enabled slot, in chain order.
struct EnabledChainPlugin {
    juce::PluginDescription description;
    PluginDescriptorRef descriptorRef;
};

/// Plugin chains created for one processing run, or the reason they could not be created.
struct ChainInstantiation {
    /// One chain of plugin instances per worker.
    std::vector<std::vector<std::unique_ptr<juce::AudioPluginInstance>>> chains;
    /// State of each enabled plugin right after it was created, in chain order.
    /// Restored before every file for a plugin without saved state,
    /// so adaptive state from one file does not carry over to the next.
    std::vector<juce::MemoryBlock> initialStates;
    /// Empty when every plugin of every chain was created.
    juce::String errorMessage;
    /// Name of the plugin that failed to load, when errorMessage is not empty.
    juce::String failedPluginName;
};

/// The options and the plugin chains of one processing run, created together by PluginLibrary::createProcessingRun.
struct ProcessingRunSetup {
    /// Settings of the run, with the plugins and their states in chain order.
    PluginProcessingOptions options;
    /// One chain of plugin instances per worker, aligned index for index with options.plugins.
    std::vector<std::vector<std::unique_ptr<juce::AudioPluginInstance>>> chains;
};

/// Owns the plugin format manager and the known-plugins list, and scans for installed plugins.
/// Scans load each plugin in a worker process, so a plugin that crashes is blacklisted instead of taking down the app.
/// The list is saved to its file whenever it changes.
class PluginLibrary : private juce::ChangeListener
{
public:
    explicit PluginLibrary(juce::ApplicationProperties& applicationProperties);

    ~PluginLibrary() override;

    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;
    PluginLibrary(PluginLibrary&&) = delete;
    PluginLibrary& operator=(PluginLibrary&&) = delete;

    [[nodiscard]] ChainInstantiation instantiateChains(
        const std::vector<EnabledChainPlugin>& enabledPlugins,
        int workerCount
    ) const;
    [[nodiscard]] std::expected<ProcessingRunSetup, juce::String> createProcessingRun(
        const std::vector<EnabledChainPlugin>& enabledPlugins,
        PluginProcessingOptions options,
        int fileCount
    ) const;

    [[nodiscard]] juce::Array<juce::PluginDescription> getKnownTypes() const;
    [[nodiscard]] juce::Array<juce::PluginDescription> getKnownEffectTypes() const;

    [[nodiscard]] std::unique_ptr<juce::PluginListComponent> createScanComponent();

    /// Returns the format manager that creates the plugin instances.
    [[nodiscard]] juce::AudioPluginFormatManager& getFormatManager() noexcept
    {
        return formatManager;
    }

    void reload();

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    bool beginPluginScan(const utils::CancellationCheck& shouldStop);
    void finishPluginScan(bool completed);
    void saveList();

    /// Taken and released on the message thread only.
    /// Declared before the list, so it outlives the scanner that the list owns.
    PluginScanLock pluginScanLock;

    juce::ApplicationProperties& appProperties;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;
    /// Modification time of the list file when this library last loaded or saved it.
    juce::Time loadedModificationTime;
    /// True once the list has been loaded, even from a file that did not exist yet.
    bool listLoaded {false};
    /// True while reload replaces the list, so its change messages do not save it back.
    bool reloading {false};
    /// True from the start of a scan from the plugin manager until it finishes, set on the message thread.
    bool scanning {false};

    JUCE_LEAK_DETECTOR(PluginLibrary)
};
}  // namespace audiobatch
