/// Implementation of the CLI analysis workflow in cli::runAnalysis:
/// analyzing the inputs, normalizing them when requested, and printing the results.
/// The argument parsing is in AudioAnalysisCliParse.cpp and the output formatting in AudioAnalysisCliOutput.cpp.

#include "cli/AudioAnalysisCli.h"

#include "analysis/AnalysisCoordinator.h"
#include "analysis/AudioAnalysis.h"
#include "cli/AudioAnalysisCliOutput.h"
#include "normalization/AudioNormalization.h"
#include "normalization/NormalizeCoordinator.h"
#include "utils/ConsolePrint.h"
#include "utils/ConsoleProgress.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include <unordered_set>

#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch
{
namespace
{
/// Logs a message and prints it to stderr, where the CLI reports its progress apart from the results on stdout.
void reportInfo(const juce::String& message)
{
    utils::logInfo(message);
    fmt::print(stderr, "{}\n", message);
}

/// Logs an error and prints it in red to stderr.
void reportError(const juce::String& message)
{
    utils::logError(message);
    console::printStyled(stderr, fmt::fg(fmt::terminal_color::red), "{}\n", message);
}

/// Logs a warning and prints it with the "Warning: " prefix to stderr.
void reportWarning(const juce::String& message)
{
    utils::logWarn(message);
    console::printWarn(message);
}

/// Reports every record whose analysis failed, with its path and reason.
void reportAnalysisFailures(const std::vector<AudioAnalysisRecord>& results)
{
    for (const auto& result : results) {
        if (result.hasError()) {
            reportError(
                utils::format("Analysis failed: {}, {}", result.file.getFullPathName().quoted(), result.errorMessage)
            );
        }
    }
}

/// Runs the analysis with a progress line on stderr, which counts only the files that are decoded,
/// leaving out the results that came from the cache.
std::vector<AudioAnalysisRecord> analyzeWithProgress(
    AnalysisCoordinator& coordinator,
    const AudioAnalysisOptions& options
)
{
    console::ProgressLine progress;
    std::mutex staleMutex;
    std::set<juce::String> stalePaths;

    coordinator.setPlanCallback([&](const AnalysisPlan& plan) {
        {
            const std::scoped_lock lock(staleMutex);
            for (const auto& file : plan.staleFiles) {
                stalePaths.insert(file.getFullPathName());
            }
        }
        progress.setTotal(plan.staleFiles.size());
    });
    coordinator.setStartingCallback([&progress](const juce::File& file, int /*runId*/) {
        progress.setMessage(file.getFileName());
    });

    auto results = coordinator.analyzeBlocking(options, [&](const AudioAnalysisRecord& record) {
        bool decoded {false};
        {
            const std::scoped_lock lock(staleMutex);
            decoded = stalePaths.contains(record.fullPath);
        }
        if (decoded) {
            progress.increment();
        }
    });

    // The callbacks refer to this function's locals, so they must not outlive it.
    coordinator.setPlanCallback({});
    coordinator.setStartingCallback({});
    progress.finish();
    return results;
}

/// Normalizes the records on `workerCount` workers through the normalization coordinator,
/// showing the progress line while the run lasts,
/// and returns the result of each record by the full path of its source.
/// A record that loses a batch output conflict comes back as a failure without being touched.
std::map<juce::String, AudioNormalizationResult> normalizeWithProgress(
    const std::vector<AudioAnalysisRecord>& records,
    const AudioNormalizationOptions& options,
    const int workerCount
)
{
    console::ProgressLine progress;
    progress.setTotal(static_cast<int>(records.size()));
    std::mutex resultsMutex;
    std::map<juce::String, AudioNormalizationResult> results;
    juce::WaitableEvent completed;

    // The coordinator is declared last, so it is destroyed, waiting for its jobs, before the locals they use.
    NormalizeCoordinator coordinator(workerCount);
    coordinator.setStartingCallback([&progress](const juce::File& file, int /*runId*/) {
        progress.setMessage(file.getFileName());
    });
    coordinator.setResultCallback([&](const AudioNormalizationResult& result, int /*runId*/) {
        {
            const std::scoped_lock lock(resultsMutex);
            results.insert_or_assign(result.file.getFullPathName(), result);
        }
        progress.increment();
    });
    coordinator.setCompletionCallback([&completed](int /*totalFiles*/, int /*runId*/) { completed.signal(); });

    static_cast<void>(coordinator.start(records, options));
    completed.wait();
    progress.finish();

    const std::scoped_lock lock(resultsMutex);
    return results;
}

/// Prints the notice of a successful normalization, such as audio blocks that failed to decode, when it has one.
/// The normalization itself already logged it.
void printNormalizationNotice(const OperationSucceeded& succeeded)
{
    if (succeeded.notice.isNotEmpty()) {
        console::printWarn(
            utils::format("Normalized {}, {}", succeeded.output.file.getFullPathName().quoted(), succeeded.notice)
        );
    }
}
}  // namespace

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
    std::unordered_set<juce::String> analyzedPaths;
    for (const auto& record : results) {
        analyzedPaths.insert(record.file.getFullPathName());
    }

    for (const auto& input : inputPaths) {
        if (!input.exists()) {
            errors.add(utils::format("Input not found: {}", input.getFullPathName().quoted()));
            continue;
        }

        if (input.isDirectory() || analyzedPaths.contains(input.getFullPathName())) {
            continue;
        }

        if (utils::isHiddenFile(input)) {
            errors.add(utils::format("Hidden files are not analyzed: {}", input.getFullPathName().quoted()));
        } else {
            errors.add(utils::format("Not a supported audio file: {}", input.getFullPathName().quoted()));
        }
    }

    return errors;
}

