/// Implementation of AudioNormalizationService.
/// Reads source files through per-thread JUCE format readers, applies gain in chunks,
/// and writes peak-normalized AIFF output.
/// Carries source tags and embedded art across through MetadataService,
/// and reports which formats this build can read and rewrite during normalization.

#include "AudioNormalizationService.h"

#include "DecodeFailureTracker.h"
#include "FileUtils.h"
#include "Log.h"
#include "MetadataService.h"
#include "StringFormat.h"
#include <unordered_map>

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

/// Returns true when a cancellation check is set and reports that cancellation was requested.
static bool isCancellationRequested(const AudioNormalizationService::CancellationCheck& isCancelled)
{
    return isCancelled != nullptr && isCancelled();
}

struct AudioNormalizationRuntimeState {
    juce::AudioFormatManager readFormatManager;
};

struct AudioNormalizationFormatSupport {
    juce::String formatName;
    juce::StringArray fileExtensions;
    bool canNormalize = false;
    juce::String detail;
};

/// Returns the absolute magnitude of a signed peak sample value.
static float peakMagnitude(const float peak)
{
    return std::abs(peak);
}

/// Copies JUCE reader metadata into the map type expected by juce::AudioFormatWriterOptions.
static std::unordered_map<juce::String, juce::String> copyMetadata(const juce::StringPairArray& metadata)
{
    std::unordered_map<juce::String, juce::String> copiedMetadata;
    const auto& keys = metadata.getAllKeys();
    const auto& values = metadata.getAllValues();

    for (int index = 0; index < metadata.size(); ++index) {
        copiedMetadata[keys[index]] = values[index];
    }

    return copiedMetadata;
}

/// Returns the file's extension without the leading dot.
static juce::String normalizedExtension(const juce::File& file)
{
    auto extension = file.getFileExtension();

    if (extension.startsWithChar('.')) {
        extension = extension.substring(1);
    }

    return extension;
}

/// Returns the extension text trimmed and without the leading dot.
static juce::String normalizedExtension(const juce::String& extension)
{
    auto value = extension.trim();

    if (value.startsWithChar('.')) {
        value = value.substring(1);
    }

    return value;
}

/// Returns true when the extension identifies an MP3 file, ignoring case.
static bool isMp3Extension(const juce::String& extension)
{
    return normalizedExtension(extension).equalsIgnoreCase("mp3");
}

/// Returns true when the file's extension identifies an MP3 source.
static bool isMp3SourceFile(const juce::File& file)
{
    return isMp3Extension(normalizedExtension(file));
}

/// Finds the AIFF format used for writing output, or nullptr when it is unavailable in this build.
static juce::AudioFormat* getAiffWriterFormat(const AudioNormalizationRuntimeState& runtimeState)
{
    if (auto* format = runtimeState.readFormatManager.findFormatForFileExtension("aif"); format != nullptr) {
        return format;
    }

    return runtimeState.readFormatManager.findFormatForFileExtension("aiff");
}

/// Moves the temporary output into its final location and disposes of the original file.
/// In-place AIFF output simply replaces the source.
/// For other formats the original is moved to the system trash after the new file is written,
/// and the new file is deleted again if trashing fails so no duplicate is left behind.
static bool finalizeNormalizationOutput(
    const juce::TemporaryFile& temporaryFile,
    const juce::File& sourceFile,
    const juce::File& outputFile,
    juce::String& errorMessage
)
{
    if (outputFile == sourceFile) {
        if (!temporaryFile.overwriteTargetFileWithTemporary()) {
            errorMessage = "Could not replace the original file with normalized audio";
            return false;
        }

        return true;
    }

    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        errorMessage = "Could not create the normalized AIF file";
        return false;
    }

    if (sourceFile.existsAsFile() && !utils::moveToTrash(sourceFile)) {
        utils::deleteFile(outputFile);
        errorMessage = "Could not move the original audio file to the system trash";
        return false;
    }

    return true;
}

/// Returns the one-line summary of normalization output behavior used in the support summary.
static juce::String getNormalizationStatusLine()
{
    return "Normalization output: same-name AIF files. "
           "Originals with a different extension are moved to the system trash.";
}

