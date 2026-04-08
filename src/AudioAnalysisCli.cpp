#include "AudioAnalysisCli.h"

#include "AudioAnalysisService.h"
#include "AudioNormalizationService.h"
#include "utils.h"
#include "version.h"

#include <iostream>

/// CLI-only formatting helpers used when printing analysis results.
namespace
{
constexpr int cliPeakColumnWidth = 7;
constexpr int cliTruePeakColumnWidth = 7;
constexpr int cliLoudnessColumnWidth = 8;

juce::String formattedPeakColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatPeakCompact(record.overallPeak).paddedLeft(' ', cliPeakColumnWidth);
}

juce::String formattedTruePeakColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatTruePeakCompact(record.overallTruePeak).paddedLeft(' ', cliTruePeakColumnWidth);
}

juce::String formattedIntegratedLoudnessColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatLoudnessCompact(record.integratedLufs).paddedLeft(' ', cliLoudnessColumnWidth);
}

void printHeaderRow()
{
    std::cout << juce::String("dBFS").paddedLeft(' ', cliPeakColumnWidth) << "  "
              << juce::String("dBTP").paddedLeft(' ', cliTruePeakColumnWidth) << "  "
              << juce::String("LUFS-I").paddedLeft(' ', cliLoudnessColumnWidth) << "  TRACK" << juce::newLine;
}

void printResultRow(const AudioAnalysisRecord& record, const juce::String& trackLabel)
{
    std::cout << formattedPeakColumn(record) << "  " << formattedTruePeakColumn(record) << "  "
              << formattedIntegratedLoudnessColumn(record) << "  " << trackLabel << juce::newLine;
}

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

void printCliError(const juce::String& message)
{
    std::cerr << ansi::red << message << ansi::reset << std::endl;
}
}  // namespace

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
    usage += "  -s, --sort <mode>       Sort by peak, name, or path";
    usage += juce::newLine;
    return usage;
}

std::optional<AudioAnalysisCliOptions> AudioAnalysisCli::parse(
    const juce::ArgumentList& sourceArguments,
    juce::String& errorMessage
)
{
    auto arguments = sourceArguments;
    AudioAnalysisCliOptions options;

    options.showHelp = arguments.removeOptionIfFound("--help|-h");
    options.showVersion = arguments.removeOptionIfFound("--version|-V");
    options.recursive = arguments.removeOptionIfFound("--recurse|-r");
    options.refresh = arguments.removeOptionIfFound("--refresh|-f");
    options.normalize = arguments.removeOptionIfFound("--normalize|-n");

    if (const auto workerCountValue = arguments.removeValueForOption("--jobs|-j"); workerCountValue.isNotEmpty()) {
        options.workerCount = workerCountValue.getIntValue();

        if (options.workerCount <= 0) {
            errorMessage = "Worker count must be greater than zero";
            return std::nullopt;
        }
    }

    if (const auto sortValue = arguments.removeValueForOption("--sort|-s"); sortValue.isNotEmpty()) {
        const auto normalizedSort = sortValue.trim().toLowerCase();

        if (normalizedSort == "peak") {
            options.sortMode = AudioAnalysisSortMode::peak;
        } else if (normalizedSort == "name") {
            options.sortMode = AudioAnalysisSortMode::name;
        } else if (normalizedSort == "path") {
            options.sortMode = AudioAnalysisSortMode::path;
        } else {
            errorMessage = "Sort mode must be one of: peak, name, path";
            return std::nullopt;
        }
    }

    for (const auto& argument : arguments.arguments) {
        if (argument.isOption()) {
            errorMessage = "Unknown option: " + argument.text;
            return std::nullopt;
        }

        options.inputPaths.add(argument.resolveAsFile());
    }

    return options;
}

int AudioAnalysisCli::run(const AudioAnalysisCliOptions& options)
{
    if (options.inputPaths.isEmpty()) {
        printCliError("No input paths provided");
        return 1;
    }

    AnalysisCache cache;
    cache.open();

    AnalysisCoordinator coordinator(cache, options.workerCount);
    AudioAnalysisOptions analysisOptions;
    analysisOptions.inputPaths = options.inputPaths;
    analysisOptions.recursive = options.recursive;
    analysisOptions.refresh = options.refresh;

    auto results = coordinator.analyzeBlocking(analysisOptions);
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
            printCliError(result.file.getFullPathName() + ": " + result.errorMessage);
            continue;
        }

        if (!options.normalize) {
            printResultRow(result, result.fileName);
            continue;
        }

        const auto normalization = AudioNormalizationService::normalizeFile(result);

        if (normalization.hasError()) {
            ++failureCount;
            printCliError(result.file.getFullPathName() + ": " + normalization.errorMessage);
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
