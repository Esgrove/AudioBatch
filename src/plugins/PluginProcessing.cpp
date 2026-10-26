/// Implementation of the plugin processing functions.
/// Covers reading the source file with a thread-local format manager,
/// applying custom or normalization gain before the chain,
/// configuring plugin buses for the file's channel layout,
/// switching plugins to offline (non-realtime) rendering mode,
/// restoring their state and preparing them per file,
/// and rendering block by block through the chain including plugin tails,
/// with the chain's reported latency removed from the start of the output.
/// Plugin setup and release are posted to the message thread as VST3 requires, with a cancellation-aware wait
/// so a worker never deadlocks against a message thread that waits for it,
/// while processBlock runs on the calling worker thread.
/// The chain output is rendered to an unquantized 32-bit float file first,
/// so the end of the audible tail is known before the final 16-bit AIFF file is written.
/// Output is truncated without additional dither or gain adjustment. Plugins handle dithering and output level.
/// The AIFF file is validated, given the original file's metadata,
/// and then moved into place, with the processed file re-analyzed for the result record.

#include "plugins/PluginProcessing.h"

#include "analysis/DecodeFailureTracker.h"
#include "normalization/AudioNormalization.h"
#include "plugins/PluginChainPreparation.h"
#include "utils/AudioFormatUtils.h"
#include "utils/AudioMetadata.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <cmath>
#include <expected>
#include <functional>
#include <memory>
#include <span>

