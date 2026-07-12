#include "PluginProcessingService.h"

#include "MetadataService.h"
#include "utils.h"

#include <algorithm>
#include <cmath>

namespace audiobatch::plugin_processing
{
constexpr int processingBlockSize = 1024;
constexpr int outputBitsPerSample = 16;
constexpr auto outputExtension = ".aiff";

/// Shorthand for building a failure result, keeping the early-return error paths compact.
static PluginProcessingResult fail(const juce::File& file, const juce::String& message)
{
    return PluginProcessingResult::failure(file, message);
}

/// Locates the AIFF format among the registered formats by name,
/// so the writer does not depend on registration order.
/// Returns nullptr when no AIFF format is available.
static juce::AudioFormat* findAiffFormat(juce::AudioFormatManager& formatManager)
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
    auto trySetMainBusChannels = [&plugin](const juce::AudioChannelSet& channelSet) {
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

PluginProcessingResult PluginProcessingService::processFile(
    const AudioAnalysisRecord& record,
    const PluginProcessingOptions& options,
    const std::vector<juce::AudioPluginInstance*>& chainInstances
)
{
    const auto& file = record.file;

    if (!file.existsAsFile()) {
        return fail(file, "File does not exist");
    }

    if (!record.isReady()) {
        return fail(file, "File analysis must finish before processing");
    }

    if (chainInstances.empty() || chainInstances.size() != options.plugins.size()) {
        return fail(file, "Plugin chain instances are not available");
    }

    if (std::ranges::any_of(chainInstances, [](const auto* instance) { return instance == nullptr; })) {
        return fail(file, "Plugin chain instances are not available");
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
    juce::TemporaryFile temporaryFile(outputFile);
    std::unique_ptr<juce::OutputStream> outputStream(temporaryFile.getFile().createOutputStream().release());

    if (outputStream == nullptr) {
        return fail(file, "Could not create temporary output file");
    }

    const auto numChannels = clampChannelCount(static_cast<int>(reader->numChannels));
    const auto sampleRate = reader->sampleRate;

    auto writerOptions = juce::AudioFormatWriterOptions()
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

    // Determine input gain.
    float inputGain = 1.0f;
    if (record.hasCustomGain) {
        inputGain = juce::Decibels::decibelsToGain(record.customGainDb);
    } else if (options.normalizeBeforePlugin) {
        const auto peak = peakMagnitude(record.overallPeak);
        if (peak > 0.0f) {
            inputGain = 1.0f / peak;
        }
    }

    // Configure and prepare every plugin in the chain against the file's channel count.
    // Each stage's tail must ring through the remaining stages,
    // so the total tail is the sum of the individually clamped per-plugin tails.
    int processChannelCount = numChannels;
    juce::int64 tailSamples = 0;

    for (std::size_t pluginIndex = 0; pluginIndex < chainInstances.size(); ++pluginIndex) {
        auto* plugin = chainInstances[pluginIndex];

        if (!configurePluginChannels(*plugin, numChannels, sampleRate)) {
            writer.reset();
            utils::deleteFile(temporaryFile.getFile());
            return fail(
                file,
                "Plugin " + options.plugins[pluginIndex].name.quoted() + " does not support the file's channel layout ("
                    + juce::String(numChannels) + " channels)"
            );
        }

        plugin->prepareToPlay(sampleRate, processingBlockSize);
        plugin->reset();

        // Restore plugin state per-file so each file starts clean.
        const auto& state = options.plugins[pluginIndex].state;
        if (state.getSize() > 0) {
            plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }

        processChannelCount
            = juce::jmax(processChannelCount, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());

        const auto tailSeconds = plugin->getTailLengthSeconds();
        tailSamples += static_cast<juce::int64>(std::ceil(juce::jlimit(0.0, 30.0, tailSeconds) * sampleRate));
    }

    juce::AudioBuffer<float> buffer(processChannelCount, processingBlockSize);
    juce::AudioBuffer<float> readBuffer(numChannels, processingBlockSize);
    juce::MidiBuffer midi;
    std::vector<float*> readChannelPointers(static_cast<std::size_t>(numChannels));

    const auto totalSamples = reader->lengthInSamples + tailSamples;

    for (juce::int64 samplePosition = 0; samplePosition < totalSamples; samplePosition += processingBlockSize) {
        const auto samplesThisBlock
            = static_cast<int>(juce::jmin<juce::int64>(processingBlockSize, totalSamples - samplePosition));

        buffer.clear();

        const auto samplesToRead = static_cast<int>(juce::jmax<juce::int64>(
            0, juce::jmin<juce::int64>(samplesThisBlock, reader->lengthInSamples - samplePosition)
        ));

        if (samplesToRead > 0) {
            readBuffer.clear();
            for (int channel = 0; channel < numChannels; ++channel) {
                readChannelPointers[static_cast<std::size_t>(channel)] = readBuffer.getWritePointer(channel);
            }

            if (!reader->read(readChannelPointers.data(), numChannels, samplePosition, samplesToRead)) {
                writer.reset();
                utils::deleteFile(temporaryFile.getFile());
                return fail(file, "Failed while reading audio data");
            }

            for (int channel = 0; channel < numChannels; ++channel) {
                buffer.copyFrom(channel, 0, readBuffer, channel, 0, samplesToRead);
            }

            if (!juce::approximatelyEqual(inputGain, 1.0f)) {
                for (int channel = 0; channel < numChannels; ++channel) {
                    buffer.applyGain(channel, 0, samplesToRead, inputGain);
                }
            }
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

        const auto channelsToWrite = juce::jmin(numChannels, buffer.getNumChannels());
        juce::AudioBuffer<float> writeView(buffer.getArrayOfWritePointers(), channelsToWrite, 0, samplesThisBlock);

        if (!writer->writeFromAudioSampleBuffer(writeView, 0, samplesThisBlock)) {
            writer.reset();
            utils::deleteFile(temporaryFile.getFile());
            return fail(file, "Failed while writing processed audio data");
        }
    }

    writer.reset();
    for (auto* plugin : chainInstances) {
        plugin->releaseResources();
    }

    // Preserve metadata from the original input file by copying it onto the temporary AIFF file.
    // This carries ID3 tags, embedded artwork, and similar information across formats such as MP3 to AIFF.
    // Failure to read metadata is non-fatal because some inputs may not have any TagLib-readable metadata.
    {
        MetadataService::Metadata metadata;
        if (MetadataService::readMetadata(file, metadata) && !metadata.isEmpty()) {
            if (!MetadataService::writeMetadata(temporaryFile.getFile(), metadata)) {
                utils::logError("Failed to copy metadata onto processed output for " + file.getFullPathName().quoted());
            }
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

    // If the original file path differs from the output path (different extension), delete the original.
    const bool replacingOriginal = file == outputFile;
    if (outputFile.existsAsFile() && !utils::deleteFile(outputFile)) {
        utils::deleteFile(temporaryFile.getFile());
        return fail(file, "Could not replace existing output file");
    }

    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        return fail(file, "Could not move processed output into place");
    }

    if (!replacingOriginal && file.existsAsFile()) {
        // Output had a different extension.
        // Move the original file to the system trash so the user can recover it if needed.
        if (!utils::moveToTrash(file)) {
            utils::logError("Could not move the original file to the system trash: " + file.getFullPathName().quoted());
        }
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
        result.errorMessage = "The file was processed, but re-analysis failed: " + result.analysisRecord.errorMessage;
        utils::logError("Plugin processing re-analysis failed for " + result.outputFile.getFullPathName().quoted());
    }

    return result;
}
