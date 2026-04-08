#include "AudioAnalysisService.h"

extern "C" {
#include <ebur128.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

/// Internal helpers for peak comparisons and stable record sorting.
namespace
{
constexpr float minimumDisplayDecibels = -100.0f;
constexpr double kilobitsPerSecondDivisor = 1000.0;
constexpr int defaultMp3BitsPerSample = 16;
constexpr int analysisBlockSize = 8192;

struct EbuR128StateDeleter {
    void operator()(ebur128_state* state) const noexcept
    {
        if (state == nullptr) {
            return;
        }

        auto* stateToDestroy = state;
        ebur128_destroy(&stateToDestroy);
    }
};

using EbuR128StatePtr = std::unique_ptr<ebur128_state, EbuR128StateDeleter>;

juce::String normalizedExtension(const juce::File& file)
{
    auto extension = file.getFileExtension();

    if (extension.startsWithChar('.')) {
        extension = extension.substring(1);
    }

    return extension.toLowerCase();
}

bool reportsDecodedBitDepth(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (normalizedExtension(file) == "mp3") {
        return true;
    }

    return reader.getFormatName().containsIgnoreCase("mp3");
}

bool readExactly(juce::InputStream& input, void* destination, const int bytesToRead)
{
    return bytesToRead >= 0 && input.read(destination, bytesToRead) == bytesToRead;
}

std::uint32_t readBigEndianUint32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) << 24U | static_cast<std::uint32_t>(bytes[1]) << 16U
        | static_cast<std::uint32_t>(bytes[2]) << 8U | static_cast<std::uint32_t>(bytes[3]);
}

std::uint32_t readSynchsafeUint32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) << 21U | static_cast<std::uint32_t>(bytes[1]) << 14U
        | static_cast<std::uint32_t>(bytes[2]) << 7U | static_cast<std::uint32_t>(bytes[3]);
}

juce::String buildSearchableMetadataText(const juce::MemoryBlock& metadata)
{
    juce::String text;
    auto* bytes = static_cast<const std::uint8_t*>(metadata.getData());
    bool previousWasSeparator = true;

    for (size_t index = 0; index < metadata.getSize(); ++index) {
        const auto byte = bytes[index];

        if ((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
            text += juce::String::charToString(byte);
            previousWasSeparator = false;
            continue;
        }

        if (byte == '-' || byte == '_' || byte == '/' || byte == '.') {
            text += juce::String::charToString(byte);
            previousWasSeparator = false;
            continue;
        }

        if (!previousWasSeparator) {
            text += ' ';
            previousWasSeparator = true;
        }
    }

    return text.toLowerCase();
}

int findBitDepthInText(const juce::String& text)
{
    constexpr std::array candidateBitDepths {8, 12, 16, 20, 24, 32};

    for (const auto candidate : candidateBitDepths) {
        if (const auto value = juce::String(candidate);
            text.contains(value + "-bit") || text.contains(value + " bit") || text.contains(value + " bits"))
        {
            return candidate;
        }
    }

    for (const auto candidate : candidateBitDepths) {
        if (const auto value = juce::String(candidate); text.contains("bit depth " + value)
            || text.contains("bitdepth " + value) || text.contains("bits per sample " + value)
            || text.contains("source bit depth " + value) || text.contains("source bits per sample " + value))
        {
            return candidate;
        }
    }

    return 0;
}

int extractTaggedMp3BitDepth(const juce::File& file)
{
    const auto input = file.createInputStream();

    if (input == nullptr) {
        return 0;
    }

    std::array<std::uint8_t, 10> header {};

    if (!readExactly(*input, header.data(), static_cast<int>(header.size()))) {
        return 0;
    }

    if (!(header[0] == 'I' && header[1] == 'D' && header[2] == '3')) {
        return 0;
    }

    if ((header[6] | header[7] | header[8] | header[9]) & 0x80U) {
        return 0;
    }

    const auto payloadSize = readSynchsafeUint32(header.data() + 6);
    const auto hasFooter = (header[5] & 0x10U) != 0;
    const auto totalSize = static_cast<juce::int64>(header.size()) + static_cast<juce::int64>(payloadSize)
        + static_cast<juce::int64>(hasFooter ? 10 : 0);

    if (totalSize <= static_cast<juce::int64>(header.size()) || totalSize > file.getSize()) {
        return 0;
    }

    juce::MemoryBlock metadata;
    metadata.setSize(static_cast<size_t>(totalSize), false);
    input->setPosition(0);

    if (!readExactly(*input, metadata.getData(), static_cast<int>(totalSize))) {
        return 0;
    }

    return findBitDepthInText(buildSearchableMetadataText(metadata));
}

int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (reportsDecodedBitDepth(reader, file)) {
        if (const auto taggedBitDepth = extractTaggedMp3BitDepth(file); taggedBitDepth > 0) {
            return taggedBitDepth;
        }

        return defaultMp3BitsPerSample;
    }

    return reader.bitsPerSample;
}