/// Builds minimal writer options from the format's advertised capabilities.
/// Returns default constructed options when the format advertises no usable configuration.
static juce::AudioFormatWriterOptions buildProbeWriterOptions(juce::AudioFormat& format)
{
    const auto sampleRates = format.getPossibleSampleRates();
    const auto bitDepths = format.getPossibleBitDepths();
    int numChannels = 0;
    if (format.canDoStereo()) {
        numChannels = 2;
    } else if (format.canDoMono()) {
        numChannels = 1;
    }

    if (sampleRates.isEmpty() || bitDepths.isEmpty() || numChannels <= 0) {
        return {};
    }

    return juce::AudioFormatWriterOptions()
        .withSampleRate(sampleRates[0])
        .withNumChannels(numChannels)
        .withBitsPerSample(bitDepths[0]);
}

/// Checks whether the format can actually create a writer by probing with an in-memory stream.
/// Some formats are read-only in JUCE even though they are registered with the format manager.
static bool canCreateProbeWriter(juce::AudioFormat& format)
{
    const auto options = buildProbeWriterOptions(format);

    if (options.getSampleRate() <= 0.0 || options.getNumChannels() <= 0 || options.getBitsPerSample() <= 0) {
        return false;
    }

    std::unique_ptr<juce::OutputStream> outputStream = std::make_unique<juce::MemoryOutputStream>();
    const auto writer = format.createWriterFor(outputStream, options);
    return writer != nullptr;
}

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

    if (isMp3SourceFile(sourceFile)) {
        return defaultMp3BitsPerSample;
    }

    return static_cast<int>(reader.bitsPerSample);
}

/// Returns the per-thread runtime state with a format manager that has the basic formats registered.
/// Thread-local state avoids locking when normalize jobs run on multiple worker threads.
static AudioNormalizationRuntimeState& getThreadLocalRuntimeState()
{
    thread_local auto runtimeState = [] {
        auto initializedState = std::make_unique<AudioNormalizationRuntimeState>();
        initializedState->readFormatManager.registerBasicFormats();
        return initializedState;
    }();

    return *runtimeState;
}

/// Describes what normalization does with files of the given format for the support summary.
static juce::String getNormalizableFormatDetail(const juce::StringArray& extensions)
{
    const auto primaryExtension = extensions.isEmpty() ? juce::String {} : normalizedExtension(extensions[0]);

    if (primaryExtension.equalsIgnoreCase("aif")) {
        // Output path matches the source path, so the existing file is rewritten in place.
        return "Rewritten in place as AIFF.";
    }

    if (isMp3Extension(primaryExtension)) {
        return "Converted to a same-name AIFF file. The original MP3 is moved to the system trash.";
    }

    return "Converted to a same-name AIFF file. The original is moved to the system trash.";
}

/// Gathers per-format normalization support information for the support summary.
/// Normalizable formats sort before read-only ones, then alphabetically by name.
static std::vector<AudioNormalizationFormatSupport> collectFormatSupport(
    AudioNormalizationRuntimeState const& runtimeState
)
{
    std::vector<AudioNormalizationFormatSupport> support;
    support.reserve(static_cast<std::size_t>(runtimeState.readFormatManager.getNumKnownFormats()));

    // Normalization always writes AIFF output, so any readable format is normalizable
    // as long as an AIFF writer is available in this build.
    auto* aiffWriterFormat = getAiffWriterFormat(runtimeState);
    const bool aiffWriterAvailable = aiffWriterFormat != nullptr && canCreateProbeWriter(*aiffWriterFormat);

    for (int index = 0; index < runtimeState.readFormatManager.getNumKnownFormats(); ++index) {
        const auto* format = runtimeState.readFormatManager.getKnownFormat(index);

        if (format == nullptr) {
            continue;
        }

        AudioNormalizationFormatSupport entry;
        entry.formatName = format->getFormatName();
        entry.fileExtensions = format->getFileExtensions();
        entry.canNormalize = aiffWriterAvailable && !entry.fileExtensions.isEmpty();

        if (entry.canNormalize) {
            entry.detail = getNormalizableFormatDetail(entry.fileExtensions);
        } else {
            entry.detail = "AIFF writer is not available in this build, so this format cannot be normalized.";
        }

        support.push_back(std::move(entry));
    }

    std::ranges::sort(support, [](const auto& lhs, const auto& rhs) {
        if (lhs.canNormalize != rhs.canNormalize) {
            return lhs.canNormalize > rhs.canNormalize;
        }

        return lhs.formatName.compareNatural(rhs.formatName) < 0;
    });

    return support;
}

