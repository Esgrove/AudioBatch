/// Implementation of AudioAnalysisService.
/// Decodes files in blocks through per-thread JUCE format readers
/// and feeds the samples to a libebur128 analyzer state to measure sample peak,
/// true peak, and integrated loudness.
/// Also implements supported file discovery, the display and CLI formatting helpers, and record sorting.

#include "AudioAnalysisService.h"

#include "AudioFormatUtils.h"
#include "DecodeFailureTracker.h"
#include "FileUtils.h"
#include "Log.h"
#include "StringFormat.h"

extern "C" {
#include <ebur128.h>
}

#include <initializer_list>

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstdint>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <utility>
#include <vector>

/// Internal helpers for peak comparisons and stable record sorting.
namespace audiobatch::analysis
{
constexpr float minimumDisplayDecibels = -100.0f;
constexpr double kilobitsPerSecondDivisor = 1000.0;
constexpr int defaultMp3BitsPerSample = 16;
/// How often the short-term loudness is sampled while decoding, in hertz.
/// EBU R128 updates short-term loudness at least ten times per second,
/// so a slower rate can miss the loudest three second window.
constexpr double shortTermUpdateRateHz = 10.0;
/// Number of bits in a byte, used to turn a file size into a bit count for the average bitrate.
constexpr double bitsPerByte = 8.0;
/// Number of hertz in a kilohertz, used when displaying sample rates.
constexpr int hertzPerKilohertz = 1000;
/// Number of payload bits in each byte of an ID3v2 synchsafe integer.
/// The most significant bit of every byte is always zero.
constexpr std::uint32_t synchsafeBitsPerByte = 7U;

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

/// Returns true when the reader reports the decoder output bit depth instead of the source bit depth.
/// JUCE's MP3 reader is the known case, since decoding hides the original encoding depth.
static bool reportsDecodedBitDepth(const juce::AudioFormatReader& reader, const juce::File& file)
{
    if (utils::isMp3File(file)) {
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
static std::uint32_t readSynchsafeUint32(const std::span<const std::uint8_t, 4> bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) << (synchsafeBitsPerByte * 3U)
        | static_cast<std::uint32_t>(bytes[1]) << (synchsafeBitsPerByte * 2U)
        | static_cast<std::uint32_t>(bytes[2]) << synchsafeBitsPerByte | static_cast<std::uint32_t>(bytes[3]);
}

/// Flattens raw ID3 tag bytes into a lowercase text blob suitable for substring searches.
/// Alphanumeric characters and a few common separators are kept,
/// and every other run of bytes collapses into a single space.
static juce::String buildSearchableMetadataText(const juce::MemoryBlock& metadata)
{
    juce::String text;
    const std::span bytes(static_cast<const std::uint8_t*>(metadata.getData()), metadata.getSize());
    bool previousWasSeparator = true;

    for (const auto byte : bytes) {
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

    constexpr std::size_t sizeFieldOffset = 6;
    constexpr std::size_t sizeFieldLength = 4;
    const auto sizeField = std::span(header).subspan<sizeFieldOffset, sizeFieldLength>();
    if (std::ranges::any_of(sizeField, [](const std::uint8_t byte) { return (byte & 0x80U) != 0U; })) {
        return 0;
    }

    const auto payloadSize = readSynchsafeUint32(std::span(header).subspan<sizeFieldOffset, sizeFieldLength>());
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

/// Picks the sample extreme with the larger magnitude, preserving its sign.
static float signedPeakFromExtrema(const float minimum, const float maximum)
{
    return utils::peakMagnitude(minimum) > utils::peakMagnitude(maximum) ? minimum : maximum;
}

/// Returns whichever sample peak has the larger magnitude, keeping its sign.
static float dominantPeak(const float firstPeak, const float secondPeak)
{
    return utils::peakMagnitude(firstPeak) > utils::peakMagnitude(secondPeak) ? firstPeak : secondPeak;
}

/// Returns whichever true peak has the larger magnitude, keeping its sign.
static double dominantPeak(const double firstPeak, const double secondPeak)
{
    return utils::peakMagnitude(firstPeak) > utils::peakMagnitude(secondPeak) ? firstPeak : secondPeak;
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

/// Creates an ebur128 state configured for integrated, short-term, and true peak measurement.
static EbuR128StatePtr createLoudnessState(const int channelCount, const int sampleRate)
{
    constexpr auto mode = static_cast<int>(
        static_cast<unsigned int>(EBUR128_MODE_I) | static_cast<unsigned int>(EBUR128_MODE_S)
        | static_cast<unsigned int>(EBUR128_MODE_TRUE_PEAK)
    );
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

/// Returns the first ordering that is not equivalent, so later keys only break ties of earlier ones.
static std::weak_ordering firstDifference(const std::initializer_list<std::weak_ordering> orderings)
{
    const auto* const difference = std::ranges::find_if(orderings, [](const auto ordering) { return ordering != 0; });
    return difference != orderings.end() ? *difference : std::weak_ordering::equivalent;
}

/// Orders two floating point values exactly.
/// An approximate comparison would not be transitive, which std::sort requires,
/// and the other sort keys already break ties between equal values.
template<typename Value>
static std::weak_ordering compareValues(const Value lhs, const Value rhs)
{
    return std::weak_order(lhs, rhs);
}

/// Orders two records ascending by the sort key, with the other columns breaking ties.
static std::weak_ordering compareBySortMode(
    const AudioAnalysisRecord& lhs,
    const AudioAnalysisRecord& rhs,
    const AudioAnalysisSortMode sortMode
)
{
    const std::weak_ordering byName = lhs.fileName.compareNatural(rhs.fileName) <=> 0;
    const std::weak_ordering byPath = lhs.fullPath.compareNatural(rhs.fullPath) <=> 0;
    const auto byLoudness = compareValues(lhs.integratedLufs, rhs.integratedLufs);
    const auto byPeak = compareValues(utils::peakMagnitude(lhs.overallPeak), utils::peakMagnitude(rhs.overallPeak));

    switch (sortMode) {
        case AudioAnalysisSortMode::name:
            return firstDifference({byName, byPath});

        case AudioAnalysisSortMode::path:
            return firstDifference({byPath, byName});

        case AudioAnalysisSortMode::loudness:
            return firstDifference({byLoudness, byPeak, byName});

        case AudioAnalysisSortMode::peak:
            return firstDifference({byPeak, byLoudness, byName});
    }

    std::unreachable();
}
}  // namespace audiobatch::analysis

using namespace audiobatch::analysis;

bool AudioAnalysisService::isSupportedAudioFile(const juce::File& file)
{
    if (!file.existsAsFile()) {
        return false;
    }

    const auto& formatManager = utils::threadLocalFormatManager();
    const auto extension = utils::fileExtension(file);
    return extension.isNotEmpty() && formatManager.findFormatForFileExtension(extension) != nullptr;
}

juce::Array<juce::File> AudioAnalysisService::collectInputFiles(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive,
    const CancellationCheck& isCancelled
)
{
    juce::Array<juce::File> files;
    std::set<juce::String> seenPaths;

    auto const addFile = [&](const juce::File& file) {
        // AppleDouble "._" files hold macOS metadata on non-Apple file systems and are never audio.
        if (file.isHidden() || file.getFileName().startsWith("._") || !isSupportedAudioFile(file)) {
            return;
        }

        if (const auto& normalizedPath = file.getFullPathName(); seenPaths.insert(normalizedPath).second) {
            files.add(file);
        }
    };

    const auto cancelled = [&isCancelled] { return utils::isCancellationRequested(isCancelled); };

    for (const auto& inputPath : inputPaths) {
        if (cancelled()) {
            break;
        }

        if (inputPath.isDirectory()) {
            for (const auto& entry : juce::RangedDirectoryIterator(
                     inputPath, recursive, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles
                 ))
            {
                if (cancelled()) {
                    break;
                }

                addFile(entry.getFile());
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

/// Left, right, and overall peak reduced from per-channel peaks.
template<typename Value>
struct PeakSummary {
    Value left;
    Value right;
    Value overall;
};

/// Reduces per-channel peaks to the left, right, and overall peak.
/// A mono source reports its only channel as both left and right.
template<typename Value>
static PeakSummary<Value> summarizePeaks(const std::vector<Value>& channelPeaks)
{
    const auto left = channelPeaks.front();
    const auto right = channelPeaks.size() > 1 ? channelPeaks[1] : left;
    auto overall = left;

    for (const auto peak : std::views::drop(channelPeaks, 1)) {
        overall = dominantPeak(overall, peak);
    }

    return {left, right, overall};
}

/// Measurements gathered while decoding a file for analysis.
struct DecodeMeasurements {
    /// Smallest sample seen on each channel.
    std::vector<float> minSamples;
    /// Largest sample seen on each channel.
    std::vector<float> maxSamples;
    /// Highest short-term loudness seen across the file.
    double maxShortTermLoudness = AudioAnalysisRecord::negativeInfinityLoudness;
    /// Empty when decoding and measuring succeeded.
    juce::String errorMessage;
};

/// Interleaves one decoded block for the loudness meter and folds its samples into the per-channel extremes.
static void interleaveBlock(
    const juce::AudioBuffer<float>& readBuffer,
    const int framesThisBlock,
    std::vector<float>& interleaved,
    DecodeMeasurements& measurements
)
{
    const auto channelCount = static_cast<size_t>(readBuffer.getNumChannels());

    for (int frame = 0; frame < framesThisBlock; ++frame) {
        for (size_t channel = 0; channel < channelCount; ++channel) {
            const auto sample = readBuffer.getSample(static_cast<int>(channel), frame);
            measurements.minSamples[channel] = std::min(measurements.minSamples[channel], sample);
            measurements.maxSamples[channel] = std::max(measurements.maxSamples[channel], sample);
            interleaved[static_cast<size_t>(frame) * channelCount + channel] = sample;
        }
    }
}

/// Decodes the whole file block by block, feeding the loudness meter and collecting the sample extremes.
/// Read failures follow the shared DecodeFailureTracker rules.
/// A tolerated failed block still holds the samples decoded before the error with the remainder zeroed.
/// When decoding has to stop early, the measurement fails instead of reporting levels for part of the file.
/// Cancellation is checked before each block and also fails the measurement.
static DecodeMeasurements measureDecodedAudio(
    juce::AudioFormatReader& reader,
    ebur128_state* loudnessState,
    const juce::String& fullPath,
    const AudioAnalysisService::CancellationCheck& isCancelled
)
{
    const auto channelCount = static_cast<int>(reader.numChannels);

    DecodeMeasurements measurements;
    measurements.minSamples.assign(static_cast<size_t>(channelCount), 0.0f);
    measurements.maxSamples.assign(static_cast<size_t>(channelCount), 0.0f);

    // One block per short-term loudness update.
    const auto analysisBlockSize = juce::jmax(1, juce::roundToInt(reader.sampleRate / shortTermUpdateRateHz));
    juce::AudioBuffer<float> readBuffer(channelCount, analysisBlockSize);
    std::vector interleaved(static_cast<size_t>(channelCount * analysisBlockSize), 0.0f);
    DecodeFailureTracker failureTracker(reader.lengthInSamples);
    bool reportedPartialDecode = false;

    for (std::int64_t samplePosition = 0; samplePosition < reader.lengthInSamples; samplePosition += analysisBlockSize)
    {
        if (utils::isCancellationRequested(isCancelled)) {
            measurements.errorMessage = "Analysis was cancelled";
            return measurements;
        }

        const auto framesThisBlock
            = static_cast<int>(juce::jmin<std::int64_t>(analysisBlockSize, reader.lengthInSamples - samplePosition));

        readBuffer.clear();

        const auto readSucceeded = reader.read(&readBuffer, 0, framesThisBlock, samplePosition, true, true);

        if (!failureTracker.recordBlock(readSucceeded, samplePosition, framesThisBlock)) {
            measurements.errorMessage = failureTracker.getErrorMessage();
            return measurements;
        }

        if (failureTracker.getFailedBlockCount() > 0 && !reportedPartialDecode) {
            utils::logWarn(
                "Audio decode failed at sample {} / {} for {}, continuing analysis with decoded audio",
                failureTracker.getFirstFailurePosition(),
                reader.lengthInSamples,
                fullPath.quoted()
            );
            reportedPartialDecode = true;
        }

        interleaveBlock(readBuffer, framesThisBlock, interleaved, measurements);

        if (ebur128_add_frames_float(loudnessState, interleaved.data(), static_cast<size_t>(framesThisBlock))
            != EBUR128_SUCCESS)
        {
            measurements.errorMessage = "Loudness analysis failed while processing audio";
            return measurements;
        }

        double shortTermLoudness = AudioAnalysisRecord::negativeInfinityLoudness;

        if (ebur128_loudness_shortterm(loudnessState, &shortTermLoudness) == EBUR128_SUCCESS) {
            measurements.maxShortTermLoudness
                = std::max(measurements.maxShortTermLoudness, normalizeLoudness(shortTermLoudness));
        }
    }

    return measurements;
}

AudioAnalysisRecord AudioAnalysisService::analyzeFile(const juce::File& file, const CancellationCheck& isCancelled)
{
    auto record = AudioAnalysisRecord::fromFile(file);

    if (!file.existsAsFile()) {
        return failAnalysis(std::move(record), "File does not exist");
    }

    auto& formatManager = utils::threadLocalFormatManager();
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));

    if (reader == nullptr) {
        return failAnalysis(std::move(record), "Unsupported or unreadable audio file");
    }

    const auto channelCount = static_cast<int>(reader->numChannels);
    const auto sampleRate = juce::roundToInt(reader->sampleRate);

    if (channelCount <= 0 || sampleRate <= 0) {
        return failAnalysis(std::move(record), "Unsupported audio stream parameters");
    }

    auto const loudnessState = createLoudnessState(channelCount, sampleRate);

    if (loudnessState == nullptr) {
        return failAnalysis(std::move(record), "Could not initialize loudness analyzer");
    }

    const auto measurements = measureDecodedAudio(*reader, loudnessState.get(), record.fullPath, isCancelled);

    if (measurements.errorMessage.isNotEmpty()) {
        return failAnalysis(std::move(record), measurements.errorMessage);
    }

    std::vector signedPeaks(static_cast<size_t>(channelCount), 0.0f);

    for (int channel = 0; channel < channelCount; ++channel) {
        const auto channelIndex = static_cast<size_t>(channel);
        signedPeaks[channelIndex]
            = signedPeakFromExtrema(measurements.minSamples[channelIndex], measurements.maxSamples[channelIndex]);
    }

    const auto samplePeaks = summarizePeaks(signedPeaks);

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

    const auto truePeakSummary = summarizePeaks(truePeaks);

    record.formatName = reader->getFormatName();
    record.sampleRate = sampleRate;
    record.channels = channelCount;
    record.bitsPerSample = resolveSourceBitsPerSample(*reader, file);
    record.lengthInSamples = reader->lengthInSamples;
    record.durationSeconds
        = reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
    record.peakLeft = samplePeaks.left;
    record.peakRight = samplePeaks.right;
    record.overallPeak = samplePeaks.overall;
    record.truePeakLeft = truePeakSummary.left;
    record.truePeakRight = truePeakSummary.right;
    record.overallTruePeak = truePeakSummary.overall;
    record.maxShortTermLufs = measurements.maxShortTermLoudness;
    record.integratedLufs = normalizeLoudness(integratedLoudness);
    record.status = AudioAnalysisStatus::analyzed;
    record.fromCache = false;
    return record;
}

juce::String AudioAnalysisService::formatPeakDisplay(const float peak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(static_cast<double>(peak)), " dBFS");
}

juce::String AudioAnalysisService::formatTruePeakDisplay(const double truePeak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(truePeak), " dBTP");
}

juce::String AudioAnalysisService::formatLoudnessDisplay(const double loudness)
{
    return formatLoudnessValue(loudness, " LUFS");
}

juce::String AudioAnalysisService::formatPeakCompact(const double peak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(peak), {});
}

juce::String AudioAnalysisService::formatTruePeakCompact(const double truePeak)
{
    return formatAmplitudeDisplay(utils::peakMagnitude(truePeak), {});
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

    return static_cast<double>(record.fileSize) * bitsPerByte / record.durationSeconds / kilobitsPerSecondDivisor;
}

juce::String AudioAnalysisService::formatBitsPerSampleDisplay(const AudioAnalysisRecord& record)
{
    if (record.bitsPerSample <= 0) {
        return utils::isMp3File(record.file) ? "Unknown" : "-";
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

    if (record.sampleRate % hertzPerKilohertz == 0) {
        return utils::format("{} kHz", record.sampleRate / hertzPerKilohertz);
    }

    return utils::format("{:.1f} kHz", static_cast<double>(record.sampleRate) / hertzPerKilohertz);
}

juce::String AudioAnalysisService::formatTypeLabel(const AudioAnalysisRecord& record)
{
    if (const auto extension = utils::fileExtension(record.file); extension.isNotEmpty()) {
        return extension.toUpperCase();
    }

    // JUCE format names end with " file", such as "WAV file", which the label leaves out.
    const juce::String formatNameFileSuffix {" file"};
    auto formatName = record.formatName.trim();

    if (formatName.endsWithIgnoreCase(formatNameFileSuffix)) {
        formatName = formatName.dropLastCharacters(formatNameFileSuffix.length()).trimEnd();
    }

    return formatName.isNotEmpty() ? formatName : juce::String("Unknown");
}

juce::String AudioAnalysisService::formatStatus(const AudioAnalysisRecord& record)
{
    switch (record.status) {
        case AudioAnalysisStatus::cached:
            return "Cached";
        case AudioAnalysisStatus::analyzed:
            return "Analyzed";
        case AudioAnalysisStatus::failed:
            return "Failed";
        case AudioAnalysisStatus::pending:
            return "Pending";
    }

    std::unreachable();
}

void AudioAnalysisService::sortRecords(
    std::vector<AudioAnalysisRecord>& records,
    const AudioAnalysisSortMode sortMode,
    const bool ascending
)
{
    std::ranges::sort(records, [sortMode, ascending](const auto& lhs, const auto& rhs) {
        if (const auto stateComparison = compareAnalysisState(lhs, rhs); stateComparison != 0) {
            return stateComparison < 0;
        }

        const auto ordering = compareBySortMode(lhs, rhs, sortMode);
        return ascending ? ordering < 0 : ordering > 0;
    });
}
