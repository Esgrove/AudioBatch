#include "AudioNormalizationService.h"

#include "MetadataService.h"
#include "utils.h"
#include <unordered_map>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

/// Helpers for format lookup, metadata passthrough, and chunked normalization.
namespace audiobatch::normalization
{
static AudioNormalizationResult failNormalization(const juce::File& file, const juce::String& message)
{
    utils::logError("Normalization failed for " + file.getFullPathName() + ": " + message);
    return AudioNormalizationResult::failure(file, message);
}

constexpr int normalizationBlockSize = 32768;
constexpr auto normalizedAiffOutputExtension = ".aif";
constexpr int defaultMp3BitsPerSample = 16;

struct AudioNormalizationRuntimeState {
    juce::AudioFormatManager readFormatManager;
};

struct AudioNormalizationFormatSupport {
    juce::String formatName;
    juce::StringArray fileExtensions;
    bool canNormalize = false;
    juce::String detail;
};

static float peakMagnitude(const float peak)
{
    return std::abs(peak);
}

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

static juce::String normalizedExtension(const juce::File& file)
{
    auto extension = file.getFileExtension();

    if (extension.startsWithChar('.')) {
        extension = extension.substring(1);
    }

    return extension;
}

static juce::String normalizedExtension(const juce::String& extension)
{
    auto value = extension.trim();

    if (value.startsWithChar('.')) {
        value = value.substring(1);
    }

    return value;
}

static bool isMp3Extension(const juce::String& extension)
{
    return normalizedExtension(extension).equalsIgnoreCase("mp3");
}

static bool isMp3SourceFile(const juce::File& file)
{
    return isMp3Extension(normalizedExtension(file));
}

static juce::AudioFormat* getAiffWriterFormat(const AudioNormalizationRuntimeState& runtimeState)
{
    if (auto* format = runtimeState.readFormatManager.findFormatForFileExtension("aif"); format != nullptr) {
        return format;
    }

    return runtimeState.readFormatManager.findFormatForFileExtension("aiff");
}

static juce::File getNormalizationOutputFile(const juce::File& sourceFile)
{
    // Output is always a sibling `.aif` file.
    // If the source is already named `<name>.aif` it will resolve to the same path and be normalized in place.
    // Any other extension (including `.aiff` / `.aifc`) is renamed to `.aif`,
    // and the original is trashed once the write succeeds.
    return sourceFile.getSiblingFile(sourceFile.getFileNameWithoutExtension() + normalizedAiffOutputExtension);
}

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

static juce::String getNormalizationStatusLine()
{
    return "Normalization output: same-name AIF files. "
           "Originals with a different extension are moved to the system trash.";
}

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

static double resolveWriterSampleRate(juce::AudioFormat& format, const double preferredSampleRate)
{
    const auto sampleRates = format.getPossibleSampleRates();

    if (sampleRates.isEmpty() || preferredSampleRate <= 0.0) {
        return preferredSampleRate;
    }

    int resolvedSampleRate = sampleRates[0];
    auto bestDistance = std::abs(preferredSampleRate - static_cast<double>(resolvedSampleRate));

    for (const auto sampleRate : sampleRates) {
        if (const auto distance = std::abs(preferredSampleRate - static_cast<double>(sampleRate));
            distance < bestDistance)
        {
            resolvedSampleRate = sampleRate;
            bestDistance = distance;
        }
    }

    return resolvedSampleRate;
}

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

static bool canAcceptReadFailureForNormalization(
    const juce::AudioFormatReader& reader,
    const juce::File& sourceFile,
    const juce::int64 samplePosition,
    const int samplesThisBlock
)
{
    if (!isMp3SourceFile(sourceFile)) {
        return false;
    }

    return samplePosition + samplesThisBlock >= reader.lengthInSamples;
}

static AudioNormalizationRuntimeState& getThreadLocalRuntimeState()
{
    thread_local auto runtimeState = [] {
        auto initializedState = std::make_unique<AudioNormalizationRuntimeState>();
        initializedState->readFormatManager.registerBasicFormats();
        return initializedState;
    }();

    return *runtimeState;
}

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

static std::vector<AudioNormalizationFormatSupport> collectFormatSupport(AudioNormalizationRuntimeState& runtimeState)
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

static juce::String formatExtensionsToText(const juce::StringArray& extensions)
{
    juce::StringArray normalizedExtensions;

    for (const auto& extension : extensions) {
        normalizedExtensions.addIfNotAlreadyThere(extension.toLowerCase());
    }

    return normalizedExtensions.joinIntoString(", ");
}

static std::unique_ptr<juce::AudioFormatWriter> createWriterPreservingMetadata(
    juce::AudioFormat& format,
    std::unique_ptr<juce::OutputStream>& outputStream,
    const juce::AudioFormatWriterOptions& writerOptions,
    const juce::StringPairArray& sourceMetadata
)
{
    juce::ignoreUnused(sourceMetadata);
    return format.createWriterFor(outputStream, writerOptions);
}

static bool preserveOutputMetadata(const juce::File& sourceFile, const juce::File& destinationFile)
{
    MetadataService::Metadata metadata;

    if (!MetadataService::readMetadata(sourceFile, metadata)) {
        // Reading failed, for example due to an unsupported format.
        // This is non-fatal, so keep going without metadata.
        utils::logInfo("No metadata could be read from " + sourceFile.getFullPathName());
        return true;
    }

    if (metadata.isEmpty()) {
        return true;
    }

    if (!MetadataService::writeMetadata(destinationFile, metadata)) {
        utils::logError("Failed to write metadata to " + destinationFile.getFullPathName());
        return false;
    }

    return true;
}

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
}  // namespace audiobatch::normalization