/// Executes the CLI analysis workflow with the analysis cache in the user's cache folder,
/// and returns the process exit code.
int cli::runAnalysis(const AnalyzeCommand& command)
{
    AnalysisCache cache;
    return runAnalysis(command, cache);
}

/// Executes the CLI analysis workflow with the given analysis cache, opening it first,
/// and returns the process exit code: 0 when every input was analyzed, and normalized when requested,
/// and 2 when any input was missing or skipped, or failed to analyze or normalize.
int cli::runAnalysis(const AnalyzeCommand& command, AnalysisCache& cache)
{
    auto inputPaths = command.inputPaths;

    if (inputPaths.isEmpty()) {
        inputPaths.add(juce::File::getCurrentWorkingDirectory());
        reportInfo(
            utils::format(
                "No input paths provided, using current directory: {}", inputPaths[0].getFullPathName().quoted()
            )
        );
    }

    // The analysis still works without the cache, it only has to read every file again on the next run.
    if (!cache.open()) {
        reportWarning("Could not open the analysis cache, results are not cached");
    }

    AnalysisCoordinator coordinator(cache, command.workerCount);
    AudioAnalysisOptions analysisOptions;
    analysisOptions.inputPaths = inputPaths;
    analysisOptions.recursive = command.recursive;
    analysisOptions.refresh = command.refresh;

    const auto analysisStartedAtMs = juce::Time::getMillisecondCounterHiRes();
    auto results = analyzeWithProgress(coordinator, analysisOptions);
    const auto analysisElapsedMs = juce::Time::getMillisecondCounterHiRes() - analysisStartedAtMs;
    const auto analysisFailureCount
        = std::ranges::count_if(results, [](const auto& record) { return record.hasError(); });
    reportInfo(
        utils::format(
            "Analysis complete: {:.2f} s, {} files ({} failed)",
            analysisElapsedMs / utils::millisecondsPerSecond,
            results.size(),
            analysisFailureCount
        )
    );

    sortRecords(results, command.sortMode, !command.reverse);
    reportAnalysisFailures(results);

    const auto skippedInputs = findSkippedInputs(command.inputPaths, results);

    for (const auto& error : skippedInputs) {
        reportError(error);
    }

    int failureCount = skippedInputs.size();
    std::vector<AudioAnalysisRecord> normalizedResults;

    juce::StringArray normalizationConflicts;
    std::map<juce::String, AudioNormalizationResult> normalizations;

    if (command.normalization.has_value()) {
        normalizedResults.reserve(results.size());
        std::vector<AudioAnalysisRecord> normalizableResults;
        std::ranges::copy_if(results, std::back_inserter(normalizableResults), [](const auto& record) {
            return !record.hasError();
        });
        normalizationConflicts = findBatchOutputConflicts(normalizableResults);
        normalizations = normalizeWithProgress(normalizableResults, *command.normalization, command.workerCount);
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

            if (command.normalization.has_value()) {
                normalizedResults.push_back(result);
            }
            continue;
        }

        if (!command.normalization.has_value()) {
            continue;
        }

        if (normalizationConflicts.contains(result.file.getFullPathName())) {
            ++failureCount;
            const auto conflictMessage = batchOutputConflictMessage(result.file);
            reportError(
                utils::format(
                    "Normalization output conflict: {}, {}", result.file.getFullPathName().quoted(), conflictMessage
                )
            );
            addNormalizationFailure(result, conflictMessage);
            continue;
        }

        const auto found = normalizations.find(result.file.getFullPathName());
        const auto normalization = found != normalizations.end()
            ? found->second
            : AudioNormalizationResult::failure(result.file, "The file was not normalized");

        // A converted original was moved to the trash, so its cached row is no longer needed,
        // even when the re-analysis of the committed output failed.
        // The cache logs its own failures, and a stale row only costs another read later.
        const auto forgetConvertedSource = [&](const AudioAnalysisRecord& output) {
            if (output.fullPath != result.fullPath) {
                static_cast<void>(cache.removeAnalysis(result.file));
            }
        };
        const auto reportFailure = [&](const AudioAnalysisRecord& record, const juce::String& error) {
            ++failureCount;
            reportError(utils::format("Normalization failed: {}, {}", result.file.getFullPathName().quoted(), error));
            addNormalizationFailure(record, error);
        };

        std::visit(
            Overloaded {
                [&](const OperationFailed& failed) { reportFailure(result, failed.error); },
                [&](const CommittedButUnanalyzed& unanalyzed) {
                    forgetConvertedSource(unanalyzed.output);
                    // The output replaced the source, so the failed re-analysis of the output is reported instead.
                    reportFailure(unanalyzed.output, unanalyzed.error);
                },
                [&](const OperationSucceeded& succeeded) {
                    forgetConvertedSource(succeeded.output);
                    printNormalizationNotice(succeeded);
                    static_cast<void>(cache.storeAnalysis(succeeded.output));
                    normalizedResults.push_back(succeeded.output);
                },
            },
            normalization.outcome
        );
    }

    if (command.normalization.has_value()) {
        sortRecords(normalizedResults, command.sortMode, !command.reverse);
        cli::printRecords(normalizedResults, command.outputFormat, true);
    } else {
        cli::printRecords(results, command.outputFormat, false);
    }

    return failureCount == 0 ? 0 : 2;
}
}  // namespace audiobatch
