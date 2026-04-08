#include "AudioNormalizationService.h"

#include <unordered_map>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

/// Helpers for format lookup, metadata passthrough, and chunked normalization.
namespace
{
constexpr int normalizationBlockSize = 32768;
constexpr auto normalizedMp3OutputExtension = ".aif";
constexpr int defaultMp3BitsPerSample = 16;

struct AudioNormalizationRuntimeState {
    juce::AudioFormatManager readFormatManager;
    juce::File lameExecutable;
#if JUCE_USE_LAME_AUDIO_FORMAT
    std::unique_ptr<juce::LAMEEncoderAudioFormat> lameEncoderFormat;
#endif
};

struct AudioNormalizationFormatSupport {
    juce::String formatName;
    juce::StringArray fileExtensions;
    bool canWriteBack = false;
    juce::String detail;
};

float peakMagnitude(float peak)
{
    return std::abs(peak);
}

std::unordered_map<juce::String, juce::String> copyMetadata(const juce::StringPairArray& metadata)
{
    std::unordered_map<juce::String, juce::String> copiedMetadata;
    const auto& keys = metadata.getAllKeys();
    const auto& values = metadata.getAllValues();

    for (int index = 0; index < metadata.size(); ++index) {
        copiedMetadata[keys[index]] = values[index];
    }

    return copiedMetadata;
}

juce::String normalizedExtension(const juce::File& file)
{
    auto extension = file.getFileExtension();

    if (extension.startsWithChar('.')) {
        extension = extension.substring(1);
    }

    return extension;
}

juce::String normalizedExtension(const juce::String& extension)
{
    auto value = extension.trim();

    if (value.startsWithChar('.')) {
        value = value.substring(1);
    }

    return value;
}

bool isMp3Extension(const juce::String& extension)
{
    return normalizedExtension(extension).equalsIgnoreCase("mp3");
}

bool isMp3SourceFile(const juce::File& file)
{
    return isMp3Extension(normalizedExtension(file));
}

juce::AudioFormat* getAiffWriterFormat(AudioNormalizationRuntimeState& runtimeState)
{
    if (auto* format = runtimeState.readFormatManager.findFormatForFileExtension("aif"); format != nullptr) {
        return format;
    }

    return runtimeState.readFormatManager.findFormatForFileExtension("aiff");
}

juce::File getNormalizationOutputFile(const juce::File& sourceFile)
{
    if (!isMp3SourceFile(sourceFile)) {
        return sourceFile;
    }

    return sourceFile.getSiblingFile(sourceFile.getFileNameWithoutExtension() + normalizedMp3OutputExtension);
}

bool finalizeNormalizationOutput(
    juce::TemporaryFile& temporaryFile,
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

    if (sourceFile.existsAsFile() && !sourceFile.moveToTrash()) {
        outputFile.deleteFile();
        errorMessage = "Could not move the original MP3 file to the system trash";
        return false;
    }

    return true;
}

bool isAiffExtension(const juce::String& extension)
{
    const auto normalized = normalizedExtension(extension);
    return normalized.equalsIgnoreCase("aif") || normalized.equalsIgnoreCase("aiff")
        || normalized.equalsIgnoreCase("aifc");
}

bool canStoreAiffId3Metadata(const juce::File& file)
{
    return isAiffExtension(normalizedExtension(file));
}

bool readExactly(juce::InputStream& input, void* destination, int bytesToRead)
{
    return bytesToRead >= 0 && input.read(destination, bytesToRead) == bytesToRead;
}

std::uint32_t readBigEndianUint32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

bool tagMatches(const std::uint8_t* bytes, const char* tag)
{
    return std::memcmp(bytes, tag, 4) == 0;
}

bool startsWithId3Tag(const std::uint8_t* bytes)
{
    return bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3';
}

bool isAiffFile(const juce::File& file)
{
    auto input = file.createInputStream();

    if (input == nullptr) {
        return false;
    }

    std::array<std::uint8_t, 12> header {};

    if (!readExactly(*input, header.data(), static_cast<int>(header.size()))) {
        return false;
    }

    return tagMatches(header.data(), "FORM")
        && (tagMatches(header.data() + 8, "AIFF") || tagMatches(header.data() + 8, "AIFC"));
}

bool extractMp3Id3Metadata(const juce::File& sourceFile, juce::MemoryBlock& metadata)
{
    auto input = sourceFile.createInputStream();

    if (input == nullptr) {
        return false;
    }

    std::array<std::uint8_t, 10> header {};

    if (!readExactly(*input, header.data(), static_cast<int>(header.size()))) {
        return sourceFile.getSize() == 0;
    }

    if (!startsWithId3Tag(header.data())) {
        return true;
    }

    if ((header[6] | header[7] | header[8] | header[9]) & 0x80U) {
        return false;
    }

    const auto payloadSize = (static_cast<std::uint32_t>(header[6]) << 21U)
        | (static_cast<std::uint32_t>(header[7]) << 14U) | (static_cast<std::uint32_t>(header[8]) << 7U)
        | static_cast<std::uint32_t>(header[9]);
    const auto hasFooter = (header[5] & 0x10U) != 0;
    const auto totalSize = static_cast<juce::int64>(header.size()) + static_cast<juce::int64>(payloadSize)
        + static_cast<juce::int64>(hasFooter ? 10 : 0);

    if (totalSize <= 0 || totalSize > sourceFile.getSize()) {
        return false;
    }

    metadata.setSize(static_cast<size_t>(totalSize), false);
    input->setPosition(0);
    return readExactly(*input, metadata.getData(), static_cast<int>(totalSize));
}

bool extractAiffId3Metadata(const juce::File& sourceFile, juce::MemoryBlock& metadata)
{
    auto input = sourceFile.createInputStream();

    if (input == nullptr) {
        return false;
    }

    std::array<std::uint8_t, 12> header {};

    if (!readExactly(*input, header.data(), static_cast<int>(header.size()))) {
        return false;
    }

    if (!tagMatches(header.data(), "FORM")
        || !(tagMatches(header.data() + 8, "AIFF") || tagMatches(header.data() + 8, "AIFC")))
    {
        return true;
    }

    const auto fileSize = sourceFile.getSize();

    while (input->getPosition() + 8 <= fileSize) {
        std::array<std::uint8_t, 8> chunkHeader {};

        if (!readExactly(*input, chunkHeader.data(), static_cast<int>(chunkHeader.size()))) {
            return false;
        }

        const auto chunkSize = readBigEndianUint32(chunkHeader.data() + 4);
        const auto chunkDataPosition = input->getPosition();
        const auto paddedChunkSize = static_cast<juce::int64>(chunkSize) + static_cast<juce::int64>(chunkSize & 1U);

        if (chunkDataPosition + paddedChunkSize > fileSize) {
            return false;
        }

        if (tagMatches(chunkHeader.data(), "ID3 ")) {
            metadata.setSize(static_cast<size_t>(chunkSize), false);
            return readExactly(*input, metadata.getData(), static_cast<int>(chunkSize));
        }

        input->setPosition(chunkDataPosition + paddedChunkSize);
    }

    return true;
}

bool extractRawId3Metadata(const juce::File& sourceFile, juce::MemoryBlock& metadata)
{
    metadata.reset();

    if (isMp3SourceFile(sourceFile)) {
        return extractMp3Id3Metadata(sourceFile, metadata);
    }

    if (isAiffExtension(normalizedExtension(sourceFile))) {
        return extractAiffId3Metadata(sourceFile, metadata);
    }

    return true;
}

bool appendAiffId3Metadata(const juce::File& destinationFile, const juce::MemoryBlock& metadata)
{
    if (metadata.getSize() == 0 || !isAiffFile(destinationFile)) {
        return metadata.getSize() == 0;
    }

    juce::FileOutputStream output(destinationFile);

    if (output.failedToOpen()) {
        return false;
    }

    const auto originalSize = destinationFile.getSize();

    if (!output.setPosition(originalSize) || !output.write("ID3 ", 4)) {
        return false;
    }

    output.writeIntBigEndian(static_cast<int>(metadata.getSize()));

    if (output.getStatus().failed() || !output.write(metadata.getData(), metadata.getSize())) {
        return false;
    }

    if ((metadata.getSize() & 1U) != 0) {
        output.writeByte(0);

        if (output.getStatus().failed()) {
            return false;
        }
    }

    const auto newSize = originalSize + 8 + static_cast<juce::int64>(metadata.getSize())
        + static_cast<juce::int64>(metadata.getSize() & 1U);

    if (!output.setPosition(4)) {
        return false;
    }

    output.writeIntBigEndian(static_cast<int>(newSize - 8));

    if (output.getStatus().failed()) {
        return false;
    }

    output.flush();
    return output.getStatus().wasOk();
}

juce::String getLameExecutableName()
{
#if JUCE_WINDOWS
    return "lame.exe";
#else
    return "lame";
#endif
}

juce::String getLameSearchLocationsDescription()
{
    return "PATH and common Scoop/Homebrew locations";
}

juce::StringArray getLameCandidatePaths()
{
    juce::StringArray candidatePaths;
    const auto addCandidate = [&candidatePaths](const juce::File& file) {
        if (file != juce::File()) {
            candidatePaths.addIfNotAlreadyThere(file.getFullPathName());
        }
    };

    const auto executableName = getLameExecutableName();
    const auto executableDirectory
        = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    const auto userHome = juce::File::getSpecialLocation(juce::File::userHomeDirectory);

    addCandidate(executableDirectory.getChildFile(executableName));
    addCandidate(executableDirectory.getChildFile("tools").getChildFile(executableName));

#if JUCE_WINDOWS
    addCandidate(userHome.getChildFile("scoop").getChildFile("shims").getChildFile(executableName));
    addCandidate(
        userHome.getChildFile("scoop").getChildFile("apps").getChildFile("lame").getChildFile("current").getChildFile(
            executableName
        )
    );
#else
    addCandidate(juce::File("/opt/homebrew/bin/lame"));
    addCandidate(juce::File("/usr/local/bin/lame"));
    addCandidate(juce::File("/opt/local/bin/lame"));
#endif

    juce::StringArray pathEntries;
    pathEntries.addTokens(juce::SystemStats::getEnvironmentVariable("PATH", {}), JUCE_WINDOWS ? ";" : ":", "\"");
    pathEntries.removeEmptyStrings();
    pathEntries.trim();

    for (const auto& pathEntry : pathEntries) {
        addCandidate(juce::File(pathEntry).getChildFile(executableName));
    }

    return candidatePaths;
}

juce::File findLameExecutable()
{
    for (const auto& candidatePath : getLameCandidatePaths()) {
        const juce::File candidate(candidatePath);

        if (candidate.existsAsFile()) {
            return candidate;
        }
    }

    return {};
}

bool extensionsContain(const juce::StringArray& extensions, const juce::String& targetExtension)
{
    for (const auto& extension : extensions) {
        if (extension.equalsIgnoreCase(targetExtension)) {
            return true;
        }
    }

    return false;
}

juce::AudioFormatWriterOptions buildProbeWriterOptions(juce::AudioFormat& format)
{
    const auto sampleRates = format.getPossibleSampleRates();
    const auto bitDepths = format.getPossibleBitDepths();
    const auto numChannels = format.canDoStereo() ? 2 : (format.canDoMono() ? 1 : 0);

    if (sampleRates.isEmpty() || bitDepths.isEmpty() || numChannels <= 0) {
        return {};
    }

    return juce::AudioFormatWriterOptions()
        .withSampleRate(static_cast<double>(sampleRates[0]))
        .withNumChannels(numChannels)
        .withBitsPerSample(bitDepths[0]);
}

bool canCreateProbeWriter(juce::AudioFormat& format)
{
    const auto options = buildProbeWriterOptions(format);

    if (options.getSampleRate() <= 0.0 || options.getNumChannels() <= 0 || options.getBitsPerSample() <= 0) {
        return false;
    }

    std::unique_ptr<juce::OutputStream> outputStream = std::make_unique<juce::MemoryOutputStream>();
    auto writer = format.createWriterFor(outputStream, options);
    return writer != nullptr;
}

int resolveWriterBitDepth(juce::AudioFormat& format, int preferredBitsPerSample)
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
        if (bitDepth > resolvedBitsPerSample) {
            resolvedBitsPerSample = bitDepth;
        }
    }

    return resolvedBitsPerSample;
}

