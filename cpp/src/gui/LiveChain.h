/// The user's plugin chain in the GUI.
/// Declares LiveChain, which holds the ordered list of plugin slots that files are processed through,
/// keeps it in the application settings, saves and loads it as named presets,
/// and owns the plugin editor, chain editor, and plugin manager windows.
/// Each slot owns the processor that the live preview and its editor share.
/// The plugins it can add and create come from the PluginLibrary.
/// Callers interact with it through a single menu invoked from a button or menu item.

#pragma once

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
/// Identifies one slot of the live chain for as long as the slot exists.
/// Rows of the chain editor refer to their slot by id, so a row that is about to be rebuilt still acts on its own slot
/// after another slot was moved or removed.
enum class PluginSlotId : std::uint64_t {};

/// A prepared live chain. Its slot-owned instances remain protected by the preview rendering lock.
struct PreviewChain {
    /// Non-owning, never null views of the enabled slot instances in chain order.
    /// They stay valid only while the slots keep their instances, so use them under the preview rendering lock.
    std::vector<juce::AudioPluginInstance*> instances;
    plugin_processing::ChainPreparation preparation;
};

/// One slot owns the same processor for editing and live playback, independently of its editor window.
/// Destroying a slot tears its processor down in the order a plugin needs:
/// the editor window first, then the parameter listener, then the prepared resources, and the instance last.
/// Moving a slot carries the processor and its editor window along and leaves the source slot empty.
struct LiveSlot {
    LiveSlot() = default;
    explicit LiveSlot(SavedChainSlot savedSlot);
    ~LiveSlot();
    LiveSlot(LiveSlot&& other) noexcept;
    LiveSlot& operator=(LiveSlot&& other) noexcept;
    LiveSlot(const LiveSlot&) = delete;
    LiveSlot& operator=(const LiveSlot&) = delete;

    void attachInstance(
        std::unique_ptr<juce::AudioPluginInstance> newInstance,
        juce::AudioProcessorListener& newListener
    );
    void closeEditor();
    void destroyProcessor();

    PluginSlotId id {};
    juce::PluginDescription description;
    juce::MemoryBlock state;
    bool enabled {true};
    std::unique_ptr<juce::AudioPluginInstance> instance;
    /// The listener registered on the instance, owned by the live chain that owns the slot.
    /// Null exactly when the slot has no instance.
    juce::AudioProcessorListener* listener {nullptr};
    juce::Component::SafePointer<juce::DialogWindow> editorWindow;
    juce::String preparationError;
    std::optional<juce::int64> latencySamples;
    bool prepared {false};
    bool wasEditorOpen {false};
};

/// The saved presets that LiveChain listed last, kept until the presets folder changes.
/// Adding, removing, or renaming a preset file changes the modification time of the folder,
/// and saving or deleting a preset through the chain drops the list as well.
struct PresetListCache {
    /// The listed presets, sorted by name.
    std::vector<ChainPresetInfo> presets;
    /// The folder the presets were listed from.
    juce::File directory;
    /// Modification time of the folder when the presets were listed.
    juce::Time directoryModified;
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

    [[nodiscard]] juce::PluginDescription getSlotDescription(PluginSlotId slotId) const;
    [[nodiscard]] juce::String getChainSummary() const;
    [[nodiscard]] juce::int64 getPreviewTailSamples() const;

    [[nodiscard]] std::optional<int> getSlotIndex(PluginSlotId slotId) const;
    [[nodiscard]] std::optional<juce::int64> getSlotLatency(PluginSlotId slotId) const;
    [[nodiscard]] std::optional<juce::int64> getTotalLatency() const;
    [[nodiscard]] std::vector<EnabledChainPlugin> getEnabledPlugins();
    [[nodiscard]] std::vector<PluginSlotId> getSlotIds() const;

    [[nodiscard]] bool hasOpenPreviewDevice() const;
    [[nodiscard]] bool isSlotEnabled(PluginSlotId slotId) const;
    [[nodiscard]] double getPreviewSampleRate() const noexcept;
    [[nodiscard]] int getNumEnabledValidSlots() const noexcept;
    [[nodiscard]] int getNumSlots() const noexcept;

