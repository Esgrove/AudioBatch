#include "AudioAnalysisCli.h"

#include "AudioAnalysisService.h"
#include "AudioNormalizationService.h"
#include "utils.h"
#include "version.h"

#include <iostream>

/// CLI-only formatting helpers used when printing analysis results.
namespace audiobatch::cli
{
constexpr int cliPeakColumnWidth = 7;
constexpr int cliTruePeakColumnWidth = 7;
constexpr int cliLoudnessColumnWidth = 8;

static juce::String formattedPeakColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatPeakCompact(record.overallPeak).paddedLeft(' ', cliPeakColumnWidth);
}

static juce::String formattedTruePeakColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatTruePeakCompact(record.overallTruePeak).paddedLeft(' ', cliTruePeakColumnWidth);
}

static juce::String formattedIntegratedLoudnessColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatLoudnessCompact(record.integratedLufs).paddedLeft(' ', cliLoudnessColumnWidth);
}

static void printHeaderRow()
{
    std::cout << juce::String("dBFS").paddedLeft(' ', cliPeakColumnWidth) << "  "
              << juce::String("dBTP").paddedLeft(' ', cliTruePeakColumnWidth) << "  "
              << juce::String("LUFS-I").paddedLeft(' ', cliLoudnessColumnWidth) << "  TRACK" << juce::newLine;
}

static void printResultRow(const AudioAnalysisRecord& record, const juce::String& trackLabel)
{
    std::cout << formattedPeakColumn(record) << "  " << formattedTruePeakColumn(record) << "  "
              << formattedIntegratedLoudnessColumn(record) << "  " << trackLabel << juce::newLine;
}

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
    return usage;
}

std::optional<AudioAnalysisCliOptions> AudioAnalysisCli::parse(juce::ArgumentList arguments, juce::String& errorMessage)
{
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
    auto inputPaths = options.inputPaths;

    if (inputPaths.isEmpty()) {
        inputPaths.add(juce::File::getCurrentWorkingDirectory());
        utils::log_info("No input paths provided, using current directory: " + inputPaths[0].getFullPathName());
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
        = std::count_if(results.begin(), results.end(), [](const auto& record) { return record.hasError(); });
    utils::log_info(
        "Analysis complete: " + juce::String(results.size()) + " files (" + juce::String(analysisFailureCount)
        + " failed) in " + juce::String(analysisElapsedMs / 1000.0, 2) + " s"
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
            utils::log_error(result.file.getFullPathName() + ": " + result.errorMessage);
            continue;
        }

        if (!options.normalize) {
            printResultRow(result, result.fileName);
            continue;
        }

        const auto normalization = AudioNormalizationService::normalizeFile(result);

        if (normalization.hasError()) {
            ++failureCount;
            utils::log_error(result.file.getFullPathName() + ": " + normalization.errorMessage);
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