double resolveWriterSampleRate(juce::AudioFormat& format, double preferredSampleRate)
{
    const auto sampleRates = format.getPossibleSampleRates();

    if (sampleRates.isEmpty() || preferredSampleRate <= 0.0) {
        return preferredSampleRate;
    }

    int resolvedSampleRate = sampleRates[0];
    auto bestDistance = std::abs(preferredSampleRate - static_cast<double>(resolvedSampleRate));

    for (const auto sampleRate : sampleRates) {
        const auto distance = std::abs(preferredSampleRate - static_cast<double>(sampleRate));

        if (distance < bestDistance) {
            resolvedSampleRate = sampleRate;
            bestDistance = distance;
        }
    }

    return static_cast<double>(resolvedSampleRate);
}

int resolvePreferredOutputBitDepth(
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

bool canAcceptReadFailureForNormalization(
    const juce::AudioFormatReader& reader,
    const juce::File& sourceFile,
    juce::int64 samplePosition,
    int samplesThisBlock
)
{
    if (!isMp3SourceFile(sourceFile)) {
        return false;
    }

    return samplePosition + samplesThisBlock >= reader.lengthInSamples;
}

juce::AudioFormat* getWriterFormatForExtension(
    AudioNormalizationRuntimeState& runtimeState,
    const juce::String& extension
)
{
    return runtimeState.readFormatManager.findFormatForFileExtension(normalizedExtension(extension));
}

juce::String getMp3WriteUnavailableReason(const AudioNormalizationRuntimeState& runtimeState)
{
    juce::ignoreUnused(runtimeState);
    return "MP3 sources are normalized to same-name AIF files, but AIFF writing is unavailable in this build.";
}

juce::String getWriteUnavailableReason(
    const juce::StringArray& extensions,
    const AudioNormalizationRuntimeState& runtimeState
)
{
    if (extensionsContain(extensions, ".mp3")) {
        return getMp3WriteUnavailableReason(runtimeState);
    }

    return "This format can be read in the current build, but no compatible writer is available for in-place "
           "normalization.";
}

bool canWriteExtension(AudioNormalizationRuntimeState& runtimeState, const juce::String& extension)
{
    const auto normalized = normalizedExtension(extension);

    if (isMp3Extension(normalized)) {
        if (auto* writerFormat = getAiffWriterFormat(runtimeState); writerFormat != nullptr) {
            return canCreateProbeWriter(*writerFormat);
        }

        return false;
    }

    if (auto* writerFormat = getWriterFormatForExtension(runtimeState, normalized); writerFormat != nullptr) {
        return canCreateProbeWriter(*writerFormat);
    }

    return false;
}

AudioNormalizationRuntimeState& getThreadLocalRuntimeState()
{
    thread_local auto runtimeState = [] {
        auto initializedState = std::make_unique<AudioNormalizationRuntimeState>();
        initializedState->readFormatManager.registerBasicFormats();
        initializedState->lameExecutable = findLameExecutable();

#if JUCE_USE_LAME_AUDIO_FORMAT
        if (initializedState->lameExecutable.existsAsFile()) {
            initializedState->lameEncoderFormat
                = std::make_unique<juce::LAMEEncoderAudioFormat>(initializedState->lameExecutable);
        }
#endif

        return initializedState;
    }();

    return *runtimeState;
}

juce::String getMp3EncoderStatusLine(const AudioNormalizationRuntimeState& runtimeState)
{
    juce::ignoreUnused(runtimeState);
    return "MP3 normalization output: same-name AIF files after moving the original MP3 to the system trash.";
}

std::vector<AudioNormalizationFormatSupport> collectFormatSupport(AudioNormalizationRuntimeState& runtimeState)
{
    std::vector<AudioNormalizationFormatSupport> support;
    support.reserve(static_cast<std::size_t>(runtimeState.readFormatManager.getNumKnownFormats()));

    for (int index = 0; index < runtimeState.readFormatManager.getNumKnownFormats(); ++index) {
        auto* format = runtimeState.readFormatManager.getKnownFormat(index);

        if (format == nullptr) {
            continue;
        }

        AudioNormalizationFormatSupport entry;
        entry.formatName = format->getFormatName();
        entry.fileExtensions = format->getFileExtensions();
        entry.canWriteBack
            = !entry.fileExtensions.isEmpty() && canWriteExtension(runtimeState, entry.fileExtensions[0]);

        if (entry.canWriteBack && !entry.fileExtensions.isEmpty()
            && isMp3Extension(normalizedExtension(entry.fileExtensions[0])))
        {
            entry.detail = "Writes same-name AIF files and moves the original MP3 to the system trash.";
        } else if (!entry.canWriteBack) {
            entry.detail = getWriteUnavailableReason(entry.fileExtensions, runtimeState);
        }

        support.push_back(std::move(entry));
    }

    std::sort(support.begin(), support.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.canWriteBack != rhs.canWriteBack) {
            return lhs.canWriteBack > rhs.canWriteBack;
        }

        return lhs.formatName.compareNatural(rhs.formatName) < 0;
    });

    return support;
}

