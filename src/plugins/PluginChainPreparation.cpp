/// Implementation of the plugin chain preparation:
/// configuring the main buses for the file's channel layout with mono and stereo fallbacks,
/// switching plugins to offline rendering, restoring their state, and collecting the chain latency and tail.

#include "plugins/PluginChainPreparation.h"

#include "utils/AudioFormatUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <span>

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace audiobatch::plugin_processing
{
/// Reconfigures the plugin's main input and output buses to match the file when possible.
/// We preserve the plugin's existing bus count and only override the main bus channel set,
/// because some plugins have sidechain or aux buses that should be left alone.
/// If that fails we fall back through several common configurations.
bool configurePluginChannels(
    juce::AudioPluginInstance& plugin,
    const int numChannels,
    const double sampleRate,
    const int blockSize
)
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

    bool layoutConfigured = trySetMainBusChannels(juce::AudioChannelSet::canonicalChannelSet(numChannels));

    if (!layoutConfigured && numChannels == 2) {
        layoutConfigured = trySetMainBusChannels(juce::AudioChannelSet::stereo());
    }

    if (!layoutConfigured && numChannels == 1) {
        layoutConfigured = trySetMainBusChannels(juce::AudioChannelSet::mono());
        // Plenty of effect plugins are stereo-only.
        // Let mono files run through a stereo configuration in that case.
        if (!layoutConfigured) {
            layoutConfigured = trySetMainBusChannels(juce::AudioChannelSet::stereo());
        }
    }

    if (!layoutConfigured && numChannels <= 2) {
        // Last resort: ask the processor to accept the channel counts directly.
        // Some plugins do not honor setBusesLayout but still process correctly if play config details are set.
        plugin.setPlayConfigDetails(numChannels, numChannels, sampleRate, blockSize);
        layoutConfigured
            = plugin.getTotalNumInputChannels() >= numChannels && plugin.getTotalNumOutputChannels() >= numChannels;
    }

    return layoutConfigured;
}

/// Bounds an effect's tail so infinite tails cannot keep either preview or offline rendering alive indefinitely.
juce::int64 getPluginTailSamples(const juce::AudioPluginInstance& plugin, const double sampleRate)
{
    const auto seconds = plugin.getTailLengthSeconds();
    const auto boundedSeconds = std::isnan(seconds) ? 0.0 : juce::jlimit(0.0, maximumTailSeconds, seconds);
    return static_cast<juce::int64>(std::ceil(boundedSeconds * sampleRate));
}

/// Formats device-rate latency consistently for slot rows and the enabled-chain total.
juce::String formatPluginLatency(const juce::int64 samples, const double sampleRate)
{
    return utils::format(
        "{} samples ({:.2f} ms)", samples, static_cast<double>(samples) * utils::millisecondsPerSecond / sampleRate
    );
}

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
}  // namespace

#if JUCE_MAC
namespace
{
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
}  // namespace
#endif

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
    // Each stage's tail must ring through the remaining stages,
    // so the total tail is the sum of the individually clamped per-plugin tails.
    // Latencies add up the same way, since every stage delays the signal it passes on.
    ChainPreparation preparation;
    preparation.processChannelCount = numChannels;

    for (std::size_t pluginIndex = 0; pluginIndex < chainInstances.size(); ++pluginIndex) {
        auto* plugin = chainInstances[pluginIndex];

        if (!configurePluginChannels(*plugin, numChannels, sampleRate, processingBlockSize)) {
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
        preparation.latencySamples += juce::jmax(0, plugin->getLatencySamples());

        preparation.processChannelCount = juce::jmax(
            preparation.processChannelCount, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()
        );

        preparation.tailSamples += getPluginTailSamples(*plugin, sampleRate);
    }

    return preparation;
}
}  // namespace audiobatch::plugin_processing
