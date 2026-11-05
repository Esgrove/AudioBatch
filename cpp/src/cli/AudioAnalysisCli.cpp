/// Implementation of the CLI analysis workflow in cli::runAnalysis:
/// analyzing the inputs, normalizing them when requested, and printing the results.
/// The argument parsing is in AudioAnalysisCliParse.cpp and the output formatting in AudioAnalysisCliOutput.cpp.

#include "cli/AudioAnalysisCli.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "cli/AudioAnalysisCliOutput.h"
#include "cli/PluginProcessCli.h"
#include "normalization/AudioNormalization.h"
#include "normalization/NormalizeCoordinator.h"
#include "utils/ConsolePrint.h"
#include "utils/ConsoleProgress.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"
#include <fmt/base.h>
#include <fmt/color.h>
#include <unordered_set>

#include <algorithm>
#include <cstdio>
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
/// Polling interval keeps cancellation responsive without busy waiting on the calling thread.
constexpr int INTERRUPT_POLL_MILLISECONDS {20};

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
    const AudioAnalysisOptions& options,
    const utils::CancellationCheck& isInterrupted
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

    auto results = coordinator.analyzeBlocking(
        options,
        [&](const AudioAnalysisRecord& record) {
            bool decoded {false};
            {
                const std::scoped_lock lock(staleMutex);
                decoded = stalePaths.contains(record.fullPath);
            }
            if (decoded) {
                progress.increment();
            }
        },
        isInterrupted
    );

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
    const int workerCount,
    AnalysisCache& cache,
    const utils::CancellationCheck& isInterrupted
)
{
    console::ProgressLine progress;
    progress.setTotal(static_cast<int>(records.size()));
    std::mutex resultsMutex;
    std::map<juce::String, AudioNormalizationResult> results;
    juce::WaitableEvent completed;

    // The coordinator is declared last, so it is destroyed, waiting for its jobs, before the locals they use.
    NormalizeCoordinator coordinator(workerCount);
    coordinator.setResultPersistenceCallback([&cache](AudioNormalizationResult& result) {
        cli::persistOperationOutput(cache, result.outcome);
    });
    coordinator.setStartingCallback([&progress](const juce::File& file, int /*runId*/) {
        progress.setMessage(file.getFileName());
    });
    coordinator.setResultCallback([&](const AudioNormalizationResult& result, int /*runId*/) {
        const auto* failed = std::get_if<OperationFailed>(&result.outcome);
        if (failed == nullptr || !failed->cancelled) {
            const std::scoped_lock lock(resultsMutex);
            results.insert_or_assign(result.file.getFullPathName(), result);
        }
        progress.increment();
    });
    coordinator.setCompletionCallback([&completed](int /*totalFiles*/, int /*runId*/) { completed.signal(); });

    static_cast<void>(coordinator.start(records, options));
    while (!completed.wait(INTERRUPT_POLL_MILLISECONDS)) {
        if (utils::isCancellationRequested(isInterrupted)) {
            coordinator.requestStop();
        }
    }
    progress.finish();

    const std::scoped_lock lock(resultsMutex);
    return results;
}

/// Prints the notice of a successful normalization, such as audio blocks that failed to decode, when it has one.
/// The normalization itself already logged it.
void printNormalizationNotice(const OperationSucceeded& succeeded, const bool processing)
{
    if (succeeded.notice.isNotEmpty()) {
        console::printWarn(
            utils::format(
                "{} {}, {}",
                processing ? "Processed" : "Normalized",
                succeeded.output.file.getFullPathName().quoted(),
                succeeded.notice
            )
        );
    }
}

/// Reconciles one completed operation with its original row, including cache removal after conversion.
/// Committed but unanalyzed outputs are reported at their published path, never as the removed original.
void reconcileOperation(
    const AudioAnalysisRecord& source,
    const AudioNormalizationResult& operation,
    AnalysisCache& cache,
    std::vector<AudioAnalysisRecord>& reported,
    int& failureCount,
    const bool processing,
    const bool conflict
)
{
    const auto* label = processing ? "Processing" : "Normalization";
    const auto reportFailure = [&](AudioAnalysisRecord record, const juce::String& error) {
        ++failureCount;
        reportError(
            utils::format(
                "{}{}: {}, {}",
                label,
                conflict ? " output conflict" : " failed",
                source.file.getFullPathName().quoted(),
                error
            )
        );
        record.status = AudioAnalysisStatus::failed;
        record.errorMessage = error;
        reported.push_back(std::move(record));
    };
    const auto forgetConvertedSource = [&](const AudioAnalysisRecord& output) {
        if (output.fullPath != source.fullPath) {
            static_cast<void>(cache.removeAnalysis(source));
        }
    };
    std::visit(
        Overloaded {
            [&](const OperationFailed& failed) { reportFailure(source, failed.error); },
            [&](const CommittedButUnanalyzed& unanalyzed) {
                forgetConvertedSource(unanalyzed.output);
                reportFailure(unanalyzed.output, unanalyzed.error);
            },
            [&](const OperationSucceeded& succeeded) {
                forgetConvertedSource(succeeded.output);
                printNormalizationNotice(succeeded, processing);
                reported.push_back(succeeded.output);
            },
        },
        operation.outcome
    );
}

