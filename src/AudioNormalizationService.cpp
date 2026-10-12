/// Implementation of AudioNormalizationService.
/// Reads source files through per-thread JUCE format readers, applies gain in chunks,
/// and writes peak-normalized AIFF output.
/// Carries source tags and embedded art across through MetadataService,
/// and reports which formats this build can read and rewrite during normalization.

#include "AudioNormalizationService.h"

#include "AudioFormatUtils.h"
#include "DecodeFailureTracker.h"
#include "FileUtils.h"
#include "Log.h"
#include "MetadataService.h"
#include "StringFormat.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

/// Helpers for format lookup, metadata passthrough, and chunked normalization.
namespace audiobatch::normalization
{
/// Logs the failure and builds a failure result for the given file.
static AudioNormalizationResult failNormalization(const juce::File& file, const juce::String& message)
{
    utils::logError("Normalization failed for {}: {}", file.getFullPathName().quoted(), message);
    return AudioNormalizationResult::failure(file, message);
}

constexpr int normalizationBlockSize = 32768;
constexpr auto normalizedAiffOutputExtension = ".aif";
constexpr int defaultMp3BitsPerSample = 16;
constexpr auto cancelledMessage = "Normalization was cancelled";

/// Picks a bit depth the writer format supports, preferring the requested depth.
/// Falls back to the format's highest supported depth so no precision is lost.
static int resolveWriterBitDepth(juce::AudioFormat& format, const int preferredBitsPerSample)
{
    const auto bitDepths = format.getPossibleBitDepths();

    if (bitDepths.isEmpty() || preferredBitsPerSample <= 0) {
        return preferredBitsPerSample;
    }

    if (bitDepths.contains(preferredBitsPerSample)) {
        return preferredBitsPerSample;
    }

    int resolvedBitsPerSample = bitDepths[0];

    for (const auto bitDepth : bitDepths) {
        resolvedBitsPerSample = std::max(bitDepth, resolvedBitsPerSample);
    }

    return resolvedBitsPerSample;
}

/// Chooses the output bit depth for the normalized file.
/// Prefers the analyzed source depth, then the MP3 default, then the reader's reported depth.
static int resolvePreferredOutputBitDepth(
    const AudioAnalysisRecord& sourceRecord,
    const juce::AudioFormatReader& reader,
    const juce::File& sourceFile
)
{
    if (sourceRecord.bitsPerSample > 0) {
        return sourceRecord.bitsPerSample;
    }

    if (utils::isMp3File(sourceFile)) {
        return defaultMp3BitsPerSample;
    }

    return static_cast<int>(reader.bitsPerSample);
}

/// Copies tags and embedded pictures from the source file to the freshly written output.
/// A source that yields no metadata is not an error, but a failed write to the destination is.
static bool preserveOutputMetadata(const juce::File& sourceFile, const juce::File& destinationFile)
{
    MetadataService::Metadata metadata;

    if (!MetadataService::readMetadata(sourceFile, metadata)) {
        // Reading failed, for example due to an unsupported format.
        // This is non-fatal, so keep going without metadata.
        utils::logInfo("No metadata could be read from {}", sourceFile.getFullPathName().quoted());
        return true;
    }

    if (metadata.isEmpty()) {
        return true;
    }

    if (!MetadataService::writeMetadata(destinationFile, metadata)) {
        utils::logError("Failed to write metadata to {}", destinationFile.getFullPathName().quoted());
        return false;
    }

    return true;
}

/// Verifies that the temporary output file exists and decodes to a non-empty audio stream.
/// Returns an empty string on success, or a user-facing error message.
static juce::String validateTemporaryNormalizedOutput(
    juce::AudioFormatManager& formatManager,
    const juce::File& temporaryOutputFile
)
{
    if (!temporaryOutputFile.existsAsFile() || temporaryOutputFile.getSize() <= 0) {
        return "Normalization failed to produce a valid output file. The original file was left unchanged.";
    }

    if (const std::unique_ptr<juce::AudioFormatReader> encodedReader(
            formatManager.createReaderFor(temporaryOutputFile)
        );
        encodedReader == nullptr || encodedReader->lengthInSamples <= 0)
    {
        return "Normalization failed before the output file could be verified. The original file was left unchanged.";
    }

    return {};
}

/// Streams the source through the writer block by block, applying the normalization gain.
/// Read failures follow the shared DecodeFailureTracker rules,
/// and `decodeNotice` receives the tracker's notice about tolerated failures.
/// Returns an empty string on success, or a user-facing error message,
/// including when cancellation is requested between blocks.
static juce::String renderNormalizedAudio(
    juce::AudioFormatReader& reader,
    juce::AudioFormatWriter& writer,
    const float gain,
    const AudioNormalizationService::CancellationCheck& isCancelled,
    juce::String& decodeNotice
)
{
    juce::AudioBuffer<float> buffer(static_cast<int>(reader.numChannels), normalizationBlockSize);
    std::vector<float*> channelPointers(reader.numChannels);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        channelPointers[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);
    }

