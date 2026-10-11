/// Implementation of PluginProcessingService.
/// Covers reading the source file with a thread-local format manager,
/// applying custom or normalization gain before the chain,
/// configuring plugin buses for the file's channel layout,
/// switching plugins to offline (non-realtime) rendering mode,
/// restoring their state and preparing them per file,
/// and rendering block by block through the chain including plugin tails,
/// with the chain's reported latency removed from the start of the output.
/// Plugin setup and release are posted to the message thread as VST3 requires,
/// with a cancellation-aware wait so a worker never deadlocks against a message thread that waits for it,
/// while processBlock runs on the calling worker thread.
/// The chain output is rendered to an unquantized 32-bit float file first,
/// so the peak and the end of the audible tail are known before the final AIFF file is written
/// at the source bit depth, turned down when it would clip, trimmed, and dithered when 16-bit.
/// The AIFF file is validated, given the original file's metadata, and then moved into place,
/// with the processed file re-analyzed for the result record.

#include "PluginProcessingService.h"

#include "AudioNormalizationService.h"
#include "DecodeFailureTracker.h"
#include "FileUtils.h"
#include "Log.h"
#include "MetadataService.h"
#include "StringFormat.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace audiobatch::plugin_processing
{
constexpr int processingBlockSize = 1024;
/// Block size for copying the rendered intermediate file into the final output.
constexpr int finalizeBlockSize = 32768;
/// Level below which rendered tail samples count as silence and are trimmed from the end of the output.
/// The value is -96 dB, the noise floor of 16-bit audio.
constexpr float silenceThreshold = 1.5849e-5f;
/// Output bit depth at and below which TPDF dither is added when quantizing the rendered audio.
constexpr int maximumDitheredBitDepth = 16;
/// Seed for the dither noise, so processing the same file twice writes identical output.
constexpr juce::int64 ditherSeed = 0x41554449;
constexpr auto cancelledMessage = "Processing was cancelled";
/// How long a worker waits for the message thread between cancellation checks.
constexpr int messageThreadPollIntervalMs = 20;
/// Upper limit for the tail length a plugin may report, in seconds.
/// Caps the silence appended after each file so a plugin reporting an infinite tail cannot stall processing.
constexpr double maximumTailSeconds = 30.0;

/// Shorthand for building a failure result, keeping the early-return error paths compact.
static PluginProcessingResult fail(const juce::File& file, const juce::String& message)
{
    return PluginProcessingResult::failure(file, message);
}

/// Locates the AIFF format among the registered formats by name,
/// so the writer does not depend on registration order.
/// Returns nullptr when no AIFF format is available.
static juce::AudioFormat* findAiffFormat(juce::AudioFormatManager const& formatManager)
{
    for (int i = 0; i < formatManager.getNumKnownFormats(); ++i) {
        auto* format = formatManager.getKnownFormat(i);
        if (format == nullptr) {
            continue;
        }

        if (format->getFormatName().containsIgnoreCase("AIFF")) {
            return format;
        }
    }

    return nullptr;
}

/// Converts reader metadata into the map type that the writer options expect,
/// so the output file keeps the metadata JUCE read from the input.
static std::unordered_map<juce::String, juce::String> copyMetadata(const juce::StringPairArray& metadata)
{
    std::unordered_map<juce::String, juce::String> result;
    const auto& keys = metadata.getAllKeys();
    const auto& values = metadata.getAllValues();

    for (int index = 0; index < keys.size(); ++index) {
        result.emplace(keys[index], values[index]);
    }

    return result;
}

/// Returns the magnitude of a peak value, since analysis records may store signed peaks.
static float peakMagnitude(const float peak)
{
    return std::abs(peak);
}

/// Clamps a file's channel count to a sane range
/// before it is used for buffer allocation and bus configuration.
static int clampChannelCount(const int channels)
{
    return juce::jlimit(1, 8, channels);
}

/// Reconfigures the plugin's main input and output buses to match the file when possible.
/// We preserve the plugin's existing bus count and only override the main bus channel set,
/// because some plugins have sidechain or aux buses that should be left alone.
/// If that fails we fall back through several common configurations.
static bool configurePluginChannels(juce::AudioPluginInstance& plugin, const int numChannels, const double sampleRate)
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
        plugin.setPlayConfigDetails(numChannels, numChannels, sampleRate, processingBlockSize);
        layoutConfigured
            = plugin.getTotalNumInputChannels() >= numChannels && plugin.getTotalNumOutputChannels() >= numChannels;
    }

    return layoutConfigured;
}