namespace audiobatch
{
namespace
{
/// Block size for copying the rendered intermediate file into the final output.
constexpr int finalizeBlockSize {32768};
/// Level below which rendered tail samples count as silence and are trimmed from the end of the output.
/// The value is -96 dB, the noise floor of 16-bit audio.
constexpr float silenceThreshold {1.5849e-5f};
/// Fixed PCM depth of plugin-processed output, independent of the source.
constexpr int outputBitsPerSample {16};
/// Scale from floating-point full scale to a signed 16-bit PCM value.
constexpr int outputPcmScale {32'768};
/// JUCE's integer writer takes PCM values aligned to the most significant bits of a 32-bit integer.
constexpr int outputPcmAlignment {65'536};
constexpr auto cancelledMessage {"Processing was cancelled"};

/// Share of a file's progress taken by analyzing a source that changed since its last analysis.
constexpr float sourceAnalysisProgressShare {0.1f};
/// Progress at which rendering through the chain ends and writing the final output begins.
constexpr float renderProgressEnd {0.7f};
/// Progress at which writing the final output ends and analyzing the output begins.
constexpr float writeProgressEnd {0.85f};

/// Length measurements of the rendered chain output, used to finalize the output file.
struct RenderSummary {
    /// Number of samples written to the intermediate file.
    juce::int64 samplesWritten {0};
    /// Position of the last sample above the silence threshold, or -1 when the output is silent.
    juce::int64 lastAudibleSample {-1};
    /// Notice about source blocks that failed to decode but were tolerated, or empty when there were none.
    juce::String decodeNotice;
};

/// Outcome of writing the final 16-bit output.
struct WriteSummary {
    /// True when the final PCM conversion saturated at least one sample.
    bool clipped {false};
};

/// Shorthand for building a failure result, keeping the early-return error paths compact.
PluginProcessingResult fail(const juce::File& file, const juce::String& message)
{
    return PluginProcessingResult::failure(file, message);
}

/// Most channels a source can have, since processing, like analysis, targets mono and stereo files.
/// A file with more channels is refused, because the output would drop the extra channels
/// before the original is replaced.
constexpr unsigned int maximumProcessingChannels {2};
}  // namespace

/// Returns true when the file's format can be read for processing.
bool canProcessFileWithPlugins(const juce::File& file)
{
    const auto reader = utils::createAudioReader(file);
    return reader != nullptr;
}

/// Returns the on-disk output path that processing would produce for the given input.
juce::File getProcessingOutputFile(const juce::File& input)
{
    return getNormalizationOutputFile(input);
}

/// Returns an error message when the record or the plugin chain cannot be processed.
static std::expected<void, juce::String> validateProcessingInputs(
    const AudioAnalysisRecord& record,
    const PluginProcessingOptions& options,
    const std::vector<juce::AudioPluginInstance*>& chainInstances
)
{
    if (!record.file.existsAsFile()) {
        return std::unexpected(juce::String("File does not exist"));
    }

    if (!record.isReady()) {
        return std::unexpected(juce::String("File analysis must finish before processing"));
    }

    const auto chainAvailable = !chainInstances.empty() && chainInstances.size() == options.plugins.size()
        && std::ranges::none_of(chainInstances, [](const auto* instance) { return instance == nullptr; });

    if (!chainAvailable) {
        return std::unexpected(juce::String("Plugin chain instances are not available"));
    }

    return {};
}

/// Returns the linear gain applied before the first plugin.
/// A custom gain wins, otherwise the file is normalized to the requested peak or loudness target,
/// and otherwise it is left unchanged.
/// A file without a measurable level for the target, such as silence, is left unchanged too.
static float getInputGain(const AudioAnalysisRecord& record, const PluginProcessingOptions& options)
{
    if (record.hasCustomGain) {
        return juce::Decibels::decibelsToGain(record.customGainDb);
    }

    if (options.normalizeBeforePlugin.has_value()) {
        return getNormalizationGain(record, *options.normalizeBeforePlugin).value_or(1.0f);
    }

    return 1.0f;
}

/// Reads `samplesToRead` frames starting at `samplePosition` into the start of
/// `buffer` and applies `inputGain`. `readBuffer` and `readChannelPointers` are
/// scratch storage reused across blocks. Returns false when the reader reports
/// a failure, following the rules of utils::readFloatSamples.
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

    const auto readSucceeded
        = utils::readFloatSamples(reader, readChannelPointers.data(), channelCount, samplePosition, samplesToRead);

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

/// Replaces the first channel with the average of the first two channels over the given sample range,
/// so a stereo signal derived from a mono source keeps both of its channels when it continues as mono.
static void foldStereoToMono(juce::AudioBuffer<float>& buffer, const int firstSample, const int numSamples)
{
    buffer.addFrom(0, firstSample, buffer, 1, firstSample, numSamples);
    buffer.applyGain(0, firstSample, numSamples, 0.5f);
}

/// Runs one block through every plugin in chain order on the same buffer,
/// and returns the number of main channels carrying signal after the last plugin.
/// The signal starts with the file's channel count.
/// A mono signal reaching a plugin with a stereo main input is copied to the second channel first,
/// so stereo-linked and mid/side processing see a centred source instead of a hard-left one,
/// and a mono plugin earlier in the chain still affects both channels.
/// A stereo signal reaching a plugin with a mono main input is folded to the average of both channels,
/// the same way the output of a mono file is written, so the order of the plugins does not drop a channel.
/// Channels beyond a plugin's main input, such as a sidechain, are cleared so no dry signal leaks into them.
/// The buffer is as wide as the widest plugin, and each plugin only sees the channels of its own buses.
int processBlockThroughChain(
    juce::AudioBuffer<float>& buffer,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const int numChannels,
    juce::MidiBuffer& midi,
    const std::span<juce::AudioBuffer<float>> preparedViews
)
{
    auto signalChannels = numChannels;

    for (std::size_t pluginIndex = 0; pluginIndex < chainInstances.size(); ++pluginIndex) {
        auto* plugin = chainInstances[pluginIndex];
        const auto mainInputChannels = juce::jmin(plugin->getMainBusNumInputChannels(), buffer.getNumChannels());

        if (signalChannels == 1 && mainInputChannels >= 2) {
            buffer.copyFrom(1, 0, buffer, 0, 0, buffer.getNumSamples());
        } else if (signalChannels >= 2 && mainInputChannels == 1) {
            foldStereoToMono(buffer, 0, buffer.getNumSamples());
        }

        for (int channel = juce::jmax(mainInputChannels, 1); channel < buffer.getNumChannels(); ++channel) {
            buffer.clear(channel, 0, buffer.getNumSamples());
        }

        // Each plugin gets a view with only as many channels as it was prepared for,
        // because JUCE's Audio Unit host asserts that the buffer is not wider than the prepared layout.
        const auto pluginChannels = juce::jmin(
            buffer.getNumChannels(), juce::jmax(plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels())
        );
        juce::AudioBuffer<float> temporaryView;
        auto& pluginBuffer = preparedViews.empty() ? temporaryView : preparedViews[pluginIndex];
        pluginBuffer.setDataToReferTo(buffer.getArrayOfWritePointers(), pluginChannels, buffer.getNumSamples());

        midi.clear();
        plugin->processBlock(pluginBuffer, midi);
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
        foldStereoToMono(buffer, firstSample, numSamples);
    }

    const auto channelsToWrite = juce::jmin(numChannels, buffer.getNumChannels());
    return {buffer.getArrayOfWritePointers(), channelsToWrite, firstSample, numSamples};
}

/// Returns true when every sample of the block is a finite number.
/// An unstable plugin can produce infinity or NaN,
/// which must be rejected before converting the rendered audio to integer PCM.
bool isBlockFinite(const juce::AudioBuffer<float>& block, const int numSamples)
{
    for (int channel = 0; channel < block.getNumChannels(); ++channel) {
        const std::span samples(block.getReadPointer(channel), static_cast<std::size_t>(numSamples));

        if (!std::ranges::all_of(samples, [](const float sample) { return std::isfinite(sample); })) {
            return false;
        }
    }

    return true;
}

/// Updates the last audible sample position with a block about to be written.
/// The block must only hold finite samples.
static void measureOutputBlock(const juce::AudioBuffer<float>& block, const int numSamples, RenderSummary& summary)
{
    for (int sample = numSamples - 1; sample >= 0; --sample) {
        for (int channel = 0; channel < block.getNumChannels(); ++channel) {
            if (std::abs(block.getSample(channel, sample)) > silenceThreshold) {
                summary.lastAudibleSample = summary.samplesWritten + sample;
                return;
            }
        }
    }
}

/// Streams the whole file through every plugin in chain order
/// and writes the processed audio, unquantized, to the intermediate writer.
/// The chain latency and tail are rendered on top of the file length,
/// and the first `latencySamples` output samples are discarded, so the processed audio lines up with the input
/// and its end is not cut off. Read failures follow the shared DecodeFailureTracker rules.
/// The rendered fraction, including the latency and tail, is reported to onProgress after each block.
/// Returns the length measurements of the written audio, or an error message.
static std::expected<RenderSummary, juce::String> renderThroughChain(
    juce::AudioFormatReader& reader,
    juce::AudioFormatWriter& writer,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const plugin_processing::ChainPreparation& preparation,
    const int numChannels,
    const float inputGain,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    const auto latencySamples = preparation.latencySamples;
    RenderSummary summary;

    juce::AudioBuffer<float> buffer(preparation.processChannelCount, plugin_processing::processingBlockSize);
    juce::AudioBuffer<float> readBuffer(numChannels, plugin_processing::processingBlockSize);
    juce::MidiBuffer midi;
    std::vector<float*> readChannelPointers(static_cast<std::size_t>(numChannels));
    DecodeFailureTracker failureTracker(reader.lengthInSamples);

    const auto totalSamples = reader.lengthInSamples + latencySamples + preparation.tailSamples;

    for (juce::int64 samplePosition = 0; samplePosition < totalSamples;
         samplePosition += plugin_processing::processingBlockSize)
    {
        if (utils::isCancellationRequested(isCancelled)) {
            return std::unexpected(juce::String(cancelledMessage));
        }

        const auto samplesThisBlock = static_cast<int>(
            juce::jmin<juce::int64>(plugin_processing::processingBlockSize, totalSamples - samplePosition)
        );

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
                return std::unexpected(failureTracker.getErrorMessage());
            }
        }

        const auto signalChannels = processBlockThroughChain(buffer, chainInstances, numChannels, midi);
        utils::reportProgress(onProgress, utils::progressFraction(samplePosition + samplesThisBlock, totalSamples));

        // Skip the output samples that still fall within the chain latency.
        const auto firstSampleToWrite
            = static_cast<int>(juce::jlimit<juce::int64>(0, samplesThisBlock, latencySamples - samplePosition));
        const auto samplesToWrite = samplesThisBlock - firstSampleToWrite;

        if (samplesToWrite <= 0) {
            continue;
        }

        const auto writeView
            = prepareOutputView(buffer, numChannels, signalChannels, firstSampleToWrite, samplesToWrite);

        if (!isBlockFinite(writeView, samplesToWrite)) {
            return std::unexpected(juce::String("The plugin chain produced invalid samples (infinity or NaN)"));
        }

        measureOutputBlock(writeView, samplesToWrite, summary);

        if (!writer.writeFromAudioSampleBuffer(writeView, 0, samplesToWrite)) {
            return std::unexpected(juce::String("Failed while writing processed audio data"));
        }

        summary.samplesWritten += samplesToWrite;
    }

