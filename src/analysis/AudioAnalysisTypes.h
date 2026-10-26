/// Shared data types for the audio analysis pipeline.
/// Defines AudioAnalysisRecord, which carries per-file peak, true peak, loudness, and shared short-term history,
/// along with the AudioAnalysisStatus and AudioAnalysisSortMode enums
/// and the AudioAnalysisOptions input parameters used by both the GUI and CLI flows.
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
/// Input parameters shared by the GUI and CLI analysis flows.
struct AudioAnalysisOptions {
    juce::Array<juce::File> inputPaths;
    bool recursive {false};
    bool refresh {false};
};

/// Combined-channel short-term LUFS readings from the existing analysis blocks.
/// Each reading describes the trailing three seconds, with libebur128's zero-filled startup history.
/// Times are block ends, with the final partial block clamped to the source length.
struct ShortTermLoudnessHistory {
    [[nodiscard]] double timeAt(std::size_t index) const noexcept;

    /// EBU-mode meter update rate, independent of the three-second measurement window.
    static constexpr double updateRateHz {10.0};

    std::vector<float> values;
    double sampleRate {0.0};
    std::int64_t lengthInSamples {0};
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

/// Analysis metadata and derived peak information for a single audio file.
struct AudioAnalysisRecord {
    static constexpr double negativeInfinityLoudness {-1000.0};

    juce::File file;
    juce::String fileName;
    juce::String fullPath;
    juce::String formatName;
    juce::String errorMessage;
    std::shared_ptr<const ShortTermLoudnessHistory> shortTermLoudness;
    std::int64_t fileSize {0};
    std::int64_t modifiedTimeMs {0};
    std::int64_t lengthInSamples {0};
    double durationSeconds {0.0};
    float peakLeft {0.0f};
    float peakRight {0.0f};
    float overallPeak {0.0f};
    double truePeakLeft {0.0};
    double truePeakRight {0.0};
    double overallTruePeak {0.0};
    double maxShortTermLufs {negativeInfinityLoudness};
    double integratedLufs {negativeInfinityLoudness};
    int sampleRate {0};
    int channels {0};
    int bitsPerSample {0};
    /// Per-file gain in dB that the user set for this version of the file, which processing applies first.
    std::optional<float> customGain;
    AudioAnalysisStatus status {AudioAnalysisStatus::pending};
    /// Why the analysis failed, set on the records of failed analyses.
    std::optional<AnalysisError> error;
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
    static AudioAnalysisRecord fromFile(const juce::File& file)
    {
        AudioAnalysisRecord record;
        record.file = file;
        record.fileName = file.getFileName();
        record.fullPath = file.getFullPathName();
        record.fileSize = file.exists() ? file.getSize() : 0;
        record.modifiedTimeMs = file.exists() ? file.getLastModificationTime().toMilliseconds() : 0;
        return record;
    }
};

/// Returns true for an error that reading the same bytes again would repeat, so a cached failure stays valid.
[[nodiscard]] constexpr bool isRepeatable(const AnalysisError error) noexcept
{
    return error == AnalysisError::unsupported;
}

/// Returns true when neither gain is set, or both are set to approximately the same value.
[[nodiscard]] inline bool sameCustomGain(const std::optional<float> first, const std::optional<float> second)
{
    return first.has_value() == second.has_value() && (!first || juce::approximatelyEqual(*first, *second));
}

/// A normalization or plugin processing run that failed before writing its output, which leaves the source as it was.
struct OperationFailed {
    /// Why the run failed.
    juce::String error;
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