    DecodeFailureTracker failureTracker(reader.lengthInSamples);

    for (juce::int64 samplePosition = 0; samplePosition < reader.lengthInSamples;
         samplePosition += normalizationBlockSize)
    {
        if (utils::isCancellationRequested(isCancelled)) {
            return cancelledMessage;
        }

        const auto samplesThisBlock = static_cast<int>(
            juce::jmin<juce::int64>(normalizationBlockSize, reader.lengthInSamples - samplePosition)
        );

        // A failed read leaves the destination untouched, so without clearing, a tolerated failure would write
        // the previous, already gained block again.
        // The samples are zeroed through the channel pointers, because AudioBuffer::clear marks the whole buffer as
        // clear and applyGain would then skip the samples the reader writes through those pointers.
        for (auto* const channelPointer : channelPointers) {
            juce::FloatVectorOperations::clear(channelPointer, samplesThisBlock);
        }

        const auto readSucceeded
            = reader.read(channelPointers.data(), buffer.getNumChannels(), samplePosition, samplesThisBlock);

        if (!failureTracker.recordBlock(readSucceeded, samplePosition, samplesThisBlock)) {
            return failureTracker.getErrorMessage();
        }

        buffer.applyGain(gain);

        if (!writer.writeFromAudioSampleBuffer(buffer, 0, samplesThisBlock)) {
            return "Failed while writing normalized audio data";
        }
    }

    decodeNotice = failureTracker.getNotice();
    return {};
}
}  // namespace audiobatch::normalization

using namespace audiobatch::normalization;

juce::File AudioNormalizationService::getOutputFile(const juce::File& sourceFile)
{
    return sourceFile.getSiblingFile(sourceFile.getFileNameWithoutExtension() + normalizedAiffOutputExtension);
}

juce::StringArray AudioNormalizationService::findBatchOutputConflicts(const std::vector<AudioAnalysisRecord>& records)
{
    std::vector<const AudioAnalysisRecord*> ordered;
    ordered.reserve(records.size());

    for (const auto& record : records) {
        ordered.push_back(&record);
    }

    // In-place sources claim their output first, then the rest in path order.
    std::ranges::sort(ordered, [](const AudioAnalysisRecord* lhs, const AudioAnalysisRecord* rhs) {
        const bool lhsInPlace = getOutputFile(lhs->file) == lhs->file;
        const bool rhsInPlace = getOutputFile(rhs->file) == rhs->file;

        if (lhsInPlace != rhsInPlace) {
            return lhsInPlace;
        }

        return lhs->file.getFullPathName() < rhs->file.getFullPathName();
    });

    juce::StringArray claimedOutputs;
    juce::StringArray conflicts;

    for (const auto* record : ordered) {
        const auto outputPath = getOutputFile(record->file).getFullPathName();

        if (claimedOutputs.contains(outputPath, !juce::File::areFileNamesCaseSensitive())) {
            conflicts.add(record->file.getFullPathName());
        } else {
            claimedOutputs.add(outputPath);
        }
    }

    return conflicts;
}

std::optional<float> AudioNormalizationService::getNormalizationGain(
    const AudioAnalysisRecord& record,
    const AudioNormalizationOptions& options,
    juce::String& errorMessage
)
{
    const auto peak = static_cast<double>(utils::peakMagnitude(record.overallPeak));

    if (peak <= 0.0) {
        errorMessage = "File contains no signal that can be normalized";
        return std::nullopt;
    }

    if (options.mode == AudioNormalizationMode::peak) {
        return static_cast<float>(juce::Decibels::decibelsToGain(options.targetPeakDbfs) / peak);
    }

    if (record.integratedLufs <= AudioAnalysisRecord::negativeInfinityLoudness) {
        errorMessage = "File has no measurable loudness to normalize";
        return std::nullopt;
    }

    auto gainDecibels = options.targetLufs - record.integratedLufs;

    // The true peak scales with the gain, so the ceiling caps how far the file can be raised.
    if (const auto truePeak = juce::jmax(record.overallTruePeak, peak); truePeak > 0.0) {
        gainDecibels = juce::jmin(gainDecibels, options.truePeakCeilingDbtp - juce::Decibels::gainToDecibels(truePeak));
    }

    return static_cast<float>(juce::Decibels::decibelsToGain(gainDecibels));
}

int AudioNormalizationService::getOutputBitDepth(
    const AudioAnalysisRecord& record,
    const juce::AudioFormatReader& reader
)
{
    auto* writerFormat = utils::findAiffFormat(utils::threadLocalFormatManager());
    const auto preferredBitDepth = resolvePreferredOutputBitDepth(record, reader, record.file);
    return writerFormat != nullptr ? resolveWriterBitDepth(*writerFormat, preferredBitDepth) : preferredBitDepth;
}

juce::String AudioNormalizationService::batchOutputConflictMessage(const juce::File& sourceFile)
{
    return utils::format("Another selected file is also written to {}", getOutputFile(sourceFile).getFileName());
}

