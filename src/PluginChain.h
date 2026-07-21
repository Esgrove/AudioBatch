/// Ownership and management of the user's plugin chain in the GUI.
/// Declares PluginChain, which holds the ordered list of plugin slots that files are processed through,
/// along with the known-plugins database, plugin scanning, persistence in application settings,
/// and the plugin editor, chain editor, and scan windows.
/// Callers interact with it through a single menu invoked from a button or menu item.

#pragma once

#include "PluginProcessing.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <vector>

/// Formats a plugin description as "Manufacturer Name (Format)"
/// for chain editor rows, menus, and log messages.
[[nodiscard]] juce::String formatPluginDescription(const juce::PluginDescription& description);

/// Owns the user's plugin chain, the known-plugins database, scanning, and the editor windows.
/// The chain is an ordered list of plugins that files are processed through in sequence.
/// Not a visible component: callers display a single button (or menu item) that invokes showMenu().
class PluginChain : public juce::ChangeListener, public juce::ChangeBroadcaster, public juce::Timer
{
public:
    using ChainChangedCallback = std::function<void()>;

    /// Description plus descriptor reference for one enabled slot, in chain order.
    struct EnabledChainPlugin {
        juce::PluginDescription description;
        PluginDescriptorRef descriptorRef;
    };

    /// Registers the default plugin formats and restores the known-plugins list
    /// and the persisted chain from application settings.
    explicit PluginChain(juce::ApplicationProperties& applicationProperties);

    /// Closes all editor, chain-editor, and scan windows and destroys their plugin instances.
    ~PluginChain() override;

    /// Returns the number of slots in the chain, including disabled ones.
    [[nodiscard]] int getNumSlots() const noexcept;

    /// Returns the number of enabled slots that identify a valid plugin.
    [[nodiscard]] int getNumEnabledValidSlots() const noexcept;

    /// Returns the plugin description for the given slot, or an empty description when out of range.
    [[nodiscard]] juce::PluginDescription getSlotDescription(int index) const;

    /// Returns true when the given slot exists and is enabled.
    [[nodiscard]] bool isSlotEnabled(int index) const;

    /// Returns the enabled plugins in chain order.
    /// Captures live state from any open editor windows first,
    /// so a processing run always uses the latest tweaks.
    [[nodiscard]] std::vector<EnabledChainPlugin> getEnabledPlugins();

    /// Returns a short human-readable summary of the chain for menu headers,
    /// with disabled slots in parentheses, or an empty string when the chain is empty.
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

    /// Invoked whenever the chain (order, slots, enabled flags, or persisted state) changes.
    void setChainChangedCallback(ChainChangedCallback callback);

    /// Pops up the plugin menu anchored to the given component.
    /// The menu includes the chain summary, Edit Chain, the Add Plugin submenu, Clear Chain, and a Scan entry.
    void showMenu(juce::Component& anchor);

    /// Appends the given plugin to the chain as an enabled slot and opens its editor.
    void addPlugin(const juce::PluginDescription& description);

    /// Removes the given slot from the chain, closing its editor window if open.
    void removeSlot(int index);

    /// Moves a slot to a new position in the chain.
    void moveSlot(int fromIndex, int toIndex);

    /// Enables or disables the given slot without removing it from the chain.
    void setSlotEnabled(int index, bool enabled);

    /// Removes all slots from the chain, closing any open editor windows.
    void clearChain();

    /// Opens the editor window for the given slot.
    /// Multiple slot editors can be open at the same time.
    void openEditorForSlot(int index);

    /// Opens the chain editor window for adding, reordering, enabling, and removing plugins.
    void showChainEditor();

    /// Opens the plugin scan dialog.
    void showScanWindow();

    /// Persists the known-plugins list whenever the plugin scanner updates it.
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    /// Polls for editor windows the user has closed,
    /// capturing their plugin state and releasing their instances.
    /// A window hidden by its close button counts as closed,
    /// because the dialog hides itself instead of deleting itself.
    /// Stops itself once no editor windows remain open.
    void timerCallback() override;

private:
    /// One slot in the chain, owning its editor instance and window while the editor is open.
    struct ChainEntry {
        juce::PluginDescription description;
        juce::MemoryBlock state;
        bool enabled = true;
        std::unique_ptr<juce::AudioPluginInstance> editorInstance;
        juce::Component::SafePointer<juce::DialogWindow> editorWindow;
        bool wasEditorOpen = false;
    };

    /// Returns true when the entry's editor window has been closed by the user.
    /// The dialog window hides itself when its close button is pressed instead of deleting itself,
    /// so a hidden window also counts as closed.
    [[nodiscard]] static bool isEditorWindowClosed(const ChainEntry& entry);

    /// Captures plugin state from a closed editor into its entry,
    /// then destroys the window and the instance.
    /// Returns true when plugin state was captured.
    static bool reapClosedEditor(ChainEntry& entry);

    /// Captures live plugin state from every open editor into its entry without closing anything.
    void captureOpenEditorStates();

    /// Closes the given entry's editor window and destroys its instance without capturing state.
    static void closeEditorForEntry(ChainEntry& entry);

    /// Returns true when any entry still has an editor window open.
    [[nodiscard]] bool anyEditorOpen() const noexcept;

    /// Restores the chain from application settings.
    /// When no chain is stored, migrates a legacy single-plugin selection into a one-slot chain
    /// and removes the legacy keys.
    void loadPersistedChain();

    /// Writes the chain to application settings,
    /// capturing the latest state from any open editors first.
    void persistChain();

    /// Writes the known-plugins list to application settings.
    void persistKnownPluginList() const;

    /// Restores the known-plugins list from application settings.
    void loadKnownPluginList();

    /// Broadcasts a change message and invokes the chain-changed callback, if one is set.
    void notifyChainChanged();

    juce::ApplicationProperties& appProperties;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;

    std::vector<ChainEntry> chain;

    juce::Component::SafePointer<juce::DialogWindow> chainEditorWindow;
    juce::Component::SafePointer<juce::DialogWindow> scanWindow;

    ChainChangedCallback chainChangedCallback;

    JUCE_DECLARE_WEAK_REFERENCEABLE(PluginChain)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginChain)
};
