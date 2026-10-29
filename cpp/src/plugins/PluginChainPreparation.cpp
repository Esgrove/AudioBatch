/// Implementation of the plugin chain preparation:
/// configuring the main buses for the file's channel layout with mono and stereo fallbacks,
/// switching plugins to offline rendering, restoring their state, and collecting the chain latency and tail.

#include "plugins/PluginChainPreparation.h"

#include "plugins/PluginProcessing.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <cmath>
#include <cstddef>
#include <exception>
#include <optional>
#include <span>

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace audiobatch::plugin_processing
{
namespace
{
/// Tells the plugin it is rendering offline rather than running live on an audio device,
/// so it can use higher-quality processing that does not need to keep up with realtime,
/// for example higher oversampling or longer lookahead.
/// Must be called before prepareToPlay:
/// JUCE passes the mode to VST3 plugins as kOffline in setupProcessing during prepareToPlay.
/// JUCE's Audio Unit host does not forward the mode,
/// so it is set directly through kAudioUnitProperty_OfflineRender while the unit is uninitialized.
void enableOfflineRendering(juce::AudioPluginInstance& plugin)
{
    plugin.setNonRealtime(true);

#if JUCE_MAC
    if (const auto* audioUnitClient = plugin.getAudioUnitClient(); audioUnitClient != nullptr) {
        if (auto* audioUnit = audioUnitClient->getAudioUnitHandle(); audioUnit != nullptr) {
            constexpr UInt32 offlineRender = 1;
            if (const auto status = AudioUnitSetProperty(
                    audioUnit,
                    kAudioUnitProperty_OfflineRender,
                    kAudioUnitScope_Global,
                    0,
                    &offlineRender,
                    sizeof(offlineRender)
                );
                status != noErr)
            {
                // Not fatal: the plugin still renders correctly, only without its offline quality mode.
                utils::logWarn(
                    "Audio Unit {} did not accept offline render mode (OSStatus {})", plugin.getName().quoted(), status
                );
            }
        }
    }
#endif
}

#if JUCE_MAC
/// Refreshes an Audio Unit's latency immediately after state restoration, without reinitializing it.
/// JUCE also listens for latency notifications, but those may arrive after per-file preparation returns.
void refreshAudioUnitLatency(juce::AudioPluginInstance& plugin, const double sampleRate)
{
    if (const auto* client = plugin.getAudioUnitClient(); client != nullptr) {
        if (auto* audioUnit = client->getAudioUnitHandle(); audioUnit != nullptr) {
            Float64 latencySeconds = 0.0;
            UInt32 propertySize = sizeof(latencySeconds);
            if (AudioUnitGetProperty(
                    audioUnit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &latencySeconds, &propertySize
                )
                == noErr)
            {
                plugin.setLatencySamples(juce::jmax(0, juce::roundToInt(latencySeconds * sampleRate)));
            }
        }
    }
}
#endif
}  // namespace

/// Reconfigures the plugin's main input and output buses to match the file when possible.
/// The plugin's existing bus count is kept and only the main bus channel set is changed,
/// because some plugins have sidechain or aux buses that must be left alone.
/// A mono file falls back to a stereo layout, since plenty of effect plugins are stereo-only.
/// Returns false when the plugin accepts neither, and leaves its layout unchanged then.
bool configurePluginChannels(juce::AudioPluginInstance& plugin, const int numChannels)
{
    auto const trySetMainBusChannels = [&plugin](const juce::AudioChannelSet& channelSet) {
        auto layout = plugin.getBusesLayout();

        if (layout.inputBuses.isEmpty() && layout.outputBuses.isEmpty()) {
            return false;
        }

        if (!layout.inputBuses.isEmpty()) {
            layout.inputBuses.getReference(0) = channelSet;
        }
        if (!layout.outputBuses.isEmpty()) {
            layout.outputBuses.getReference(0) = channelSet;
        }

        return plugin.checkBusesLayoutSupported(layout) && plugin.setBusesLayout(layout);
    };

    if (trySetMainBusChannels(juce::AudioChannelSet::canonicalChannelSet(numChannels))) {
        return true;
    }

    return numChannels == 1 && trySetMainBusChannels(juce::AudioChannelSet::stereo());
}

/// Configures the plugin's main buses for the stereo preview, or for mono when it only supports mono.
/// processBlockThroughChain folds both channels into a mono plugin and copies its output to both,
/// so a mono-only plugin works in the preview of any file, and its editor can open.
/// Offline processing still refuses a stereo file through a mono-only plugin.
bool configurePreviewChannels(juce::AudioPluginInstance& plugin)
{
    return configurePluginChannels(plugin, 2) || configurePluginChannels(plugin, 1);
}

/// Bounds an effect's tail so infinite tails cannot keep either preview or offline rendering alive indefinitely.
juce::int64 getPluginTailSamples(const juce::AudioPluginInstance& plugin, const double sampleRate)
{
    const auto seconds = plugin.getTailLengthSeconds();
    const auto boundedSeconds = std::isnan(seconds) ? 0.0 : juce::jlimit(0.0, maximumTailSeconds, seconds);
    return static_cast<juce::int64>(std::ceil(boundedSeconds * sampleRate));
}

/// Adds a prepared plugin to the totals of its chain: its reported latency, its bounded tail,
/// and the channel count of its widest bus.
/// Each stage's tail must ring through the remaining stages, and every stage delays the signal it passes on,
/// so the chain tail and latency are the sums over its plugins.
/// Offline processing and the live preview both measure their chains with it.
void addToPreparation(ChainPreparation& preparation, const juce::AudioPluginInstance& plugin, const double sampleRate)
{
    preparation.latencySamples += juce::jmax(0, plugin.getLatencySamples());
    preparation.processChannelCount = juce::jmax(
        preparation.processChannelCount, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels()
    );
    preparation.tailSamples += getPluginTailSamples(plugin, sampleRate);
}

/// Formats device-rate latency consistently for slot rows and the enabled-chain total.
juce::String formatPluginLatency(const juce::int64 samples, const double sampleRate)
{
    return utils::format(
        "{} samples ({:.2f} ms)", samples, static_cast<double>(samples) * utils::millisecondsPerSecond / sampleRate
    );
}

/// Restores saved settings and prepares an inactive plugin on the message thread.
/// Audio Units must be initialized before restoring state to avoid crashes in units such as Ozone 12.
/// Never prepare them again after restoration, because JUCE resets and uninitializes them on every prepare call.
/// Other formats receive state before activation, preserving the VST3 editor and processing lifecycle.
void restoreStateAndPrepare(
    juce::AudioPluginInstance& plugin,
    const juce::MemoryBlock& state,
    const double sampleRate,
    const int blockSize
)
{
    const bool hasState = state.getSize() > 0;
    const bool isAudioUnit = plugin.getAudioUnitClient() != nullptr;

    if (hasState && !isAudioUnit) {
        plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }

    plugin.prepareToPlay(sampleRate, blockSize);

    if (hasState && isAudioUnit) {
        plugin.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
#if JUCE_MAC
        refreshAudioUnitLatency(plugin, sampleRate);
#endif
    }
}

/// Releases the resources of every plugin in the chain, leaving the chain inactive for the next file.
/// Must run on the message thread, because releasing a VST3 plugin deactivates it through setActive.
/// Plugins that are not prepared ignore the call.
void releasePluginResources(std::span<juce::AudioPluginInstance* const> chainInstances)
{
    for (auto* plugin : chainInstances) {
        plugin->releaseResources();
    }
}

/// Configures the buses, enables offline rendering, restores the saved state,
/// prepares, and resets every plugin in the chain for one file,
/// and collects the channel count, tail, and latency of the chain.
/// Must run on the message thread:
/// the VST3 specification restricts bus arrangement, setupProcessing, setActive, setState,
/// and the tail and latency queries to the UI thread,
/// and JUCE's VST3 host asserts the message thread in prepareToPlay and setStateInformation.
/// When a plugin rejects the file's channel layout,
/// the plugins prepared before it are released again so the chain stays usable for the next file.
ChainPreparation prepareChain(
    std::span<juce::AudioPluginInstance* const> chainInstances,
    const PluginProcessingOptions& options,
    const int numChannels,
    const double sampleRate
)
{
    ChainPreparation preparation;
    preparation.processChannelCount = numChannels;

    for (std::size_t pluginIndex = 0; pluginIndex < chainInstances.size(); ++pluginIndex) {
        auto* plugin = chainInstances[pluginIndex];

        if (!configurePluginChannels(*plugin, numChannels)) {
            releasePluginResources(chainInstances);
            preparation.errorMessage = utils::format(
                "Plugin {} does not support the file's channel layout ({} channels)",
                options.plugins[pluginIndex].name.quoted(),
                numChannels
            );
            return preparation;
        }

        enableOfflineRendering(*plugin);

        // Restore plugin state per-file so each file starts clean.
        restoreStateAndPrepare(*plugin, options.plugins[pluginIndex].state, sampleRate, processingBlockSize);
        plugin->reset();
        addToPreparation(preparation, *plugin, sampleRate);
    }

    return preparation;
}

/// Keeps references to the chain and the cancellation check without touching the plugins yet.
PreparedChain::PreparedChain(
    const std::span<juce::AudioPluginInstance* const> instances,
    const utils::CancellationCheck& cancellationCheck
) :
    chainInstances(instances),
    isCancelled(cancellationCheck)
{}

/// Releases a successfully prepared chain on the message thread unless the run was cancelled.
/// A failure while releasing is logged rather than thrown, because this runs while unwinding as well.
PreparedChain::~PreparedChain()
{
    if (!prepared) {
        return;
    }

    try {
        static_cast<void>(utils::callOnMessageThread([this] { releasePluginResources(chainInstances); }, isCancelled));
    } catch (const std::exception& exception) {
        utils::logError("Releasing the plugin chain failed: {}", exception.what());
    } catch (...) {
        utils::logError("Releasing the plugin chain failed with an unknown exception");
    }
}

/// Prepares every plugin in the chain for a file with the given channel count and sample rate on the message thread.
/// Returns nothing when the run was cancelled before the preparation started.
/// A failed preparation has already released the chain and reports its error in the returned errorMessage.
std::optional<ChainPreparation> PreparedChain::prepare(
    const PluginProcessingOptions& options,
    const int numChannels,
    const double sampleRate
)
{
    ChainPreparation preparation;
    const bool called = utils::callOnMessageThread(
        [&] { preparation = prepareChain(chainInstances, options, numChannels, sampleRate); }, isCancelled
    );

    if (!called) {
        return std::nullopt;
    }

    prepared = preparation.errorMessage.isEmpty();
    return preparation;
}
}  // namespace audiobatch::plugin_processing