template<typename Value>
Value peakMagnitude(const Value peak)
{
    return std::abs(peak);
}

float signedPeakFromExtrema(const float minimum, const float maximum)
{
    return peakMagnitude(minimum) > peakMagnitude(maximum) ? minimum : maximum;
}

float dominantPeak(const float firstPeak, const float secondPeak)
{
    return peakMagnitude(firstPeak) > peakMagnitude(secondPeak) ? firstPeak : secondPeak;
}

double dominantPeak(const double firstPeak, const double secondPeak)
{
    return peakMagnitude(firstPeak) > peakMagnitude(secondPeak) ? firstPeak : secondPeak;
}

double normalizeLoudness(const double loudness)
{
    if (!std::isfinite(loudness) || loudness <= AudioAnalysisRecord::negativeInfinityLoudness) {
        return AudioAnalysisRecord::negativeInfinityLoudness;
    }

    return loudness;
}

juce::String formatAmplitudeDisplay(const double amplitude, const juce::String& unitSuffix)
{
    if (amplitude <= 0.0) {
        return "-INF" + unitSuffix;
    }

    const auto decibels = juce::Decibels::gainToDecibels(static_cast<float>(amplitude), minimumDisplayDecibels);
    return juce::String::formatted("%.2f", decibels) + unitSuffix;
}

juce::String formatLoudnessValue(const double loudness, const juce::String& unitSuffix)
{
    if (loudness <= AudioAnalysisRecord::negativeInfinityLoudness) {
        return "-INF" + unitSuffix;
    }

    return juce::String::formatted("%.2f", loudness) + unitSuffix;
}

bool isEndOfFileReadFailure(
    const bool readSucceeded,
    const std::int64_t samplePosition,
    const int framesRead,
    const std::int64_t totalFrames
)
{
    return !readSucceeded && samplePosition + static_cast<std::int64_t>(framesRead) >= totalFrames;
}

EbuR128StatePtr createLoudnessState(const int channelCount, const int sampleRate)
{
    constexpr auto mode = EBUR128_MODE_I | EBUR128_MODE_S | EBUR128_MODE_TRUE_PEAK;
    return EbuR128StatePtr(
        ebur128_init(static_cast<unsigned int>(channelCount), static_cast<unsigned long>(sampleRate), mode)
    );
}

int compareAnalysisState(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    if (lhs.hasError() != rhs.hasError()) {
        return lhs.hasError() ? 1 : -1;
    }

    return 0;
}

int compareText(const juce::String& lhs, const juce::String& rhs)
{
    return lhs.compareNatural(rhs);
}
}  // namespace

juce::AudioFormatManager& AudioAnalysisService::getThreadLocalFormatManager()
{
    thread_local juce::AudioFormatManager formatManager;
    thread_local const bool initialized = [] {
        formatManager.registerBasicFormats();
        return true;
    }();

    juce::ignoreUnused(initialized);
    return formatManager;
}

