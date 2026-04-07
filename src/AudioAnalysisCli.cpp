#include "AudioAnalysisCli.h"

#include "AudioAnalysisService.h"
#include "version.h"

#include <iostream>

/// CLI-only formatting helpers used when printing analysis results.
namespace
{
juce::String formattedPeakColumn(const AudioAnalysisRecord& record)
{
    return AudioAnalysisService::formatPeakDisplay(record.overallPeak).paddedLeft(' ', 11);
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
    usage += " [-c|--cli] [options] <paths...>";
    usage += juce::newLine;
    usage += juce::newLine;
    usage += "Options:";
    usage += juce::newLine;
    usage += "  -h, --help              Print usage and exit";
    usage += juce::newLine;
    usage += "  -V, --version           Print version and exit";
    usage += juce::newLine;
    usage += "  -c, --cli               Run audio analysis in CLI mode";
    usage += juce::newLine;
    usage += "  -r, --recurse           Recurse into directories";
    usage += juce::newLine;
    usage += "  -f, --refresh           Ignore cached analysis and re-analyze files";
    usage += juce::newLine;
    usage += "  -j, --jobs <count>      Override worker count";
    usage += juce::newLine;
    usage += "  -s, --sort <mode>       Sort by peak, name, or path";
    usage += juce::newLine;
    usage += juce::newLine;
    usage += "Default output format:";
    usage += juce::newLine;
    usage += "  <peak>  <audio filename>";
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
    options.cliMode = arguments.removeOptionIfFound("--cli|-c");
    options.recursive = arguments.removeOptionIfFound("--recurse|-r");
    options.refresh = arguments.removeOptionIfFound("--refresh|-f");

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

    if (!options.inputPaths.isEmpty()) {
        options.cliMode = true;
    }

    return options;
}

int AudioAnalysisCli::run(const AudioAnalysisCliOptions& options)
{
    if (options.inputPaths.isEmpty()) {
        std::cerr << "No input paths provided" << juce::newLine;
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

    for (const auto& result : results) {
        if (result.hasError()) {
            ++failureCount;
            std::cerr << result.file.getFullPathName() << ": " << result.errorMessage << juce::newLine;
            continue;
        }

        std::cout << formattedPeakColumn(result) << "  " << result.fileName << juce::newLine;
    }

    return failureCount == 0 ? 0 : 2;
}
