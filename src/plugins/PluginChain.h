/// Ownership and management of the user's plugin chain in the GUI.
/// Declares PluginChain, which holds the ordered list of plugin slots that files are processed through,
/// along with the known-plugins database, plugin scanning, persistence in application settings,
/// and the plugin editor, chain editor, and scan windows.
/// Callers interact with it through a single menu invoked from a button or menu item.

#pragma once

#include "plugins/PluginProcessing.h"
#include "plugins/PluginScanner.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch
{
[[nodiscard]] juce::String formatPluginDescription(const juce::PluginDescription& description);

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

/// One slot in the chain, owning its editor instance and window while the editor is open.
struct PluginChainEntry {
    juce::PluginDescription description;
    juce::MemoryBlock state;
    bool enabled = true;
    std::unique_ptr<juce::AudioPluginInstance> editorInstance;
    juce::Component::SafePointer<juce::DialogWindow> editorWindow;
    bool wasEditorOpen = false;
};

/// Owns the user's plugin chain, the known-plugins database, scanning, and the editor windows.
/// The chain is an ordered list of plugins that files are processed through in sequence.
/// Not a visible component: callers display a single button (or menu item) that invokes showMenu().
class PluginChain : public juce::ChangeListener, public juce::ChangeBroadcaster, public juce::Timer
{
public:
    using ChainChangedCallback = std::function<void()>;

    explicit PluginChain(juce::ApplicationProperties& applicationProperties);

    ~PluginChain() override;

    PluginChain(const PluginChain&) = delete;
    PluginChain& operator=(const PluginChain&) = delete;
    PluginChain(PluginChain&&) = delete;
    PluginChain& operator=(PluginChain&&) = delete;

    [[nodiscard]] int getNumSlots() const noexcept;

    [[nodiscard]] int getNumEnabledValidSlots() const noexcept;

    [[nodiscard]] juce::PluginDescription getSlotDescription(int index) const;

    [[nodiscard]] bool isSlotEnabled(int index) const;

    [[nodiscard]] std::vector<EnabledChainPlugin> getEnabledPlugins();

    [[nodiscard]] ChainInstantiation instantiateChains(
        const std::vector<EnabledChainPlugin>& enabledPlugins,
        int workerCount
    ) const;

    [[nodiscard]] juce::String getChainSummary() const;

    /// Returns the format manager used for instantiation (so callers can create more instances).
    [[nodiscard]] juce::AudioPluginFormatManager& getFormatManager() noexcept
    {
        return formatManager;
    }

    /// Returns the known plugin list (for the scan dialog).
    [[nodiscard]] juce::KnownPluginList& getKnownPluginList() noexcept
    {
        return knownPluginList;
    }

    void addPlugin(const juce::PluginDescription& description);
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void clearChain();
    void moveSlot(int fromIndex, int toIndex);
    void openEditorForSlot(int index);
    void removeSlot(int index);
    void setChainChangedCallback(ChainChangedCallback callback);
    void setSlotEnabled(int index, bool enabled);
    void showChainEditor();
    void showMenu(juce::Component& anchor);
    void showScanWindow();
    void timerCallback() override;

private:
    [[nodiscard]] static bool isEditorWindowClosed(const PluginChainEntry& entry);
    [[nodiscard]] static std::optional<PluginChainEntry> parseSlot(const juce::XmlElement& slotElement);
    static bool reapClosedEditor(PluginChainEntry& entry);
    static void closeEditorForEntry(PluginChainEntry& entry);
    static void restoreState(PluginChainEntry& entry, const juce::String& stateBase64);

    [[nodiscard]] static std::optional<PluginChainEntry> parseLegacyEntry(
        const juce::String& pluginXml,
        const juce::String& stateBase64
    );

    [[nodiscard]] bool anyEditorOpen() const noexcept;
    bool beginPluginScan();

    void captureOpenEditorStates();
    void loadKnownPluginList();
    void loadPersistedChain();
    void notifyChainChanged();
    void persistChain();
    void persistKnownPluginList() const;

    // Declared before the list, so it outlives the scanner that the list owns.
    PluginScanLock pluginScanLock;

    ChainChangedCallback chainChangedCallback;

    juce::ApplicationProperties& appProperties;
    juce::AudioPluginFormatManager formatManager;
    juce::Component::SafePointer<juce::DialogWindow> chainEditorWindow;
    juce::Component::SafePointer<juce::DialogWindow> scanWindow;
    juce::KnownPluginList knownPluginList;
    std::vector<PluginChainEntry> chain;

    JUCE_DECLARE_WEAK_REFERENCEABLE(PluginChain)
    JUCE_LEAK_DETECTOR(PluginChain)
};
}  // namespace audiobatch
