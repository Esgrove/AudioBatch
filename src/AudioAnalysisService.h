#pragma once

#include "AudioAnalysisTypes.h"

#include <vector>

/// Stateless helpers for file discovery, audio analysis, and result formatting.
class AudioAnalysisService
{
public:
    /// Analyzes a single supported audio file and returns the populated result record.
    static AudioAnalysisRecord analyzeFile(const juce::File& file);

    /// Expands the input paths into a de-duplicated list of supported files.
    static juce::Array<juce::File> collectInputFiles(const juce::Array<juce::File>& inputPaths, bool recursive);

    /// Formats a peak value for display in the GUI and CLI.
    static juce::String formatPeakDisplay(float peak);

    /// Formats the human-readable status text for a record.
    static juce::String formatStatus(const AudioAnalysisRecord& record);

    /// Returns true when the path is a supported audio file for analysis.
    static bool isSupportedAudioFile(const juce::File& file);

    /// Sorts records by the requested field and direction.
    static void sortRecords(std::vector<AudioAnalysisRecord>& records, AudioAnalysisSortMode sortMode, bool ascending);

private:
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};
