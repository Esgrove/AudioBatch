/// Implementation of the CLI result output:
/// the aligned peak, true peak, and loudness table, and the CSV and JSON records with null for silence.

#include "cli/AudioAnalysisCliOutput.h"

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "utils/ConsolePrint.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"

#include <cmath>

namespace audiobatch::cli
{
constexpr int cliPeakColumnWidth = 7;
constexpr int cliTruePeakColumnWidth = 7;
constexpr int cliLoudnessColumnWidth = 8;

/// Formats the sample peak value right-aligned to the fixed dBFS column width.
static juce::String formattedPeakColumn(const AudioAnalysisRecord& record)
{
    return formatPeakCompact(static_cast<double>(record.overallPeak)).paddedLeft(' ', cliPeakColumnWidth);
}

/// Formats the true peak value right-aligned to the fixed dBTP column width.
static juce::String formattedTruePeakColumn(const AudioAnalysisRecord& record)
{
    return formatTruePeakCompact(record.overallTruePeak).paddedLeft(' ', cliTruePeakColumnWidth);
}

/// Formats the integrated loudness value right-aligned to the fixed LUFS column width.
static juce::String formattedIntegratedLoudnessColumn(const AudioAnalysisRecord& record)
{
    return formatLoudnessCompact(record.integratedLufs).paddedLeft(' ', cliLoudnessColumnWidth);
}

/// Prints the bold column header line to stdout, matching the widths used by the result rows.
static void printHeaderRow()
{
    console::printBold(
        "{:>{}}  {:>{}}  {:>{}}  TRACK\n",
        "dBFS",
        cliPeakColumnWidth,
        "dBTP",
        cliTruePeakColumnWidth,
        "LUFS-I",
        cliLoudnessColumnWidth
    );
}

/// Prints one aligned result line to stdout.
/// The caller chooses the track label so plain analysis can show file names
/// while normalization output can show full output paths.
static void printResultRow(const AudioAnalysisRecord& record, const juce::String& trackLabel)
{
    fmt::print(
        "{}  {}  {}  {}\n",
        formattedPeakColumn(record),
        formattedTruePeakColumn(record),
        formattedIntegratedLoudnessColumn(record),
        trackLabel
    );
}

/// Picks the most informative path available for a normalized record,
/// falling back from the cached full path to the file object and finally the bare file name.
static juce::String reportedOutputPath(const AudioAnalysisRecord& record)
{
    if (record.fullPath.isNotEmpty()) {
        return record.fullPath;
    }

    if (record.file != juce::File()) {
        return record.file.getFullPathName();
    }

    return record.fileName;
}

/// Converts a linear amplitude to dB rounded to two decimals,
/// or returns a void var, which JSON writes as null, for silence.
static juce::var decibelsOrNull(const double amplitude)
{
    if (amplitude <= 0.0) {
        return {};
    }

    return std::round(juce::Decibels::gainToDecibels(amplitude, AudioAnalysisRecord::negativeInfinityLoudness) * 100.0)
        / 100.0;
}

/// Rounds a loudness value to two decimals,
/// or returns a void var, which JSON writes as null, for the negative infinity sentinel.
static juce::var loudnessOrNull(const double loudness)
{
    if (loudness <= AudioAnalysisRecord::negativeInfinityLoudness) {
        return {};
    }

    return std::round(loudness * 100.0) / 100.0;
}

/// Builds the machine-readable fields of one record, shared by the CSV and JSON output.
/// The keys keep their insertion order, which is also the CSV column order.
static juce::DynamicObject::Ptr buildRecordObject(const AudioAnalysisRecord& record)
{
    juce::DynamicObject::Ptr object = new juce::DynamicObject();
    object->setProperty("path", reportedOutputPath(record));
    object->setProperty("name", record.fileName);
    object->setProperty("format", record.formatName);
    object->setProperty("sampleRate", record.sampleRate);
    object->setProperty("channels", record.channels);
    object->setProperty("bitsPerSample", record.bitsPerSample);
    object->setProperty(
        "durationSeconds",
        std::round(record.durationSeconds * utils::millisecondsPerSecond) / utils::millisecondsPerSecond
    );
    object->setProperty("peakDbfs", decibelsOrNull(std::abs(static_cast<double>(record.overallPeak))));
    object->setProperty("truePeakDbtp", decibelsOrNull(record.overallTruePeak));
    object->setProperty("integratedLufs", loudnessOrNull(record.integratedLufs));
    object->setProperty("maxShortTermLufs", loudnessOrNull(record.maxShortTermLufs));
    object->setProperty("status", formatStatus(record).toLowerCase());
    object->setProperty("error", record.errorMessage);
    return object;
}

/// Returns the records as CSV text with a header line, one line per record, and a trailing newline.
/// The header is written even when there are no records, so scripts can always parse the columns.
juce::String formatRecordsAsCsv(const std::vector<AudioAnalysisRecord>& records)
{
    juce::StringArray lines;
    juce::StringArray headerFields;

    for (const auto& property : buildRecordObject(AudioAnalysisRecord {})->getProperties()) {
        headerFields.add(property.name.toString());
    }
    lines.add(headerFields.joinIntoString(","));

    for (const auto& record : records) {
        const auto object = buildRecordObject(record);
        juce::StringArray fields;

        for (const auto& property : object->getProperties()) {
            fields.add(utils::escapeCsvField(property.value.toString()));
        }
        lines.add(fields.joinIntoString(","));
    }

    return lines.joinIntoString("\n") + "\n";
}

/// Returns the records as a JSON array with LF line endings and a trailing newline, with null for silent levels.
juce::String formatRecordsAsJson(const std::vector<AudioAnalysisRecord>& records)
{
    juce::Array<juce::var> array;

    for (const auto& record : records) {
        array.add(juce::var(buildRecordObject(record).get()));
    }

    // juce::JSON separates lines with juce::newLine, which is CRLF on every platform,
    // and the text-mode stdout on Windows would turn it into CR CR LF.
    return juce::JSON::toString(juce::var(array)).replace("\r\n", "\n") + "\n";
}

/// Prints the records in the requested format to stdout.
/// The table shows file names, or full paths when showFullPaths is set, as for normalization output.
/// The table shows only successful records, since failures are already logged to stderr,
/// while CSV and JSON include every record with its status and error.
void printRecords(
    const std::vector<AudioAnalysisRecord>& records,
    const AudioAnalysisOutputFormat format,
    const bool showFullPaths
)
{
    switch (format) {
        case AudioAnalysisOutputFormat::table:
            printHeaderRow();

            for (const auto& record : records) {
                if (!record.hasError()) {
                    printResultRow(record, showFullPaths ? reportedOutputPath(record) : record.fileName);
                }
            }
            break;

        case AudioAnalysisOutputFormat::csv:
            fmt::print("{}", formatRecordsAsCsv(records));
            break;

        case AudioAnalysisOutputFormat::json:
            fmt::print("{}", formatRecordsAsJson(records));
            break;
    }
}
}  // namespace audiobatch::cli