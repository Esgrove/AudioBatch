/// Implementation of AudioAnalysisService.
/// Decodes files in blocks through per-thread JUCE format readers
/// and feeds the samples to a libebur128 analyzer state
/// to measure sample peak, true peak, and integrated loudness.
/// Also implements supported file discovery, the display and CLI formatting helpers, and record sorting.

#include "AudioAnalysisService.h"

#include "utils.h"

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
namespace audiobatch::analysis
{
constexpr float minimumDisplayDecibels = -100.0f;
constexpr double kilobitsPerSecondDivisor = 1000.0;
constexpr int defaultMp3BitsPerSample = 16;
constexpr int analysisBlockSize = 8192;
constexpr int maxConsecutiveReadFailures = 3;

/// Marks the record as failed with the given message and logs the error.
/// Takes the record by value so callers can hand over a partially filled record with std::move.
static AudioAnalysisRecord failAnalysis(AudioAnalysisRecord record, const juce::String& message)
{
    record.status = AudioAnalysisStatus::failed;
    record.errorMessage = message;
    utils::logError("Analysis failed for {}: {}", record.fullPath.quoted(), message);
    return record;
}

/// Deleter that lets a std::unique_ptr own an ebur128 analyzer state.
struct EbuR128StateDeleter {
    /// Destroys the analyzer state, safely ignoring null pointers.
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

/// Returns the file extension in lowercase without the leading dot.
static juce::String normalizedExtension(const juce::File& file)
{
    auto extension = file.getFileExtension();

    if (extension.startsWithChar('.')) {
        extension = extension.substring(1);
    }

    return extension.toLowerCase();
}

/// Returns true when the reader reports the decoder output bit depth instead of the source bit depth.
/// JUCE's MP3 reader is the known case, since decoding hides the original encoding depth.
static bool reportsDecodedBitDepth(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (normalizedExtension(file) == "mp3") {
        return true;
    }

    return reader.getFormatName().containsIgnoreCase("mp3");
}

/// Reads exactly the requested number of bytes from the stream.
/// Returns false on a short read, so callers can treat truncated data as missing.
static bool readExactly(juce::InputStream& input, void* destination, const int bytesToRead)
{
    return bytesToRead >= 0 && input.read(destination, bytesToRead) == bytesToRead;
}

/// Decodes a 28-bit synchsafe integer, the encoding ID3v2 uses for tag and frame sizes.
static std::uint32_t readSynchsafeUint32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) << 21U | static_cast<std::uint32_t>(bytes[1]) << 14U
        | static_cast<std::uint32_t>(bytes[2]) << 7U | static_cast<std::uint32_t>(bytes[3]);
}

