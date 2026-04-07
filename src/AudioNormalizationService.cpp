#include "AudioNormalizationService.h"

#include <unordered_map>

#include <cmath>

/// Helpers for format lookup, metadata passthrough, and chunked normalization.
namespace
{
constexpr int normalizationBlockSize = 32768;

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
}  // namespace

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
        return AudioNormalizationResult::failure(
            file, "This format can be read, but JUCE cannot write it back with the current build"
        );
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
