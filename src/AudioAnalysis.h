/// Audio analysis interface shared by the GUI and CLI targets.
/// Declares the free functions that decode audio files and measure sample peak, true peak,
/// and integrated loudness, and provide file discovery and record sorting built on the AudioAnalysisRecord types.
/// Display formatting is declared in AudioAnalysisFormatting.h.

#pragma once

#include "AudioAnalysisTypes.h"
#include "ThreadUtils.h"

#include <vector>

namespace audiobatch
{
AudioAnalysisRecord analyzeFile(const juce::File& file, const utils::CancellationCheck& isCancelled = {});

juce::Array<juce::File> collectInputFiles(
    const juce::Array<juce::File>& inputPaths,
    bool recursive,
    const utils::CancellationCheck& isCancelled = {}
);

bool isSupportedAudioFile(const juce::File& file);
void sortRecords(std::vector<AudioAnalysisRecord>& records, AudioAnalysisSortMode sortMode, bool ascending);
}  // namespace audiobatch
