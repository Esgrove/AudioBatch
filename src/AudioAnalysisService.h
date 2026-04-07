#pragma once

#include "AudioAnalysisTypes.h"

#include <vector>

class AudioAnalysisService
{
public:
    static AudioAnalysisRecord analyzeFile(const juce::File& file);
    static juce::Array<juce::File> collectInputFiles(const juce::Array<juce::File>& inputPaths, bool recursive);
    static juce::String formatPeakDisplay(float peak);
    static juce::String formatStatus(const AudioAnalysisRecord& record);
    static bool isSupportedAudioFile(const juce::File& file);
    static void sortRecords(std::vector<AudioAnalysisRecord>& records, AudioAnalysisSortMode sortMode, bool ascending);

private:
    static juce::AudioFormatManager& getThreadLocalFormatManager();
};