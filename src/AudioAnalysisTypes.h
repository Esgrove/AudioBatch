#pragma once

#include <JuceHeader.h>

/// Lifecycle states for a file analysis record.
enum class AudioAnalysisStatus {
    pending = 0,
    cached = 1,
    analyzed = 2,
    failed = 3,
};

/// Available sort keys for CLI output and in-memory result lists.
enum class AudioAnalysisSortMode {
    peak,
    name,
    path,
    loudness,
};

/// Input parameters shared by the GUI and CLI analysis flows.
struct AudioAnalysisOptions {
    juce::Array<juce::File> inputPaths;
    bool recursive = false;
    bool refresh = false;
};

/// Analysis metadata and derived peak information for a single audio file.
struct AudioAnalysisRecord {
    static constexpr double negativeInfinityLoudness = -1000.0;

    juce::File file;
    juce::String fileName;
    juce::String fullPath;
    juce::String formatName;
    juce::String errorMessage;
    std::int64_t fileSize = 0;
    std::int64_t modifiedTimeMs = 0;
    std::int64_t lengthInSamples = 0;
    double durationSeconds = 0.0;
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    float overallPeak = 0.0f;
    double truePeakLeft = 0.0;
    double truePeakRight = 0.0;
    double overallTruePeak = 0.0;
    double maxShortTermLufs = negativeInfinityLoudness;
    double integratedLufs = negativeInfinityLoudness;
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    AudioAnalysisStatus status = AudioAnalysisStatus::pending;
    bool fromCache = false;

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
