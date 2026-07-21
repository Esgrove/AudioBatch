/// Audio analysis interface shared by the GUI and CLI targets.
/// AudioAnalysisService decodes audio files and measures sample peak, true peak,
/// and integrated loudness, and provides file discovery, display formatting,
/// and record sorting helpers built on the AudioAnalysisRecord types.

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

    /// Formats a true-peak value for display in the GUI.
    static juce::String formatTruePeakDisplay(double truePeak);

    /// Formats a loudness value for display in the GUI.
    static juce::String formatLoudnessDisplay(double loudness);

    /// Formats a peak value for compact CLI display without a unit suffix.
    static juce::String formatPeakCompact(double peak);

    /// Formats a true-peak value for compact CLI display without a unit suffix.
    static juce::String formatTruePeakCompact(double truePeak);

    /// Formats a loudness value for compact CLI display without a unit suffix.
    static juce::String formatLoudnessCompact(double loudness);

    /// Returns the average file bitrate in kbps when it can be derived from the analyzed file.
    static double getAverageBitrateKbps(const AudioAnalysisRecord& record);

    /// Formats the reported source bit depth for display, or "-" when unavailable.
    static juce::String formatBitsPerSampleDisplay(const AudioAnalysisRecord& record);

    /// Formats the average bitrate for display in the GUI.
    static juce::String formatBitrateDisplay(const AudioAnalysisRecord& record);

    /// Formats the sample rate for display in the GUI.
    static juce::String formatSampleRateDisplay(const AudioAnalysisRecord& record);

    /// Formats the human-readable status text for a record.
    static juce::String formatStatus(const AudioAnalysisRecord& record);

    /// Returns true when the path is a supported audio file for analysis.
    static bool isSupportedAudioFile(const juce::File& file);

    /// Sorts records by the requested field and direction.
    static void sortRecords(std::vector<AudioAnalysisRecord>& records, AudioAnalysisSortMode sortMode, bool ascending);

private:
    /// Returns a per-thread format manager with the basic JUCE formats registered.
    /// Thread-local state avoids locking when analysis jobs run on multiple worker threads.
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};
