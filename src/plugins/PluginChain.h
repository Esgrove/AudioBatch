/// Ownership and management of the user's plugin chain in the GUI.
/// Declares PluginChain, which holds the ordered list of plugin slots that files are processed through,
/// along with the known-plugins database, plugin scanning, persistence in application settings,
/// and the plugin editor, chain editor, and scan windows.
/// Callers interact with it through a single menu invoked from a button or menu item.

#pragma once

#include "plugins/PluginChainPreparation.h"
#include "plugins/PluginProcessing.h"
#include "plugins/PluginScanner.h"
#include "utils/AudioFormatUtils.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
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

/// A prepared live chain. Its slot-owned instances remain protected by the preview rendering lock.
struct PreviewChain {
    /// Non-owning, never null views of the enabled slot instances in chain order.
    /// They stay valid only while the slots keep their instances, so use them under the preview rendering lock.
    std::vector<juce::AudioPluginInstance*> instances;
    plugin_processing::ChainPreparation preparation;
};

/// One slot owns the same processor for editing and live playback, independently of its editor window.
struct PluginChainEntry {
    juce::PluginDescription description;
    juce::MemoryBlock state;
    bool enabled {true};
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::Component::SafePointer<juce::DialogWindow> editorWindow;
    juce::String preparationError;
    std::optional<juce::int64> latencySamples;
    bool prepared {false};
    bool wasEditorOpen {false};
};

/// Owns the user's plugin chain, the known-plugins database, scanning, and the editor windows.
/// The chain is an ordered list of plugins that files are processed through in sequence.
/// Not a visible component: callers display a single button (or menu item) that invokes showMenu().
class PluginChain : public juce::ChangeListener, public juce::ChangeBroadcaster, private juce::AudioProcessorListener
{
public:
    using ChainChangedCallback = std::function<void()>;

    PluginChain(
        juce::ApplicationProperties& applicationProperties,
        juce::CriticalSection& previewRenderingLock,
        juce::AudioDeviceManager& audioDeviceManager
    );

    ~PluginChain() override;

    PluginChain(const PluginChain&) = delete;
    PluginChain& operator=(const PluginChain&) = delete;
    PluginChain(PluginChain&&) = delete;
    PluginChain& operator=(PluginChain&&) = delete;

    [[nodiscard]] juce::PluginDescription getSlotDescription(int index) const;
    [[nodiscard]] juce::String getChainSummary() const;
    [[nodiscard]] juce::int64 getPreviewTailSamples() const;

    [[nodiscard]] std::optional<juce::int64> getSlotLatency(int index) const;
    [[nodiscard]] std::optional<juce::int64> getTotalLatency() const;
    [[nodiscard]] std::vector<EnabledChainPlugin> getEnabledPlugins();

    [[nodiscard]] bool hasOpenPreviewDevice() const;
    [[nodiscard]] bool isSlotEnabled(int index) const;
    [[nodiscard]] double getPreviewSampleRate() const noexcept;
    [[nodiscard]] int getNumEnabledValidSlots() const noexcept;
    [[nodiscard]] int getNumSlots() const noexcept;

    [[nodiscard]] ChainInstantiation instantiateChains(
        const std::vector<EnabledChainPlugin>& enabledPlugins,
        int workerCount
    ) const;

    /// Distinguishes structural changes from latency-only label refreshes.
    [[nodiscard]] std::uint64_t getRevision() const noexcept
    {
        return revision;
    }
    [[nodiscard]] PreviewChain preparePreview(
        double sampleRate,
        int blockSize,
        int channels,
        bool inspectAll = false,
        bool loadEnabled = true
    );

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
    void setLatencyChangedCallback(ChainChangedCallback callback);
    void setSlotEnabled(int index, bool enabled);
    void showChainEditor();
    void showMenu(juce::Component& anchor);
    void showScanWindow();
    void updatePreviewStatus();

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

    void audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) override;
    void audioProcessorParameterChanged(juce::AudioProcessor* processor, int parameterIndex, float value) override;

    void captureOpenEditorStates();
    void destroyLiveEntry(PluginChainEntry& entry);
    void loadKnownPluginList();
    void loadPersistedChain();
    void notifyChainChanged();
    void persistChain();
    void persistKnownPluginList() const;

    bool beginPluginScan();
    bool prepareLiveEntry(PluginChainEntry& entry);

    // Declared before the list, so it outlives the scanner that the list owns.
    PluginScanLock pluginScanLock;

    ChainChangedCallback chainChangedCallback;
    ChainChangedCallback latencyChangedCallback;

    juce::ApplicationProperties& appProperties;
    juce::AudioDeviceManager& previewDeviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::Component::SafePointer<juce::DialogWindow> chainEditorWindow;
    juce::Component::SafePointer<juce::DialogWindow> scanWindow;
    juce::CriticalSection& renderingLock;
    juce::KnownPluginList knownPluginList;
    std::atomic<bool> latencyDirty {false};
    std::uint64_t revision {0};
    std::vector<PluginChainEntry> chain;

    double previewSampleRate {utils::defaultSampleRate};
    int previewBlockSize {512};
    int previewChannels {2};

    JUCE_DECLARE_WEAK_REFERENCEABLE(PluginChain)
    JUCE_LEAK_DETECTOR(PluginChain)
};

[[nodiscard]] juce::String formatPluginDescription(const juce::PluginDescription& description);
[[nodiscard]] juce::String formatPluginPreviewRate(double sampleRate, bool hasOpenDevice);
}  // namespace audiobatch
