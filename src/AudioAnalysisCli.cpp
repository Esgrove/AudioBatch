/// Implementation of the CLI analysis workflow in AudioAnalysisCli::run:
/// analyzing the inputs, normalizing them when requested, and printing the results.
/// The argument parsing is in AudioAnalysisCliParse.cpp and the output formatting in AudioAnalysisCliOutput.cpp.

#include "AudioAnalysisCli.h"

#include "AudioAnalysisCliOutput.h"
#include "AudioAnalysisService.h"
#include "AudioNormalizationService.h"
#include "Log.h"
#include "StringFormat.h"

#include <algorithm>
#include <iterator>
#include <vector>

/// Constants of the CLI analysis workflow.
namespace audiobatch::cli
{
/// Divisor that converts the millisecond counter durations into seconds for log output.
constexpr double millisecondsPerSecond = 1000.0;
}  // namespace audiobatch::cli

using namespace audiobatch::cli;

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
