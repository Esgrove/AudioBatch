/// Console output of the CLI analysis results.
/// Declares printRecords, which prints analyzed records to stdout as an aligned table, CSV, or JSON,
/// and the CSV and JSON formatting it uses.

#pragma once

#include "AudioAnalysisCli.h"

#include <vector>

namespace audiobatch::cli
{
/// Returns the records as CSV text with a header line, one line per record, and a trailing newline.
/// The header is written even when there are no records, so scripts can always parse the columns.
juce::String formatRecordsAsCsv(const std::vector<AudioAnalysisRecord>& records);

/// Returns the records as a JSON array with a trailing newline, with null for silent levels.
juce::String formatRecordsAsJson(const std::vector<AudioAnalysisRecord>& records);

/// Prints the records in the requested format to stdout.
/// The table shows file names, or full paths when showFullPaths is set, as for normalization output.
/// The table shows only successful records, since failures are already logged to stderr,
/// while CSV and JSON include every record with its status and error.
void printRecords(
    const std::vector<AudioAnalysisRecord>& records,
    AudioAnalysisOutputFormat format,
    bool showFullPaths
);
}  // namespace audiobatch::cli
