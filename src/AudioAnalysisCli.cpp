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
#include <utility>
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

    // The CSV and JSON output of a normalize run lists failed files too, with their error.
    const auto addNormalizationFailure = [&normalizedResults](AudioAnalysisRecord record, const juce::String& error) {
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = error;
        normalizedResults.push_back(std::move(record));
    };

    for (const auto& result : results) {
        if (result.hasError()) {
            ++failureCount;
            utils::logError("{}: {}", result.file.getFullPathName().quoted(), result.errorMessage);

            if (options.normalize) {
                normalizedResults.push_back(result);
            }
            continue;
        }

        if (!options.normalize) {
            continue;
        }

        if (normalizationConflicts.contains(result.file.getFullPathName())) {
            ++failureCount;
            const auto conflictMessage = AudioNormalizationService::batchOutputConflictMessage(result.file);
            utils::logError("{}: {}", result.file.getFullPathName().quoted(), conflictMessage);
            addNormalizationFailure(result, conflictMessage);
            continue;
        }

        auto normalizationOptions = options.normalization;
        normalizationOptions.overwriteExisting = options.force;
        const auto normalization = AudioNormalizationService::normalizeFile(result, normalizationOptions);

        // A converted original was moved to the trash, so its cached row is no longer needed,
        // even when the re-analysis of the committed output failed.
        if (normalization.committed && normalization.analysisRecord.fullPath != result.fullPath) {
            cache.removeAnalysis(result.file);
        }

        if (normalization.hasError()) {
            ++failureCount;
            utils::logError("{}: {}", result.file.getFullPathName().quoted(), normalization.errorMessage);
            // A committed output replaced the source, so the failed re-analysis of the output is reported instead.
            addNormalizationFailure(
                normalization.committed ? normalization.analysisRecord : result, normalization.errorMessage
            );
            continue;
        }

        cache.storeAnalysis(normalization.analysisRecord);

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
