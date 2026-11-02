/// The user's plugin chain in the GUI.
/// Declares LiveChain, which holds the ordered list of plugin slots that files are processed through,
/// keeps it in the application settings, saves and loads it as named presets,
/// and owns the plugin editor, chain editor, and plugin manager windows.
/// Each slot owns the processor that the live preview and its editor share.
/// The plugins it can add and create come from the PluginLibrary.
/// Callers interact with it through a single menu invoked from a button or menu item.

#pragma once

#include "gui/LiveSlot.h"
#include "plugins/PluginChainPreparation.h"
#include "plugins/PluginChainPresets.h"
#include "plugins/PluginLibrary.h"
#include "utils/AudioFormatUtils.h"

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch
{
/// A prepared live chain. Its slot-owned instances remain protected by the preview rendering lock.
struct PreviewChain {
    // Preserve aggregate order used by preview preparation and callers.
    /// Non-owning, never null views of the enabled slot instances in chain order.
    /// They stay valid only while the slots keep their instances, so use them under the preview rendering lock.
    std::vector<juce::AudioPluginInstance*> instances;
    /// The summed latency and tails and the widest channel count of the enabled instances,
    /// with the error of the first enabled slot that could not be prepared.
    plugin_processing::ChainPreparation preparation;
};

/// The saved presets that LiveChain listed last, kept until the presets folder changes.
/// Adding, removing, or renaming a preset file changes the modification time of the folder,
/// and saving or deleting a preset through the chain drops the list as well.
struct PresetListCache {
    /// The folder the presets were listed from.
    juce::File directory;
    /// Modification time of the folder when the presets were listed.
    juce::Time directoryModified;
    /// The listed presets, sorted by name.
    std::vector<ChainPresetInfo> presets;
};

/// Owns the user's plugin chain and its editor and plugin manager windows.
/// The chain is an ordered list of plugins that files are processed through in sequence.
/// Not a visible component: callers display a single button (or menu item) that invokes showMenu().
class LiveChain : public juce::ChangeListener, public juce::ChangeBroadcaster, private juce::AudioProcessorListener
{
public:
    using ChainChangedCallback = std::function<void()>;

    LiveChain(
        PluginLibrary& pluginLibrary,
        juce::ApplicationProperties& applicationProperties,
        juce::CriticalSection& previewRenderingLock,
        juce::AudioDeviceManager& audioDeviceManager
    );

    ~LiveChain() override;

    LiveChain(const LiveChain&) = delete;
    LiveChain& operator=(const LiveChain&) = delete;
    LiveChain(LiveChain&&) = delete;
    LiveChain& operator=(LiveChain&&) = delete;

    /// Returns the library of known plugins that slots are added from.
    [[nodiscard]] PluginLibrary& getLibrary() const noexcept
    {
        return library;
    }

    [[nodiscard]] juce::File getPresetDirectory() const;
    [[nodiscard]] juce::PluginDescription getSlotDescription(PluginSlotId slotId) const;
    [[nodiscard]] juce::PopupMenu createPresetsMenu(int firstItemId) const;
    [[nodiscard]] juce::String getChainSummary() const;
    [[nodiscard]] juce::String getCurrentPresetName() const;

    [[nodiscard]] PreviewChain preparePreview(
        double sampleRate,
        int blockSize,
        bool inspectAll = false,
        bool loadEnabled = true
    );

    [[nodiscard]] std::vector<EnabledChainPlugin> getEnabledPlugins();
    [[nodiscard]] std::vector<ChainPresetInfo> getPresets() const;
    [[nodiscard]] std::vector<PluginSlotId> getSlotIds() const;

    [[nodiscard]] std::optional<int> getSlotIndex(PluginSlotId slotId) const;
    [[nodiscard]] std::optional<juce::int64> getSlotLatency(PluginSlotId slotId) const;
    [[nodiscard]] std::optional<juce::int64> getTotalLatency() const;

    std::expected<void, juce::String> loadPreset(const juce::File& file);
    std::expected<juce::File, juce::String> saveAsPreset(const juce::String& name);

    bool deletePreset(const juce::File& file);
    bool handlePresetsMenuItem(int itemId, int firstItemId);
    [[nodiscard]] bool hasOpenPreviewDevice() const;
    [[nodiscard]] bool isPresetModified() const noexcept;
    [[nodiscard]] bool isSlotEnabled(PluginSlotId slotId) const;

    [[nodiscard]] double getPreviewSampleRate() const noexcept;
    [[nodiscard]] int getNumEnabledValidSlots() const noexcept;
    [[nodiscard]] int getNumSlots() const noexcept;
    [[nodiscard]] juce::int64 getPreviewTailSamples() const;

    /// Distinguishes structural changes from latency-only label refreshes.
    [[nodiscard]] std::uint64_t getRevision() const noexcept
    {
        return revision;
    }

    void addPlugin(const juce::PluginDescription& description);
    void clearChain();
    void confirmDeletePreset(const ChainPresetInfo& preset);
    void loadPresetAndReport(const juce::File& file);
    void moveSlot(PluginSlotId slotId, int toIndex);
    void openEditorForSlot(PluginSlotId slotId);
    void promptSavePreset();
    void removeSlot(PluginSlotId slotId);
    void saveChain();

    // View callbacks, windows and preview status.
    void setChainChangedCallback(ChainChangedCallback callback);
    void setLatencyChangedCallback(ChainChangedCallback callback);
    void setSlotEnabled(PluginSlotId slotId, bool enabled);
    void showChainEditor();
    void showMenu(juce::Component& anchor);
    void showPresetFolder() const;
    void showScanWindow();
    void updateCurrentPreset();
    void updatePreviewStatus();

    // juce::ChangeListener
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

private:
    [[nodiscard]] static std::optional<LiveSlot> parseLegacyEntry(
        const juce::String& pluginXml,
        const juce::String& stateBase64
    );

    [[nodiscard]] static bool isEditorWindowClosed(const LiveSlot& entry);
    static bool reapClosedEditor(LiveSlot& entry);

    PluginSlotId appendSlot(LiveSlot entry);

    [[nodiscard]] std::vector<SavedChainSlot> captureSavedSlots();

    bool instantiateLiveEntry(LiveSlot& entry);
    bool prepareLiveEntry(LiveSlot& entry);

    void captureOpenEditorStates();
    void loadPersistedChain();
    void notifyChainChanged();
    void persistChain();
    void savePresetAndReport(const juce::String& name);
    void savePresetConfirmingReplace(const juce::String& name);

    // juce::AudioProcessorListener
    void audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) override;
    void audioProcessorParameterChanged(juce::AudioProcessor* processor, int parameterIndex, float value) override;

    /// Called on the message thread after structural changes.
    ChainChangedCallback chainChangedCallback;
    /// Called on the message thread after prepared latency changes.
    ChainChangedCallback latencyChangedCallback;
    /// Known plugins, owned by the view and outliving this chain.
    PluginLibrary& library;

    /// Settings that keep the chain and locate presets.
    juce::ApplicationProperties& appProperties;
    /// Preview device, observed for sample-rate context.
    juce::AudioDeviceManager& previewDeviceManager;
    /// Borrowed chain editor window, null before opening.
    juce::Component::SafePointer<juce::DialogWindow> chainEditorWindow;
    /// Borrowed plugin manager window, null before opening.
    juce::Component::SafePointer<juce::DialogWindow> scanWindow;
    /// Shared rendering lock held while processors are reconfigured.
    juce::CriticalSection& renderingLock;
    /// Loaded preset name, empty without one.
    juce::String presetName;

    /// Set from processor callbacks until the display reads the change.
    std::atomic<bool> latencyDirty {false};
    /// Pending processor settings change, transferred to settingsChanged on the message thread.
    std::atomic<bool> pendingSettingsChange {false};

    /// Last preset listing, invalidated by changes. Listing does not modify the chain.
    mutable std::optional<PresetListCache> presetList;
    /// Preset files in menu id order. Building a menu does not modify the chain.
    mutable std::vector<juce::File> presetMenuFiles;

    /// Whether plugin state changed since saving or loading a preset.
    bool settingsChanged {false};
    /// Rate at which processors are prepared.
    double previewSampleRate {utils::defaultSampleRate};
    /// Block size at which processors are prepared.
    int previewBlockSize {512};

    /// Last assigned slot identity, never reused.
    std::uint64_t lastSlotId {0};
    /// Revision at the last preset load or save.
    std::uint64_t presetRevision {0};
    /// Structural revision used by editor rows.
    std::uint64_t revision {0};

    /// Slots remain last so they unregister listeners before the callback and revision state is destroyed.
    std::vector<LiveSlot> chain;

    JUCE_DECLARE_WEAK_REFERENCEABLE(LiveChain)
    JUCE_LEAK_DETECTOR(LiveChain)
};

[[nodiscard]] juce::String formatPluginDescription(const juce::PluginDescription& description);
[[nodiscard]] juce::String formatPluginPreviewRate(double sampleRate, bool hasOpenDevice);
}  // namespace audiobatch
