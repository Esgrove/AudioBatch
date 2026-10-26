/// Display formatting of analysis values shared by the GUI and CLI targets.
/// Declares the free functions that format peak, true peak, and loudness levels,
/// bitrate, sample rate, bit depth, per-file gain, duration, file type, and status of an AudioAnalysisRecord as text.
/// Compact forms leave out the unit for the CLI and the results table, whose column headers name it.

#pragma once

#include "analysis/AudioAnalysisTypes.h"

namespace audiobatch
{
juce::String formatBitrateCompact(const AudioAnalysisRecord& record);
juce::String formatBitrateDisplay(const AudioAnalysisRecord& record);
juce::String formatBitsPerSampleDisplay(const AudioAnalysisRecord& record);
juce::String formatCustomGainCompact(const AudioAnalysisRecord& record);
juce::String formatDurationClock(double durationSeconds);
juce::String formatDurationClockTenths(double durationSeconds);
juce::String formatLoudnessCompact(double loudness);
juce::String formatPeakCompact(double peak);
juce::String formatSampleRateCompact(const AudioAnalysisRecord& record);
juce::String formatSampleRateDisplay(const AudioAnalysisRecord& record);
juce::String formatStatus(const AudioAnalysisRecord& record);
juce::String formatTruePeakCompact(double truePeak);
juce::String formatTypeLabel(const AudioAnalysisRecord& record);
double getAverageBitrateKbps(const AudioAnalysisRecord& record);
}  // namespace audiobatch