AudioNormalizationResult AudioNormalizationService::normalizeFile(
    const AudioAnalysisRecord& sourceRecord,
    const AudioNormalizationOptions& options,
    const CancellationCheck& isCancelled
)
{
    const auto& file = sourceRecord.file;
    const auto outputFile = getOutputFile(file);

    if (!file.existsAsFile()) {
        return failNormalization(file, "File does not exist");
    }

    if (outputFile != file && outputFile.exists() && !options.overwriteExisting) {
        return failNormalization(file, utils::existingOutputMessage(outputFile));
    }

    if (!sourceRecord.isReady()) {
        return failNormalization(file, "File analysis must finish before normalization");
    }

    // The gain comes from the analyzed peak, so a file that changed since its analysis is measured again first.
    const auto record = sourceRecord.matchesFileOnDisk() ? sourceRecord : AudioAnalysisService::analyzeFile(file);

    if (record.hasError()) {
        return failNormalization(file, record.errorMessage);
    }

    auto& formatManager = utils::threadLocalFormatManager();
    auto* writerFormat = utils::findAiffFormat(utils::threadLocalFormatManager());

    if (writerFormat == nullptr) {
        return failNormalization(file, "Unsupported audio format");
    }

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        return failNormalization(file, "Unsupported or unreadable audio file");
    }

    juce::String gainError;
    const auto normalizationGain = getNormalizationGain(record, options, gainError);

    if (!normalizationGain.has_value()) {
        return failNormalization(file, gainError);
    }

    const auto gain = *normalizationGain;

    if (juce::approximatelyEqual(gain, 1.0f) && outputFile == file) {
        AudioNormalizationResult result;
        result.file = file;
        result.fileName = file.getFileName();
        result.fullPath = file.getFullPathName();
        result.analysisRecord = AudioAnalysisService::analyzeFile(file);
        result.succeeded = !result.analysisRecord.hasError();

        if (!result.succeeded) {
            result.errorMessage = result.analysisRecord.errorMessage;
        }

        return result;
    }

    juce::TemporaryFile const temporaryFile(outputFile);
    std::unique_ptr<juce::OutputStream> outputStream(temporaryFile.getFile().createOutputStream().release());

    if (outputStream == nullptr) {
        return failNormalization(file, "Could not create temporary output file");
    }

    // The AIFF writer accepts any sample rate, and keeping the source rate avoids a pitch change,
    // since normalization does not resample.
    const auto writerSampleRate = reader->sampleRate;
    const auto writerBitDepth = getOutputBitDepth(record, *reader);

    // Tags and pictures are copied separately through TagLib after the audio is rendered,
    // and the reader metadata passed here only covers what the JUCE AIFF writer itself stores.
    const auto writerOptions = juce::AudioFormatWriterOptions()
                                   .withSampleRate(writerSampleRate)
                                   .withNumChannels(static_cast<int>(reader->numChannels))
                                   .withBitsPerSample(writerBitDepth)
                                   .withMetadataValues(utils::copyMetadata(reader->metadataValues));

    auto writer = writerFormat->createWriterFor(outputStream, writerOptions);

    if (writer == nullptr) {
        return failNormalization(file, getNormalizationSupportMessage(file));
    }

    juce::String decodeNotice;

    if (const auto renderError = renderNormalizedAudio(*reader, *writer, gain, isCancelled, decodeNotice);
        renderError.isNotEmpty())
    {
        return failNormalization(file, renderError);
    }

    writer.reset();

    if (!preserveOutputMetadata(file, temporaryFile.getFile())) {
        return failNormalization(file, "Could not preserve metadata while writing normalized audio");
    }

    if (const auto validationError = validateTemporaryNormalizedOutput(formatManager, temporaryFile.getFile());
        validationError.isNotEmpty())
    {
        return failNormalization(file, validationError);
    }

    // Check again before touching the original, since cancellation may arrive during the last block.
    // The coordinator drops results of a cancelled run,
    // so committing here would change files on disk without the caller ever learning about it.
    if (utils::isCancellationRequested(isCancelled)) {
        return failNormalization(file, cancelledMessage);
    }

    if (const auto commitError
        = utils::commitTemporaryOutput(temporaryFile, file, outputFile, options.overwriteExisting);
        commitError.isNotEmpty())
    {
        return failNormalization(file, commitError);
    }

    AudioNormalizationResult result;
    result.file = file;
    result.fileName = file.getFileName();
    result.fullPath = file.getFullPathName();
    result.notice = decodeNotice;
    result.analysisRecord = AudioAnalysisService::analyzeFile(outputFile);
    result.succeeded = !result.analysisRecord.hasError();

    if (result.notice.isNotEmpty()) {
        utils::logWarn("{}: {}", outputFile.getFullPathName().quoted(), result.notice);
    }

    if (!result.succeeded) {
        result.errorMessage
            = utils::format("The file was normalized, but re-analysis failed: {}", result.analysisRecord.errorMessage);
        utils::logError("Normalization failed for {}: {}", result.fullPath.quoted(), result.errorMessage);
    }

    return result;
}