/// Collects operation results in source order, retaining analysis failures and omitting unstarted cancelled files.
int collectOperationRecords(
    const std::vector<AudioAnalysisRecord>& sources,
    const std::map<juce::String, AudioNormalizationResult>& operations,
    AnalysisCache& cache,
    std::vector<AudioAnalysisRecord>& reported,
    const juce::StringArray& conflicts,
    const bool processing,
    const utils::CancellationCheck& isInterrupted
)
{
    int failures {0};
    for (const auto& source : sources) {
        if (source.hasError()) {
            ++failures;
            reported.push_back(source);
            continue;
        }
        const auto found = operations.find(source.fullPath);
        if (found == operations.end() && utils::isCancellationRequested(isInterrupted)) {
            continue;
        }
        const auto operation = found != operations.end()
            ? found->second
            : AudioNormalizationResult::failure(source.file, "The file was not processed");
        reconcileOperation(
            source, operation, cache, reported, failures, processing, conflicts.contains(source.fullPath)
        );
    }
    return failures;
}
}  // namespace

/// Persists exact rendered measurements and optional artifacts only while the committed identity still owns the output.
void cli::persistOperationOutput(AnalysisCache& cache, const OperationOutcome& outcome)
{
    const auto* succeeded = std::get_if<OperationSucceeded>(&outcome);
    if (succeeded == nullptr || !succeeded->output.isReady()) {
        return;
    }
    const auto& output = succeeded->output;
    const utils::FileIdentity identity(output.file);
    if (!output.fromCache && output.matchesFileOnDisk() && identity.matches(output.file)
        && (output.renderedIdentity == nullptr || output.renderedIdentity->owns(output.file))
        && cache.storeAnalysis(output) && output.matchesFileOnDisk() && identity.matches(output.file)
        && (output.renderedIdentity == nullptr || output.renderedIdentity->owns(output.file)))
    {
        if (output.nativeWaveform) {
            static_cast<void>(cache.storeWaveformData(identity, *output.nativeWaveform));
        }
        if (output.spectrogram != nullptr) {
            static_cast<void>(cache.storeSpectrogramData(identity, *output.spectrogram));
        }
    }
}

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
int cli::runAnalysis(const AnalyzeCommand& command, const utils::CancellationCheck& isInterrupted)
{
    AnalysisCache cache;
    return runAnalysis(command, cache, isInterrupted);
}

/// Executes the CLI analysis workflow with the given analysis cache, opening it first,
/// and returns the process exit code: 0 when every input was analyzed, and normalized when requested,
/// and 2 when any input was missing or skipped, or failed to analyze or normalize.
int cli::runAnalysis(
    const AnalyzeCommand& command,
    AnalysisCache& cache,
    const utils::CancellationCheck& isInterrupted,
    const ProcessCommand* processing
)
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
    auto results = analyzeWithProgress(coordinator, analysisOptions, isInterrupted);
    if (utils::isCancellationRequested(isInterrupted)) {
        reportWarning("Interrupted. Analysis cancelled.");
        return EXIT_INTERRUPTED;
    }
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

    const bool rewriting = command.normalization.has_value() || processing != nullptr;
    if (rewriting) {
        normalizedResults.reserve(results.size());
        std::vector<AudioAnalysisRecord> normalizableResults;
        std::ranges::copy_if(results, std::back_inserter(normalizableResults), [](const auto& record) {
            return !record.hasError();
        });
        normalizationConflicts = findBatchOutputConflicts(normalizableResults);
        if (processing != nullptr) {
            auto processed = cli::processWithProgress(normalizableResults, *processing, cache, isInterrupted);
            if (!processed) {
                reportError(processed.error());
                return utils::isCancellationRequested(isInterrupted) ? EXIT_INTERRUPTED : 1;
            }
            normalizations = std::move(*processed);
        } else if (command.normalization.has_value()) {
            normalizations = normalizeWithProgress(
                normalizableResults, *command.normalization, command.workerCount, cache, isInterrupted
            );
        }
    }

    if (rewriting) {
        failureCount += collectOperationRecords(
            results,
            normalizations,
            cache,
            normalizedResults,
            normalizationConflicts,
            processing != nullptr,
            isInterrupted
        );
        sortRecords(normalizedResults, command.sortMode, !command.reverse);
        cli::printRecords(normalizedResults, command.outputFormat, true);
    } else {
        failureCount += static_cast<int>(analysisFailureCount);
        cli::printRecords(results, command.outputFormat, false);
    }

    if (utils::isCancellationRequested(isInterrupted)) {
        reportWarning(
            processing != nullptr ? "Interrupted. Processing cancelled, committed outputs retained."
                                  : "Interrupted. Normalization cancelled, committed outputs retained."
        );
        return EXIT_INTERRUPTED;
    }
    return failureCount == 0 ? 0 : 2;
}
}  // namespace audiobatch
