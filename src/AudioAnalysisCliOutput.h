/// Console output of the CLI analysis results.
/// Declares printRecords, which prints analyzed records to stdout as an aligned table, CSV, or JSON.

#pragma once

#include "AudioAnalysisCli.h"

#include <vector>

namespace audiobatch::cli
{
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