/// Tells the plugin it is rendering offline rather than running live on an audio device,
/// so it can use higher-quality processing that does not need to keep up with realtime,
/// for example higher oversampling or longer lookahead.
/// Must be called before prepareToPlay:
/// JUCE passes the mode to VST3 plugins as kOffline in setupProcessing during prepareToPlay.
/// JUCE's Audio Unit host does not forward the mode,
/// so it is set directly through kAudioUnitProperty_OfflineRender while the unit is uninitialized.
static void enableOfflineRendering(juce::AudioPluginInstance& plugin)
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

/// Returns true when a cancellation check is set and reports that processing should stop.
static bool isCancellationRequested(const PluginProcessingService::CancellationCheck& isCancelled)
{
    return isCancelled != nullptr && isCancelled();
}

/// A function posted from a worker thread to the message thread,
/// shared between the waiting worker and the queued message.
/// The state moves from pending to either running or abandoned exactly once,
/// and whichever side gets there first wins.
/// This guarantees that the function never starts after the worker has stopped waiting for it,
/// so it can safely refer to the worker's stack and to the plugin chain the worker holds.
struct MessageThreadCall {
    enum class State : std::uint8_t { pending, running, abandoned };

    std::function<void()> function;
    std::atomic<State> state {State::pending};
    juce::WaitableEvent finished;
};

/// Runs a function on the message thread and blocks the calling worker until it has finished.
/// Unlike MessageManager::callSync, the wait gives up when cancellation is requested while the call is still queued,
/// because the message thread may itself be blocked waiting for this worker,
/// for example in PluginProcessingCoordinator::cancelAndWait when the app quits during processing.
/// The abandoned call is skipped when the message thread later dequeues it.
/// A call that has already started always runs to completion,
/// since the message thread is then busy running it and cannot be waiting for the worker.
/// Calls the function directly when already on the message thread.
/// Returns true when the function ran,
/// and false when it was cancelled before starting or could not be posted to the message thread.
static bool callOnMessageThread(
    std::function<void()> function,
    const PluginProcessingService::CancellationCheck& isCancelled
)
{
    auto const* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (messageManager == nullptr) {
        return false;
    }

    if (messageManager->isThisTheMessageThread()) {
        function();
        return true;
    }

    if (isCancellationRequested(isCancelled)) {
        return false;
    }

    auto const call = std::make_shared<MessageThreadCall>();
    call->function = std::move(function);

    const bool posted = juce::MessageManager::callAsync([call] {
        auto expected = MessageThreadCall::State::pending;
        if (!call->state.compare_exchange_strong(expected, MessageThreadCall::State::running)) {
            return;
        }

        call->function();
        call->finished.signal();
    });

    // Posting fails once the app has started quitting and the message loop no longer dispatches.
    if (!posted) {
        return false;
    }

    while (!call->finished.wait(messageThreadPollIntervalMs)) {
        if (!isCancellationRequested(isCancelled)) {
            continue;
        }

        auto expected = MessageThreadCall::State::pending;
        if (call->state.compare_exchange_strong(expected, MessageThreadCall::State::abandoned)) {
            return false;
        }

        // The message thread is already running the function, so it finishes without waiting on this worker.
        call->finished.wait();
        break;
    }

    return true;
}

/// Releases the resources of every plugin in the chain, leaving the chain inactive for the next file.
/// Must run on the message thread, because releasing a VST3 plugin deactivates it through setActive.
/// Plugins that are not prepared ignore the call.
static void releasePluginResources(const std::vector<juce::AudioPluginInstance*>& chainInstances)
{
    for (auto* plugin : chainInstances) {
        plugin->releaseResources();
    }
}

/// Values gathered while preparing a plugin chain for one file that rendering needs.
struct ChainPreparation {
    /// Empty when every plugin in the chain was prepared.
    juce::String errorMessage;
    /// Widest channel count of any plugin in the chain, at least the file's channel count.
    int processChannelCount = 0;
    /// Sum of the individually clamped per-plugin tails.
    juce::int64 tailSamples = 0;
    /// Sum of the latencies the plugins report after preparation.
    juce::int64 latencySamples = 0;
};

