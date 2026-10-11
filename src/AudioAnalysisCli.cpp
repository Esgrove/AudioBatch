/// Implementation of the AudioAnalysisCli argument parsing and analysis workflow.
/// Covers option validation, the usage text,
/// and the console output formatting that prints aligned peak, true peak,
/// and loudness columns for analysis and normalization results.

#include "AudioAnalysisCli.h"

#include "AudioAnalysisService.h"
#include "AudioNormalizationService.h"
#include "ConsolePrint.h"
#include "Log.h"
#include "StringFormat.h"
#include "version.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

/// CLI-only formatting helpers used when printing analysis results.
namespace audiobatch::cli
{
constexpr int cliPeakColumnWidth = 7;
constexpr int cliTruePeakColumnWidth = 7;
constexpr int cliLoudnessColumnWidth = 8;
/// Divisor that converts the millisecond counter durations into seconds for log output.
constexpr double millisecondsPerSecond = 1000.0;

/// Formats the sample peak value right-aligned to the fixed dBFS column width.
static juce::String formattedPeakColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatPeakCompact(static_cast<double>(record.overallPeak))
        .paddedLeft(' ', cliPeakColumnWidth);
}

/// Formats the true peak value right-aligned to the fixed dBTP column width.
static juce::String formattedTruePeakColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatTruePeakCompact(record.overallTruePeak).paddedLeft(' ', cliTruePeakColumnWidth);
}

/// Formats the integrated loudness value right-aligned to the fixed LUFS column width.
static juce::String formattedIntegratedLoudnessColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatLoudnessCompact(record.integratedLufs).paddedLeft(' ', cliLoudnessColumnWidth);
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

/// Returns true when the argument is a plain number such as "-14" or "0.5",
/// which may start with a minus sign but is a value and not an option.
static bool isNumericArgument(const juce::String& text)
{
    const auto digits = text.startsWithChar('-') || text.startsWithChar('+') ? text.substring(1) : text;
    return digits.isNotEmpty() && digits.containsOnly("0123456789.") && digits.containsAnyOf("0123456789")
        && digits.indexOfChar('.') == digits.lastIndexOfChar('.');
}

/// Removes an option and its value from the arguments.
/// Accepts the value as the next argument for both the short and the long form, like "-g type" and "--group-by type",
/// and also as "--group-by=type".
/// juce::ArgumentList::removeValueForOption only takes the next argument for the short form.
/// Returns an empty optional when the option is not present,
/// and an empty string when it is present without a value.
static std::optional<juce::String> removeOptionWithValue(juce::ArgumentList& arguments, const juce::StringRef option)
{
    for (int index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments.arguments.getReference(index);

        if (argument != option) {
            continue;
        }

        if (argument.isLongOption() && argument.text.containsChar('=')) {
            auto value = argument.getLongOptionValue();
            arguments.arguments.remove(index);
            return value;
        }

        if (index + 1 < arguments.size() && !arguments[index + 1].isOption()) {
            auto value = arguments[index + 1].text;
            arguments.arguments.removeRange(index, 2);
            return value;
        }

        arguments.arguments.remove(index);
        return juce::String();
    }

    return std::nullopt;
}

/// Returns an error message for the first argument that combines several short options, such as "-rf",
/// or an empty string when there is none.
/// juce::ArgumentList matches a short option by searching for its letter anywhere in the argument,
/// and removes the whole argument on the first match, so "-rf" would silently set only one of the flags.
static juce::String findCombinedShortOptionError(const juce::ArgumentList& arguments)
{
    for (const auto& argument : arguments.arguments) {
        if (argument.isShortOption() && argument.text.length() > 2 && !isNumericArgument(argument.text)) {
            return utils::format(
                "Combined short options are not supported: {}. Pass each option separately.", argument.text
            );
        }
    }

    return {};
}

/// Parses a sort mode name, accepting "loudness" as an alias for "lufs".
/// Returns an empty optional for an unknown name.
static std::optional<AudioAnalysisSortMode> parseSortMode(const juce::String& value)
{
    const auto normalizedSort = value.trim().toLowerCase();

    if (normalizedSort == "peak") {
        return AudioAnalysisSortMode::peak;
    }

    if (normalizedSort == "loudness" || normalizedSort == "lufs") {
        return AudioAnalysisSortMode::loudness;
    }

    if (normalizedSort == "name") {
        return AudioAnalysisSortMode::name;
    }

    if (normalizedSort == "path") {
        return AudioAnalysisSortMode::path;
    }

    return std::nullopt;
}

