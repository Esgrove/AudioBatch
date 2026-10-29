/// Shared data types for the audio analysis pipeline.
/// Defines AudioAnalysisRecord, which carries per-file peak, true peak, loudness, and shared short-term history,
/// along with the AudioAnalysisStatus and AudioAnalysisSortMode enums
/// and the AudioAnalysisOptions input parameters used by both the GUI and CLI flows.
/// LinearGain and Decibels keep the two gain units apart at the service boundaries that take a gain.
/// Also defines OperationOutcome, the variant that normalization and plugin processing return for each file,
/// and Overloaded, which visits such variants with one lambda per alternative.

#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace audiobatch
{
/// Lifecycle states for a file analysis record.
enum class AudioAnalysisStatus : std::uint8_t {
    pending = 0,
    cached = 1,
    analyzed = 2,
    failed = 3,
};

/// Available sort keys for CLI output and in-memory result lists.
enum class AudioAnalysisSortMode : std::uint8_t {
    peak,
    name,
    path,
    loudness,
};

/// Why an analysis failed, which decides whether the cache keeps the failed record.
enum class AnalysisError : std::uint8_t {
    /// The file does not exist.
    missingFile,
    /// The file exists but could not be opened for reading,
    /// for example because of its permissions or a network drive that went away, so a later run may succeed.
    unreadable,
    /// No decoder reads the file, or its stream parameters are unusable.
    /// Reading the same bytes again fails the same way.
    unsupported,
    /// Too many audio blocks failed to decode, which a transient read error, such as on a network drive, can cause.
    decodeFailed,
    /// The loudness measurement failed.
    measurementFailed,
    /// The analysis was cancelled before it finished.
    cancelled,
    /// An unexpected exception stopped the analysis.
    exception,
};

/// A linear amplitude factor, where 1 leaves the signal unchanged.
/// A separate type from Decibels, so a gain in one unit cannot be passed where the other is expected.
struct LinearGain {
    /// The factor the samples are multiplied by.
    float value {1.0f};
};

/// A gain in decibels, where 0 dB leaves the signal unchanged.
struct Decibels {
    /// Returns the linear factor that applies this gain, with JUCE's default -100 dB floor.
    [[nodiscard]] LinearGain toLinearGain() const
    {
        return LinearGain {juce::Decibels::decibelsToGain(value)};
    }

    /// The gain in decibels.
    float value {0.0f};
};

/// Input parameters shared by the GUI and CLI analysis flows.
struct AudioAnalysisOptions {
    /// Files and folders to analyze.
    juce::Array<juce::File> inputPaths;
    /// True to also analyze the files in the subfolders of the given folders.
    bool recursive {false};
    /// True to analyze every file again instead of reading unchanged files from the cache.
    bool refresh {false};
};

/// Combined-channel short-term LUFS readings from the existing analysis blocks.
/// Each reading describes the trailing three seconds, with libebur128's zero-filled startup history.
/// Times are block ends, with the final partial block clamped to the source length.
struct ShortTermLoudnessHistory {
    [[nodiscard]] double timeAt(std::size_t index) const noexcept;

    /// EBU-mode meter update rate, independent of the three-second measurement window.
    static constexpr double updateRateHz {10.0};

    /// Short-term loudness readings in LUFS, one per block in time order.
    std::vector<float> values;
    /// Sample rate of the analyzed file.
    double sampleRate {0.0};
    /// Length of the analyzed file in samples.
    std::int64_t lengthInSamples {0};
    /// Source samples per reading, a tenth of a second at the file's sample rate.
    int samplesPerPoint {0};
};

/// Returns the exact measurement time, including a final block shorter than the normal interval.
inline double ShortTermLoudnessHistory::timeAt(const std::size_t index) const noexcept
{
    if (sampleRate <= 0.0 || samplesPerPoint <= 0 || lengthInSamples <= 0 || index >= values.size()) {
        return 0.0;
    }
    const auto position = std::min(
        static_cast<double>(index + 1) * static_cast<double>(samplesPerPoint), static_cast<double>(lengthInSamples)
    );
    return position / sampleRate;
}

/// Per-channel source overload flags over the length of a file, one byte per bucket and channel.
/// A bucket covers samplesPerBucket source frames, the final one possibly fewer.
/// Bit sampleOver marks a decoded sample whose magnitude exceeds full scale,
/// and bit truePeakOver marks a true peak above 0 dBTP that the loudness meter reported for the bucket.
/// The true peak flags are block-local maxima without a precise time,
/// so they mark the analysis blocks the meter measured them in.
/// A history with no flags set describes a file without overloads, unlike a missing history.
struct OverloadHistory {
    [[nodiscard]] std::uint8_t flagsAt(std::size_t bucket, int channel) const noexcept;
    [[nodiscard]] std::size_t bucketCount() const noexcept;
    [[nodiscard]] double bucketStartTime(std::size_t bucket) const noexcept;
    [[nodiscard]] double bucketEndTime(std::size_t bucket) const noexcept;
    [[nodiscard]] bool hasOverloads() const noexcept;

    /// Flag bit of a decoded sample whose magnitude exceeds 1.0.
    static constexpr std::uint8_t sampleOver {0x01};
    /// Flag bit of a true peak above 1.0.
    static constexpr std::uint8_t truePeakOver {0x02};
    /// Every legal flag bit.
    static constexpr std::uint8_t allFlags {sampleOver | truePeakOver};
    /// Largest number of buckets, reached by merging neighbouring buckets of longer files.
    static constexpr std::size_t maximumBuckets {8192};

    /// Bucket flags in time order, channelCount bytes per bucket.
    std::vector<std::uint8_t> flags;
    /// Sample rate of the analyzed file.
    double sampleRate {0.0};
    /// Length of the analyzed file in samples.
    std::int64_t lengthInSamples {0};
    /// Source frames per bucket, the analysis block size times a power of two.
    std::int64_t samplesPerBucket {0};
    /// Number of channels the flags describe.
    int channelCount {0};
};

/// Returns the flags of one bucket and channel, or zero outside the history.
inline std::uint8_t OverloadHistory::flagsAt(const std::size_t bucket, const int channel) const noexcept
{
    if (channelCount <= 0 || channel < 0 || channel >= channelCount) {
        return 0;
    }
    const auto index = bucket * static_cast<std::size_t>(channelCount) + static_cast<std::size_t>(channel);
    return index < flags.size() ? flags[index] : std::uint8_t {0};
}

/// Returns the number of time buckets.
inline std::size_t OverloadHistory::bucketCount() const noexcept
{
    return channelCount > 0 ? flags.size() / static_cast<std::size_t>(channelCount) : 0;
}

/// Returns the start of a bucket in seconds.
inline double OverloadHistory::bucketStartTime(const std::size_t bucket) const noexcept
{
    if (sampleRate <= 0.0 || samplesPerBucket <= 0) {
        return 0.0;
    }
    const auto position = std::min(
        static_cast<double>(bucket) * static_cast<double>(samplesPerBucket), static_cast<double>(lengthInSamples)
    );
    return position / sampleRate;
}

/// Returns the end of a bucket in seconds, with the final partial bucket clamped to the source length.
inline double OverloadHistory::bucketEndTime(const std::size_t bucket) const noexcept
{
    return bucketStartTime(bucket + 1);
}

/// Returns true when any bucket of any channel has a flag set.
inline bool OverloadHistory::hasOverloads() const noexcept
{
    return std::ranges::any_of(flags, [](const auto value) { return value != 0; });
}

/// Analysis metadata and derived peak information for a single audio file.
struct AudioAnalysisRecord {
    /// Loudness that stands for silence, where no finite LUFS value exists.
    static constexpr double negativeInfinityLoudness {-1000.0};

    /// The analyzed file.
    juce::File file;
    /// Name of the file with its extension.
    juce::String fileName;
    /// Absolute path of the file, which identifies its table row and its cache row.
    juce::String fullPath;
    /// Name of the audio format that decoded the file, such as "WAV file".
    juce::String formatName;
    /// Why the analysis failed, or empty when it did not.
    juce::String errorMessage;
    /// Short-term loudness over the length of the file, shared between copies of the record.
    std::shared_ptr<const ShortTermLoudnessHistory> shortTermLoudness;
    /// Source sample and true peak overloads over the length of the file, shared between copies of the record.
    std::shared_ptr<const OverloadHistory> overloads;
    /// Size of the file in bytes when it was analyzed.
    std::int64_t fileSize {0};
    /// Modification time of the file when it was analyzed, in milliseconds since the epoch.
    std::int64_t modifiedTimeMs {0};
    /// Length of the audio in samples per channel.
    std::int64_t lengthInSamples {0};
    /// Length of the audio in seconds.
    double durationSeconds {0.0};
    /// Sample peak of the left or only channel as a linear sample value that keeps its sign.
    float peakLeft {0.0f};
    /// Sample peak of the right channel as a linear sample value that keeps its sign, the left one for a mono file.
    float peakRight {0.0f};
    /// The sample peak of the channel with the largest magnitude.
    float overallPeak {0.0f};
    /// True peak of the left or only channel as a linear amplitude.
    double truePeakLeft {0.0};
    /// True peak of the right channel as a linear amplitude, the left one for a mono file.
    double truePeakRight {0.0};
    /// The larger of the channel true peaks.
    double overallTruePeak {0.0};
    /// Largest short-term loudness in LUFS, or negativeInfinityLoudness for silence.
    double maxShortTermLufs {negativeInfinityLoudness};
    /// Integrated loudness in LUFS, or negativeInfinityLoudness for silence.
    double integratedLufs {negativeInfinityLoudness};
    /// Sample rate of the file in hertz.
    int sampleRate {0};
    /// Number of audio channels of the file.
    int channels {0};
    /// Bit depth of the source audio, as BitDepthDetection resolves it.
    int bitsPerSample {0};
    /// Per-file gain that the user set for this version of the file, which processing applies first.
    std::optional<Decibels> customGain;
    /// Where the record is in its lifecycle, from a pending placeholder to an analyzed, cached, or failed file.
    AudioAnalysisStatus status {AudioAnalysisStatus::pending};
    /// Why the analysis failed, set on the records of failed analyses.
    std::optional<AnalysisError> error;
    /// True when the record was read from the analysis cache instead of decoding the file.
    bool fromCache {false};

    /// Returns true when the record represents a failed analysis attempt.
    [[nodiscard]] bool hasError() const noexcept
    {
        return status == AudioAnalysisStatus::failed;
    }

    /// Returns true when the record has usable cached or freshly analyzed data.
    [[nodiscard]] bool isReady() const noexcept
    {
        return status == AudioAnalysisStatus::cached || status == AudioAnalysisStatus::analyzed;
    }

    /// Returns true when the file still has the size and modification time recorded at analysis,
    /// so the measured levels still describe it.
    [[nodiscard]] bool matchesFileOnDisk() const
    {
        return file.existsAsFile() && file.getSize() == fileSize
            && file.getLastModificationTime().toMilliseconds() == modifiedTimeMs;
    }

    /// Builds a baseline record from filesystem metadata before analysis begins.
    /// A missing file gets a size and modification time of zero,
    /// which JUCE reports for it without a separate existence check.
    static AudioAnalysisRecord fromFile(const juce::File& file)
    {
        AudioAnalysisRecord record;
        record.file = file;
        record.fileName = file.getFileName();
        record.fullPath = file.getFullPathName();
        record.fileSize = file.getSize();
        record.modifiedTimeMs = file.getLastModificationTime().toMilliseconds();
        return record;
    }
};

/// Returns true for an error that reading the same bytes again would repeat, so a cached failure stays valid.
[[nodiscard]] constexpr bool isRepeatable(const AnalysisError error) noexcept
{
    return error == AnalysisError::unsupported;
}

/// Returns true when neither gain is set, or both are set to approximately the same value.
[[nodiscard]] inline bool sameCustomGain(const std::optional<Decibels> first, const std::optional<Decibels> second)
{
    return first.has_value() == second.has_value() && (!first || juce::approximatelyEqual(first->value, second->value));
}

/// A normalization or plugin processing run that failed before writing its output, which leaves the source as it was.
struct OperationFailed {
    /// Why the run failed.
    juce::String error;
    /// True when the run stopped because the user cancelled it, before changing the file,
    /// so the file is shown as it was instead of as a failure.
    bool cancelled {false};
};

/// A run that wrote its output, which then could not be analyzed.
/// A converted source is in the trash by then, so the output takes the place of the source either way.
struct CommittedButUnanalyzed {
    /// The failed analysis of the output file.
    AudioAnalysisRecord output;
    /// Why the run counts as failed.
    juce::String error;
};

/// A run that wrote and analyzed its output, or found the file already at its target and analyzed it again.
struct OperationSucceeded {
    /// The analysis of the output file.
    AudioAnalysisRecord output;
    /// Information about the run worth showing to the user, such as blocks that failed to decode, or empty.
    juce::String notice;
};

/// What a normalization or plugin processing run did with one file.
using OperationOutcome = std::variant<OperationFailed, CommittedButUnanalyzed, OperationSucceeded>;

/// Combines callables into one with an overload of the call operator from each of them,
/// for visiting a std::variant such as OperationOutcome with one lambda per alternative.
/// Class template argument deduction picks the types, as in `std::visit(Overloaded {lambdas...}, variant)`.
template<typename... Callables>
struct Overloaded : Callables... {
    using Callables::operator()...;
};
}  // namespace audiobatch