/// Configures the buses, enables offline rendering, restores the saved state,
/// prepares, and resets every plugin in the chain for one file,
/// and collects the channel count, tail, and latency of the chain.
/// Must run on the message thread:
/// the VST3 specification restricts bus arrangement, setupProcessing, setActive, setState,
/// and the tail and latency queries to the UI thread,
/// and JUCE's VST3 host asserts the message thread in prepareToPlay and setStateInformation.
/// When a plugin rejects the file's channel layout,
/// the plugins prepared before it are released again so the chain stays usable for the next file.
static ChainPreparation prepareChain(
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
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

        if (!configurePluginChannels(*plugin, numChannels, sampleRate)) {
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
        // The state is restored before prepareToPlay because settings
        // such as lookahead or linear phase change the plugin's latency,
        // and JUCE refreshes the reported latency of VST3 and AU plugins when they are prepared.
        const auto& state = options.plugins[pluginIndex].state;
        if (state.getSize() > 0) {
            plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }

        plugin->prepareToPlay(sampleRate, processingBlockSize);
        plugin->reset();
        preparation.latencySamples += juce::jmax(0, plugin->getLatencySamples());

        preparation.processChannelCount = juce::jmax(
            preparation.processChannelCount, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()
        );

        const auto tailSeconds = plugin->getTailLengthSeconds();
        preparation.tailSamples
            += static_cast<juce::int64>(std::ceil(juce::jlimit(0.0, maximumTailSeconds, tailSeconds) * sampleRate));
    }

    return preparation;
}
}  // namespace audiobatch::plugin_processing

using namespace audiobatch::plugin_processing;

bool PluginProcessingService::canProcessFile(const juce::File& file)
{
    auto& formatManager = getThreadLocalFormatManager();
    const std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    return reader != nullptr;
}

juce::File PluginProcessingService::getProcessingOutputFile(const juce::File& input)
{
    return AudioNormalizationService::getOutputFile(input);
}

juce::AudioFormatManager& PluginProcessingService::getThreadLocalFormatManager()
{
    thread_local juce::AudioFormatManager formatManager;
    thread_local bool initialized = false;

    if (!initialized) {
        formatManager.registerBasicFormats();
        initialized = true;
    }

    return formatManager;
}

/// Returns an error message when the record or the plugin chain cannot be processed, or an empty string when it can.
static juce::String validateProcessingInputs(
    const AudioAnalysisRecord& record,
    const PluginProcessingOptions& options,
    const std::vector<juce::AudioPluginInstance*>& chainInstances
)
{
    if (!record.file.existsAsFile()) {
        return "File does not exist";
    }

    if (!record.isReady()) {
        return "File analysis must finish before processing";
    }

    const auto chainAvailable = !chainInstances.empty() && chainInstances.size() == options.plugins.size()
        && std::ranges::none_of(chainInstances, [](const auto* instance) { return instance == nullptr; });

    return chainAvailable ? juce::String() : juce::String("Plugin chain instances are not available");
}

/// Returns the linear gain applied before the first plugin.
/// A custom gain wins, otherwise the file is peak normalized when requested, and otherwise it is left unchanged.
static float getInputGain(const AudioAnalysisRecord& record, const PluginProcessingOptions& options)
{
    if (record.hasCustomGain) {
        return juce::Decibels::decibelsToGain(record.customGainDb);
    }

    if (options.normalizeBeforePlugin) {
        const auto peak = peakMagnitude(record.overallPeak);
        if (peak > 0.0f) {
            return 1.0f / peak;
        }
    }

    return 1.0f;
}

