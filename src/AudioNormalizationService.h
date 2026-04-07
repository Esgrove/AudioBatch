#pragma once

#include "AudioAnalysisService.h"

/// Result payload for a single normalize-and-reanalyze operation.
struct AudioNormalizationResult {
    juce::File file;
    juce::String fileName;
    juce::String fullPath;
    juce::String errorMessage;
    AudioAnalysisRecord analysisRecord;
    bool succeeded = false;

    /// Returns true when the normalize pass could not complete successfully.
    [[nodiscard]] bool hasError() const noexcept
    {
        return !succeeded;
    }

    /// Creates a failure result for the given file and message.
    static AudioNormalizationResult failure(const juce::File& file, const juce::String& message)
    {
        AudioNormalizationResult result;
        result.file = file;
        result.fileName = file.getFileName();
        result.fullPath = file.getFullPathName();
        result.errorMessage = message;
        return result;
    }
};

/// Stateless helpers for normalizing audio files and refreshing their analysis data.
class AudioNormalizationService
{
public:
    /// Returns true when the file's format can be rewritten by this build during normalization.
    static bool canNormalizeFile(const juce::File& file);

    /// Describes why normalization is unavailable for the given file format, or returns an empty string when supported.
    static juce::String getNormalizationSupportMessage(const juce::File& file);

    /// Summarizes the readable and writable formats available for in-place normalization in this build.
    static juce::String getFormatSupportSummary();

    /// Rewrites a file so its peak reaches 0 dBFS, then re-analyzes the result.
    static AudioNormalizationResult normalizeFile(const AudioAnalysisRecord& record);

private:
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};