    summary.decodeNotice = failureTracker.getNotice();
    return summary;
}

/// Returns the number of samples to keep from the rendered output.
/// Trailing silence from the tail allowance is dropped, but the output is never shorter than the source.
static juce::int64 getFinalLength(const RenderSummary& summary, const juce::int64 sourceLength)
{
    return juce::jlimit<juce::int64>(
        0, summary.samplesWritten, juce::jmax(sourceLength, summary.lastAudibleSample + 1)
    );
}

/// Copies the rendered intermediate audio into the final writer, trimming the trailing silence.
/// Low bits are discarded without dither and out-of-range samples saturate without changing gain.
/// Explicit quantization avoids the float writer's full-scale scaling changing exact 16-bit sample values.
/// The written fraction is reported to onProgress after each block.
/// Returns whether a sample was saturated during this final PCM conversion, or an error message.
static std::expected<WriteSummary, juce::String> writeFinalOutput(
    juce::AudioFormatReader& renderedReader,
    juce::AudioFormatWriter& writer,
    const juce::int64 finalLength,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    WriteSummary summary;
    juce::AudioBuffer<float> block(static_cast<int>(renderedReader.numChannels), finalizeBlockSize);
    juce::AudioBuffer<int> quantizedBlock(block.getNumChannels(), finalizeBlockSize);
    std::vector<const int*> outputChannels(static_cast<std::size_t>(block.getNumChannels()) + 1, nullptr);
    for (int channel = 0; channel < block.getNumChannels(); ++channel) {
        outputChannels[static_cast<std::size_t>(channel)] = quantizedBlock.getReadPointer(channel);
    }

    for (juce::int64 samplePosition = 0; samplePosition < finalLength; samplePosition += finalizeBlockSize) {
        if (utils::isCancellationRequested(isCancelled)) {
            return std::unexpected(juce::String(cancelledMessage));
        }

        const auto samplesThisBlock
            = static_cast<int>(juce::jmin<juce::int64>(finalizeBlockSize, finalLength - samplePosition));

        if (!renderedReader.read(&block, 0, samplesThisBlock, samplePosition, true, true)) {
            return std::unexpected(juce::String("Failed while reading the rendered audio"));
        }

        for (int channel = 0; channel < block.getNumChannels(); ++channel) {
            const std::span<const float> input(
                block.getReadPointer(channel), static_cast<std::size_t>(samplesThisBlock)
            );
            const std::span<int> output(
                quantizedBlock.getWritePointer(channel), static_cast<std::size_t>(samplesThisBlock)
            );
            for (std::size_t sample = 0; sample < input.size(); ++sample) {
                constexpr float minimumPcmValue {-static_cast<float>(outputPcmScale)};
                constexpr float maximumPcmValue {static_cast<float>(outputPcmScale - 1)};
                const auto scaled = input[sample] * static_cast<float>(outputPcmScale);
                summary.clipped = summary.clipped || scaled < minimumPcmValue || scaled > maximumPcmValue;
                const auto limited = juce::jlimit(minimumPcmValue, maximumPcmValue, scaled);
                output[sample] = static_cast<int>(std::floor(limited)) * outputPcmAlignment;
            }
        }

        if (!writer.write(outputChannels.data(), samplesThisBlock)) {
            return std::unexpected(juce::String("Failed while writing processed audio data"));
        }

        utils::reportProgress(onProgress, utils::progressFraction(samplePosition + samplesThisBlock, finalLength));
    }

    return summary;
}

/// Returns the record unchanged when the file still matches it on disk,
/// otherwise analyzes the file again, so the input gain is never computed from an outdated peak.
/// The custom gain is dropped for a changed file, since it was set for the previous version of the file.
/// The analysis stops when isCancelled returns true, and the returned record then carries the cancellation error.
/// The analysis progress is reported to onProgress.
static AudioAnalysisRecord refreshIfChanged(
    const AudioAnalysisRecord& record,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    return record.matchesFileOnDisk() ? record : analyzeFile(record.file, isCancelled, onProgress);
}

/// Prepares the chain, renders the whole file through it into an unquantized 32-bit float WAV file,
/// and releases the chain again.
/// Plugin setup and release run on the message thread as VST3 requires, and only processBlock runs on this thread.
/// The render progress is reported to onProgress.
/// Returns the length measurements of the rendered audio, or an error message.
static std::expected<RenderSummary, juce::String> renderChainToFile(
    juce::AudioFormatReader& reader,
    const juce::File& intermediateFile,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const PluginProcessingOptions& options,
    const int numChannels,
    const float inputGain,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::OutputStream> outputStream(intermediateFile.createOutputStream().release());

    if (outputStream == nullptr) {
        return std::unexpected(juce::String("Could not create temporary output file"));
    }

    const auto writerOptions = juce::AudioFormatWriterOptions()
                                   .withSampleRate(reader.sampleRate)
                                   .withNumChannels(numChannels)
                                   .withBitsPerSample(32)
                                   .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    const auto writer = wavFormat.createWriterFor(outputStream, writerOptions);

    if (writer == nullptr) {
        return std::unexpected(juce::String("Could not create the temporary render file"));
    }

    plugin_processing::ChainPreparation preparation;
    const bool prepared = utils::callOnMessageThread(
        [&] { preparation = plugin_processing::prepareChain(chainInstances, options, numChannels, reader.sampleRate); },
        isCancelled
    );

    if (!prepared || preparation.errorMessage.isNotEmpty()) {
        return std::unexpected(prepared ? preparation.errorMessage : juce::String(cancelledMessage));
    }

    if (preparation.latencySamples > 0) {
        utils::logDebug("Compensating {} samples of plugin chain latency", preparation.latencySamples);
    }

    auto summary = renderThroughChain(
        reader, *writer, chainInstances, preparation, numChannels, inputGain, isCancelled, onProgress
    );

    // Release on every path so a failed file does not leave the chain active for the next one.
    // After cancellation the message thread may be blocked waiting for this worker, so the release is skipped,
    // and the plugin instances release their resources when they are destroyed instead.
    static_cast<void>(utils::callOnMessageThread(
        [&chainInstances] { plugin_processing::releasePluginResources(chainInstances); }, isCancelled
    ));

    return summary;
}

/// Writes the trimmed intermediate audio as 16-bit AIFF without additional gain adjustment or dither.
/// The write progress is reported to onProgress.
/// Returns whether the final PCM conversion saturated samples, or an error message.
static std::expected<WriteSummary, juce::String> writeProcessedAiff(
    juce::AudioFormat& aiffFormat,
    const juce::File& intermediateFile,
    const juce::File& outputFile,
    const juce::AudioFormatReader& sourceReader,
    const juce::int64 finalLength,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    juce::WavAudioFormat wavFormat;
    auto renderedStream = intermediateFile.createInputStream();

    if (renderedStream == nullptr) {
        return std::unexpected(juce::String("Could not open the rendered audio"));
    }

    const std::unique_ptr<juce::AudioFormatReader> renderedReader(
        wavFormat.createReaderFor(renderedStream.release(), true)
    );

    if (renderedReader == nullptr) {
        return std::unexpected(juce::String("Could not read the rendered audio"));
    }

    std::unique_ptr<juce::OutputStream> outputStream(outputFile.createOutputStream().release());

    if (outputStream == nullptr) {
        return std::unexpected(juce::String("Could not create temporary output file"));
    }

    const auto writerOptions = juce::AudioFormatWriterOptions()
                                   .withSampleRate(sourceReader.sampleRate)
                                   .withNumChannels(static_cast<int>(renderedReader->numChannels))
                                   .withBitsPerSample(outputBitsPerSample)
                                   .withMetadataValues(utils::aiffWriterMetadata(sourceReader.metadataValues));
    const auto writer = aiffFormat.createWriterFor(outputStream, writerOptions);

    if (writer == nullptr) {
        return std::unexpected(juce::String("Could not create AIFF writer for output"));
    }

    return writeFinalOutput(*renderedReader, *writer, finalLength, isCancelled, onProgress);
}

/// Processes a single file through the given plugin instances in order.
/// The instances must align index-for-index with options.plugins.
/// The function restores state on each plugin, prepares it in offline mode, and resets it
/// before processing, compensates for the chain's total reported latency, and releases the plugins'
/// resources afterwards, also when processing fails. Intended to run on a worker thread.
/// Plugin setup and release are marshalled to the message thread,
/// because VST3 restricts bus arrangement, setupProcessing, setActive, and setState to the UI thread,
/// so only processBlock runs on the calling thread.
/// The message loop must keep running while a worker is inside this call, unless isCancelled starts returning true:
/// a worker waiting for the message thread then gives up instead of blocking,
/// so the message thread may wait for workers after requesting cancellation without deadlocking.
/// A call cancelled before its output is committed returns a failure result and leaves the original file untouched.
/// It may leave the chain prepared, in which case the plugin instances release their resources when destroyed.
/// The caller owns the plugin instances and is responsible for thread-safety:
/// a chain of instances must only be in use by one thread at a time.
/// A mono file running through a stereo chain feeds both plugin inputs and is written back as mono,
/// using the average of both output channels. The chain renders into an unquantized intermediate file first.
/// Rendered samples outside the 16-bit PCM range saturate without gain reduction,
/// and clipping is reported in the result notice.
/// Isolated source blocks that fail to decode are processed as decoded and reported in the notice too.
/// Trailing silence from the tail allowance is trimmed.
/// Output is always 16-bit PCM without additional dithering or post-chain gain adjustment.
/// When the file changed on disk since sourceRecord was analyzed, it is analyzed again first.
/// An `.aif` source is rewritten in place after a copy of it is disposed of through options.disposeFile.
/// Progress of the whole file, through any source analysis, the render, the final write, and the output analysis,
/// is reported to onProgress.
PluginProcessingResult processFileWithPlugins(
    const AudioAnalysisRecord& sourceRecord,
    const PluginProcessingOptions& options,
    const std::vector<juce::AudioPluginInstance*>& chainInstances,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress
)
{
    const auto startedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
    const auto& file = sourceRecord.file;

    if (const auto inputs = validateProcessingInputs(sourceRecord, options, chainInstances); !inputs) {
        return fail(file, inputs.error());
    }

    const auto renderProgressStart = sourceRecord.matchesFileOnDisk() ? 0.0f : sourceAnalysisProgressShare;
    const auto record
        = refreshIfChanged(sourceRecord, isCancelled, utils::makeProgressRange(onProgress, 0.0f, renderProgressStart));

    if (record.hasError()) {
        return fail(file, record.errorMessage);
    }

    auto& formatManager = utils::threadLocalFormatManager();
    const auto reader = utils::createAudioReader(file);

    if (reader == nullptr) {
        return fail(file, "Unsupported or unreadable audio file");
    }

    auto* aiffFormat = utils::findAiffFormat(formatManager);
    if (aiffFormat == nullptr) {
        return fail(file, "AIFF writer not available in this build");
    }

    const auto outputFile = getProcessingOutputFile(file);

    if (const auto outputCheck = utils::checkOutputPath(file, outputFile, options.overwriteExisting); !outputCheck) {
        return fail(file, outputCheck.error());
    }

    if (reader->numChannels < 1 || reader->numChannels > maximumProcessingChannels) {
        return fail(
            file,
            utils::format("Only mono and stereo files can be processed, this file has {} channels", reader->numChannels)
        );
    }

    const auto numChannels = static_cast<int>(reader->numChannels);
    const juce::TemporaryFile intermediateFile(".wav");

    const auto summary = renderChainToFile(
        *reader,
        intermediateFile.getFile(),
        chainInstances,
        options,
        numChannels,
        getInputGain(record, options),
        isCancelled,
        utils::makeProgressRange(onProgress, renderProgressStart, renderProgressEnd)
    );

    if (!summary) {
        return fail(file, summary.error());
    }

    // The hidden name keeps a half-written output out of folder scans,
    // and out of later scans after a crash.
    const juce::TemporaryFile temporaryFile(outputFile, juce::TemporaryFile::useHiddenFile);
    const auto finalLength = getFinalLength(*summary, reader->lengthInSamples);

    const auto written = writeProcessedAiff(
        *aiffFormat,
        intermediateFile.getFile(),
        temporaryFile.getFile(),
        *reader,
        finalLength,
        isCancelled,
        utils::makeProgressRange(onProgress, renderProgressEnd, writeProgressEnd)
    );

    if (!written) {
        return fail(file, written.error());
    }

    // Carries ID3 tags, embedded artwork, and similar information across formats
    // such as MP3 to AIFF.
    if (!copyMetadata(file, temporaryFile.getFile())) {
        return fail(file, "Could not copy metadata onto the processed output");
    }

    if (const auto validation = utils::validateWrittenAudio(
            formatManager, temporaryFile.getFile(), numChannels, reader->sampleRate, finalLength
        );
        !validation)
    {
        return fail(file, utils::format("Processed output failed validation: {}", validation.error()));
    }

    // Check again before touching the original, since cancellation may arrive during the last render block.
    // The coordinator drops results of a cancelled run, so committing here would change files on disk
    // without the caller ever learning about it.
    if (utils::isCancellationRequested(isCancelled)) {
        return fail(file, cancelledMessage);
    }

    // Processing cannot be repeated to get back to the original,
    // so an original rewritten in place keeps a copy in the trash like a converted original does.
    if (outputFile == file && !utils::disposeOfCopy(file, options.disposeFile)) {
        return fail(file, "Could not move a copy of the original file to the system trash");
    }

    if (const auto commit
        = utils::commitTemporaryOutput(temporaryFile, file, outputFile, options.overwriteExisting, options.disposeFile);
        !commit)
    {
        return fail(file, commit.error());
    }

    PluginProcessingResult result;
    result.originalFile = file;
    result.originalFullPath = file.getFullPathName();
    result.outputFile = outputFile;
    result.committed = true;

    juce::StringArray notices;

    if (written->clipped) {
        const juce::String clippingNotice {"Output clipping: final render exceeded the 16-bit PCM range"};
        notices.add(clippingNotice);
        utils::logWarn("Plugin processing warning: {}, {}", outputFile.getFullPathName().quoted(), clippingNotice);
    }

    if (summary->decodeNotice.isNotEmpty()) {
        notices.add(summary->decodeNotice);
        utils::logWarn(
            "Plugin processing warning: {}, {}", outputFile.getFullPathName().quoted(), summary->decodeNotice
        );
    }

    result.notice = notices.joinIntoString(". ");

    result.fileName = outputFile.getFileName();
    result.analysisRecord = analyzeFile(outputFile, {}, utils::makeProgressRange(onProgress, writeProgressEnd, 1.0f));
    // Custom gain has been baked in. Clear it on the new record.
    result.analysisRecord.customGainDb = 0.0f;
    result.analysisRecord.hasCustomGain = false;
    result.succeeded = !result.analysisRecord.hasError();

    if (!result.succeeded) {
        result.errorMessage
            = utils::format("The file was processed, but re-analysis failed: {}", result.analysisRecord.errorMessage);
        utils::logError("Plugin processing re-analysis failed for {}", result.outputFile.getFullPathName().quoted());
    } else {
        utils::logInfo(
            "Processed: {:.2f} s, {}",
            utils::elapsedSecondsSince(startedAtMilliseconds),
            result.outputFile.getFileName().quoted()
        );
    }

    return result;
}
}  // namespace audiobatch
