/// Implementation of the audio analysis functions.
/// Decodes files in blocks through per-thread JUCE format readers
/// and feeds the samples to a libebur128 analyzer state to measure sample peak, true peak, and integrated loudness.
/// Also implements supported file discovery and record sorting.
/// The display formatting helpers live in AudioAnalysisFormatting.cpp.

#include "analysis/AudioAnalysis.h"

#include "analysis/AudioAnalysisTypes.h"
#include "analysis/AudioMeasurement.h"
#include "analysis/BitDepthDetection.h"
#include "analysis/DecodeFailureTracker.h"
#include "analysis/OverloadHistory.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/ThreadUtils.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace audiobatch
{
namespace
{
constexpr auto CANCELLED_MESSAGE {"Analysis was cancelled"};

/// Preset-specific fit reproduced by fixtures/limiter_model.py from four full-file L2 renders.
constexpr double LIMITER_CEILING_OFFSET_LU {-0.30};
constexpr double LIMITER_LOSS_SCALE {1.3481396721815457};
constexpr double LIMITER_LOSS_EXPONENT {0.7035904112335774};
constexpr double LIMITER_MAX_GAIN_DB {6.0};

/// Why decoding or measuring a file failed.
struct MeasurementFailure {
    /// Kind of the failure, which decides whether the cache keeps it.
    AnalysisError error {AnalysisError::measurementFailed};
    /// The reason shown to the user.
    juce::String message;
};

/// Marks the record as failed with the given error and message, and logs the failure.
/// Cancellation is logged at debug level only,
/// because a rescan cancels every file still in flight and none of them failed.
/// Takes the record by value so callers can hand over a partially filled record with std::move.
AudioAnalysisRecord failAnalysis(AudioAnalysisRecord record, const AnalysisError error, const juce::String& message)
{
    record.status = AudioAnalysisStatus::failed;
    record.error = error;
    record.errorMessage = message;

    if (error == AnalysisError::cancelled) {
        utils::logDebug("Analysis cancelled for {}", record.fullPath.quoted());
    } else {
        utils::logError("Analysis failed for {}: {}", record.fullPath.quoted(), message);
    }

    return record;
}

/// Orders failed records after successful ones, regardless of the requested sort key.
int compareAnalysisState(const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs)
{
    if (lhs.hasError() != rhs.hasError()) {
        return lhs.hasError() ? 1 : -1;
    }

    return 0;
}

/// Returns the first ordering that is not equivalent, so later keys only break ties of earlier ones.
template<typename... Comparisons>
std::weak_ordering firstDifference(const Comparisons&... comparisons)
{
    auto ordering = std::weak_ordering::equivalent;
    static_cast<void>(((ordering = comparisons(), ordering != 0) || ...));
    return ordering;
}

/// Orders two floating point values exactly.
/// An approximate comparison would not be transitive, which std::sort requires,
/// and the other sort keys already break ties between equal values.
template<std::floating_point Value>
std::weak_ordering compareValues(const Value lhs, const Value rhs)
{
    return std::weak_order(lhs, rhs);
}

/// Orders two records ascending by the sort key, with the other columns breaking ties.
std::weak_ordering compareBySortMode(
    const AudioAnalysisRecord& lhs,
    const AudioAnalysisRecord& rhs,
    const AudioAnalysisSortMode sortMode
)
{
    const auto byName = [&] { return lhs.fileName.compareNatural(rhs.fileName) <=> 0; };
    const auto byPath = [&] { return lhs.fullPath.compareNatural(rhs.fullPath) <=> 0; };
    const auto byLoudness = [&] { return compareValues(lhs.integratedLufs, rhs.integratedLufs); };
    const auto byPeak
        = [&] { return compareValues(utils::peakMagnitude(lhs.overallPeak), utils::peakMagnitude(rhs.overallPeak)); };

    switch (sortMode) {
        case AudioAnalysisSortMode::name:
            return firstDifference(byName, byPath);

        case AudioAnalysisSortMode::path:
            return firstDifference(byPath, byName);

        case AudioAnalysisSortMode::loudness:
            return firstDifference(byLoudness, byPeak, byName);

        case AudioAnalysisSortMode::peak:
            return firstDifference(byPeak, byLoudness, byName);
    }

    std::unreachable();
}

/// Converts custom gain in double precision without a decibel floor, so very quiet samples can cross the gate.
double previewLinearGain(const Decibels gain)
{
    constexpr double decibelBase {10.0};
    constexpr double amplitudeDecibelScale {20.0};
    return std::pow(decibelBase, static_cast<double>(gain.value) / amplitudeDecibelScale);
}

/// Applies the transient model only when requested, rejecting non-finite PCM rather than inventing loudness.
bool applyPreviewGain(juce::AudioBuffer<float>& samples, const int frames, const std::optional<double> linearGain)
{
    for (int channel = 0; channel < samples.getNumChannels(); ++channel) {
        for (int frame = 0; frame < frames; ++frame) {
            const auto sample = static_cast<double>(samples.getSample(channel, frame));
            if (!std::isfinite(sample)) {
                return false;
            }
            if (linearGain) {
                samples.setSample(channel, frame, static_cast<float>(std::clamp(sample * *linearGain, -1.0, 1.0)));
            }
        }
    }
    return true;
}

/// Decodes the whole file block by block, feeding the loudness meter and collecting the sample extremes.
/// Read failures follow the shared DecodeFailureTracker rules.
/// A tolerated failed block still holds the samples decoded before the error with the remainder zeroed.
/// When decoding has to stop early, the measurement fails instead of reporting levels for part of the file.
/// Cancellation is checked before each block and also fails the measurement.
/// The decoded fraction of the file is reported to onProgress after each block.
std::expected<void, MeasurementFailure> measureDecodedAudio(
    juce::AudioFormatReader& reader,
    AudioMeasurement& measurement,
    const juce::String& fullPath,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress,
    const AnalysisPreviewObserver& onPreview,
    const std::optional<Decibels> gain = std::nullopt
)
{
    const auto channelCount = static_cast<int>(reader.numChannels);

    // One block per short-term loudness update, which is also one overload bucket before merging.
    const auto analysisBlockSize = overloadBlockSize(reader.sampleRate);
    juce::AudioBuffer<float> readBuffer(channelCount, analysisBlockSize);
    DecodeFailureTracker failureTracker(reader.lengthInSamples);
    bool reportedPartialDecode = false;
    const auto linearGain = gain ? std::optional(previewLinearGain(*gain)) : std::nullopt;

    for (std::int64_t samplePosition = 0; samplePosition < reader.lengthInSamples; samplePosition += analysisBlockSize)
    {
        if (utils::isCancellationRequested(isCancelled)) {
            return std::unexpected(
                MeasurementFailure {.error = AnalysisError::cancelled, .message = CANCELLED_MESSAGE}
            );
        }

        const auto framesThisBlock
            = static_cast<int>(juce::jmin<std::int64_t>(analysisBlockSize, reader.lengthInSamples - samplePosition));

        readBuffer.clear();

        const auto readSucceeded
            = utils::readFloatSamples(reader, utils::writePointers(readBuffer), samplePosition, framesThisBlock);

        if ((gain && !readSucceeded) || !failureTracker.recordBlock(readSucceeded, samplePosition, framesThisBlock)) {
            return std::unexpected(
                MeasurementFailure {
                    .error = AnalysisError::decodeFailed,
                    .message = failureTracker.getErrorMessage(),
                }
            );
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

        if (!applyPreviewGain(readBuffer, framesThisBlock, linearGain)) {
            return std::unexpected(
                MeasurementFailure {.error = AnalysisError::unsupported, .message = "Non-finite decoded sample"}
            );
        }
        if (const auto measured = measurement.push(readBuffer, 0, framesThisBlock); !measured) {
            return std::unexpected(
                MeasurementFailure {
                    .error = AnalysisError::measurementFailed,
                    .message = measured.error(),
                }
            );
        }

        const auto decodedFrames = samplePosition + framesThisBlock;
        if (onPreview) {
            try {
                onPreview(measurement, decodedFrames);
            } catch (...) {
                utils::logWarn(
                    "Analysis preview failed for {}: {}", fullPath.quoted(), utils::describeCurrentException()
                );
            }
        }

        utils::reportProgress(onProgress, utils::progressFraction(decodedFrames, reader.lengthInSamples));
    }

    return {};
}
}  // namespace

/// Returns true when the path is a supported audio file for analysis.
bool isSupportedAudioFile(const juce::File& file)
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
juce::Array<juce::File> collectInputFiles(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive,
    const utils::CancellationCheck& isCancelled
)
{
    juce::Array<juce::File> files;
    std::set<juce::String> seenPaths;

    auto const addFile = [&](const juce::File& file) {
        if (utils::isHiddenFile(file) || !isSupportedAudioFile(file)) {
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
            // The iterator follows every symlink by default, which never ends on a link that points to its parent.
            for (const auto& entry : juce::RangedDirectoryIterator(
                     inputPath,
                     recursive,
                     "*",
                     static_cast<int>(
                         static_cast<unsigned int>(juce::File::findFiles)
                         | static_cast<unsigned int>(juce::File::ignoreHiddenFiles)
                     ),
                     juce::File::FollowSymlinks::noCycles
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

/// Analyzes a single supported audio file and returns the populated result record.
/// When isCancelled starts returning true, decoding stops and a failed record is returned,
/// which callers must not cache.
/// The decoded fraction of the file is reported to onProgress while decoding.
/// onPreview borrows the live measurement after every successful block, before progress is reported.
/// Exceptions from this optional observer are logged without interrupting analysis.
AudioAnalysisRecord analyzeFile(
    const juce::File& file,
    const utils::CancellationCheck& isCancelled,
    const utils::ProgressCallback& onProgress,
    const AnalysisPreviewObserver& onPreview
)
{
    auto record = AudioAnalysisRecord::fromFile(file);

    if (!file.existsAsFile()) {
        return failAnalysis(std::move(record), AnalysisError::missingFile, "File does not exist");
    }

    const auto reader = utils::createAudioReader(file);

    if (reader == nullptr) {
        // JUCE returns no reader both for a format it cannot decode and for a file it cannot open,
        // and only the first fails the same way on the next run.
        if (file.createInputStream() == nullptr) {
            return failAnalysis(std::move(record), AnalysisError::unreadable, "File could not be opened for reading");
        }
        return failAnalysis(std::move(record), AnalysisError::unsupported, "Unsupported or unreadable audio file");
    }

    const auto channelCount = static_cast<int>(reader->numChannels);
    const auto sampleRate = juce::roundToInt(reader->sampleRate);

    // The loudness meter refuses streams outside its limits, which reading the file again cannot change.
    if (!isSupportedMeasurementStream(channelCount, sampleRate)) {
        return failAnalysis(std::move(record), AnalysisError::unsupported, "Unsupported audio stream parameters");
    }

    AudioMeasurement measurement(channelCount, reader->sampleRate, reader->lengthInSamples);
    if (const auto initialized = measurement.checkInitialization(); !initialized) {
        return failAnalysis(std::move(record), AnalysisError::measurementFailed, initialized.error());
    }

    if (const auto decoded
        = measureDecodedAudio(*reader, measurement, record.fullPath, isCancelled, onProgress, onPreview);
        !decoded)
    {
        return failAnalysis(std::move(record), decoded.error().error, decoded.error().message);
    }

    auto measured = measurement.finish();
    if (!measured) {
        return failAnalysis(std::move(record), AnalysisError::measurementFailed, measured.error());
    }

    measured->file = record.file;
    measured->fileName = record.fileName;
    measured->fullPath = record.fullPath;
    measured->fileSize = record.fileSize;
    measured->modifiedTimeMs = record.modifiedTimeMs;
    measured->formatName = reader->getFormatName();
    measured->bitsPerSample = analysis::resolveSourceBitsPerSample(*reader, file);
    return std::move(*measured);
}

/// Estimates integrated loudness for the saved FabFilter Pro-L 2 L2 preset with a -0.3 dB ceiling.
/// Uses the full-file hard-clip measurement and the source's gain-only loudness loss as a content proxy.
/// Calibrated on four tracks spanning -13.3286 to -8.676 LUFS-I, with gains from 0 through 6 dB.
/// Source loudness outside that range is extrapolated, not independently validated.
/// Returns no fit for invalid or silent measurements and gains outside the calibrated interval.
/// Presentation only: never replaces measured short-term loudness, peaks, stored analysis, or rendered audio.
std::optional<double> estimateLimiterIntegratedLoudness(
    const double sourceIntegratedLufs,
    const Decibels gain,
    const double hardClipIntegratedLufs
)
{
    if (!std::isfinite(sourceIntegratedLufs) || !std::isfinite(hardClipIntegratedLufs) || !std::isfinite(gain.value)
        || sourceIntegratedLufs <= AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS
        || hardClipIntegratedLufs <= AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS || gain.value < 0.0f
        || static_cast<double>(gain.value) > LIMITER_MAX_GAIN_DB)
    {
        return std::nullopt;
    }
    const auto loss = std::max(0.0, sourceIntegratedLufs + static_cast<double>(gain.value) - hardClipIntegratedLufs);
    const auto estimate = hardClipIntegratedLufs + LIMITER_CEILING_OFFSET_LU
        - LIMITER_LOSS_SCALE * std::pow(loss, LIMITER_LOSS_EXPONENT);
    return std::isfinite(estimate) ? std::optional {estimate} : std::nullopt;
}

/// Remeasures the complete gained, hard-clipped PCM stream, including absolute and relative loudness gating.
/// Uses bounded decode buffers and the source measurement cadence without allocating waveform or spectrogram previews.
/// Read-only and cache-independent. Any cancellation, changed file, or measurement failure yields no estimate.
std::optional<GainAdjustedLoudness> measureGainAdjustedLoudness(
    const juce::File& file,
    const Decibels gain,
    const utils::CancellationCheck& isCancelled
)
{
    if (!std::isfinite(gain.value) || !std::isfinite(previewLinearGain(gain))
        || utils::isCancellationRequested(isCancelled))
    {
        return std::nullopt;
    }
    const utils::FileIdentity identity(file);
    const auto reader = utils::createAudioReader(file);
    if (reader == nullptr || !identity.matches(file)
        || !isSupportedMeasurementStream(static_cast<int>(reader->numChannels), reader->sampleRate))
    {
        return std::nullopt;
    }
    AudioMeasurement measurement(
        static_cast<int>(reader->numChannels), reader->sampleRate, reader->lengthInSamples, false
    );
    if (!measurement.checkInitialization()
        || !measureDecodedAudio(*reader, measurement, file.getFullPathName(), isCancelled, {}, {}, gain)
        || utils::isCancellationRequested(isCancelled) || !identity.matches(file))
    {
        return std::nullopt;
    }
    const auto result = measurement.finish();
    if (!result || utils::isCancellationRequested(isCancelled) || !identity.matches(file)) {
        return std::nullopt;
    }
    return GainAdjustedLoudness {
        .integratedLufs = result->integratedLufs,
        .maxShortTermLufs = result->maxShortTermLufs,
    };
}

/// Sorts records by the requested field and direction.
void sortRecords(std::vector<AudioAnalysisRecord>& records, const AudioAnalysisSortMode sortMode, const bool ascending)
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