/// Reads `samplesToRead` frames starting at `samplePosition` into the start of `buffer` and applies `inputGain`.
/// `readBuffer` and `readChannelPointers` are scratch storage reused across blocks.
/// Returns false when the reader reports a failure, in which case the block holds what was decoded before it.
static bool readInputBlock(
    juce::AudioFormatReader& reader,
    juce::AudioBuffer<float>& buffer,
    juce::AudioBuffer<float>& readBuffer,
    std::vector<float*>& readChannelPointers,
    const juce::int64 samplePosition,
    const int samplesToRead,
    const float inputGain
)
{
    const auto channelCount = static_cast<int>(readChannelPointers.size());
    readBuffer.clear();

    for (int channel = 0; channel < channelCount; ++channel) {
        readChannelPointers[static_cast<std::size_t>(channel)] = readBuffer.getWritePointer(channel);
    }

    const auto readSucceeded = reader.read(readChannelPointers.data(), channelCount, samplePosition, samplesToRead);

    for (int channel = 0; channel < channelCount; ++channel) {
        buffer.copyFrom(channel, 0, readBuffer, channel, 0, samplesToRead);
    }

    if (!juce::approximatelyEqual(inputGain, 1.0f)) {
        for (int channel = 0; channel < channelCount; ++channel) {
            buffer.applyGain(channel, 0, samplesToRead, inputGain);
        }
    }

    return readSucceeded;
}

/// Level and length measurements of the rendered chain output, used to finalize the output file.
struct RenderSummary {
    /// Largest absolute sample value of the rendered output.
    float peak = 0.0f;
    /// Number of samples written to the intermediate file.
    juce::int64 samplesWritten = 0;
    /// Position of the last sample above the silence threshold, or -1 when the output is silent.
    juce::int64 lastAudibleSample = -1;
};

/// Runs one block through every plugin in chain order on the same buffer,
/// and returns the number of main channels carrying signal after the last plugin.
/// The signal starts with the file's channel count.
/// A mono signal reaching a plugin with a stereo main input is copied to the second channel first,
/// so stereo-linked and mid/side processing see a centred source instead of a hard-left one,
/// and a mono plugin earlier in the chain still affects both channels.
/// Channels beyond a plugin's main input, such as a sidechain, are cleared so no dry signal leaks into them.
static int processBlockThroughChain(
    juce::AudioBuffer<float>& buffer,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const int numChannels,
    juce::MidiBuffer& midi
)
{
    auto signalChannels = numChannels;

    for (auto* plugin : chainInstances) {
        const auto mainInputChannels = juce::jmin(plugin->getMainBusNumInputChannels(), buffer.getNumChannels());

        if (signalChannels == 1 && mainInputChannels >= 2) {
            buffer.copyFrom(1, 0, buffer, 0, 0, buffer.getNumSamples());
        }

        for (int channel = juce::jmax(mainInputChannels, 1); channel < buffer.getNumChannels(); ++channel) {
            buffer.clear(channel, 0, buffer.getNumSamples());
        }

        midi.clear();
        plugin->processBlock(buffer, midi);
        signalChannels = juce::jmax(1, juce::jmin(plugin->getMainBusNumOutputChannels(), buffer.getNumChannels()));
    }

    return signalChannels;
}

/// Folds the processed channels down to the file's channel count before they are written.
/// A mono file whose signal ended up in stereo is written as the average of both main channels.
/// Returns the buffer view to write, which aliases `buffer`.
static juce::AudioBuffer<float> prepareOutputView(
    juce::AudioBuffer<float>& buffer,
    const int numChannels,
    const int signalChannels,
    const int firstSample,
    const int numSamples
)
{
    if (numChannels == 1 && signalChannels >= 2) {
        buffer.addFrom(0, firstSample, buffer, 1, firstSample, numSamples);
        buffer.applyGain(0, firstSample, numSamples, 0.5f);
    }

    const auto channelsToWrite = juce::jmin(numChannels, buffer.getNumChannels());
    return {buffer.getArrayOfWritePointers(), channelsToWrite, firstSample, numSamples};
}

/// Updates the peak and the last audible sample position with a block about to be written.
static void measureOutputBlock(const juce::AudioBuffer<float>& block, const int numSamples, RenderSummary& summary)
{
    for (int channel = 0; channel < block.getNumChannels(); ++channel) {
        summary.peak = juce::jmax(summary.peak, block.getMagnitude(channel, 0, numSamples));
    }

    for (int sample = numSamples - 1; sample >= 0; --sample) {
        for (int channel = 0; channel < block.getNumChannels(); ++channel) {
            if (std::abs(block.getSample(channel, sample)) > silenceThreshold) {
                summary.lastAudibleSample = summary.samplesWritten + sample;
                return;
            }
        }
    }
}

