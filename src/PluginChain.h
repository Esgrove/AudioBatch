#pragma once

#include "PluginProcessing.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>

/// Owns the user's plugin selection, the known-plugins database, scanning, and the editor window.
/// Not a visible component: callers display a single button (or menu item) that invokes showMenu().
class PluginChain : public juce::ChangeListener, public juce::Timer
{
public:
    using SelectionChangedCallback = std::function<void(const PluginDescriptorRef&)>;

    explicit PluginChain(juce::ApplicationProperties& applicationProperties);
    ~PluginChain() override;

    /// Returns the currently selected plugin (may be invalid when none selected).
    [[nodiscard]] PluginDescriptorRef getSelectedPlugin() const;

    /// Returns the underlying juce::PluginDescription for the current selection, or empty when none.
    [[nodiscard]] juce::PluginDescription getSelectedPluginDescription() const;

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

    /// Invoked whenever the selected plugin (or its persisted state) changes.
    void setSelectionChangedCallback(SelectionChangedCallback callback);

    /// Pops up the plugin menu anchored to the given component.
    /// The menu includes the current selection, Edit/Clear, the Choose submenu (known plugins), and a Scan entry.
    void showMenu(juce::Component& anchor);

    /// Opens the plugin's editor window (no-op when no plugin is selected).
    void openEditor();

    /// Clears the current selection and saved state.
    void clearSelection();

    /// Sets the active plugin to the given description, discarding any previously saved state.
    void selectPlugin(const juce::PluginDescription& description);

    /// Opens the plugin scan dialog.
    void showScanWindow();

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void timerCallback() override;

private:
    void loadPersistedSelection();
    void persistSelection();
    void persistKnownPluginList() const;
    void loadKnownPluginList();
    void notifySelectionChanged() const;

    juce::ApplicationProperties& appProperties;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;

    juce::PluginDescription selectedDescription;
    juce::MemoryBlock selectedState;

    std::unique_ptr<juce::AudioPluginInstance> editorInstance;  ///< Used only while the editor window is open.
    juce::Component::SafePointer<juce::DialogWindow> editorWindow;
    juce::Component::SafePointer<juce::DialogWindow> scanWindow;

    SelectionChangedCallback selectionChangedCallback;
    bool wasEditorOpen = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE(PluginChain)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginChain)
};
