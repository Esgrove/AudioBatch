/// Display formatting of analysis values shared by the GUI and CLI targets.
/// Declares the free functions that format peak, true peak, and loudness levels,
/// bitrate, sample rate, bit depth, file type, and status of an AudioAnalysisRecord as text.

#pragma once

#include "analysis/AudioAnalysisTypes.h"

namespace audiobatch
{
juce::String formatBitrateDisplay(const AudioAnalysisRecord& record);
juce::String formatBitsPerSampleDisplay(const AudioAnalysisRecord& record);
juce::String formatLoudnessCompact(double loudness);
juce::String formatLoudnessDisplay(double loudness);
juce::String formatPeakCompact(double peak);
juce::String formatPeakDisplay(float peak);
juce::String formatSampleRateDisplay(const AudioAnalysisRecord& record);
juce::String formatStatus(const AudioAnalysisRecord& record);
juce::String formatTruePeakCompact(double truePeak);
juce::String formatTruePeakDisplay(double truePeak);
juce::String formatTypeLabel(const AudioAnalysisRecord& record);
double getAverageBitrateKbps(const AudioAnalysisRecord& record);
}  // namespace audiobatch