juce::String formatExtensionsToText(const juce::StringArray& extensions)
{
    juce::StringArray normalizedExtensions;

    for (const auto& extension : extensions) {
        normalizedExtensions.addIfNotAlreadyThere(extension.toLowerCase());
    }

    return normalizedExtensions.joinIntoString(", ");
}

std::unique_ptr<juce::AudioFormatWriter> createWriterPreservingMetadata(
    juce::AudioFormat& format,
    std::unique_ptr<juce::OutputStream>& outputStream,
    const juce::AudioFormatWriterOptions& writerOptions,
    const juce::StringPairArray& sourceMetadata
)
{
    juce::ignoreUnused(sourceMetadata);
    return format.createWriterFor(outputStream, writerOptions);
}

bool preserveOutputMetadata(const juce::File& sourceFile, const juce::File& destinationFile)
{
    if (!canStoreAiffId3Metadata(destinationFile)) {
        return true;
    }

    juce::MemoryBlock metadata;

    if (!extractRawId3Metadata(sourceFile, metadata)) {
        return false;
    }

    return appendAiffId3Metadata(destinationFile, metadata);
}

juce::String validateTemporaryNormalizedOutput(
    juce::AudioFormatManager& formatManager,
    const juce::File& temporaryOutputFile
)
{
    if (!temporaryOutputFile.existsAsFile() || temporaryOutputFile.getSize() <= 0) {
        return "Normalization failed to produce a valid output file. The original file was left unchanged.";
    }

    std::unique_ptr<juce::AudioFormatReader> encodedReader(formatManager.createReaderFor(temporaryOutputFile));

    if (encodedReader == nullptr || encodedReader->lengthInSamples <= 0) {
        return "Normalization failed before the output file could be verified. The original file was left unchanged.";
    }

    return {};
}
}  // namespace

