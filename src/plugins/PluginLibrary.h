/// The plugins the GUI knows about and can create.
/// Declares PluginLibrary, which owns the plugin format manager and the known-plugins list,
/// keeps the list in its KnownPlugins.xml file, scans for plugins out of process,
/// and creates the plugin instances of a processing run.

#pragma once

#include "plugins/PluginProcessing.h"
#include "plugins/PluginScanner.h"

#include <JuceHeader.h>

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

    [[nodiscard]] juce::Array<juce::PluginDescription> getKnownTypes() const;

    [[nodiscard]] std::unique_ptr<juce::PluginListComponent> createScanComponent();

    /// Returns the format manager that creates the plugin instances.
    [[nodiscard]] juce::AudioPluginFormatManager& getFormatManager() noexcept
    {
        return formatManager;
    }

    void reload();

private:
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    bool beginPluginScan();

    // Declared before the list, so it outlives the scanner that the list owns.
    PluginScanLock pluginScanLock;

    juce::ApplicationProperties& appProperties;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;

    JUCE_LEAK_DETECTOR(PluginLibrary)
};
}  // namespace audiobatch