    /// Distinguishes structural changes from latency-only label refreshes.
    [[nodiscard]] std::uint64_t getRevision() const noexcept
    {
        return revision;
    }
    [[nodiscard]] PreviewChain preparePreview(
        double sampleRate,
        int blockSize,
        bool inspectAll = false,
        bool loadEnabled = true
    );

    /// Returns the library of known plugins that slots are added from.
    [[nodiscard]] PluginLibrary& getLibrary() const noexcept
    {
        return library;
    }

    [[nodiscard]] juce::File getPresetDirectory() const;
    [[nodiscard]] std::vector<ChainPresetInfo> getPresets() const;
    [[nodiscard]] juce::String getCurrentPresetName() const;
    [[nodiscard]] juce::PopupMenu createPresetsMenu(int firstItemId) const;
    std::expected<juce::File, juce::String> saveAsPreset(const juce::String& name);
    std::expected<void, juce::String> loadPreset(const juce::File& file);
    bool deletePreset(const juce::File& file);
    bool handlePresetsMenuItem(int itemId, int firstItemId);
    void confirmDeletePreset(const ChainPresetInfo& preset);
    void loadPresetAndReport(const juce::File& file);
    void promptSavePreset();
    void showPresetFolder() const;

    void addPlugin(const juce::PluginDescription& description);
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void clearChain();
    void moveSlot(PluginSlotId slotId, int toIndex);
    void openEditorForSlot(PluginSlotId slotId);
    void removeSlot(PluginSlotId slotId);
    void setChainChangedCallback(ChainChangedCallback callback);
    void setLatencyChangedCallback(ChainChangedCallback callback);
    void setSlotEnabled(PluginSlotId slotId, bool enabled);
    void showChainEditor();
    void showMenu(juce::Component& anchor);
    void showScanWindow();
    void updatePreviewStatus();

private:
    [[nodiscard]] static bool isEditorWindowClosed(const LiveSlot& entry);
    static bool reapClosedEditor(LiveSlot& entry);

    [[nodiscard]] static std::optional<LiveSlot> parseLegacyEntry(
        const juce::String& pluginXml,
        const juce::String& stateBase64
    );

    PluginSlotId appendSlot(LiveSlot entry);

    [[nodiscard]] std::vector<SavedChainSlot> captureSavedSlots();

    void savePresetAndReport(const juce::String& name);
    void savePresetConfirmingReplace(const juce::String& name);

    void audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) override;
    void audioProcessorParameterChanged(juce::AudioProcessor* processor, int parameterIndex, float value) override;

    void captureOpenEditorStates();
    void loadPersistedChain();
    void notifyChainChanged();
    void persistChain();

    bool prepareLiveEntry(LiveSlot& entry);

    ChainChangedCallback chainChangedCallback;
    ChainChangedCallback latencyChangedCallback;

    PluginLibrary& library;
    juce::ApplicationProperties& appProperties;
    juce::AudioDeviceManager& previewDeviceManager;
    juce::Component::SafePointer<juce::DialogWindow> chainEditorWindow;
    juce::Component::SafePointer<juce::DialogWindow> scanWindow;
    juce::CriticalSection& renderingLock;
    std::atomic<bool> latencyDirty {false};
    std::uint64_t revision {0};
    std::uint64_t lastSlotId {0};
    /// Name of the preset the chain was last loaded from or saved as.
    /// It only names the chain while the revision still equals presetRevision.
    juce::String presetName;
    std::uint64_t presetRevision {0};
    /// Preset files of the presets submenu last built by createPresetsMenu, in menu id order.
    /// Mutable, because building a menu does not change the chain.
    mutable std::vector<juce::File> presetMenuFiles;
    /// The presets getPresets listed last, or nothing before the first listing and after a save or delete.
    /// Mutable, because listing the presets does not change the chain.
    mutable std::optional<PresetListCache> presetList;
    std::vector<LiveSlot> chain;

    double previewSampleRate {utils::defaultSampleRate};
    int previewBlockSize {512};

    JUCE_DECLARE_WEAK_REFERENCEABLE(LiveChain)
    JUCE_LEAK_DETECTOR(LiveChain)
};

[[nodiscard]] juce::String formatPluginDescription(const juce::PluginDescription& description);
[[nodiscard]] juce::String formatPluginPreviewRate(double sampleRate, bool hasOpenDevice);
}  // namespace audiobatch