using namespace audiobatch::normalization;

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
    auto& runtimeState = getThreadLocalRuntimeState();
    const auto support = collectFormatSupport(runtimeState);

    juce::StringArray writableLines;
    juce::StringArray readOnlyLines;

    for (const auto& [formatName, fileExtensions, canNormalize, detail] : support) {
        auto line = "- " + formatName + " (" + formatExtensionsToText(fileExtensions) + ")";

        if (detail.isNotEmpty()) {
            line << ": " << detail;
        }

        if (canNormalize) {
            writableLines.add(line);
        } else {
            readOnlyLines.add(line);
        }
    }

    juce::String message;
    message << getNormalizationStatusLine() << juce::newLine << juce::newLine;
    message << "Normalization rewrites files in place when they are already AIFF, and converts every other"
            << " supported format to an AIFF file of the same base name. Metadata (tags, album art, custom"
            << " frames) is read from the source via TagLib and written back to the AIFF output as ID3v2.4."
            << juce::newLine << juce::newLine;

    if (!writableLines.isEmpty()) {
        message << "Normalization available here:" << juce::newLine << writableLines.joinIntoString(juce::newLine)
                << juce::newLine << juce::newLine;
    }

    if (!readOnlyLines.isEmpty()) {
        message << "Readable but not normalizable here:" << juce::newLine
                << readOnlyLines.joinIntoString(juce::newLine);
    }

    return message.trimEnd();
}

juce::AudioFormatManager& AudioNormalizationService::getThreadLocalFormatManager()
{
    return getThreadLocalRuntimeState().readFormatManager;
}

AudioNormalizationResult AudioNormalizationService::normalizeFile(const AudioAnalysisRecord& record)
{
    const auto& file = record.file;
    const auto outputFile = getNormalizationOutputFile(file);

    if (!file.existsAsFile()) {
        return failNormalization(file, "File does not exist");
    }

    if (!record.isReady()) {
        return failNormalization(file, "File analysis must finish before normalization");
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

    const auto peak = peakMagnitude(record.overallPeak);

    if (peak <= 0.0f) {
        return failNormalization(file, "File contains no signal that can be normalized");
    }

    const auto gain = 1.0f / peak;

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

    juce::TemporaryFile temporaryFile(outputFile);
    std::unique_ptr<juce::OutputStream> outputStream(temporaryFile.getFile().createOutputStream().release());

    if (outputStream == nullptr) {
        return failNormalization(file, "Could not create temporary output file");
    }

    const auto writerSampleRate = resolveWriterSampleRate(*writerFormat, reader->sampleRate);
    const auto preferredBitDepth = resolvePreferredOutputBitDepth(record, *reader, file);
    const auto writerBitDepth = resolveWriterBitDepth(*writerFormat, preferredBitDepth);

    auto writerOptions = juce::AudioFormatWriterOptions()
                             .withSampleRate(writerSampleRate)
                             .withNumChannels(static_cast<int>(reader->numChannels))
                             .withBitsPerSample(writerBitDepth)
                             .withMetadataValues(copyMetadata(reader->metadataValues));

    if (reader->usesFloatingPointData && writerBitDepth == 32) {
        writerOptions = writerOptions.withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    }

    auto writer = createWriterPreservingMetadata(*writerFormat, outputStream, writerOptions, reader->metadataValues);

    if (writer == nullptr) {
        return failNormalization(file, getNormalizationSupportMessage(file));
    }

    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), normalizationBlockSize);
    std::vector<float*> channelPointers(reader->numChannels);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        channelPointers[static_cast<std::size_t>(channel)] = buffer.getWritePointer(channel);
    }

    for (juce::int64 samplePosition = 0; samplePosition < reader->lengthInSamples;
         samplePosition += normalizationBlockSize)
    {
        const auto samplesThisBlock = static_cast<int>(
            juce::jmin<juce::int64>(normalizationBlockSize, reader->lengthInSamples - samplePosition)
        );

        if (!reader->read(channelPointers.data(), buffer.getNumChannels(), samplePosition, samplesThisBlock)
            && !canAcceptReadFailureForNormalization(*reader, file, samplePosition, samplesThisBlock))
        {
            return failNormalization(file, "Failed while reading audio data for normalization");
        }

        buffer.applyGain(gain);

        if (!writer->writeFromAudioSampleBuffer(buffer, 0, samplesThisBlock)) {
            return failNormalization(file, "Failed while writing normalized audio data");
        }
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
        result.errorMessage = "The file was normalized, but re-analysis failed: " + result.analysisRecord.errorMessage;
        utils::logError("Normalization failed for " + result.fullPath + ": " + result.errorMessage);
    }

    return result;
}
