/// Implementation of the CLI analysis workflow in cli::runAnalysis:
/// analyzing the inputs, normalizing them when requested, and printing the results.
/// The argument parsing is in AudioAnalysisCliParse.cpp and the output formatting in AudioAnalysisCliOutput.cpp.

#include "cli/AudioAnalysisCli.h"

#include "analysis/AudioAnalysis.h"
#include "cli/AudioAnalysisCliOutput.h"
#include "normalization/AudioNormalization.h"
#include "utils/ConsolePrint.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace audiobatch
{
/// Returns an error for every path named on the command line that produced no result:
/// a path that does not exist, or a file that the analysis skips because it is hidden or not a supported format.
/// Inside a folder those files are skipped on purpose, but a file named on the command line was asked for,
/// so skipping it silently would report success for work that was never done.
juce::StringArray cli::findSkippedInputs(
    const juce::Array<juce::File>& inputPaths,
    const std::vector<AudioAnalysisRecord>& results
)
{
    juce::StringArray errors;

    for (const auto& input : inputPaths) {
        if (!input.exists()) {
            errors.add(utils::format("Input not found: {}", input.getFullPathName().quoted()));
            continue;
        }

        if (input.isDirectory()
            || std::ranges::any_of(
                results, [&input](const AudioAnalysisRecord& record) { return record.file == input; }
            ))
        {
            continue;
        }

        if (input.isHidden() || input.getFileName().startsWithChar('.')) {
            errors.add(utils::format("Hidden files are not analyzed: {}", input.getFullPathName().quoted()));
        } else {
            errors.add(utils::format("Not a supported audio file: {}", input.getFullPathName().quoted()));
        }
    }

    return errors;
}

/// Executes the CLI analysis workflow and returns the process exit code.
int cli::runAnalysis(const AudioAnalysisCliOptions& options)
{
    auto inputPaths = options.inputPaths;

    if (inputPaths.isEmpty()) {
        inputPaths.add(juce::File::getCurrentWorkingDirectory());
        utils::logInfo(
            "No input paths provided, using current directory: {}", inputPaths[0].getFullPathName().quoted()
        );
    }

    AnalysisCache cache;

    // The analysis still works without the cache, it only has to read every file again on the next run.
    if (!cache.open()) {
        const juce::String cacheWarning {"Could not open the analysis cache, results are not cached"};
        utils::logWarn(cacheWarning);
        console::printWarn(cacheWarning);
    }

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
        "Analysis complete: {:.2f} s, {} files ({} failed)",
        analysisElapsedMs / utils::millisecondsPerSecond,
        results.size(),
        analysisFailureCount
    );

    sortRecords(results, options.sortMode, !options.reverse);

    const auto skippedInputs = findSkippedInputs(options.inputPaths, results);

    for (const auto& error : skippedInputs) {
        utils::logError(error);
        console::printError(error);
    }

    int failureCount = skippedInputs.size();
    std::vector<AudioAnalysisRecord> normalizedResults;

    juce::StringArray normalizationConflicts;

    if (options.normalize) {
        normalizedResults.reserve(results.size());
        std::vector<AudioAnalysisRecord> normalizableResults;
        std::ranges::copy_if(results, std::back_inserter(normalizableResults), [](const auto& record) {
            return !record.hasError();
        });
        normalizationConflicts = findBatchOutputConflicts(normalizableResults);
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
            utils::logError("Analysis failed: {}, {}", result.file.getFullPathName().quoted(), result.errorMessage);

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
            const auto conflictMessage = batchOutputConflictMessage(result.file);
            utils::logError(
                "Normalization output conflict: {}, {}", result.file.getFullPathName().quoted(), conflictMessage
            );
            addNormalizationFailure(result, conflictMessage);
            continue;
        }

        auto normalizationOptions = options.normalization;
        normalizationOptions.overwriteExisting = options.force;
        const auto normalization = normalizeFile(result, normalizationOptions);

        // A converted original was moved to the trash, so its cached row is no longer needed,
        // even when the re-analysis of the committed output failed.
        // The cache logs its own failures, and a stale row only costs another read later.
        if (normalization.committed && normalization.analysisRecord.fullPath != result.fullPath) {
            static_cast<void>(cache.removeAnalysis(result.file));
        }

        if (normalization.hasError()) {
            ++failureCount;
            utils::logError(
                "Normalization failed: {}, {}", result.file.getFullPathName().quoted(), normalization.errorMessage
            );
            // A committed output replaced the source, so the failed re-analysis of the output is reported instead.
            addNormalizationFailure(
                normalization.committed ? normalization.analysisRecord : result, normalization.errorMessage
            );
            continue;
        }

        static_cast<void>(cache.storeAnalysis(normalization.analysisRecord));

        normalizedResults.push_back(normalization.analysisRecord);
    }

    if (options.normalize) {
        sortRecords(normalizedResults, options.sortMode, !options.reverse);
        cli::printRecords(normalizedResults, options.outputFormat, true);
    } else {
        cli::printRecords(results, options.outputFormat, false);
    }

    return failureCount == 0 ? 0 : 2;
}
}  // namespace audiobatch