bool AudioAnalysisService::isSupportedAudioFile(const juce::File& file)
{
    if (!file.existsAsFile()) {
        return false;
    }

    const auto& formatManager = getThreadLocalFormatManager();
    auto extension = file.getFileExtension();

    if (extension.startsWithChar('.')) {
        extension = extension.substring(1);
    }

    return extension.isNotEmpty() && formatManager.findFormatForFileExtension(extension) != nullptr;
}

juce::Array<juce::File> AudioAnalysisService::collectInputFiles(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive
)
{
    juce::Array<juce::File> files;
    std::set<juce::String> seenPaths;

    auto addFile = [&](const juce::File& file) {
        if (!isSupportedAudioFile(file)) {
            return;
        }

        if (const auto normalizedPath = file.getFullPathName(); seenPaths.insert(normalizedPath).second) {
            files.add(file);
        }
    };

    for (const auto& inputPath : inputPaths) {
        if (inputPath.isDirectory()) {
            juce::Array<juce::File> discoveredFiles;
            inputPath.findChildFiles(discoveredFiles, juce::File::findFiles, recursive, "*");

            for (const auto& file : discoveredFiles) {
                addFile(file);
            }
        } else {
            addFile(inputPath);
        }
    }

    std::ranges::sort(files, [](const juce::File& lhs, const juce::File& rhs) {
        return lhs.getFullPathName().compareNatural(rhs.getFullPathName()) < 0;
    });

    return files;
}