/// Streams the whole file through every plugin in chain order and writes the processed audio,
/// unquantized, to the intermediate writer.
/// The chain latency and tail are rendered on top of the file length,
/// and the first `latencySamples` output samples are discarded,
/// so the processed audio lines up with the input and its end is not cut off.
/// Read failures follow the shared DecodeFailureTracker rules.
/// Returns an error message, or an empty string when the whole file was written.
static juce::String renderThroughChain(
    juce::AudioFormatReader& reader,
    juce::AudioFormatWriter& writer,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const ChainPreparation& preparation,
    const int numChannels,
    const float inputGain,
    const PluginProcessingService::CancellationCheck& isCancelled,
    RenderSummary& summary
)
{
    const auto latencySamples = preparation.latencySamples;

    juce::AudioBuffer<float> buffer(preparation.processChannelCount, processingBlockSize);
    juce::AudioBuffer<float> readBuffer(numChannels, processingBlockSize);
    juce::MidiBuffer midi;
    std::vector<float*> readChannelPointers(static_cast<std::size_t>(numChannels));
    DecodeFailureTracker failureTracker(reader.lengthInSamples);

    const auto totalSamples = reader.lengthInSamples + latencySamples + preparation.tailSamples;

    for (juce::int64 samplePosition = 0; samplePosition < totalSamples; samplePosition += processingBlockSize) {
        if (isCancellationRequested(isCancelled)) {
            return cancelledMessage;
        }

        const auto samplesThisBlock
            = static_cast<int>(juce::jmin<juce::int64>(processingBlockSize, totalSamples - samplePosition));

        // Clearing the whole block also pads a short final block with silence up to the prepared block size,
        // since many plugins assume a constant block size.
        buffer.clear();

        const auto samplesToRead = static_cast<int>(juce::jmax<juce::int64>(
            0, juce::jmin<juce::int64>(samplesThisBlock, reader.lengthInSamples - samplePosition)
        ));

        if (samplesToRead > 0) {
            const auto readSucceeded = readInputBlock(
                reader, buffer, readBuffer, readChannelPointers, samplePosition, samplesToRead, inputGain
            );

            if (!failureTracker.recordBlock(readSucceeded, samplePosition, samplesToRead)) {
                return failureTracker.getErrorMessage();
            }
        }

        const auto signalChannels = processBlockThroughChain(buffer, chainInstances, numChannels, midi);

        // Skip the output samples that still fall within the chain latency.
        const auto firstSampleToWrite
            = static_cast<int>(juce::jlimit<juce::int64>(0, samplesThisBlock, latencySamples - samplePosition));
        const auto samplesToWrite = samplesThisBlock - firstSampleToWrite;

        if (samplesToWrite <= 0) {
            continue;
        }

        const auto writeView
            = prepareOutputView(buffer, numChannels, signalChannels, firstSampleToWrite, samplesToWrite);
        measureOutputBlock(writeView, samplesToWrite, summary);

        if (!writer.writeFromAudioSampleBuffer(writeView, 0, samplesToWrite)) {
            return "Failed while writing processed audio data";
        }

        summary.samplesWritten += samplesToWrite;
    }

    if (failureTracker.getFailedBlockCount() > 0) {
        utils::logWarn(
            "{} audio block(s) failed to decode and were processed as decoded so far, first at sample {}",
            failureTracker.getFailedBlockCount(),
            failureTracker.getFirstFailurePosition()
        );
    }

    return {};
}

/// Returns the gain that keeps the rendered output from clipping, which is 1 when the peak is within full scale.
/// The ceiling leaves room for the dither noise when the output is dithered.
static float getClipGuardGain(const RenderSummary& summary, const int bitsPerSample)
{
    const auto ditherHeadroom
        = bitsPerSample <= maximumDitheredBitDepth ? 2.0f / std::ldexp(1.0f, bitsPerSample - 1) : 0.0f;
    const auto ceiling = 1.0f - ditherHeadroom;
    return summary.peak > ceiling ? ceiling / summary.peak : 1.0f;
}

/// Returns the number of samples to keep from the rendered output.
/// Trailing silence from the tail allowance is dropped, but the output is never shorter than the source.
static juce::int64 getFinalLength(const RenderSummary& summary, const juce::int64 sourceLength)
{
    return juce::jlimit<juce::int64>(
        0, summary.samplesWritten, juce::jmax(sourceLength, summary.lastAudibleSample + 1)
    );
}