/// Parses an output format name.
/// Returns an empty optional for an unknown name.
static std::optional<AudioAnalysisOutputFormat> parseOutputFormat(const juce::String& value)
{
    const auto normalizedFormat = value.trim().toLowerCase();

    if (normalizedFormat == "table") {
        return AudioAnalysisOutputFormat::table;
    }

    if (normalizedFormat == "csv") {
        return AudioAnalysisOutputFormat::csv;
    }

    if (normalizedFormat == "json") {
        return AudioAnalysisOutputFormat::json;
    }

    return std::nullopt;
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
        "durationSeconds", std::round(record.durationSeconds * millisecondsPerSecond) / millisecondsPerSecond
    );
    object->setProperty("peakDbfs", decibelsOrNull(std::abs(static_cast<double>(record.overallPeak))));
    object->setProperty("truePeakDbtp", decibelsOrNull(record.overallTruePeak));
    object->setProperty("integratedLufs", loudnessOrNull(record.integratedLufs));
    object->setProperty("maxShortTermLufs", loudnessOrNull(record.maxShortTermLufs));
    object->setProperty("status", AudioAnalysisService::formatStatus(record).toLowerCase());
    object->setProperty("error", record.errorMessage);
    return object;
}

/// Quotes a CSV field when it contains a separator, a quote, or a line break, doubling any quotes inside.
static juce::String escapeCsvField(const juce::String& field)
{
    if (!field.containsAnyOf(",\"\r\n")) {
        return field;
    }

    return utils::format("\"{}\"", field.replace("\"", "\"\""));
}

/// Prints the records in the requested format to stdout.
/// The table shows file names, or full paths when showFullPaths is set, as for normalization output.
/// The table shows only successful records, since failures are already logged to stderr,
/// while CSV and JSON include every record with its status and error.
static void printRecords(
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

        case AudioAnalysisOutputFormat::csv: {
            bool headerPrinted = false;

            for (const auto& record : records) {
                const auto object = buildRecordObject(record);
                juce::StringArray fields;

                if (!headerPrinted) {
                    for (const auto& property : object->getProperties()) {
                        fields.add(property.name.toString());
                    }
                    fmt::print("{}\n", fields.joinIntoString(","));
                    fields.clear();
                    headerPrinted = true;
                }

                for (const auto& property : object->getProperties()) {
                    fields.add(escapeCsvField(property.value.toString()));
                }
                fmt::print("{}\n", fields.joinIntoString(","));
            }
            break;
        }

        case AudioAnalysisOutputFormat::json: {
            juce::Array<juce::var> array;

            for (const auto& record : records) {
                array.add(juce::var(buildRecordObject(record).get()));
            }

            fmt::print("{}\n", juce::JSON::toString(juce::var(array)));
            break;
        }
    }
}

/// Parses a target level in the given range for --peak or --lufs into the normalization options.
/// Returns an error message, or an empty string when the value is valid.
static juce::String parseTargetLevel(
    const juce::String& option,
    const juce::String& value,
    const double minimum,
    const double maximum,
    double& target
)
{
    const auto trimmedValue = value.trim();

    if (!isNumericArgument(trimmedValue)) {
        return utils::format("{} needs a number, for example {} {}", option, option, minimum);
    }

    target = trimmedValue.getDoubleValue();

    if (target < minimum || target > maximum) {
        return utils::format("{} must be between {} and {}", option, minimum, maximum);
    }

    return {};
}

/// Removes --peak and --lufs from the arguments and sets the normalization mode and target.
/// Returns an error message, or an empty string when the options are valid or absent.
static juce::String parseNormalizationTarget(juce::ArgumentList& arguments, AudioAnalysisCliOptions& options)
{
    const auto peakValue = removeOptionWithValue(arguments, "--peak");
    const auto loudnessValue = removeOptionWithValue(arguments, "--lufs");

    if (peakValue.has_value() && loudnessValue.has_value()) {
        return "--peak and --lufs cannot be used together";
    }

    if (peakValue.has_value()) {
        options.hasNormalizationTarget = true;
        options.normalization.mode = AudioNormalizationMode::peak;
        return parseTargetLevel(
            "--peak",
            *peakValue,
            AudioNormalizationOptions::minimumTargetPeakDbfs,
            AudioNormalizationOptions::maximumTargetPeakDbfs,
            options.normalization.targetPeakDbfs
        );
    }

    if (loudnessValue.has_value()) {
        options.hasNormalizationTarget = true;
        options.normalization.mode = AudioNormalizationMode::loudness;
        return parseTargetLevel(
            "--lufs",
            *loudnessValue,
            AudioNormalizationOptions::minimumTargetLufs,
            AudioNormalizationOptions::maximumTargetLufs,
            options.normalization.targetLufs
        );
    }

    return {};
}