/// Flattens raw ID3 tag bytes into a lowercase text blob suitable for substring searches.
/// Alphanumeric characters and a few common separators are kept,
/// and every other run of bytes collapses into a single space.
static juce::String buildSearchableMetadataText(const juce::MemoryBlock& metadata)
{
    juce::String text;
    const auto* bytes = static_cast<const std::uint8_t*>(metadata.getData());
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

/// Searches flattened metadata text for a plausible source bit depth mention.
/// Returns the matched depth, or 0 when no known pattern is present.
static int findBitDepthInText(const juce::String& text)
{
    constexpr std::array candidateBitDepths {8, 12, 16, 20, 24, 32};

    for (const auto candidate : candidateBitDepths) {
        if (const auto value = juce::String(candidate); text.contains(utils::format("{}-bit", value))
            || text.contains(utils::format("{} bit", value)) || text.contains(utils::format("{} bits", value)))
        {
            return candidate;
        }
    }

    for (const auto candidate : candidateBitDepths) {
        if (const auto value = juce::String(candidate); text.contains(utils::format("bit depth {}", value))
            || text.contains(utils::format("bitdepth {}", value))
            || text.contains(utils::format("bits per sample {}", value))
            || text.contains(utils::format("source bit depth {}", value))
            || text.contains(utils::format("source bits per sample {}", value)))
        {
            return candidate;
        }
    }

    return 0;
}

/// Scans the ID3v2 tag of an MP3 file for a mention of the original source bit depth.
/// Returns 0 when the file has no tag, the tag is malformed, or no bit depth is mentioned.
static int extractTaggedMp3BitDepth(const juce::File& file)
{
    const auto input = file.createInputStream();

    if (input == nullptr) {
        return 0;
    }

    constexpr juce::int64 id3HeaderSize = 10;
    std::array<std::uint8_t, id3HeaderSize> header {};

    if (!readExactly(*input, header.data(), static_cast<int>(header.size()))) {
        return 0;
    }

    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        return 0;
    }

    if (((header[6] | header[7] | header[8] | header[9]) & 0x80U) != 0U) {
        return 0;
    }

    const auto payloadSize = readSynchsafeUint32(header.data() + 6);
    const auto hasFooter = (header[5] & 0x10U) != 0;
    const auto totalSize
        = id3HeaderSize + static_cast<juce::int64>(payloadSize) + (hasFooter ? id3HeaderSize : juce::int64 {0});

    if (totalSize <= id3HeaderSize || totalSize > file.getSize()) {
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

/// Determines the best available source bit depth for the analyzed file.
/// MP3 files fall back to a tagged value or the default of 16,
/// because the decoder does not expose the original encoding depth.
static int resolveSourceBitsPerSample(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (reportsDecodedBitDepth(reader, file)) {
        if (const auto taggedBitDepth = extractTaggedMp3BitDepth(file); taggedBitDepth > 0) {
            return taggedBitDepth;
        }

        return defaultMp3BitsPerSample;
    }

    return static_cast<int>(reader.bitsPerSample);
}

/// Returns the absolute magnitude of a signed peak sample value.
template<typename Value>
static Value peakMagnitude(const Value peak)
{
    return std::abs(peak);
}

/// Picks the sample extreme with the larger magnitude, preserving its sign.
static float signedPeakFromExtrema(const float minimum, const float maximum)
{
    return peakMagnitude(minimum) > peakMagnitude(maximum) ? minimum : maximum;
}

/// Returns whichever sample peak has the larger magnitude, keeping its sign.
static float dominantPeak(const float firstPeak, const float secondPeak)
{
    return peakMagnitude(firstPeak) > peakMagnitude(secondPeak) ? firstPeak : secondPeak;
}

/// Returns whichever true peak has the larger magnitude, keeping its sign.
static double dominantPeak(const double firstPeak, const double secondPeak)
{
    return peakMagnitude(firstPeak) > peakMagnitude(secondPeak) ? firstPeak : secondPeak;
}

/// Clamps non-finite or silent loudness readings to the shared negative infinity sentinel.
static double normalizeLoudness(const double loudness)
{
    if (!std::isfinite(loudness) || loudness <= AudioAnalysisRecord::negativeInfinityLoudness) {
        return AudioAnalysisRecord::negativeInfinityLoudness;
    }

    return loudness;
}

/// Converts a linear amplitude into a decibel string with the given unit suffix.
/// Silence renders as "-INF" so it never shows a misleading finite value.
static juce::String formatAmplitudeDisplay(const double amplitude, const juce::String& unitSuffix)
{
    if (amplitude <= 0.0) {
        return utils::format("-INF{}", unitSuffix);
    }

    const auto decibels = juce::Decibels::gainToDecibels(static_cast<float>(amplitude), minimumDisplayDecibels);
    return utils::format("{:.2f}{}", decibels, unitSuffix);
}

/// Formats a LUFS loudness value with the given unit suffix, rendering the sentinel value as "-INF".
static juce::String formatLoudnessValue(const double loudness, const juce::String& unitSuffix)
{
    if (loudness <= AudioAnalysisRecord::negativeInfinityLoudness) {
        return utils::format("-INF{}", unitSuffix);
    }

    return utils::format("{:.2f}{}", loudness, unitSuffix);
}

/// Returns true when a failed read happened in the final block of the stream.
/// Such failures are expected for MP3 files whose reported length overshoots the decodable audio.
static bool isEndOfFileReadFailure(
    const bool readSucceeded,
    const std::int64_t samplePosition,
    const int framesRead,
    const std::int64_t totalFrames
)
{
    return !readSucceeded && samplePosition + static_cast<std::int64_t>(framesRead) >= totalFrames;
}

/// Creates an ebur128 state configured for integrated, short-term, and true peak measurement.
static EbuR128StatePtr createLoudnessState(const int channelCount, const int sampleRate)
{
    constexpr auto mode = EBUR128_MODE_I | EBUR128_MODE_S | EBUR128_MODE_TRUE_PEAK;
    return EbuR128StatePtr(
        ebur128_init(static_cast<unsigned int>(channelCount), static_cast<unsigned long>(sampleRate), mode)
    );
}

/// Orders failed records after successful ones, regardless of the requested sort key.
static int compareAnalysisState(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    if (lhs.hasError() != rhs.hasError()) {
        return lhs.hasError() ? 1 : -1;
    }

    return 0;
}

/// Compares strings using natural ordering so numbered file names sort as expected.
static int compareText(const juce::String& lhs, const juce::String& rhs)
{
    return lhs.compareNatural(rhs);
}
}  // namespace audiobatch::analysis

using namespace audiobatch::analysis;

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
        return failAnalysis(std::move(record), "File does not exist");
    }

    auto& formatManager = getThreadLocalFormatManager();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        return failAnalysis(std::move(record), "Unsupported or unreadable audio file");
    }

    const auto channelCount = static_cast<int>(reader->numChannels);
    const auto sampleRate = juce::roundToInt(reader->sampleRate);

    if (channelCount <= 0 || sampleRate <= 0) {
        return failAnalysis(std::move(record), "Unsupported audio stream parameters");
    }

    auto loudnessState = createLoudnessState(channelCount, sampleRate);

    if (loudnessState == nullptr) {
        return failAnalysis(std::move(record), "Could not initialize loudness analyzer");
    }

    std::vector minSamples(static_cast<size_t>(channelCount), 0.0f);
    std::vector maxSamples(static_cast<size_t>(channelCount), 0.0f);
    juce::AudioBuffer<float> readBuffer(channelCount, analysisBlockSize);
    std::vector interleaved(static_cast<size_t>(channelCount * analysisBlockSize), 0.0f);
    double maxShortTermLoudness = AudioAnalysisRecord::negativeInfinityLoudness;
    std::int64_t framesDecoded = 0;
    int consecutiveReadFailures = 0;
    bool reportedPartialDecode = false;

    for (std::int64_t samplePosition = 0; samplePosition < reader->lengthInSamples; samplePosition += analysisBlockSize)
    {
        const auto remainingFrames = reader->lengthInSamples - samplePosition;
        const auto framesThisBlock = static_cast<int>(juce::jmin<std::int64_t>(analysisBlockSize, remainingFrames));

        readBuffer.clear();

        if (const auto readSucceeded = reader->read(&readBuffer, 0, framesThisBlock, samplePosition, true, true);
            readSucceeded)
        {
            consecutiveReadFailures = 0;
            framesDecoded += framesThisBlock;
        } else if (!isEndOfFileReadFailure(readSucceeded, samplePosition, framesThisBlock, reader->lengthInSamples)) {
            // JUCE's built-in MP3 decoder can fail mid-stream (frame sync errors, overestimated stream length)
            // on files that other decoders handle fine.
            // A failed block still holds the samples decoded before the error with the remainder zeroed,
            // so keep analyzing instead of discarding the whole file.
            ++consecutiveReadFailures;

            if (!reportedPartialDecode) {
                utils::logWarn(
                    "Audio decode failed at sample {} / {} for {}, continuing analysis with decoded audio",
                    samplePosition,
                    reader->lengthInSamples,
                    record.fullPath.quoted()
                );
                reportedPartialDecode = true;
            }
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
            return failAnalysis(std::move(record), "Loudness analysis failed while processing audio");
        }

        double shortTermLoudness = AudioAnalysisRecord::negativeInfinityLoudness;

        if (ebur128_loudness_shortterm(loudnessState.get(), &shortTermLoudness) == EBUR128_SUCCESS) {
            maxShortTermLoudness = std::max(maxShortTermLoudness, normalizeLoudness(shortTermLoudness));
        }

        if (consecutiveReadFailures >= maxConsecutiveReadFailures) {
            // Repeated failures mean the rest of the stream is undecodable,
            // so finish the analysis with the audio decoded so far.
            break;
        }
    }

    if (reportedPartialDecode && framesDecoded == 0) {
        return failAnalysis(std::move(record), "Audio decode failed during analysis");
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
            return failAnalysis(std::move(record), "True peak analysis failed");
        }
    }

    double integratedLoudness = AudioAnalysisRecord::negativeInfinityLoudness;

    if (ebur128_loudness_global(loudnessState.get(), &integratedLoudness) != EBUR128_SUCCESS) {
        return failAnalysis(std::move(record), "Integrated loudness analysis failed");
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

    return utils::format("{} kbps", juce::roundToInt(bitrateKbps));
}

