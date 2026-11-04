/// Display formatting of analysis values shared by the GUI and CLI targets.
/// Declares the free functions that format peak, true peak, and loudness levels,
/// bitrate, sample rate, bit depth, per-file gain, duration, file size, file type, and status as text.
/// Musical key mapping supplies a Camelot equivalent without altering the source tag.
/// Compact forms leave out the unit for the CLI and the results table, whose column headers name it.

#pragma once

#include "analysis/AudioAnalysisTypes.h"

#include <optional>

namespace audiobatch
{
std::optional<juce::String> camelotKey(const juce::String& key);

juce::String formatBitrateCompact(const AudioAnalysisRecord& record);
juce::String formatBitrateDisplay(const AudioAnalysisRecord& record);
juce::String formatBitsPerSampleDisplay(const AudioAnalysisRecord& record);
juce::String formatCustomGainCompact(const AudioAnalysisRecord& record);
juce::String formatDurationClock(double durationSeconds);
juce::String formatDurationClockTenths(double durationSeconds);
juce::String formatFileSizeDisplay(const AudioAnalysisRecord& record);
juce::String formatFolder(const juce::File& file);
juce::String formatFolderText(const juce::String& folder, bool macos);
juce::String formatGuiStatus(const AudioAnalysisRecord& record);
juce::String formatLoudnessCompact(double loudness);
juce::String formatPeakCompact(double peak);
juce::String formatSampleRateCompact(const AudioAnalysisRecord& record);
juce::String formatSampleRateDisplay(const AudioAnalysisRecord& record);
juce::String formatStatus(const AudioAnalysisRecord& record);
juce::String formatTruePeakCompact(double truePeak);
juce::String formatTypeLabel(const AudioAnalysisRecord& record);

double getAverageBitrateKbps(const AudioAnalysisRecord& record);
}  // namespace audiobatch