/// Returns an error message for options that cannot be used together, or an empty string when they can.
/// The plugin listing takes no input paths and no analysis options,
/// its grouping and rescan options need --list-plugins, and --force, --peak, and --lufs need --normalize.
static juce::String validateOptionCombinations(
    const AudioAnalysisCliOptions& options,
    const bool hasGrouping,
    const bool hasAnalysisValueOption
)
{
    if ((hasGrouping || options.rescanPlugins) && !options.listPlugins) {
        return "--group-by and --rescan can only be used with --list-plugins";
    }

    if (options.listPlugins && !options.inputPaths.isEmpty()) {
        return "--list-plugins does not take input paths";
    }

    const bool hasAnalysisOption = hasAnalysisValueOption || options.recursive || options.refresh || options.normalize
        || options.force || options.reverse || options.hasNormalizationTarget;

    if (options.listPlugins && hasAnalysisOption) {
        return "--list-plugins cannot be combined with analysis options";
    }

    if (options.force && !options.normalize) {
        return "--force can only be used with --normalize";
    }

    if (options.hasNormalizationTarget && !options.normalize) {
        return "--peak and --lufs can only be used with --normalize";
    }

    return {};
}
}  // namespace audiobatch::cli

using namespace audiobatch::cli;

juce::String AudioAnalysisCli::buildUsage(const juce::String& executableName)
{
    // Lines are joined with juce::newLine outside the format strings,
    // so the text uses the platform line ending when printed.
    const juce::StringArray lines {
        utils::format("{} {}", executableName, version::VERSION_INFO),
        utils::format("Usage: {} [options] <paths...>", executableName),
        "",
        "Options:",
        "  -h, --help              Print usage and exit",
        "  -V, --version           Print version and exit",
        "  -r, --recurse           Recurse into directories",
        "  -f, --refresh           Ignore cached analysis and re-analyze files",
        "  -n, --normalize         Normalize analyzed files, then report the re-analyzed output peaks",
        "  -F, --force             Let --normalize replace an existing output file",
        "      --peak <dBFS>       Normalize to this sample peak, -12 to 0 (default 0)",
        "      --lufs <LUFS>       Normalize to this integrated loudness, -24 to -6, with a -0.3 dBTP ceiling",
        "  -j, --jobs <count>      Override worker count",
        "  -s, --sort <mode>       Sort by peak, lufs, name, or path",
        "      --reverse           Sort in descending order",
        "  -o, --output <format>   Print results as table (default), csv, or json",
        "  -l, --list-plugins      List the available audio plugins and exit",
        "  -g, --group-by <mode>   Group the plugin list by name (alphabetical), manufacturer, type, or format",
        "  -R, --rescan            Scan for new or changed plugins before listing them",
        "",
        "Short options cannot be combined, so pass -r -f instead of -rf.",
    };

    return lines.joinIntoString(juce::newLine) + juce::newLine;
}

void AudioAnalysisCli::attachNegativeOptionValues(juce::ArgumentList& arguments)
{
    for (int index = 0; index + 1 < arguments.size(); ++index) {
        const auto& argument = arguments.arguments.getReference(index);
        const auto& next = arguments.arguments.getReference(index + 1);

        if (argument.isLongOption() && !argument.text.containsChar('=') && next.text.startsWithChar('-')
            && isNumericArgument(next.text))
        {
            arguments.arguments.set(index, {utils::format("{}={}", argument.text, next.text)});
            arguments.arguments.remove(index + 1);
        }
    }
}

std::optional<AudioAnalysisCliOptions> AudioAnalysisCli::parse(juce::ArgumentList arguments, juce::String& errorMessage)
{
    AudioAnalysisCliOptions options;
    attachNegativeOptionValues(arguments);

    if (errorMessage = findCombinedShortOptionError(arguments); errorMessage.isNotEmpty()) {
        return std::nullopt;
    }

    options.recursive = arguments.removeOptionIfFound("--recurse|-r");
    options.refresh = arguments.removeOptionIfFound("--refresh|-f");
    options.normalize = arguments.removeOptionIfFound("--normalize|-n");
    options.force = arguments.removeOptionIfFound("--force|-F");
    options.reverse = arguments.removeOptionIfFound("--reverse");
    options.listPlugins = arguments.removeOptionIfFound("--list-plugins|-l");
    options.rescanPlugins = arguments.removeOptionIfFound("--rescan|-R");

    const auto workerCountValue = removeOptionWithValue(arguments, "--jobs|-j");
    if (workerCountValue.has_value()) {
        options.workerCount = workerCountValue->getIntValue();

        if (options.workerCount <= 0) {
            errorMessage = "Worker count must be greater than zero";
            return std::nullopt;
        }
    }

    const auto sortValue = removeOptionWithValue(arguments, "--sort|-s");
    if (sortValue.has_value()) {
        const auto sortMode = parseSortMode(*sortValue);

        if (!sortMode.has_value()) {
            errorMessage = "Sort mode must be one of: peak, lufs, name, path";
            return std::nullopt;
        }

        options.sortMode = *sortMode;
    }

    if (errorMessage = parseNormalizationTarget(arguments, options); errorMessage.isNotEmpty()) {
        return std::nullopt;
    }

    const auto outputValue = removeOptionWithValue(arguments, "--output|-o");
    if (outputValue.has_value()) {
        const auto outputFormat = parseOutputFormat(*outputValue);

        if (!outputFormat.has_value()) {
            errorMessage = "Output format must be one of: table, csv, json";
            return std::nullopt;
        }

        options.outputFormat = *outputFormat;
    }

    const auto groupingValue = removeOptionWithValue(arguments, "--group-by|-g");
    if (groupingValue.has_value()) {
        const auto grouping = PluginListCli::parseGrouping(*groupingValue);

        if (!grouping.has_value()) {
            errorMessage = "Plugin grouping must be one of: name, manufacturer, type, format";
            return std::nullopt;
        }

        options.pluginGrouping = *grouping;
    }

    for (const auto& argument : arguments.arguments) {
        if (argument.isOption()) {
            errorMessage = utils::format("Unknown option: {}", argument.text);
            return std::nullopt;
        }

        options.inputPaths.add(argument.resolveAsFile());
    }

    const bool hasAnalysisValueOption
        = workerCountValue.has_value() || sortValue.has_value() || outputValue.has_value();

    if (errorMessage = validateOptionCombinations(options, groupingValue.has_value(), hasAnalysisValueOption);
        errorMessage.isNotEmpty())
    {
        return std::nullopt;
    }

    return options;
}