AudioAnalysisRecord AudioAnalysisService::analyzeFile(const juce::File& file)
{
    auto record = AudioAnalysisRecord::fromFile(file);

    if (!file.existsAsFile()) {
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = "File does not exist";
        return record;
    }

    auto& formatManager = getThreadLocalFormatManager();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = "Unsupported or unreadable audio file";
        return record;
    }

    const auto channelCount = static_cast<int>(reader->numChannels);
    const auto sampleRate = juce::roundToInt(reader->sampleRate);

    if (channelCount <= 0 || sampleRate <= 0) {
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = "Unsupported audio stream parameters";
        return record;
    }

    auto loudnessState = createLoudnessState(channelCount, sampleRate);

    if (loudnessState == nullptr) {
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = "Could not initialize loudness analyzer";
        return record;
    }

    std::vector minSamples(static_cast<size_t>(channelCount), 0.0f);
    std::vector maxSamples(static_cast<size_t>(channelCount), 0.0f);
    juce::AudioBuffer<float> readBuffer(channelCount, analysisBlockSize);
    std::vector interleaved(static_cast<size_t>(channelCount * analysisBlockSize), 0.0f);
    double maxShortTermLoudness = AudioAnalysisRecord::negativeInfinityLoudness;

    for (std::int64_t samplePosition = 0; samplePosition < reader->lengthInSamples; samplePosition += analysisBlockSize)
    {
        const auto remainingFrames = reader->lengthInSamples - samplePosition;
        const auto framesThisBlock = static_cast<int>(juce::jmin<std::int64_t>(analysisBlockSize, remainingFrames));

        readBuffer.clear();

        if (const auto readSucceeded = reader->read(&readBuffer, 0, framesThisBlock, samplePosition, true, true);
            !readSucceeded
            && !isEndOfFileReadFailure(readSucceeded, samplePosition, framesThisBlock, reader->lengthInSamples))
        {
            record.status = AudioAnalysisStatus::failed;
            record.errorMessage = "Audio decode failed during analysis";
            return record;
        }

        for (int frame = 0; frame < framesThisBlock; ++frame) {
            for (int channel = 0; channel < channelCount; ++channel) {
                const auto sample = readBuffer.getSample(channel, frame);
                minSamples[static_cast<size_t>(channel)] = std::min(minSamples[static_cast<size_t>(channel)], sample);
                maxSamples[static_cast<size_t>(channel)] = std::max(maxSamples[static_cast<size_t>(channel)], sample);
                interleaved[static_cast<size_t>(frame * channelCount + channel)] = sample;
            }
        }

        if (ebur128_add_frames_float(loudnessState.get(), interleaved.data(), static_cast<size_t>(framesThisBlock))
            != EBUR128_SUCCESS)
        {
            record.status = AudioAnalysisStatus::failed;
            record.errorMessage = "Loudness analysis failed while processing audio";
            return record;
        }

        double shortTermLoudness = AudioAnalysisRecord::negativeInfinityLoudness;

        if (ebur128_loudness_shortterm(loudnessState.get(), &shortTermLoudness) == EBUR128_SUCCESS) {
            maxShortTermLoudness = std::max(maxShortTermLoudness, normalizeLoudness(shortTermLoudness));
        }
    }

    std::vector signedPeaks(static_cast<size_t>(channelCount), 0.0f);

    for (int channel = 0; channel < channelCount; ++channel) {
        signedPeaks[static_cast<size_t>(channel)]
            = signedPeakFromExtrema(minSamples[static_cast<size_t>(channel)], maxSamples[static_cast<size_t>(channel)]);
    }

    const auto leftPeak = signedPeaks.front();
    const auto rightPeak = channelCount > 1 ? signedPeaks[1] : leftPeak;
    auto overallPeak = leftPeak;

    for (int channel = 1; channel < channelCount; ++channel) {
        overallPeak = dominantPeak(overallPeak, signedPeaks[static_cast<size_t>(channel)]);
    }

    std::vector truePeaks(static_cast<size_t>(channelCount), 0.0);

    for (int channel = 0; channel < channelCount; ++channel) {
        if (ebur128_true_peak(
                loudnessState.get(), static_cast<unsigned int>(channel), &truePeaks[static_cast<size_t>(channel)]
            )
            != EBUR128_SUCCESS)
        {
            record.status = AudioAnalysisStatus::failed;
            record.errorMessage = "True peak analysis failed";
            return record;
        }
    }

    double integratedLoudness = AudioAnalysisRecord::negativeInfinityLoudness;

    if (ebur128_loudness_global(loudnessState.get(), &integratedLoudness) != EBUR128_SUCCESS) {
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = "Integrated loudness analysis failed";
        return record;
    }

    const auto truePeakLeft = truePeaks.front();
    const auto truePeakRight = channelCount > 1 ? truePeaks[1] : truePeakLeft;
    auto overallTruePeak = truePeakLeft;

    for (int channel = 1; channel < channelCount; ++channel) {
        overallTruePeak = dominantPeak(overallTruePeak, truePeaks[static_cast<size_t>(channel)]);
    }

    record.formatName = reader->getFormatName();
    record.sampleRate = sampleRate;
    record.channels = channelCount;
    record.bitsPerSample = resolveSourceBitsPerSample(*reader, file);
    record.lengthInSamples = reader->lengthInSamples;
    record.durationSeconds
        = reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
    record.peakLeft = leftPeak;
    record.peakRight = rightPeak;
    record.overallPeak = overallPeak;
    record.truePeakLeft = truePeakLeft;
    record.truePeakRight = truePeakRight;
    record.overallTruePeak = overallTruePeak;
    record.maxShortTermLufs = maxShortTermLoudness;
    record.integratedLufs = normalizeLoudness(integratedLoudness);
    record.status = AudioAnalysisStatus::analyzed;
    record.fromCache = false;
    return record;
}

juce::String AudioAnalysisService::formatPeakDisplay(const float peak)
{
    return formatAmplitudeDisplay(peakMagnitude(peak), " dBFS");
}

juce::String AudioAnalysisService::formatTruePeakDisplay(const double truePeak)
{
    return formatAmplitudeDisplay(peakMagnitude(truePeak), " dBTP");
}

