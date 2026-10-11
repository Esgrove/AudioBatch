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
/// The output is written to a temporary 16-bit AIFF file that is validated,
/// given the original file's metadata, and then moved into place,
/// with the processed file re-analyzed for the result record.

#include "PluginProcessingService.h"

#include "MetadataService.h"
#include "utils.h"

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
constexpr int outputBitsPerSample = 16;
constexpr auto outputExtension = ".aiff";
constexpr auto cancelledMessage = "Processing was cancelled";
/// How long a worker waits for the message thread between cancellation checks.
constexpr int messageThreadPollIntervalMs = 20;

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
        // The state is restored before prepareToPlay because settings such as lookahead or linear phase
        // change the plugin's latency, and JUCE refreshes the reported latency of VST3 and AU plugins
        // when they are prepared.
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
            += static_cast<juce::int64>(std::ceil(juce::jlimit(0.0, 30.0, tailSeconds) * sampleRate));
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
    return input.withFileExtension(outputExtension);
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
/// Returns false when the reader fails.
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

    if (!reader.read(readChannelPointers.data(), channelCount, samplePosition, samplesToRead)) {
        return false;
    }

    for (int channel = 0; channel < channelCount; ++channel) {
        buffer.copyFrom(channel, 0, readBuffer, channel, 0, samplesToRead);
    }

    if (!juce::approximatelyEqual(inputGain, 1.0f)) {
        for (int channel = 0; channel < channelCount; ++channel) {
            buffer.applyGain(channel, 0, samplesToRead, inputGain);
        }
    }

    return true;
}

/// Streams the whole file through every plugin in chain order and writes the processed audio.
/// The chain latency and tail are rendered on top of the file length,
/// and the first `latencySamples` output samples are discarded,
/// so the processed audio lines up with the input and its end is not cut off.
/// Returns an error message, or an empty string when the whole file was written.
static juce::String renderThroughChain(
    juce::AudioFormatReader& reader,
    juce::AudioFormatWriter& writer,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const ChainPreparation& preparation,
    const int numChannels,
    const float inputGain,
    const PluginProcessingService::CancellationCheck& isCancelled
)
{
    const auto latencySamples = preparation.latencySamples;

    juce::AudioBuffer<float> buffer(preparation.processChannelCount, processingBlockSize);
    juce::AudioBuffer<float> readBuffer(numChannels, processingBlockSize);
    juce::MidiBuffer midi;
    std::vector<float*> readChannelPointers(static_cast<std::size_t>(numChannels));

    const auto totalSamples = reader.lengthInSamples + latencySamples + preparation.tailSamples;
    juce::String renderError;

    for (juce::int64 samplePosition = 0; samplePosition < totalSamples; samplePosition += processingBlockSize) {
        if (isCancellationRequested(isCancelled)) {
            renderError = cancelledMessage;
            break;
        }

        const auto samplesThisBlock
            = static_cast<int>(juce::jmin<juce::int64>(processingBlockSize, totalSamples - samplePosition));

        buffer.clear();

        const auto samplesToRead = static_cast<int>(juce::jmax<juce::int64>(
            0, juce::jmin<juce::int64>(samplesThisBlock, reader.lengthInSamples - samplePosition)
        ));

        if (samplesToRead > 0
            && !readInputBlock(
                reader, buffer, readBuffer, readChannelPointers, samplePosition, samplesToRead, inputGain
            ))
        {
            renderError = "Failed while reading audio data";
            break;
        }

        // Process: trim buffer to the block size the plugin expects (it was prepared at processingBlockSize).
        if (samplesThisBlock < processingBlockSize) {
            // Pad with silence to the prepared block size.
            // Many plugins assume a constant block size.
            for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
                buffer.clear(channel, samplesThisBlock, processingBlockSize - samplesThisBlock);
            }
        }

        // Run the block through every plugin in chain order on the same buffer.
        for (auto* plugin : chainInstances) {
            midi.clear();
            plugin->processBlock(buffer, midi);
        }

        // Skip the output samples that still fall within the chain latency.
        const auto firstSampleToWrite
            = static_cast<int>(juce::jlimit<juce::int64>(0, samplesThisBlock, latencySamples - samplePosition));
        const auto samplesToWrite = samplesThisBlock - firstSampleToWrite;

        if (samplesToWrite <= 0) {
            continue;
        }

        const auto channelsToWrite = juce::jmin(numChannels, buffer.getNumChannels());
        juce::AudioBuffer<float> const writeView(
            buffer.getArrayOfWritePointers(), channelsToWrite, firstSampleToWrite, samplesToWrite
        );

        if (!writer.writeFromAudioSampleBuffer(writeView, 0, samplesToWrite)) {
            renderError = "Failed while writing processed audio data";
            break;
        }
    }

    return renderError;
}

