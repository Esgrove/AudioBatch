/// Message-thread lifecycle of the processors shared by live preview and plugin editors.
/// Prepares slot instances lazily, reports their device-rate latency and bounded tails,
/// and observes plugin changes without doing host work in processor callbacks.

#include "plugins/PluginChain.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <utility>

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
bool PluginChain::hasOpenPreviewDevice() const
{
    auto* const device = previewDeviceManager.getCurrentAudioDevice();
    return device != nullptr && device->isOpen();
}

/// Instantiates and prepares a slot once for the current real-time context.
/// The caller holds the preview rendering lock, so editor and device access cannot race reconfiguration.
bool PluginChain::prepareLiveEntry(PluginChainEntry& entry)
{
    if (entry.prepared) {
        return true;
    }
    entry.preparationError.clear();
    if (entry.instance == nullptr) {
        entry.instance = formatManager.createPluginInstance(
            entry.description, previewSampleRate, previewBlockSize, entry.preparationError
        );
        if (entry.instance == nullptr) {
            if (entry.preparationError.isEmpty()) {
                entry.preparationError = "Plugin instantiation failed";
            }
            utils::logError("Live plugin {}: {}", entry.description.name.quoted(), entry.preparationError);
            return false;
        }
        entry.instance->addListener(this);
    }
    auto& plugin = *entry.instance;
    plugin.setNonRealtime(false);
    if (!plugin_processing::configurePluginChannels(plugin, previewChannels, previewSampleRate, previewBlockSize)) {
        entry.preparationError = "The plugin does not support mono or stereo preview";
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
PreviewChain PluginChain::preparePreview(
    const double sampleRate,
    const int blockSize,
    const int channels,
    const bool inspectAll,
    const bool loadEnabled
)
{
    const juce::ScopedLock lock(renderingLock);
    const bool contextChanged = !juce::approximatelyEqual(sampleRate, previewSampleRate)
        || blockSize != previewBlockSize || channels != previewChannels;
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
        previewChannels = channels;
    }

    PreviewChain result;
    result.preparation.processChannelCount = channels;
    for (auto& entry : chain) {
        if (!inspectAll && entry.instance == nullptr && (!entry.enabled || !loadEnabled)) {
            continue;
        }
        if (!prepareLiveEntry(entry)) {
            if (entry.enabled && result.preparation.errorMessage.isEmpty()) {
                result.preparation.errorMessage
                    = utils::format("{}: {}", entry.description.name, entry.preparationError);
            }
            continue;
        }
        if (entry.enabled) {
            result.instances.push_back(entry.instance.get());
            result.preparation.latencySamples += juce::jmax(0, entry.instance->getLatencySamples());
            result.preparation.tailSamples += plugin_processing::getPluginTailSamples(*entry.instance, sampleRate);
            result.preparation.processChannelCount = juce::jmax(
                result.preparation.processChannelCount,
                entry.instance->getTotalNumInputChannels(),
                entry.instance->getTotalNumOutputChannels()
            );
        }
    }
    sendChangeMessage();
    return result;
}

/// Closes the editor before unregistering and destroying a slot's live processor.
void PluginChain::destroyLiveEntry(PluginChainEntry& entry)
{
    closeEditorForEntry(entry);
    if (entry.instance != nullptr) {
        entry.instance->removeListener(this);
        if (entry.prepared) {
            entry.instance->releaseResources();
        }
        entry.instance = nullptr;
    }
    entry.prepared = false;
    entry.latencySamples.reset();
}

/// Ordinary parameter edits affect the shared processor directly, without copying state or restarting playback.
void PluginChain::audioProcessorParameterChanged(
    juce::AudioProcessor* /*processor*/,
    int /*parameterIndex*/,
    float /*value*/
)
{}

/// Defers latency queries and playback re-priming to the message-thread display update.
void PluginChain::audioProcessorChanged(juce::AudioProcessor* /*processor*/, const ChangeDetails& details)
{
    if (details.latencyChanged) {
        latencyDirty.store(true);
    }
}

/// Installs the message-thread callback used to re-prime playback after an actual latency change.
void PluginChain::setLatencyChangedCallback(ChainChangedCallback callback)
{
    latencyChangedCallback = std::move(callback);
}

/// Returns a measured slot latency, distinguishing unavailable values from a valid zero.
std::optional<juce::int64> PluginChain::getSlotLatency(const int index) const
{
    return juce::isPositiveAndBelow(index, getNumSlots()) ? chain[static_cast<std::size_t>(index)].latencySamples
                                                          : std::nullopt;
}

/// Sums enabled slots only, refusing to present a partial sum as a complete measurement.
std::optional<juce::int64> PluginChain::getTotalLatency() const
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
double PluginChain::getPreviewSampleRate() const noexcept
{
    return previewSampleRate;
}

/// Queries current tail estimates on the message thread without changing the live processing state.
juce::int64 PluginChain::getPreviewTailSamples() const
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
