/// Implementation of AudioAnalysisService.
/// Decodes files in blocks through per-thread JUCE format readers
/// and feeds the samples to a libebur128 analyzer state to measure sample peak, true peak, and integrated loudness.
/// Also implements supported file discovery and record sorting.
/// The display formatting helpers live in AudioAnalysisFormatting.cpp.

#include "AudioAnalysisService.h"

#include "AudioFormatUtils.h"
#include "BitDepthDetection.h"
#include "DecodeFailureTracker.h"
#include "FileUtils.h"
#include "Log.h"
#include "StringFormat.h"

extern "C" {
#include <ebur128.h>
}

#include <initializer_list>

#include <algorithm>
#include <compare>
#include <memory>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

namespace audiobatch
{
/// Internal helpers for peak comparisons and stable record sorting.
namespace
{
/// How often the short-term loudness is sampled while decoding, in hertz.
/// EBU R128 updates short-term loudness at least ten times per second,
/// so a slower rate can miss the loudest three second window.
constexpr double shortTermUpdateRateHz = 10.0;
constexpr auto cancelledMessage = "Analysis was cancelled";

/// Left, right, and overall peak reduced from per-channel peaks.
template<typename Value>
struct PeakSummary {
    Value left;
    Value right;
    Value overall;
};

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
}  // namespace

/// Marks the record as failed with the given message and logs the error.
/// Cancellation is logged at debug level only,
/// because a rescan cancels every file still in flight and none of them failed.
/// Takes the record by value so callers can hand over a partially filled record with std::move.
static AudioAnalysisRecord failAnalysis(AudioAnalysisRecord record, const juce::String& message)
{
    record.status = AudioAnalysisStatus::failed;
    record.errorMessage = message;

    if (message == cancelledMessage) {
        utils::logDebug("Analysis cancelled for {}", record.fullPath.quoted());
    } else {
        utils::logError("Analysis failed for {}: {}", record.fullPath.quoted(), message);
    }

    return record;
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

/// Creates an ebur128 state configured for integrated, short-term, and true peak measurement.
/// A mono source is measured as dual mono, because it is played on both speakers,
/// so a mono file reads the same loudness as the same content in stereo
/// and loudness normalization brings both to the same perceived level.
static std::unique_ptr<ebur128_state, EbuR128StateDeleter> createLoudnessState(
    const int channelCount,
    const int sampleRate
)
{
    constexpr auto mode = static_cast<int>(
        static_cast<unsigned int>(EBUR128_MODE_I) | static_cast<unsigned int>(EBUR128_MODE_S)
        | static_cast<unsigned int>(EBUR128_MODE_TRUE_PEAK)
    );
    std::unique_ptr<ebur128_state, EbuR128StateDeleter> state(
        ebur128_init(static_cast<unsigned int>(channelCount), static_cast<unsigned long>(sampleRate), mode)
    );

    if (state != nullptr && channelCount == 1
        && ebur128_set_channel(state.get(), 0, EBUR128_DUAL_MONO) != EBUR128_SUCCESS)
    {
        return nullptr;
    }

    return state;
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

/// Returns true when the path is a supported audio file for analysis.
bool AudioAnalysisService::isSupportedAudioFile(const juce::File& file)
{
    if (!file.existsAsFile()) {
        return false;
    }

    const auto& formatManager = utils::threadLocalFormatManager();
    const auto extension = utils::fileExtension(file);
    return extension.isNotEmpty() && formatManager.findFormatForFileExtension(extension) != nullptr;
}

/// Expands the input paths into a de-duplicated list of supported files.
/// The directory walk checks isCancelled between entries and returns the files found so far when it is set,
/// so a scan of a large or slow folder can be abandoned quickly.
juce::Array<juce::File> AudioAnalysisService::collectInputFiles(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive,
    const utils::CancellationCheck& isCancelled
)
{
    juce::Array<juce::File> files;
    std::set<juce::String> seenPaths;

    auto const addFile = [&](const juce::File& file) {
        // Dot-prefixed files are skipped on every platform, because juce::File::isHidden only checks the hidden
        // attribute on Windows. This includes AppleDouble "._" files, which hold macOS metadata on non-Apple file
        // systems and are never audio.
        if (file.isHidden() || file.getFileName().startsWithChar('.') || !isSupportedAudioFile(file)) {
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
    const utils::CancellationCheck& isCancelled
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
            measurements.errorMessage = cancelledMessage;
            return measurements;
        }

        const auto framesThisBlock
            = static_cast<int>(juce::jmin<std::int64_t>(analysisBlockSize, reader.lengthInSamples - samplePosition));

        readBuffer.clear();

        const auto readSucceeded = utils::readFloatSamples(
            reader, readBuffer.getArrayOfWritePointers(), channelCount, samplePosition, framesThisBlock
        );

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

/// Analyzes a single supported audio file and returns the populated result record.
/// When isCancelled starts returning true, decoding stops and a failed record is returned,
/// which callers must not cache.
AudioAnalysisRecord AudioAnalysisService::analyzeFile(
    const juce::File& file,
    const utils::CancellationCheck& isCancelled
)
{
    auto record = AudioAnalysisRecord::fromFile(file);

    if (!file.existsAsFile()) {
        return failAnalysis(std::move(record), "File does not exist");
    }

    auto& formatManager = utils::threadLocalFormatManager();
    std::unique_ptr<juce::AudioFormatReader> const reader(formatManager.createReaderFor(file));

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

    const auto [minSamples, maxSamples, maxShortTermLoudness, errorMessage]
        = measureDecodedAudio(*reader, loudnessState.get(), record.fullPath, isCancelled);

    if (errorMessage.isNotEmpty()) {
        return failAnalysis(std::move(record), errorMessage);
    }

    std::vector signedPeaks(static_cast<size_t>(channelCount), 0.0f);

    for (int channel = 0; channel < channelCount; ++channel) {
        const auto channelIndex = static_cast<size_t>(channel);
        signedPeaks[channelIndex] = signedPeakFromExtrema(minSamples[channelIndex], maxSamples[channelIndex]);
    }

    const auto [peakLeft, peakRight, peakOverall] = summarizePeaks(signedPeaks);

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

    const auto [truePeakLeft, truePeakRight, truePeakOverall] = summarizePeaks(truePeaks);

    record.formatName = reader->getFormatName();
    record.sampleRate = sampleRate;
    record.channels = channelCount;
    record.bitsPerSample = analysis::resolveSourceBitsPerSample(*reader, file);
    record.lengthInSamples = reader->lengthInSamples;
    record.durationSeconds
        = reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
    record.peakLeft = peakLeft;
    record.peakRight = peakRight;
    record.overallPeak = peakOverall;
    record.truePeakLeft = truePeakLeft;
    record.truePeakRight = truePeakRight;
    record.overallTruePeak = truePeakOverall;
    record.maxShortTermLufs = maxShortTermLoudness;
    record.integratedLufs = normalizeLoudness(integratedLoudness);
    record.status = AudioAnalysisStatus::analyzed;
    record.fromCache = false;
    return record;
}

/// Sorts records by the requested field and direction.
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
}  // namespace audiobatch
