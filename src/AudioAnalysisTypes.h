#pragma once

#include <JuceHeader.h>

enum class AudioAnalysisStatus {
    pending = 0,
    cached = 1,
    analyzed = 2,
    failed = 3,
};

enum class AudioAnalysisSortMode {
    peak,
    name,
    path,
};

struct AudioAnalysisOptions {
    juce::Array<juce::File> inputPaths;
    bool recursive = false;
    bool refresh = false;
};

struct AudioAnalysisRecord {
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
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    AudioAnalysisStatus status = AudioAnalysisStatus::pending;
    bool fromCache = false;

    [[nodiscard]] bool hasError() const noexcept
    {
        return status == AudioAnalysisStatus::failed;
    }
    [[nodiscard]] bool isReady() const noexcept
    {
        return status == AudioAnalysisStatus::cached || status == AudioAnalysisStatus::analyzed;
    }

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