juce::String AudioAnalysisService::formatLoudnessDisplay(const double loudness)
{
    return formatLoudnessValue(loudness, " LUFS");
}

juce::String AudioAnalysisService::formatPeakCompact(const double peak)
{
    return formatAmplitudeDisplay(peakMagnitude(peak), {});
}

juce::String AudioAnalysisService::formatTruePeakCompact(const double truePeak)
{
    return formatAmplitudeDisplay(peakMagnitude(truePeak), {});
}

juce::String AudioAnalysisService::formatLoudnessCompact(const double loudness)
{
    return formatLoudnessValue(loudness, {});
}

double AudioAnalysisService::getAverageBitrateKbps(const AudioAnalysisRecord& record)
{
    if (record.durationSeconds <= 0.0 || record.fileSize <= 0) {
        return 0.0;
    }

    return static_cast<double>(record.fileSize) * 8.0 / record.durationSeconds / kilobitsPerSecondDivisor;
}

juce::String AudioAnalysisService::formatBitsPerSampleDisplay(const AudioAnalysisRecord& record)
{
    if (record.bitsPerSample <= 0) {
        return normalizedExtension(record.file) == "mp3" ? "Unknown" : "-";
    }

    return juce::String(record.bitsPerSample);
}

juce::String AudioAnalysisService::formatBitrateDisplay(const AudioAnalysisRecord& record)
{
    const auto bitrateKbps = getAverageBitrateKbps(record);

    if (bitrateKbps <= 0.0) {
        return "-";
    }

    return juce::String(juce::roundToInt(bitrateKbps)) + " kbps";
}

juce::String AudioAnalysisService::formatStatus(const AudioAnalysisRecord& record)
{
    switch (record.status) {
        case AudioAnalysisStatus::cached:
            return "Cached";
        case AudioAnalysisStatus::analyzed:
            return "Analyzed";
        case AudioAnalysisStatus::failed:
            return record.errorMessage.isNotEmpty() ? "Failed" : "Failed";
        case AudioAnalysisStatus::pending:
        default:
            return "Pending";
    }
}

void AudioAnalysisService::sortRecords(
    std::vector<AudioAnalysisRecord>& records,
    AudioAnalysisSortMode sortMode,
    bool ascending
)
{
    std::ranges::sort(records, [sortMode, ascending](const auto& lhs, const auto& rhs) {
        if (const auto stateComparison = compareAnalysisState(lhs, rhs); stateComparison != 0) {
            return stateComparison < 0;
        }

        auto comparePeaks = [ascending](const float left, const float right) {
            const auto leftMagnitude = peakMagnitude(left);
            const auto rightMagnitude = peakMagnitude(right);
            return ascending ? leftMagnitude < rightMagnitude : leftMagnitude > rightMagnitude;
        };

        auto compareStrings = [ascending](const juce::String& left, const juce::String& right) {
            return ascending ? compareText(left, right) < 0 : compareText(left, right) > 0;
        };

        switch (sortMode) {
            case AudioAnalysisSortMode::name:
                if (lhs.fileName != rhs.fileName) {
                    return compareStrings(lhs.fileName, rhs.fileName);
                }
                return compareStrings(lhs.fullPath, rhs.fullPath);

            case AudioAnalysisSortMode::path:
                if (lhs.fullPath != rhs.fullPath) {
                    return compareStrings(lhs.fullPath, rhs.fullPath);
                }
                return compareStrings(lhs.fileName, rhs.fileName);

            case AudioAnalysisSortMode::peak:
            default:
                if (!juce::approximatelyEqual(peakMagnitude(lhs.overallPeak), peakMagnitude(rhs.overallPeak))) {
                    return comparePeaks(lhs.overallPeak, rhs.overallPeak);
                }
                return compareStrings(lhs.fileName, rhs.fileName);
        }
    });
}
