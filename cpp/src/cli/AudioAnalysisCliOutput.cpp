/// Implementation of the CLI result output:
/// the aligned peak, true peak, and loudness table, and the CSV and JSON records with null for silence.

#include "cli/AudioAnalysisCliOutput.h"

#include "analysis/AudioAnalysisFormatting.h"
#include "analysis/AudioAnalysisTypes.h"
#include "cli/AudioAnalysisCli.h"
#include "utils/ConsolePrint.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"
#include <fmt/base.h>

#include <cmath>
#include <optional>
#include <vector>

namespace audiobatch::cli
{
namespace
{
/// Width in characters of the dBFS sample peak column of the results table.
constexpr int CLI_PEAK_COLUMN_WIDTH {7};
/// Width in characters of the dBTP true peak column of the results table.
constexpr int CLI_TRUE_PEAK_COLUMN_WIDTH {7};
/// Width in characters of the LUFS integrated loudness column of the results table.
constexpr int CLI_LOUDNESS_COLUMN_WIDTH {8};
/// Most decimals a number has in the CSV and JSON output.
/// Levels are rounded to two decimals and durations to milliseconds,
/// and the limit keeps the binary rounding error of those values out of the text.
constexpr int MACHINE_READABLE_DECIMAL_PLACES {6};
/// Precision used for signed channel means and correlation in machine-readable output.
constexpr double SOURCE_STATISTIC_SCALE {1'000'000.0};

/// Returns the JSON options that write numbers with at most the machine-readable number of decimals.
juce::JSON::FormatOptions machineReadableJsonOptions()
{
    return juce::JSON::FormatOptions {}.withMaxDecimalPlaces(MACHINE_READABLE_DECIMAL_PLACES);
}

/// Returns the CSV text of one field value, writing a number the same way as the JSON output does.
juce::String csvFieldText(const juce::var& value)
{
    return value.isDouble() || value.isArray()
        ? juce::JSON::toString(value, machineReadableJsonOptions().withSpacing(juce::JSON::Spacing::none))
        : value.toString();
}

/// Formats the sample peak value right-aligned to the fixed dBFS column width.
juce::String formattedPeakColumn(const AudioAnalysisRecord& record)
{
    return formatPeakCompact(static_cast<double>(record.overallPeak)).paddedLeft(' ', CLI_PEAK_COLUMN_WIDTH);
}

/// Formats the true peak value right-aligned to the fixed dBTP column width.
juce::String formattedTruePeakColumn(const AudioAnalysisRecord& record)
{
    return formatTruePeakCompact(record.overallTruePeak).paddedLeft(' ', CLI_TRUE_PEAK_COLUMN_WIDTH);
}

/// Formats the integrated loudness value right-aligned to the fixed LUFS column width.
juce::String formattedIntegratedLoudnessColumn(const AudioAnalysisRecord& record)
{
    return formatLoudnessCompact(record.integratedLufs).paddedLeft(' ', CLI_LOUDNESS_COLUMN_WIDTH);
}

/// Prints the bold column header line to stdout, matching the widths used by the result rows.
void printHeaderRow()
{
    console::printBold(
        "{:>{}}  {:>{}}  {:>{}}  TRACK\n",
        "dBFS",
        CLI_PEAK_COLUMN_WIDTH,
        "dBTP",
        CLI_TRUE_PEAK_COLUMN_WIDTH,
        "LUFS-I",
        CLI_LOUDNESS_COLUMN_WIDTH
    );
}

/// Prints one aligned result line to stdout.
/// The caller chooses the track label so plain analysis can show file names
/// while normalization output can show full output paths.
void printResultRow(const AudioAnalysisRecord& record, const juce::String& trackLabel)
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
juce::String reportedOutputPath(const AudioAnalysisRecord& record)
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
juce::var decibelsOrNull(const double amplitude)
{
    if (amplitude <= 0.0) {
        return {};
    }

    return std::round(
               juce::Decibels::gainToDecibels(amplitude, AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS) * 100.0
           )
        / 100.0;
}

/// Rounds a loudness value to two decimals,
/// or returns a void var, which JSON writes as null, for the negative infinity sentinel.
juce::var loudnessOrNull(const double loudness)
{
    if (loudness <= AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS) {
        return {};
    }

    return std::round(loudness * 100.0) / 100.0;
}

/// Builds the machine-readable fields of one record, shared by the CSV and JSON output.
/// The keys keep their insertion order, which is also the CSV column order.
juce::DynamicObject::Ptr buildRecordObject(const AudioAnalysisRecord& record)
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
        std::round(record.durationSeconds * utils::MILLISECONDS_PER_SECOND) / utils::MILLISECONDS_PER_SECOND
    );
    object->setProperty("peakDbfs", decibelsOrNull(std::abs(static_cast<double>(record.overallPeak))));
    object->setProperty("truePeakDbtp", decibelsOrNull(record.overallTruePeak));
    object->setProperty("integratedLufs", loudnessOrNull(record.integratedLufs));
    object->setProperty("maxShortTermLufs", loudnessOrNull(record.maxShortTermLufs));
    const auto rounded = [](const std::optional<double> value, const double scale) -> juce::var {
        return value && std::isfinite(*value) ? juce::var(std::round(*value * scale) / scale) : juce::var();
    };
    const auto& metrics = record.signalMetrics;
    object->setProperty("plrDb", rounded(metrics.plrDb, 100.0));
    object->setProperty("psrDb", rounded(metrics.psrDb, 100.0));
    object->setProperty("crestFactorDb", rounded(metrics.crestFactorDb, 100.0));
    juce::Array<juce::var> offsets;
    for (const auto value : metrics.dcOffsets) {
        offsets.add(rounded(value, SOURCE_STATISTIC_SCALE));
    }
    juce::Array<juce::var> counts;
    for (const auto value : metrics.clipCounts) {
        counts.add(juce::var(static_cast<juce::int64>(value)));
    }
    object->setProperty("dcOffsets", offsets);
    object->setProperty("clipCounts", counts);
    object->setProperty(
        "clipsTotal", record.isReady() ? juce::var(static_cast<juce::int64>(metrics.totalClips())) : juce::var()
    );
    object->setProperty("stereoCorrelation", rounded(metrics.stereoCorrelation, SOURCE_STATISTIC_SCALE));
    object->setProperty("status", formatStatus(record).toLowerCase());
    object->setProperty("error", record.errorMessage);
    return object;
}
}  // namespace

/// Returns the records as CSV text with a header line, one line per record, and a trailing newline.
/// The header is written even when there are no records, so scripts can always parse the columns.
juce::String formatRecordsAsCsv(const std::vector<AudioAnalysisRecord>& records)
{
    juce::StringArray lines;
    juce::StringArray headerFields;

    for (const auto& [name, value] : buildRecordObject(AudioAnalysisRecord {})->getProperties()) {
        headerFields.add(name.toString());
    }
    lines.add(headerFields.joinIntoString(","));

    for (const auto& record : records) {
        const auto object = buildRecordObject(record);
        juce::StringArray fields;

        for (const auto& [name, value] : object->getProperties()) {
            fields.add(utils::escapeCsvField(csvFieldText(value)));
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
    return juce::JSON::toString(juce::var(array), machineReadableJsonOptions()).replace("\r\n", "\n") + "\n";
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