juce::String AudioAnalysisService::formatSampleRateDisplay(const AudioAnalysisRecord& record)
{
    if (record.sampleRate <= 0) {
        return "-";
    }

    if (record.sampleRate % 1000 == 0) {
        return utils::format("{} kHz", record.sampleRate / 1000);
    }

    return utils::format("{:.1f} kHz", static_cast<double>(record.sampleRate) / 1000.0);
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

        auto compareLoudness
            = [ascending](const double left, const double right) { return ascending ? left < right : left > right; };

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

            case AudioAnalysisSortMode::loudness:
                if (!juce::approximatelyEqual(lhs.integratedLufs, rhs.integratedLufs)) {
                    return compareLoudness(lhs.integratedLufs, rhs.integratedLufs);
                }
                if (!juce::approximatelyEqual(peakMagnitude(lhs.overallPeak), peakMagnitude(rhs.overallPeak))) {
                    return comparePeaks(lhs.overallPeak, rhs.overallPeak);
                }
                return compareStrings(lhs.fileName, rhs.fileName);

            case AudioAnalysisSortMode::peak:
            default:
                if (!juce::approximatelyEqual(peakMagnitude(lhs.overallPeak), peakMagnitude(rhs.overallPeak))) {
                    return comparePeaks(lhs.overallPeak, rhs.overallPeak);
                }
                if (!juce::approximatelyEqual(lhs.integratedLufs, rhs.integratedLufs)) {
                    return compareLoudness(lhs.integratedLufs, rhs.integratedLufs);
                }
                return compareStrings(lhs.fileName, rhs.fileName);
        }
    });
}