int AudioAnalysisCli::run(const AudioAnalysisCliOptions& options)
{
    auto inputPaths = options.inputPaths;

    if (inputPaths.isEmpty()) {
        inputPaths.add(juce::File::getCurrentWorkingDirectory());
        utils::logInfo(
            "No input paths provided, using current directory: {}", inputPaths[0].getFullPathName().quoted()
        );
    }

    AnalysisCache cache;
    cache.open();

    AnalysisCoordinator coordinator(cache, options.workerCount);
    AudioAnalysisOptions analysisOptions;
    analysisOptions.inputPaths = inputPaths;
    analysisOptions.recursive = options.recursive;
    analysisOptions.refresh = options.refresh;

    const auto analysisStartedAtMs = juce::Time::getMillisecondCounterHiRes();
    auto results = coordinator.analyzeBlocking(analysisOptions);
    const auto analysisElapsedMs = juce::Time::getMillisecondCounterHiRes() - analysisStartedAtMs;
    const auto analysisFailureCount
        = std::ranges::count_if(results, [](const auto& record) { return record.hasError(); });
    utils::logInfo(
        "Analysis complete: {} files ({} failed) in {:.2f} s",
        results.size(),
        analysisFailureCount,
        analysisElapsedMs / millisecondsPerSecond
    );

    AudioAnalysisService::sortRecords(results, options.sortMode, !options.reverse);

    int failureCount = 0;
    std::vector<AudioAnalysisRecord> normalizedResults;

    juce::StringArray normalizationConflicts;

    if (options.normalize) {
        normalizedResults.reserve(results.size());
        std::vector<AudioAnalysisRecord> normalizableResults;
        std::ranges::copy_if(results, std::back_inserter(normalizableResults), [](const auto& record) {
            return !record.hasError();
        });
        normalizationConflicts = AudioNormalizationService::findBatchOutputConflicts(normalizableResults);
    }

    for (const auto& result : results) {
        if (result.hasError()) {
            ++failureCount;
            utils::logError("{}: {}", result.file.getFullPathName().quoted(), result.errorMessage);
            continue;
        }

        if (!options.normalize) {
            continue;
        }

        if (normalizationConflicts.contains(result.file.getFullPathName())) {
            ++failureCount;
            utils::logError(
                "{}: {}",
                result.file.getFullPathName().quoted(),
                AudioNormalizationService::batchOutputConflictMessage(result.file)
            );
            continue;
        }

        auto normalizationOptions = options.normalization;
        normalizationOptions.overwriteExisting = options.force;
        const auto normalization = AudioNormalizationService::normalizeFile(result, normalizationOptions);

        if (normalization.hasError()) {
            ++failureCount;
            utils::logError("{}: {}", result.file.getFullPathName().quoted(), normalization.errorMessage);
            continue;
        }

        cache.storeAnalysis(normalization.analysisRecord);

        // A converted original was moved to the trash, so its cached row is no longer needed.
        if (normalization.analysisRecord.fullPath != result.fullPath) {
            cache.removeAnalysis(result.file);
        }

        normalizedResults.push_back(normalization.analysisRecord);
    }

    if (options.normalize) {
        AudioAnalysisService::sortRecords(normalizedResults, options.sortMode, !options.reverse);
        printRecords(normalizedResults, options.outputFormat, true);
    } else {
        printRecords(results, options.outputFormat, false);
    }

    return failureCount == 0 ? 0 : 2;
}
