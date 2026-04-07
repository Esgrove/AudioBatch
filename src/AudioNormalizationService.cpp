#include "AudioNormalizationService.h"

#include <unordered_map>

#include <algorithm>
#include <cmath>
#include <vector>

/// Helpers for format lookup, metadata passthrough, and chunked normalization.
namespace
{
constexpr int normalizationBlockSize = 32768;

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

juce::String getWriteUnavailableReason(const juce::AudioFormat& format)
{
    const auto extensions = format.getFileExtensions();

    if (extensionsContain(extensions, ".mp3")) {
        return "JUCE can read MP3 in this build, but MP3 writing needs an external encoder such as LAME.";
    }

    return "This format can be read in the current build, but no compatible writer is available for in-place "
           "normalization.";
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

std::vector<AudioNormalizationFormatSupport> collectFormatSupport(juce::AudioFormatManager& formatManager)
{
    std::vector<AudioNormalizationFormatSupport> support;
    support.reserve(static_cast<std::size_t>(formatManager.getNumKnownFormats()));

    for (int index = 0; index < formatManager.getNumKnownFormats(); ++index) {
        auto* format = formatManager.getKnownFormat(index);

        if (format == nullptr) {
            continue;
        }

        AudioNormalizationFormatSupport entry;
        entry.formatName = format->getFormatName();
        entry.fileExtensions = format->getFileExtensions();
        entry.canWriteBack = canCreateProbeWriter(*format);

        if (!entry.canWriteBack) {
            entry.detail = getWriteUnavailableReason(*format);
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
}  // namespace

bool AudioNormalizationService::canNormalizeFile(const juce::File& file)
{
    auto& formatManager = getThreadLocalFormatManager();
    auto* format = formatManager.findFormatForFileExtension(normalizedExtension(file));

    return format != nullptr && canCreateProbeWriter(*format);
}

juce::String AudioNormalizationService::getNormalizationSupportMessage(const juce::File& file)
{
    auto& formatManager = getThreadLocalFormatManager();
    auto* format = formatManager.findFormatForFileExtension(normalizedExtension(file));

    if (format == nullptr) {
        return "Unsupported audio format";
    }

    if (canCreateProbeWriter(*format)) {
        return {};
    }

    return getWriteUnavailableReason(*format);
}

juce::String AudioNormalizationService::getFormatSupportSummary()
{
    auto& formatManager = getThreadLocalFormatManager();
    const auto support = collectFormatSupport(formatManager);

    juce::StringArray writableLines;
    juce::StringArray readOnlyLines;

    for (const auto& entry : support) {
        const auto line = "- " + entry.formatName + " (" + formatExtensionsToText(entry.fileExtensions) + ")";

        if (entry.canWriteBack) {
            writableLines.add(line);
        } else {
            auto detailedLine = line;

            if (entry.detail.isNotEmpty()) {
                detailedLine << ": " << entry.detail;
            }

            readOnlyLines.add(detailedLine);
        }
    }

    juce::String message;
    message << "Normalization rewrites files in place, so the source format must also be writable in this build."
            << juce::newLine << juce::newLine;

    if (!writableLines.isEmpty()) {
        message << "Writable here:" << juce::newLine << writableLines.joinIntoString(juce::newLine) << juce::newLine
                << juce::newLine;
    }

    if (!readOnlyLines.isEmpty()) {
        message << "Readable but not writable here:" << juce::newLine << readOnlyLines.joinIntoString(juce::newLine);
    }

    return message.trimEnd();
}

juce::AudioFormatManager& AudioNormalizationService::getThreadLocalFormatManager()
{
    thread_local juce::AudioFormatManager formatManager;
    thread_local const bool initialized = [] {
        formatManager.registerBasicFormats();
        return true;
    }();

    juce::ignoreUnused(initialized);
    return formatManager;
}

AudioNormalizationResult AudioNormalizationService::normalizeFile(const AudioAnalysisRecord& sourceRecord)
{
    const auto& file = sourceRecord.file;

    if (!file.existsAsFile()) {
        return AudioNormalizationResult::failure(file, "File does not exist");
    }

    if (!sourceRecord.isReady()) {
        return AudioNormalizationResult::failure(file, "File analysis must finish before normalization");
    }

    auto& formatManager = getThreadLocalFormatManager();
    auto* format = formatManager.findFormatForFileExtension(normalizedExtension(file));

    if (format == nullptr) {
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

    if (juce::approximatelyEqual(gain, 1.0f)) {
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

    juce::TemporaryFile temporaryFile(file);
    std::unique_ptr<juce::OutputStream> outputStream(temporaryFile.getFile().createOutputStream().release());

    if (outputStream == nullptr) {
        return AudioNormalizationResult::failure(file, "Could not create temporary output file");
    }

    auto writerOptions = juce::AudioFormatWriterOptions()
                             .withSampleRate(reader->sampleRate)
                             .withNumChannels(static_cast<int>(reader->numChannels))
                             .withBitsPerSample(static_cast<int>(reader->bitsPerSample))
                             .withMetadataValues(copyMetadata(reader->metadataValues));

    if (reader->usesFloatingPointData && reader->bitsPerSample == 32) {
        writerOptions = writerOptions.withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    }

    auto writer = format->createWriterFor(outputStream, writerOptions);

    if (writer == nullptr) {
        return AudioNormalizationResult::failure(file, getWriteUnavailableReason(*format));
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

        if (!reader->read(channelPointers.data(), buffer.getNumChannels(), samplePosition, samplesThisBlock)) {
            return AudioNormalizationResult::failure(file, "Failed while reading audio data for normalization");
        }

        buffer.applyGain(static_cast<float>(gain));

        if (!writer->writeFromAudioSampleBuffer(buffer, 0, samplesThisBlock)) {
            return AudioNormalizationResult::failure(file, "Failed while writing normalized audio data");
        }
    }

    writer.reset();

    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        return AudioNormalizationResult::failure(file, "Could not replace the original file with normalized audio");
    }

    AudioNormalizationResult result;
    result.file = file;
    result.fileName = file.getFileName();
    result.fullPath = file.getFullPathName();
    result.analysisRecord = AudioAnalysisService::analyzeFile(file);
    result.succeeded = !result.analysisRecord.hasError();

    if (!result.succeeded) {
        result.errorMessage = "The file was normalized, but re-analysis failed: " + result.analysisRecord.errorMessage;
    }

    return result;
}
