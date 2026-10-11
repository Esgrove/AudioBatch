/// Implementation of the AudioAnalysisCli argument parsing and analysis workflow.
/// Covers option validation, the usage text,
/// and the console output formatting that prints aligned peak, true peak,
/// and loudness columns for analysis and normalization results.

#include "AudioAnalysisCli.h"

#include "AudioAnalysisService.h"
#include "AudioNormalizationService.h"
#include "utils.h"
#include "version.h"

/// CLI-only formatting helpers used when printing analysis results.
namespace audiobatch::cli
{
constexpr int cliPeakColumnWidth = 7;
constexpr int cliTruePeakColumnWidth = 7;
constexpr int cliLoudnessColumnWidth = 8;

/// Formats the sample peak value right-aligned to the fixed dBFS column width.
static juce::String formattedPeakColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatPeakCompact(record.overallPeak).paddedLeft(' ', cliPeakColumnWidth);
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
            const auto value = argument.getLongOptionValue();
            arguments.arguments.remove(index);
            return value;
        }

        if (index + 1 < arguments.size() && !arguments[index + 1].isOption()) {
            const auto value = arguments[index + 1].text;
            arguments.arguments.removeRange(index, 2);
            return value;
        }

        arguments.arguments.remove(index);
        return juce::String();
    }

    return std::nullopt;
}

}  // namespace audiobatch::cli

using namespace audiobatch::cli;

juce::String AudioAnalysisCli::buildUsage(const juce::String& executableName)
{
    juce::String usage;
    usage += executableName;
    usage += " ";
    usage += juce::String(version::VERSION_INFO);
    usage += juce::newLine;
    usage += "Usage: ";
    usage += executableName;
    usage += " [options] <paths...>";
    usage += juce::newLine;
    usage += juce::newLine;
    usage += "Options:";
    usage += juce::newLine;
    usage += "  -h, --help              Print usage and exit";
    usage += juce::newLine;
    usage += "  -V, --version           Print version and exit";
    usage += juce::newLine;
    usage += "  -r, --recurse           Recurse into directories";
    usage += juce::newLine;
    usage += "  -f, --refresh           Ignore cached analysis and re-analyze files";
    usage += juce::newLine;
    usage += "  -n, --normalize         Normalize analyzed files, then report the re-analyzed output peaks";
    usage += juce::newLine;
    usage += "  -j, --jobs <count>      Override worker count";
    usage += juce::newLine;
    usage += "  -s, --sort <mode>       Sort by peak, lufs, name, or path";
    usage += juce::newLine;
    usage += "  -l, --list-plugins      List the available audio plugins and exit";
    usage += juce::newLine;
    usage += "  -g, --group-by <mode>   Group the plugin list by name (alphabetical), manufacturer, type, or format";
    usage += juce::newLine;
    usage += "  -R, --rescan            Scan for new or changed plugins before listing them";
    usage += juce::newLine;
    return usage;
}

std::optional<AudioAnalysisCliOptions> AudioAnalysisCli::parse(juce::ArgumentList arguments, juce::String& errorMessage)
{
    AudioAnalysisCliOptions options;

    options.recursive = arguments.removeOptionIfFound("--recurse|-r");
    options.refresh = arguments.removeOptionIfFound("--refresh|-f");
    options.normalize = arguments.removeOptionIfFound("--normalize|-n");
    options.listPlugins = arguments.removeOptionIfFound("--list-plugins|-l");
    options.rescanPlugins = arguments.removeOptionIfFound("--rescan|-R");

    if (const auto workerCountValue = removeOptionWithValue(arguments, "--jobs|-j"); workerCountValue.has_value()) {
        options.workerCount = workerCountValue->getIntValue();

        if (options.workerCount <= 0) {
            errorMessage = "Worker count must be greater than zero";
            return std::nullopt;
        }
    }

    if (const auto sortValue = removeOptionWithValue(arguments, "--sort|-s"); sortValue.has_value()) {
        const auto normalizedSort = sortValue->trim().toLowerCase();

        if (normalizedSort == "peak") {
            options.sortMode = AudioAnalysisSortMode::peak;
        } else if (normalizedSort == "loudness" || normalizedSort == "lufs") {
            options.sortMode = AudioAnalysisSortMode::loudness;
        } else if (normalizedSort == "name") {
            options.sortMode = AudioAnalysisSortMode::name;
        } else if (normalizedSort == "path") {
            options.sortMode = AudioAnalysisSortMode::path;
        } else {
            errorMessage = "Sort mode must be one of: peak, lufs, name, path";
            return std::nullopt;
        }
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

    if ((groupingValue.has_value() || options.rescanPlugins) && !options.listPlugins) {
        errorMessage = "--group-by and --rescan can only be used with --list-plugins";
        return std::nullopt;
    }

    if (options.listPlugins && !options.inputPaths.isEmpty()) {
        errorMessage = "--list-plugins does not take input paths";
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
        analysisElapsedMs / 1000.0
    );

    AudioAnalysisService::sortRecords(results, options.sortMode, true);

    int failureCount = 0;
    std::vector<AudioAnalysisRecord> normalizedResults;

    if (options.normalize) {
        normalizedResults.reserve(results.size());
    }

    if (!options.normalize) {
        printHeaderRow();
    }

    for (const auto& result : results) {
        if (result.hasError()) {
            ++failureCount;
            utils::logError("{}: {}", result.file.getFullPathName().quoted(), result.errorMessage);
            continue;
        }

        if (!options.normalize) {
            printResultRow(result, result.fileName);
            continue;
        }

        const auto normalization = AudioNormalizationService::normalizeFile(result);

        if (normalization.hasError()) {
            ++failureCount;
            utils::logError("{}: {}", result.file.getFullPathName().quoted(), normalization.errorMessage);
            continue;
        }

        normalizedResults.push_back(normalization.analysisRecord);
    }

    if (options.normalize) {
        AudioAnalysisService::sortRecords(normalizedResults, options.sortMode, true);
        printHeaderRow();

        for (const auto& record : normalizedResults) {
            printResultRow(record, reportedOutputPath(record));
        }
    }

    return failureCount == 0 ? 0 : 2;
}
