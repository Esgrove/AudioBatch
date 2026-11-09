/// Message-thread lifecycle of the processors shared by live preview and plugin editors.
/// Prepares slot instances lazily, reports their device-rate latency and bounded tails,
/// and observes plugin changes without doing host work in processor callbacks.

#include "gui/LiveChain.h"
#include "gui/LiveSlot.h"
#include "plugins/PluginChainPreparation.h"
#include "utils/CallbackAccess.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace audiobatch
{
/// Labels latency inspection as device-free only when there is no open playback device.
/// Uses the actual preparation rate rather than assuming that inspection always runs at 48 kHz.
juce::String formatPluginPreviewRate(const double sampleRate, const bool hasOpenDevice)
{
    return hasOpenDevice ? utils::format("Preview rate: {:.0f} Hz.", sampleRate)
                         : utils::format("No audio device. Inspection rate: {:.0f} Hz.", sampleRate);
}

/// Queries the device manager directly so a closed or disconnected device cannot leave a stale availability flag.
bool LiveChain::hasOpenPreviewDevice() const
{
    auto* const device = previewDeviceManager.getCurrentAudioDevice();
    return device != nullptr && device->isOpen();
}

/// Creates the processor of a slot that has none yet, and returns false when the plugin cannot be created.
/// A new processor is not part of live preview, so creation runs before callback exclusion,
/// and loading a large plugin does not silence the preview.
bool LiveChain::instantiateLiveEntry(const PluginSlotId slotId, const double sampleRate, const int blockSize)
{
    const auto index = getSlotIndex(slotId);
    if (!index) {
        return false;
    }
    if (chain[static_cast<std::size_t>(*index)].instance != nullptr) {
        return true;
    }
    if (std::ranges::contains(instantiatingSlots, slotId)) {
        return false;
    }
    // Native creation may pump a nested message loop. Never retain a slot reference across it.
    const auto description = chain[static_cast<std::size_t>(*index)].description;
    instantiatingSlots.push_back(slotId);
    const juce::WeakReference safeThis(this);
    juce::String error;
    auto instance = library.getFormatManager().createPluginInstance(description, sampleRate, blockSize, error);
    if (safeThis == nullptr) {
        return false;
    }
    std::erase(instantiatingSlots, slotId);
    const auto current = getSlotIndex(slotId);
    if (!current) {
        return false;
    }
    auto& entry = chain[static_cast<std::size_t>(*current)];
    if (instance == nullptr) {
        entry.preparationError = error.isEmpty() ? juce::String("Plugin instantiation failed") : error;
        utils::logError("Live plugin {}: {}", description.name.quoted(), entry.preparationError);
        return false;
    }
    entry.preparationError.clear();
    entry.attachInstance(std::move(instance), *this);
    return true;
}

/// Creates only unattached instances before callback exclusion, using stable ids across native nested event loops.
void LiveChain::instantiatePreview(
    const double sampleRate,
    const int blockSize,
    const bool inspectAll,
    const bool loadEnabled
)
{
    const auto ids = getSlotIds();
    const juce::WeakReference safeThis(this);
    for (const auto slotId : ids) {
        const auto index = getSlotIndex(slotId);
        if (!index) {
            continue;
        }
        const bool visit = inspectAll || chain[static_cast<std::size_t>(*index)].instance != nullptr
            || (chain[static_cast<std::size_t>(*index)].enabled && loadEnabled);
        if (visit) {
            static_cast<void>(instantiateLiveEntry(slotId, sampleRate, blockSize));
            if (safeThis == nullptr) {
                return;
            }
        }
    }
}

/// Prepares an existing slot once for the current real-time context.
/// The caller holds control admission, so callback access cannot race reconfiguration.
bool LiveChain::prepareLiveEntry(LiveSlot& entry)
{
    if (entry.prepared) {
        return true;
    }
    if (entry.instance == nullptr) {
        return false;
    }
    entry.preparationError.clear();
    auto& plugin = *entry.instance;
    // Host preparation can report state changes, which are not user edits.
    // Detaching only this processor preserves pending edits from it and every other slot.
    plugin.removeListener(this);
    const juce::ScopeGuard restoreListener {[&plugin, this] { plugin.addListener(this); }};
    plugin.setNonRealtime(false);
    if (!plugin_processing::configurePreviewChannels(plugin)) {
        entry.preparationError = "The plugin supports neither a stereo nor a mono layout";
    } else if (juce::jmax(plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels()) >= 32) {
        entry.preparationError = "The plugin exposes too many auxiliary channels for real-time preview";
    }
    if (entry.preparationError.isNotEmpty()) {
        utils::logError("Live plugin {}: {}", entry.description.name.quoted(), entry.preparationError);
        return false;
    }
    plugin_processing::restoreStateAndPrepare(plugin, entry.state, previewSampleRate, previewBlockSize);
    plugin.reset();
    entry.prepared = true;
    entry.latencySamples = juce::jmax(0, plugin.getLatencySamples());
    return true;
}

/// Configures the enabled live processors while keeping disabled instantiated slots in the same device context.
/// Inspection additionally prepares disabled slots so every row can display an actual measured latency.
/// The preview always runs in stereo, so loading a file with another channel count does not prepare the plugins again.
PreviewChain LiveChain::preparePreview(
    const double sampleRate,
    const int blockSize,
    const bool inspectAll,
    const bool loadEnabled
)
{
    const juce::WeakReference safeThis(this);
    instantiatePreview(sampleRate, blockSize, inspectAll, loadEnabled);
    if (safeThis == nullptr) {
        return {};
    }
    return prepareInstantiatedPreview(sampleRate, blockSize, inspectAll, loadEnabled);
}

/// Reconfigures only existing instances under callback exclusion, never entering native plugin creation.
/// Callers perform instantiatePreview before any enclosing control guard, including nested player operations.
PreviewChain LiveChain::prepareInstantiatedPreview(
    const double sampleRate,
    const int blockSize,
    const bool inspectAll,
    const bool loadEnabled
)
{
    const utils::CallbackControlGuard control(renderingAccess);
    const bool contextChanged
        = !juce::approximatelyEqual(sampleRate, previewSampleRate) || blockSize != previewBlockSize;
    if (contextChanged) {
        captureOpenEditorStates();
        for (auto& entry : chain) {
            if (entry.instance != nullptr && entry.prepared) {
                entry.instance->releaseResources();
                entry.prepared = false;
                entry.latencySamples.reset();
            }
        }
        previewSampleRate = sampleRate;
        previewBlockSize = blockSize;
    }

    PreviewChain result;
    result.preparation.processChannelCount = 2;
    for (auto& entry : chain) {
        if (!inspectAll && entry.instance == nullptr && !(entry.enabled && loadEnabled)) {
            continue;
        }
        // Missing instances are not created while callback admission is closed.
        if (entry.instance == nullptr || !prepareLiveEntry(entry)) {
            if (entry.enabled && result.preparation.errorMessage.isEmpty()) {
                result.preparation.errorMessage
                    = utils::format("{}: {}", entry.description.name, entry.preparationError);
            }
            continue;
        }
        if (entry.enabled) {
            result.instances.push_back(entry.instance.get());
            plugin_processing::addToPreparation(result.preparation, *entry.instance, sampleRate);
        }
    }
    sendChangeMessage();
    return result;
}

/// Ordinary parameter edits affect the shared processor directly, without copying state or restarting playback.
/// Only records that the settings changed, for the display update to mark the preset modified.
void LiveChain::audioProcessorParameterChanged(
    juce::AudioProcessor* /*processor*/,
    int /*parameterIndex*/,
    float /*value*/
)
{
    pendingSettingsChange.store(true);
}

/// Defers latency queries, playback re-priming, and marking the preset modified to the message-thread display update.
void LiveChain::audioProcessorChanged(juce::AudioProcessor* /*processor*/, const ChangeDetails& details)
{
    if (details.latencyChanged) {
        latencyDirty.store(true);
    }
    if (details.nonParameterStateChanged) {
        pendingSettingsChange.store(true);
    }
}

/// Installs the message-thread callback used to re-prime playback after an actual latency change.
void LiveChain::setLatencyChangedCallback(ChainChangedCallback callback)
{
    latencyChangedCallback = std::move(callback);
}

/// Returns a measured slot latency, distinguishing unavailable values from a valid zero.
std::optional<juce::int64> LiveChain::getSlotLatency(const PluginSlotId slotId) const
{
    const auto index = getSlotIndex(slotId);
    return index.has_value() ? chain[static_cast<std::size_t>(*index)].latencySamples : std::nullopt;
}

/// Sums enabled slots only, refusing to present a partial sum as a complete measurement.
std::optional<juce::int64> LiveChain::getTotalLatency() const
{
    juce::int64 total = 0;
    for (const auto& entry : chain) {
        if (entry.enabled) {
            if (!entry.latencySamples) {
                return std::nullopt;
            }
            total += *entry.latencySamples;
        }
    }
    return total;
}

/// Returns the processing rate used to convert the measured sample counts into milliseconds.
double LiveChain::getPreviewSampleRate() const noexcept
{
    return previewSampleRate;
}

/// Queries current tail estimates on the message thread without changing the live processing state.
juce::int64 LiveChain::getPreviewTailSamples() const
{
    juce::int64 total = 0;
    for (const auto& entry : chain) {
        if (entry.enabled && entry.prepared && entry.instance != nullptr) {
            total += plugin_processing::getPluginTailSamples(*entry.instance, previewSampleRate);
        }
    }
    return total;
}
}  // namespace audiobatch
