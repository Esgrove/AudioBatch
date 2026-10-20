/// Console output of the CLI analysis results.
/// Declares printRecords, which prints analyzed records to stdout as an aligned table, CSV, or JSON,
/// and the CSV and JSON formatting it uses.

#pragma once

#include "cli/AudioAnalysisCli.h"

#include <vector>

namespace audiobatch::cli
{
juce::String formatRecordsAsCsv(const std::vector<AudioAnalysisRecord>& records);

juce::String formatRecordsAsJson(const std::vector<AudioAnalysisRecord>& records);

void printRecords(
    const std::vector<AudioAnalysisRecord>& records,
    AudioAnalysisOutputFormat format,
    bool showFullPaths
);
}  // namespace audiobatch::cli