PluginProcessingResult PluginProcessingService::processFile(
    const AudioAnalysisRecord& record,
    const PluginProcessingOptions& options,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const CancellationCheck& isCancelled
)
{
    const auto& file = record.file;

    if (const auto inputError = validateProcessingInputs(record, options, chainInstances); inputError.isNotEmpty()) {
        return fail(file, inputError);
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
    juce::TemporaryFile const temporaryFile(outputFile);
    std::unique_ptr<juce::OutputStream> outputStream(temporaryFile.getFile().createOutputStream().release());

    if (outputStream == nullptr) {
        return fail(file, "Could not create temporary output file");
    }

    const auto numChannels = clampChannelCount(static_cast<int>(reader->numChannels));
    const auto sampleRate = reader->sampleRate;

    auto const writerOptions = juce::AudioFormatWriterOptions()
                                   .withSampleRate(sampleRate)
                                   .withNumChannels(numChannels)
                                   .withBitsPerSample(outputBitsPerSample)
                                   .withMetadataValues(copyMetadata(reader->metadataValues));

    std::unique_ptr<juce::AudioFormatWriter> writer(aiffFormat->createWriterFor(outputStream, writerOptions));

    if (writer == nullptr) {
        return fail(file, "Could not create AIFF writer for output");
    }

    // createWriterFor takes ownership of the stream on success.
    juce::ignoreUnused(outputStream);

    const auto inputGain = getInputGain(record, options);

    // Plugin setup runs on the message thread as VST3 requires, and only processBlock runs on this worker thread.
    ChainPreparation preparation;
    const bool prepared = callOnMessageThread(
        [&] { preparation = prepareChain(chainInstances, options, numChannels, sampleRate); }, isCancelled
    );

    if (!prepared || preparation.errorMessage.isNotEmpty()) {
        writer.reset();
        utils::deleteFile(temporaryFile.getFile());
        return fail(file, prepared ? preparation.errorMessage : juce::String(cancelledMessage));
    }

    const auto latencySamples = preparation.latencySamples;

    if (latencySamples > 0) {
        utils::logDebug(
            "Compensating {} samples of plugin chain latency for {}", latencySamples, file.getFullPathName().quoted()
        );
    }

    const auto renderError
        = renderThroughChain(*reader, *writer, chainInstances, preparation, numChannels, inputGain, isCancelled);

    writer.reset();

    // Release on every path so a failed file does not leave the chain active for the next one.
    // After cancellation the message thread may be blocked waiting for this worker, so the release is skipped,
    // and the plugin instances release their resources when they are destroyed instead.
    callOnMessageThread([&chainInstances] { releasePluginResources(chainInstances); }, isCancelled);

    if (renderError.isNotEmpty()) {
        utils::deleteFile(temporaryFile.getFile());
        return fail(file, renderError);
    }

    // Preserve metadata from the original input file by copying it onto the temporary AIFF file.
    // This carries ID3 tags, embedded artwork, and similar information across formats such as MP3 to AIFF.
    // Failure to read metadata is non-fatal because some inputs may not have any TagLib-readable metadata.
    {
        MetadataService::Metadata metadata;
        if (MetadataService::readMetadata(file, metadata) && !metadata.isEmpty()
            && !MetadataService::writeMetadata(temporaryFile.getFile(), metadata))
        {
            utils::logError("Failed to copy metadata onto processed output for {}", file.getFullPathName().quoted());
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

    // If the original file path differs from the output path (different extension), delete the original.
    const bool replacingOriginal = file == outputFile;
    if (outputFile.existsAsFile() && !utils::deleteFile(outputFile)) {
        utils::deleteFile(temporaryFile.getFile());
        return fail(file, "Could not replace existing output file");
    }

    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        return fail(file, "Could not move processed output into place");
    }

    // When the output had a different extension,
    // move the original file to the system trash so the user can recover it if needed.
    if (!replacingOriginal && file.existsAsFile() && !utils::moveToTrash(file)) {
        utils::logError("Could not move the original file to the system trash: {}", file.getFullPathName().quoted());
    }

    PluginProcessingResult result;
    result.originalFile = file;
    result.originalFullPath = file.getFullPathName();
    result.outputFile = outputFile;
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