/// Adds triangular (TPDF) dither of one least significant bit to the block before it is quantized,
/// which turns the quantization error into benign noise instead of distortion on quiet material.
static void addDither(
    juce::AudioBuffer<float>& block,
    const int numSamples,
    const int bitsPerSample,
    juce::Random& random
)
{
    // The JUCE writers scale full scale to 2^(bits - 1) - 1 integer steps.
    const auto leastSignificantBit = 1.0f / (std::ldexp(1.0f, bitsPerSample - 1) - 1.0f);

    for (int channel = 0; channel < block.getNumChannels(); ++channel) {
        for (int sample = 0; sample < numSamples; ++sample) {
            block.addSample(channel, sample, (random.nextFloat() - random.nextFloat()) * leastSignificantBit);
        }
    }
}

/// Copies the rendered intermediate audio into the final writer,
/// applying the clip guard gain, trimming the trailing silence, and dithering 16-bit output.
/// Returns an error message, or an empty string when the whole output was written.
static juce::String writeFinalOutput(
    juce::AudioFormatReader& renderedReader,
    juce::AudioFormatWriter& writer,
    const juce::int64 finalLength,
    const float gain,
    const int bitsPerSample,
    const PluginProcessingService::CancellationCheck& isCancelled
)
{
    juce::AudioBuffer<float> block(static_cast<int>(renderedReader.numChannels), finalizeBlockSize);
    juce::Random random(ditherSeed);

    for (juce::int64 samplePosition = 0; samplePosition < finalLength; samplePosition += finalizeBlockSize) {
        if (isCancellationRequested(isCancelled)) {
            return cancelledMessage;
        }

        const auto samplesThisBlock
            = static_cast<int>(juce::jmin<juce::int64>(finalizeBlockSize, finalLength - samplePosition));

        if (!renderedReader.read(&block, 0, samplesThisBlock, samplePosition, true, true)) {
            return "Failed while reading the rendered audio";
        }

        block.applyGain(0, samplesThisBlock, gain);

        if (bitsPerSample <= maximumDitheredBitDepth) {
            addDither(block, samplesThisBlock, bitsPerSample, random);
        }

        if (!writer.writeFromAudioSampleBuffer(block, 0, samplesThisBlock)) {
            return "Failed while writing processed audio data";
        }
    }

    return {};
}

/// Moves the validated temporary output into place and disposes of the original.
/// A different file already at the output path is only replaced when overwriting is allowed,
/// and it goes to the system trash so it can be recovered.
/// When the output path differs from the source, the original is moved to the system trash afterwards,
/// and the output is removed again when that fails.
/// Returns an empty string on success, or a user-facing error message.
static juce::String commitProcessedOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& file,
    const juce::File& outputFile,
    const PluginProcessingOptions& options
)
{
    const bool replacingOriginal = file == outputFile;
    if (!replacingOriginal && outputFile.exists()) {
        if (!options.overwriteExisting) {
            utils::deleteFile(temporaryFile.getFile());
            return utils::format("Output file {} already exists", outputFile.getFileName());
        }

        if (!utils::moveToTrash(outputFile)) {
            utils::deleteFile(temporaryFile.getFile());
            return "Could not move the existing output file to the system trash";
        }
    }

    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        return "Could not move processed output into place";
    }

    // Keeping both files would leave a duplicate next to the original, so a failed trash undoes the output,
    // the same as in normalization.
    if (!replacingOriginal && file.existsAsFile() && !utils::moveToTrash(file)) {
        utils::deleteFile(outputFile);
        return "Could not move the original audio file to the system trash";
    }

    return {};
}

/// Returns the record unchanged when the file still matches it on disk,
/// otherwise analyzes the file again, so the input gain is never computed from an outdated peak.
/// The custom gain is dropped for a changed file, since it was set for the previous version of the file.
static AudioAnalysisRecord refreshIfChanged(const AudioAnalysisRecord& record)
{
    return record.matchesFileOnDisk() ? record : AudioAnalysisService::analyzeFile(record.file);
}