bool AudioNormalizationService::canNormalizeFile(const juce::File& file)
{
    auto& runtimeState = getThreadLocalRuntimeState();
    return canWriteExtension(runtimeState, normalizedExtension(file));
}

juce::String AudioNormalizationService::getNormalizationSupportMessage(const juce::File& file)
{
    auto& runtimeState = getThreadLocalRuntimeState();
    auto* format = runtimeState.readFormatManager.findFormatForFileExtension(normalizedExtension(file));

    if (format == nullptr) {
        return "Unsupported audio format";
    }

    if (canWriteExtension(runtimeState, normalizedExtension(file))) {
        return {};
    }

    return getWriteUnavailableReason(format->getFileExtensions(), runtimeState);
}

juce::String AudioNormalizationService::getFormatSupportSummary()
{
    auto& runtimeState = getThreadLocalRuntimeState();
    const auto support = collectFormatSupport(runtimeState);

    juce::StringArray writableLines;
    juce::StringArray readOnlyLines;

    for (const auto& entry : support) {
        auto line = "- " + entry.formatName + " (" + formatExtensionsToText(entry.fileExtensions) + ")";

        if (entry.detail.isNotEmpty()) {
            line << ": " << entry.detail;
        }

        if (entry.canWriteBack) {
            writableLines.add(line);
        } else {
            readOnlyLines.add(line);
        }
    }

    juce::String message;
    message << getMp3EncoderStatusLine(runtimeState) << juce::newLine << juce::newLine;
    message << "Normalization rewrites files in place when possible. MP3 sources are replaced by same-name AIF files."
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

AudioNormalizationResult AudioNormalizationService::normalizeFile(const AudioAnalysisRecord& sourceRecord)
{
    const auto& file = sourceRecord.file;
    const auto outputFile = getNormalizationOutputFile(file);

    if (!file.existsAsFile()) {
        return AudioNormalizationResult::failure(file, "File does not exist");
    }

    if (!sourceRecord.isReady()) {
        return AudioNormalizationResult::failure(file, "File analysis must finish before normalization");
    }

    auto& runtimeState = getThreadLocalRuntimeState();
    auto& formatManager = runtimeState.readFormatManager;
    auto* writerFormat = isMp3SourceFile(file) ? getAiffWriterFormat(runtimeState)
                                               : getWriterFormatForExtension(runtimeState, normalizedExtension(file));

    if (writerFormat == nullptr) {
        return AudioNormalizationResult::failure(file, "Unsupported audio format");
    }

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        return AudioNormalizationResult::failure(file, "Unsupported or unreadable audio file");
    }

    const auto peak = peakMagnitude(sourceRecord.overallPeak);

    if (peak <= 0.0f) {
        return AudioNormalizationResult::failure(file, "File contains no signal that can be normalized");
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
        return AudioNormalizationResult::failure(file, "Could not create temporary output file");
    }

    const auto writerSampleRate = resolveWriterSampleRate(*writerFormat, reader->sampleRate);
    const auto preferredBitDepth = resolvePreferredOutputBitDepth(sourceRecord, *reader, file);
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
        return AudioNormalizationResult::failure(file, getNormalizationSupportMessage(file));
    }

    juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), normalizationBlockSize);
    std::vector<float*> channelPointers(static_cast<std::size_t>(reader->numChannels));

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
            return AudioNormalizationResult::failure(file, "Failed while reading audio data for normalization");
        }

        buffer.applyGain(static_cast<float>(gain));

        if (!writer->writeFromAudioSampleBuffer(buffer, 0, samplesThisBlock)) {
            return AudioNormalizationResult::failure(file, "Failed while writing normalized audio data");
        }
    }

    writer.reset();

    if (!preserveOutputMetadata(file, temporaryFile.getFile())) {
        return AudioNormalizationResult::failure(file, "Could not preserve metadata while writing normalized audio");
    }

    if (const auto validationError = validateTemporaryNormalizedOutput(formatManager, temporaryFile.getFile());
        validationError.isNotEmpty())
    {
        return AudioNormalizationResult::failure(file, validationError);
    }

    juce::String finalizeError;

    if (!finalizeNormalizationOutput(temporaryFile, file, outputFile, finalizeError)) {
        return AudioNormalizationResult::failure(file, finalizeError);
    }

    AudioNormalizationResult result;
    result.file = file;
    result.fileName = file.getFileName();
    result.fullPath = file.getFullPathName();
    result.analysisRecord = AudioAnalysisService::analyzeFile(outputFile);
    result.succeeded = !result.analysisRecord.hasError();

    if (!result.succeeded) {
        result.errorMessage = "The file was normalized, but re-analysis failed: " + result.analysisRecord.errorMessage;
    }

    return result;
}
