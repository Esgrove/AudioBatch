/// Shared data types for the audio analysis pipeline.
/// Defines AudioAnalysisRecord, which carries per-file peak, true peak, loudness, and shared short-term history,
/// along with the AudioAnalysisStatus and AudioAnalysisSortMode enums
/// and the AudioAnalysisOptions input parameters used by both the GUI and CLI flows.

#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cstdint>
#include <memory>
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
    float customGainDb {0.0f};
    bool hasCustomGain {false};
    AudioAnalysisStatus status {AudioAnalysisStatus::pending};
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
}  // namespace audiobatch