/// Joins the format's extensions into a lowercase, de-duplicated display string.
static juce::String formatExtensionsToText(const juce::StringArray& extensions)
{
    juce::StringArray normalizedExtensions;

    for (const auto& extension : extensions) {
        normalizedExtensions.addIfNotAlreadyThere(extension.toLowerCase());
    }

    return normalizedExtensions.joinIntoString(", ");
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

/// Returns the user-facing error for an output path that is already taken by another file.
static juce::String existingOutputMessage(const juce::File& outputFile)
{
    return utils::format("Output file {} already exists. Use force overwrite to replace it.", outputFile.getFileName());
}

/// Makes room for the output when it would replace a different, existing file.
/// The existing file is moved to the system trash only when overwriting is allowed.
/// Returns an empty string on success, or a user-facing error message.
static juce::String clearExistingOutput(
    const juce::File& sourceFile,
    const juce::File& outputFile,
    const AudioNormalizationOptions& options
)
{
    if (outputFile == sourceFile || !outputFile.exists()) {
        return {};
    }

    if (!options.overwriteExisting) {
        return existingOutputMessage(outputFile);
    }

    if (!utils::moveToTrash(outputFile)) {
        return "Could not move the existing output file to the system trash";
    }

    return {};
}

/// Streams the source through the writer block by block, applying the normalization gain.
/// Read failures follow the shared DecodeFailureTracker rules.
/// Returns an empty string on success, or a user-facing error message,
/// including when cancellation is requested between blocks.
static juce::String renderNormalizedAudio(
    juce::AudioFormatReader& reader,
    juce::AudioFormatWriter& writer,
    const float gain,
    const AudioNormalizationService::CancellationCheck& isCancelled
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
        if (isCancellationRequested(isCancelled)) {
            return cancelledMessage;
        }

        const auto samplesThisBlock = static_cast<int>(
            juce::jmin<juce::int64>(normalizationBlockSize, reader.lengthInSamples - samplePosition)
        );

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

    if (failureTracker.getFailedBlockCount() > 0) {
        utils::logWarn(
            "{} audio block(s) failed to decode and were written as decoded so far, first at sample {}",
            failureTracker.getFailedBlockCount(),
            failureTracker.getFirstFailurePosition()
        );
    }

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
    const auto peak = static_cast<double>(peakMagnitude(record.overallPeak));

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
    auto* writerFormat = getAiffWriterFormat(getThreadLocalRuntimeState());
    const auto preferredBitDepth = resolvePreferredOutputBitDepth(record, reader, record.file);
    return writerFormat != nullptr ? resolveWriterBitDepth(*writerFormat, preferredBitDepth) : preferredBitDepth;
}

juce::String AudioNormalizationService::batchOutputConflictMessage(const juce::File& sourceFile)
{
    return utils::format("Another selected file is also written to {}", getOutputFile(sourceFile).getFileName());
}

bool AudioNormalizationService::canNormalizeFile(const juce::File& file)
{
    const auto& runtimeState = getThreadLocalRuntimeState();

    // Source must be readable by JUCE and AIFF output must be writable.
    const auto* readerFormat = runtimeState.readFormatManager.findFormatForFileExtension(normalizedExtension(file));

    if (readerFormat == nullptr) {
        return false;
    }

    return getAiffWriterFormat(runtimeState) != nullptr;
}

juce::String AudioNormalizationService::getNormalizationSupportMessage(const juce::File& file)
{
    const auto& runtimeState = getThreadLocalRuntimeState();
    const auto* format = runtimeState.readFormatManager.findFormatForFileExtension(normalizedExtension(file));

    if (format == nullptr) {
        return "Unsupported audio format";
    }

    if (getAiffWriterFormat(runtimeState) == nullptr) {
        return "AIFF writer is not available in this build";
    }

    return {};
}

juce::String AudioNormalizationService::getFormatSupportSummary()
{
    const auto& runtimeState = getThreadLocalRuntimeState();
    const auto support = collectFormatSupport(runtimeState);

    juce::StringArray writableLines;
    juce::StringArray readOnlyLines;

    for (const auto& [formatName, fileExtensions, canNormalize, detail] : support) {
        const auto line = detail.isNotEmpty()
            ? utils::format("- {} ({}): {}", formatName, formatExtensionsToText(fileExtensions), detail)
            : utils::format("- {} ({})", formatName, formatExtensionsToText(fileExtensions));

        if (canNormalize) {
            writableLines.add(line);
        } else {
            readOnlyLines.add(line);
        }
    }

    // The summary is only shown in GUI dialogs, so plain '\n' line breaks are fine.
    auto message = utils::format(
        "{}\n\n"
        "Normalization rewrites files in place when they are already AIFF, "
        "and converts every other supported format to an AIFF file of the same base name. "
        "Metadata (tags, album art, custom frames) is read from the source via TagLib "
        "and written back to the AIFF output as ID3v2.4.\n\n",
        getNormalizationStatusLine()
    );

    if (!writableLines.isEmpty()) {
        message += utils::format("Normalization available here:\n{}\n\n", writableLines.joinIntoString("\n"));
    }

    if (!readOnlyLines.isEmpty()) {
        message += utils::format("Readable but not normalizable here:\n{}", readOnlyLines.joinIntoString("\n"));
    }

    return message.trimEnd();
}

juce::AudioFormatManager& AudioNormalizationService::getThreadLocalFormatManager()
{
    return getThreadLocalRuntimeState().readFormatManager;
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
        return failNormalization(file, existingOutputMessage(outputFile));
    }

    if (!sourceRecord.isReady()) {
        return failNormalization(file, "File analysis must finish before normalization");
    }

    // The gain comes from the analyzed peak, so a file that changed since its analysis is measured again first.
    const auto record = sourceRecord.matchesFileOnDisk() ? sourceRecord : AudioAnalysisService::analyzeFile(file);

    if (record.hasError()) {
        return failNormalization(file, record.errorMessage);
    }

    auto& runtimeState = getThreadLocalRuntimeState();
    auto& formatManager = runtimeState.readFormatManager;
    auto* writerFormat = getAiffWriterFormat(runtimeState);

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
                                   .withMetadataValues(copyMetadata(reader->metadataValues));

    auto writer = writerFormat->createWriterFor(outputStream, writerOptions);

    if (writer == nullptr) {
        return failNormalization(file, getNormalizationSupportMessage(file));
    }

    if (const auto renderError = renderNormalizedAudio(*reader, *writer, gain, isCancelled); renderError.isNotEmpty()) {
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
    if (isCancellationRequested(isCancelled)) {
        return failNormalization(file, cancelledMessage);
    }

    if (const auto clearError = clearExistingOutput(file, outputFile, options); clearError.isNotEmpty()) {
        return failNormalization(file, clearError);
    }

    if (juce::String finalizeError; !finalizeNormalizationOutput(temporaryFile, file, outputFile, finalizeError)) {
        return failNormalization(file, finalizeError);
    }

    AudioNormalizationResult result;
    result.file = file;
    result.fileName = file.getFileName();
    result.fullPath = file.getFullPathName();
    result.analysisRecord = AudioAnalysisService::analyzeFile(outputFile);
    result.succeeded = !result.analysisRecord.hasError();

    if (!result.succeeded) {
        result.errorMessage
            = utils::format("The file was normalized, but re-analysis failed: {}", result.analysisRecord.errorMessage);
        utils::logError("Normalization failed for {}: {}", result.fullPath.quoted(), result.errorMessage);
    }

    return result;
}
