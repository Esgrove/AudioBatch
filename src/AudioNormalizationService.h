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
    /// Rewrites a file so its peak reaches 0 dBFS, then re-analyzes the result.
    static AudioNormalizationResult normalizeFile(const AudioAnalysisRecord& record);

private:
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};