/// Prepares the chain, renders the whole file through it into an unquantized 32-bit float WAV file,
/// and releases the chain again.
/// Plugin setup and release run on the message thread as VST3 requires, and only processBlock runs on this thread.
/// Returns an error message, or an empty string when the whole file was rendered.
static juce::String renderChainToFile(
    juce::AudioFormatReader& reader,
    const juce::File& intermediateFile,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const PluginProcessingOptions& options,
    const int numChannels,
    const float inputGain,
    const PluginProcessingService::CancellationCheck& isCancelled,
    RenderSummary& summary
)
{
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::OutputStream> outputStream(intermediateFile.createOutputStream().release());

    if (outputStream == nullptr) {
        return "Could not create temporary output file";
    }

    const auto writerOptions = juce::AudioFormatWriterOptions()
                                   .withSampleRate(reader.sampleRate)
                                   .withNumChannels(numChannels)
                                   .withBitsPerSample(32)
                                   .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wavFormat.createWriterFor(outputStream, writerOptions);

    if (writer == nullptr) {
        return "Could not create the temporary render file";
    }

    ChainPreparation preparation;
    const bool prepared = callOnMessageThread(
        [&] { preparation = prepareChain(chainInstances, options, numChannels, reader.sampleRate); }, isCancelled
    );

    if (!prepared || preparation.errorMessage.isNotEmpty()) {
        return prepared ? preparation.errorMessage : juce::String(cancelledMessage);
    }

    if (preparation.latencySamples > 0) {
        utils::logDebug("Compensating {} samples of plugin chain latency", preparation.latencySamples);
    }

    const auto renderError = renderThroughChain(
        reader, *writer, chainInstances, preparation, numChannels, inputGain, isCancelled, summary
    );

    // Release on every path so a failed file does not leave the chain active for the next one.
    // After cancellation the message thread may be blocked waiting for this worker, so the release is skipped,
    // and the plugin instances release their resources when they are destroyed instead.
    callOnMessageThread([&chainInstances] { releasePluginResources(chainInstances); }, isCancelled);

    return renderError;
}

/// Writes the rendered intermediate audio to the AIFF output file at the given bit depth,
/// with the clip guard gain, the trimmed length, and dither for 16-bit output.
/// Returns an error message, or an empty string when the output was written.
static juce::String writeProcessedAiff(
    juce::AudioFormat& aiffFormat,
    const juce::File& intermediateFile,
    const juce::File& outputFile,
    const juce::AudioFormatReader& sourceReader,
    const juce::int64 finalLength,
    const float gain,
    const int bitsPerSample,
    const PluginProcessingService::CancellationCheck& isCancelled
)
{
    juce::WavAudioFormat wavFormat;
    auto renderedStream = intermediateFile.createInputStream();

    if (renderedStream == nullptr) {
        return "Could not open the rendered audio";
    }

    const std::unique_ptr<juce::AudioFormatReader> renderedReader(
        wavFormat.createReaderFor(renderedStream.release(), true)
    );

    if (renderedReader == nullptr) {
        return "Could not read the rendered audio";
    }

    std::unique_ptr<juce::OutputStream> outputStream(outputFile.createOutputStream().release());

    if (outputStream == nullptr) {
        return "Could not create temporary output file";
    }

    const auto writerOptions = juce::AudioFormatWriterOptions()
                                   .withSampleRate(sourceReader.sampleRate)
                                   .withNumChannels(static_cast<int>(renderedReader->numChannels))
                                   .withBitsPerSample(bitsPerSample)
                                   .withMetadataValues(copyMetadata(sourceReader.metadataValues));
    const auto writer = aiffFormat.createWriterFor(outputStream, writerOptions);

    if (writer == nullptr) {
        return "Could not create AIFF writer for output";
    }

    return writeFinalOutput(*renderedReader, *writer, finalLength, gain, bitsPerSample, isCancelled);
}

