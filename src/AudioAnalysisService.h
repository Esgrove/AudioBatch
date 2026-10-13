/// Audio analysis interface shared by the GUI and CLI targets.
/// AudioAnalysisService decodes audio files and measures sample peak, true peak,
/// and integrated loudness, and provides file discovery, display formatting,
/// and record sorting helpers built on the AudioAnalysisRecord types.

#pragma once

#include "AudioAnalysisTypes.h"
#include "ThreadUtils.h"

#include <vector>

namespace audiobatch
{
/// Stateless helpers for file discovery, audio analysis, and result formatting.
class AudioAnalysisService
{
public:
    static AudioAnalysisRecord analyzeFile(const juce::File& file, const utils::CancellationCheck& isCancelled = {});

    static juce::Array<juce::File> collectInputFiles(
        const juce::Array<juce::File>& inputPaths,
        bool recursive,
        const utils::CancellationCheck& isCancelled = {}
    );

    static juce::String formatBitrateDisplay(const AudioAnalysisRecord& record);
    static juce::String formatBitsPerSampleDisplay(const AudioAnalysisRecord& record);
    static juce::String formatLoudnessCompact(double loudness);
    static juce::String formatLoudnessDisplay(double loudness);
    static juce::String formatPeakCompact(double peak);
    static juce::String formatPeakDisplay(float peak);
    static juce::String formatSampleRateDisplay(const AudioAnalysisRecord& record);
    static juce::String formatStatus(const AudioAnalysisRecord& record);
    static juce::String formatTruePeakCompact(double truePeak);
    static juce::String formatTruePeakDisplay(double truePeak);
    static juce::String formatTypeLabel(const AudioAnalysisRecord& record);

    static bool isSupportedAudioFile(const juce::File& file);
    static double getAverageBitrateKbps(const AudioAnalysisRecord& record);
    static void sortRecords(std::vector<AudioAnalysisRecord>& records, AudioAnalysisSortMode sortMode, bool ascending);
};
}  // namespace audiobatch