PluginProcessingResult PluginProcessingService::processFile(
    const AudioAnalysisRecord& sourceRecord,
    const PluginProcessingOptions& options,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const CancellationCheck& isCancelled
)
{
    const auto& file = sourceRecord.file;

    if (const auto inputError = validateProcessingInputs(sourceRecord, options, chainInstances);
        inputError.isNotEmpty())
    {
        return fail(file, inputError);
    }

    const auto record = refreshIfChanged(sourceRecord);

    if (record.hasError()) {
        return fail(file, record.errorMessage);
    }

    auto& formatManager = getThreadLocalFormatManager();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        return fail(file, "Unsupported or unreadable audio file");
    }

    auto* aiffFormat = findAiffFormat(formatManager);
    if (aiffFormat == nullptr) {
        return fail(file, "AIFF writer not available in this build");
    }

    const auto outputFile = getProcessingOutputFile(file);

    if (outputFile != file && outputFile.exists() && !options.overwriteExisting) {
        return fail(
            file,
            utils::format("Output file {} already exists. Use force overwrite to replace it.", outputFile.getFileName())
        );
    }

    const auto numChannels = clampChannelCount(static_cast<int>(reader->numChannels));
    const juce::TemporaryFile intermediateFile(".wav");
    RenderSummary summary;

    if (const auto renderError = renderChainToFile(
            *reader,
            intermediateFile.getFile(),
            chainInstances,
            options,
            numChannels,
            getInputGain(record, options),
            isCancelled,
            summary
        );
        renderError.isNotEmpty())
    {
        return fail(file, renderError);
    }

    const juce::TemporaryFile temporaryFile(outputFile);
    const auto bitsPerSample = AudioNormalizationService::getOutputBitDepth(record, *reader);
    const auto clipGuardGain = getClipGuardGain(summary, bitsPerSample);

    if (const auto writeError = writeProcessedAiff(
            *aiffFormat,
            intermediateFile.getFile(),
            temporaryFile.getFile(),
            *reader,
            getFinalLength(summary, reader->lengthInSamples),
            clipGuardGain,
            bitsPerSample,
            isCancelled
        );
        writeError.isNotEmpty())
    {
        return fail(file, writeError);
    }

    // Preserve metadata from the original input file by copying it onto the temporary AIFF file.
    // This carries ID3 tags, embedded artwork, and similar information across formats such as MP3 to AIFF.
    // Failure to read metadata is non-fatal because some inputs may not have any TagLib-readable metadata,
    // but failing to write metadata that was read is fatal, so the original and its tags are kept.
    {
        MetadataService::Metadata metadata;
        if (MetadataService::readMetadata(file, metadata) && !metadata.isEmpty()
            && !MetadataService::writeMetadata(temporaryFile.getFile(), metadata))
        {
            utils::deleteFile(temporaryFile.getFile());
            return fail(file, "Could not copy metadata onto the processed output");
        }
    }

    // Validate the temporary AIFF file by opening it for reading.
    {
        const std::unique_ptr<juce::AudioFormatReader> verifyReader(
            formatManager.createReaderFor(temporaryFile.getFile())
        );

        if (verifyReader == nullptr) {
            utils::deleteFile(temporaryFile.getFile());
            return fail(file, "Processed output failed validation");
        }
    }

    // Check again before touching the original, since cancellation may arrive during the last render block.
    // The coordinator drops results of a cancelled run,
    // so committing here would change files on disk without the caller ever learning about it.
    if (isCancellationRequested(isCancelled)) {
        utils::deleteFile(temporaryFile.getFile());
        return fail(file, cancelledMessage);
    }

    if (const auto commitError = commitProcessedOutput(temporaryFile, file, outputFile, options);
        commitError.isNotEmpty())
    {
        return fail(file, commitError);
    }

    PluginProcessingResult result;
    result.originalFile = file;
    result.originalFullPath = file.getFullPathName();
    result.outputFile = outputFile;

    if (clipGuardGain < 1.0f) {
        result.notice
            = utils::format("Reduced {:.1f} dB to avoid clipping", -juce::Decibels::gainToDecibels(clipGuardGain));
        utils::logInfo("{}: {}", outputFile.getFullPathName().quoted(), result.notice);
    }

    result.fileName = outputFile.getFileName();
    result.analysisRecord = AudioAnalysisService::analyzeFile(outputFile);
    // Custom gain has been baked in. Clear it on the new record.
    result.analysisRecord.customGainDb = 0.0f;
    result.analysisRecord.hasCustomGain = false;
    result.succeeded = !result.analysisRecord.hasError();

    if (!result.succeeded) {
        result.errorMessage
            = utils::format("The file was processed, but re-analysis failed: {}", result.analysisRecord.errorMessage);
        utils::logError("Plugin processing re-analysis failed for {}", result.outputFile.getFullPathName().quoted());
    }

    return result;
}
