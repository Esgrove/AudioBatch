/// Implementation of BackgroundWorkController.
/// Covers moving the coordinator callbacks to the message thread, the results in batches,
/// dropping the messages of replaced runs and of rows removed during a run,
/// carrying the inputs of a replaced analysis run over to the next one,
/// and the blocked reasons and progress text derived from the state of the running pass.

#include "gui/BackgroundWorkController.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/AudioFileTableModel.h"
#include "gui/FileActivityTracker.h"
#include "normalization/AudioNormalization.h"
#include "plugins/PluginProcessing.h"
#include "plugins/PluginProcessingCoordinator.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <functional>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch
{
namespace
{
/// Applies a batch of start, progress, and result events of a normalization or processing pass in posting order,
/// and returns the results to pass on.
/// Starts and progress reports of a pass that is no longer running are dropped,
/// since they would otherwise mark the same file busy in the next pass.
/// A result counts towards its pass while the pass runs,
/// and is returned even after the pass ended, because its output file was already written.
/// Every event of the discarded run is dropped, results too, since the list they belonged to was cleared.
template<typename Pass, typename Result>
std::vector<Result> applyOperationEvents(
    std::vector<OperationEvent<Result>> events,
    BackgroundWork& work,
    FileActivityTracker& fileActivity,
    const FileActivity activity,
    const juce::String Result::* resultPath,
    const int discardedRunId
)
{
    std::vector<Result> results;

    for (auto& [event, runId] : events) {
        if (runId == discardedRunId) {
            continue;
        }
        auto* pass = std::get_if<Pass>(&work);
        const auto isCurrentRun = pass != nullptr && pass->runId == runId;

        std::visit(
            Overloaded {
                [&](const FileStartedEvent& started) {
                    if (isCurrentRun) {
                        fileActivity.setActive(started.file.getFullPathName(), activity);
                    }
                },
                [&](const FileProgressEvent& progressed) {
                    if (isCurrentRun) {
                        fileActivity.setProgress(progressed.file.getFullPathName(), progressed.progress);
                    }
                },
                [&](Result& result) {
                    if (isCurrentRun) {
                        ++pass->completed;
                    }
                    fileActivity.clearActive(result.*resultPath);
                    results.push_back(std::move(result));
                },
            },
            event
        );
    }

    return results;
}
}  // namespace

/// Creates the coordinators and connects their callbacks.
/// The cache, the activity tracker, and the listener must outlive the controller.
BackgroundWorkController::BackgroundWorkController(
    AnalysisCache& analysisCache,
    FileActivityTracker& activityTracker,
    BackgroundWorkListener& workListener
) :
    fileActivity(activityTracker),
    listener(workListener),
    analysisCoordinator(analysisCache)
{
    connectAnalysisCallbacks();
    connectOperationCallbacks();
}

/// Stops every pass and waits for its workers, so no worker posts to the controller after it is destroyed.
BackgroundWorkController::~BackgroundWorkController()
{
    cancelAndWait();
}

/// Returns the state of the running pass.
const BackgroundWork& BackgroundWorkController::getWork() const noexcept
{
    return work;
}

/// True while no analysis, normalization, or plugin processing is running.
bool BackgroundWorkController::isIdle() const noexcept
{
    return std::holds_alternative<IdleWork>(work);
}

/// True from the start of an analysis run until the coordinator reports its completion.
bool BackgroundWorkController::isAnalysisInProgress() const noexcept
{
    return std::holds_alternative<AnalyzingWork>(work);
}

/// True from the start of a plugin processing pass until the coordinator reports its completion.
bool BackgroundWorkController::isProcessingInProgress() const noexcept
{
    return std::holds_alternative<ProcessingWork>(work);
}

/// True while normalization or plugin processing is rewriting files.
bool BackgroundWorkController::isFileOperationInProgress() const noexcept
{
    return std::holds_alternative<NormalizingWork>(work) || isProcessingInProgress();
}

/// Starts normalizing the records and returns the number of files the pass started.
/// A pass that started no files leaves the controller idle.
int BackgroundWorkController::startNormalization(
    const std::vector<AudioAnalysisRecord>& records,
    const AudioNormalizationOptions& options
)
{
    if (!isIdle()) {
        return 0;
    }
    auto& normalizing = work.emplace<NormalizingWork>();
    normalizing.startedAtMilliseconds = juce::Time::getMillisecondCounterHiRes();
    normalizing.expected = normalizeCoordinator.start(records, options);
    normalizing.runId = normalizeCoordinator.getCurrentRunId();

    const auto expected = normalizing.expected;
    if (expected <= 0) {
        work = IdleWork {};
    }

    return expected;
}

/// Starts processing the records through the prepared plugin chains and returns the number of files the pass started.
/// A pass that started no files leaves the controller idle.
int BackgroundWorkController::startProcessing(
    const std::vector<AudioAnalysisRecord>& records,
    const PluginProcessingOptions& options,
    std::vector<PluginChainInstances> chainInstances,
    const double startedAtMilliseconds
)
{
    if (!isIdle()) {
        return 0;
    }
    auto& processing = work.emplace<ProcessingWork>();
    processing.startedAtMilliseconds = startedAtMilliseconds;
    processing.expected = pluginCoordinator.start(records, options, std::move(chainInstances));
    processing.runId = pluginCoordinator.getCurrentRunId();

    const auto expected = processing.expected;
    if (expected <= 0) {
        work = IdleWork {};
    }

    return expected;
}

/// Ends the running analysis, if any, without waiting for its workers.
/// The results already queued are dropped, since they belong to a run that is no longer current.
void BackgroundWorkController::cancelAnalysis()
{
    if (isAnalysisInProgress()) {
        analysisCoordinator.cancel();
        work = IdleWork {};
    }
}

/// Cancels the running analysis, normalization, or processing pass from the Cancel action, without waiting.
/// An analysis ends at once.
/// A normalization or processing pass stops its files that are not committed yet, which keep their rows,
/// and completes once the files already being committed have reported their results.
/// Returns false when nothing was running.
bool BackgroundWorkController::cancelCurrentWork()
{
    if (isAnalysisInProgress()) {
        cancelAnalysis();
        return true;
    }

    if (auto* normalizing = std::get_if<NormalizingWork>(&work)) {
        normalizing->cancelling = true;
        normalizeCoordinator.requestStop();
        return true;
    }

    if (auto* processing = std::get_if<ProcessingWork>(&work)) {
        processing->cancelling = true;
        pluginCoordinator.requestStop();
        return true;
    }

    return false;
}

/// Clears presentation ownership without releasing a writer that may still commit.
/// Stopped writers retain their run and completion callback until all files have returned,
/// but none of their queued or later results can repopulate the cleared list.
void BackgroundWorkController::stopAllWork()
{
    // Results of these runs may still be queued or posted by a worker that passed its last check,
    // and they would bring rows back into the cleared list.
    if (const auto* normalizing = std::get_if<NormalizingWork>(&work)) {
        discardedNormalizeRunId = normalizing->runId;
    }
    if (const auto* processing = std::get_if<ProcessingWork>(&work)) {
        discardedProcessingRunId = processing->runId;
    }
    analysisCoordinator.cancel();
    if (isFileOperationInProgress()) {
        // Keep the run current so its last job still publishes the terminal callback.
        static_cast<void>(cancelCurrentWork());
    } else {
        work = IdleWork {};
    }
    fileActivity.clearAllActive();
}

/// Stops every pass and waits for its workers to finish.
void BackgroundWorkController::cancelAndWait()
{
    analysisCoordinator.cancelAndWait();
    normalizeCoordinator.cancelAndWait();
    pluginCoordinator.cancelAndWait();
}

/// Keeps the running analysis from adding the removed rows back,
/// and lowers its expected count by the removed rows that were still waiting for a result.
/// Finished rows were already counted, so they leave the count unchanged.
void BackgroundWorkController::forgetRemovedPaths(const juce::StringArray& removedPaths, const int removedPendingCount)
{
    auto* analyzing = std::get_if<AnalyzingWork>(&work);
    if (analyzing == nullptr) {
        return;
    }

    // A carried over run could deliver a result for a finished row again, so every removed path is kept.
    for (const auto& path : removedPaths) {
        analyzing->removedPaths.insert(path);
    }

    analyzing->expected = juce::jmax(0, analyzing->expected - removedPendingCount);
    analyzing->completed = juce::jmin(analyzing->completed, analyzing->expected);
}

/// Adds a "file: error" line to the failure summary of the running normalization or processing pass.
void BackgroundWorkController::recordFailure(const juce::String& failure)
{
    if (auto* normalizing = std::get_if<NormalizingWork>(&work)) {
        normalizing->failures.add(failure);
    } else if (auto* processing = std::get_if<ProcessingWork>(&work)) {
        processing->failures.add(failure);
    }
}

/// Starts an analysis run of the input paths.
/// A run that keeps the results replaces a running one, so it also takes over the inputs of that run,
/// which would otherwise leave its placeholder rows waiting forever.
void BackgroundWorkController::startAnalysis(
    const juce::Array<juce::File>& inputPaths,
    const bool recursive,
    const bool forceRefresh,
    const bool clearResults
)
{
    if (isFileOperationInProgress()) {
        return;
    }
    AudioAnalysisOptions options;
    options.inputPaths = inputPaths;
    options.recursive = recursive;
    options.refresh = forceRefresh;

    AnalyzingWork run;

    if (const auto* replaced = std::get_if<AnalyzingWork>(&work); replaced != nullptr && !clearResults) {
        for (const auto& path : replaced->options.inputPaths) {
            options.inputPaths.addIfNotAlreadyThere(path);
        }

        options.recursive = options.recursive || replaced->options.recursive;
        options.refresh = options.refresh || replaced->options.refresh;
        run.removedPaths = replaced->removedPaths;

        // A file the user adds again is wanted back in the table.
        for (const auto& path : inputPaths) {
            run.removedPaths.erase(path.getFullPathName());
        }
    }

    run.options = options;
    run.startedAtMs = juce::Time::getMillisecondCounterHiRes();
    run.replacesResults = clearResults;
    run.runId = analysisCoordinator.start(options);
    work = std::move(run);
}

/// Connects the analysis coordinator to the message thread handlers and the activity tracker.
/// Results go through the batcher, which merges a burst of them into one batch.
void BackgroundWorkController::connectAnalysisCallbacks()
{
    const juce::WeakReference<BackgroundWorkController> weakThis(this);

    analysisCoordinator.setPlanCallback([weakThis](const AnalysisPlan& plan) {
        juce::MessageManager::callAsync([weakThis, plan] {
            if (auto* controller = weakThis.get()) {
                controller->handleAnalysisPlan(plan);
            }
        });
    });
    analysisCoordinator.setResultCallback([this](const AudioAnalysisRecord& record, const int runId) {
        analysisResultBatcher.post({.record = record, .runId = runId});
    });
    analysisCoordinator.setCompletionCallback([weakThis](const int totalFiles, const int runId) {
        juce::MessageManager::callAsync([weakThis, totalFiles, runId] {
            if (auto* controller = weakThis.get()) {
                controller->handleAnalysisComplete(totalFiles, runId);
            }
        });
    });
    analysisCoordinator.setStartingCallback([weakThis](const juce::File& file, const int runId) {
        juce::MessageManager::callAsync([weakThis, file, runId] {
            if (auto* controller = weakThis.get(); controller != nullptr && controller->isCurrentAnalysisRun(runId)) {
                controller->fileActivity.setActive(file.getFullPathName(), FileActivity::analyzing);
            }
        });
    });
    analysisCoordinator.setProgressCallback([weakThis](const juce::File& file, const float progress, const int runId) {
        juce::MessageManager::callAsync([weakThis, file, progress, runId] {
            if (auto* controller = weakThis.get(); controller != nullptr && controller->isCurrentAnalysisRun(runId)) {
                controller->fileActivity.setProgress(file.getFullPathName(), progress);
            }
        });
    });
}

/// Connects the normalization and plugin processing coordinators to the message thread handlers
/// and the activity tracker.
/// Files switch from waiting to busy when a worker starts on them.
/// The starts, progress reports, and results of each pass go through the batcher of the pass,
/// so a burst of results costs one table refresh, and the completion goes through the message queue after them.
void BackgroundWorkController::connectOperationCallbacks()
{
    const juce::WeakReference<BackgroundWorkController> weakThis(this);

    const auto postToMessageThread = [weakThis](auto handler) {
        return [weakThis, handler](const auto&... arguments) {
            juce::MessageManager::callAsync([weakThis, handler, arguments...] {
                if (auto* controller = weakThis.get()) {
                    std::invoke(handler, controller, arguments...);
                }
            });
        };
    };

    const auto postEvents = [](auto& batcher, auto& coordinator) {
        coordinator.setStartingCallback([&batcher](const juce::File& file, const int runId) {
            batcher.post({.event = FileStartedEvent {.file = file}, .runId = runId});
        });
        coordinator.setProgressCallback([&batcher](const juce::File& file, const float progress, const int runId) {
            batcher.post({.event = FileProgressEvent {.file = file, .progress = progress}, .runId = runId});
        });
        coordinator.setResultCallback([&batcher](const auto& result, const int runId) {
            batcher.post({.event = result, .runId = runId});
        });
    };

    postEvents(normalizeEventBatcher, normalizeCoordinator);
    normalizeCoordinator.setCompletionCallback(postToMessageThread(&BackgroundWorkController::handleNormalizeComplete));

    postEvents(processingEventBatcher, pluginCoordinator);
    pluginCoordinator.setCompletionCallback(postToMessageThread(&BackgroundWorkController::handleProcessingComplete));
    pluginCoordinator.setStartErrorCallback(postToMessageThread(&BackgroundWorkController::handleProcessingStartError));
}

/// Ends the running analysis and reports its completion.
/// Results are batched separately from this message, so the ones still queued are delivered first,
/// otherwise the listener would see their rows as unfinished.
/// Completion of a replaced run is ignored.
void BackgroundWorkController::handleAnalysisComplete(const int totalFiles, const int runId)
{
    if (!isCurrentAnalysisRun(runId)) {
        return;
    }

    analysisResultBatcher.deliverPending();

    const auto run = std::get<AnalyzingWork>(work);
    work = IdleWork {};
    listener.analysisCompleted(run, totalFiles);
}

/// Records how many files the running analysis found and how many it decodes, then reports the plan.
/// Called once per run, after the coordinator has scanned the input folders.
void BackgroundWorkController::handleAnalysisPlan(const AnalysisPlan& plan)
{
    auto* analyzing = std::get_if<AnalyzingWork>(&work);
    if (analyzing == nullptr || plan.runId != analyzing->runId) {
        return;
    }

    analyzing->planReceived = true;
    analyzing->expected = plan.files.size();
    analyzing->analyzedFiles = plan.staleFiles.size();
    utils::logInfo("Found {} audio files", analyzing->expected);
    listener.analysisPlanReceived(plan, *analyzing);
}

/// Counts a batch of analysis results and passes on the ones the table still wants.
/// Results from a replaced run are ignored, since their queued messages can arrive after a new run started.
void BackgroundWorkController::handleAnalysisResults(std::vector<AnalysisResultMessage> messages)
{
    auto* analyzing = std::get_if<AnalyzingWork>(&work);
    if (analyzing == nullptr) {
        return;
    }

    std::vector<AudioAnalysisRecord> records;
    records.reserve(messages.size());

    for (auto& [record, runId] : messages) {
        if (runId != analyzing->runId) {
            continue;
        }

        fileActivity.clearActive(record.fullPath);

        // The expected count already went down when the row was removed.
        if (analyzing->removedPaths.contains(record.fullPath)) {
            continue;
        }

        ++analyzing->completed;
        records.push_back(std::move(record));
    }

    listener.analysisResultsReceived(std::move(records));
}

/// Ends the normalization pass and reports its completion.
/// The events still queued in the batcher are delivered first, so the pass counts its last results.
/// A pass that started no files has already ended,
/// and other work, even another normalization pass, may run by the time its completion arrives.
void BackgroundWorkController::handleNormalizeComplete(const int totalFiles, const int runId)
{
    normalizeEventBatcher.deliverPending();

    if (!isCurrentNormalizationRun(runId)) {
        return;
    }

    const auto pass = std::get<NormalizingWork>(work);
    work = IdleWork {};
    listener.normalizeCompleted(pass, totalFiles);
}

/// Applies a batch of normalization events and passes the results in it on together.
void BackgroundWorkController::handleNormalizeEvents(std::vector<OperationEvent<AudioNormalizationResult>> events)
{
    auto results = applyOperationEvents<NormalizingWork>(
        std::move(events),
        work,
        fileActivity,
        FileActivity::normalizing,
        &AudioNormalizationResult::fullPath,
        discardedNormalizeRunId
    );
    if (!results.empty()) {
        listener.normalizeResultsReceived(std::move(results));
    }
}

/// Ends the plugin processing pass and reports its completion.
/// The events still queued in the batcher are delivered first, so the pass counts its last results.
/// A pass that started no files or failed to start has already ended,
/// and another pass may run by the time its completion arrives.
void BackgroundWorkController::handleProcessingComplete(const int totalFiles, const int runId)
{
    processingEventBatcher.deliverPending();

    if (!isCurrentProcessingRun(runId)) {
        return;
    }

    const auto pass = std::get<ProcessingWork>(work);
    work = IdleWork {};
    listener.processingCompleted(pass, totalFiles);
}

/// Applies a batch of plugin processing events and passes the results in it on together.
void BackgroundWorkController::handleProcessingEvents(std::vector<OperationEvent<PluginProcessingResult>> events)
{
    auto results = applyOperationEvents<ProcessingWork>(
        std::move(events),
        work,
        fileActivity,
        FileActivity::processing,
        &PluginProcessingResult::originalFullPath,
        discardedProcessingRunId
    );
    if (!results.empty()) {
        listener.processingResultsReceived(std::move(results));
    }
}

/// Ends a processing pass whose workers could not start, and reports the error.
void BackgroundWorkController::handleProcessingStartError(const juce::String& errorMessage)
{
    if (isProcessingInProgress()) {
        work = IdleWork {};
    }
    listener.processingStartFailed(errorMessage);
}

/// True while the analysis run with the given id is running,
/// which drops the queued messages of runs that were replaced or have ended.
bool BackgroundWorkController::isCurrentAnalysisRun(const int runId) const noexcept
{
    const auto* analyzing = std::get_if<AnalyzingWork>(&work);
    return analyzing != nullptr && analyzing->runId == runId;
}

/// True while the normalization pass with the given normalize coordinator run id is running.
bool BackgroundWorkController::isCurrentNormalizationRun(const int runId) const noexcept
{
    const auto* normalizing = std::get_if<NormalizingWork>(&work);
    return normalizing != nullptr && normalizing->runId == runId;
}

/// True while the processing pass with the given plugin coordinator run id is running.
/// The coordinators count their run ids separately, so each kind of pass is checked on its own.
bool BackgroundWorkController::isCurrentProcessingRun(const int runId) const noexcept
{
    const auto* processing = std::get_if<ProcessingWork>(&work);
    return processing != nullptr && processing->runId == runId;
}

/// Returns the name of the running pass and its completed fraction.
/// The fraction counts finished files plus the completed fractions of the files being worked on,
/// so it advances smoothly within long files instead of jumping once per file.
BackgroundProgress getBackgroundProgress(const BackgroundWork& work, const float activeFileProgress)
{
    const auto getRunFraction = [activeFileProgress](const int completed, const int expected) -> std::optional<float> {
        if (expected <= 0) {
            return std::nullopt;
        }

        const auto done = static_cast<float>(completed) + activeFileProgress;
        return juce::jlimit(0.0f, 1.0f, done / static_cast<float>(expected));
    };

    return std::visit(
        Overloaded {
            [](const IdleWork& /*idle*/) { return BackgroundProgress {}; },
            [&](const AnalyzingWork& analyzing) {
                if (!analyzing.planReceived) {
                    return BackgroundProgress {.activityName = "Scanning", .fraction = std::nullopt};
                }
                return BackgroundProgress {
                    .activityName = "Analyzing",
                    .fraction = getRunFraction(analyzing.completed, analyzing.expected),
                };
            },
            [&](const NormalizingWork& normalizing) {
                return BackgroundProgress {
                    .activityName = "Normalizing",
                    .fraction = getRunFraction(normalizing.completed, normalizing.expected),
                };
            },
            [&](const ProcessingWork& processing) {
                return BackgroundProgress {
                    .activityName = "Processing",
                    .fraction = getRunFraction(processing.completed, processing.expected),
                };
            },
        },
        work
    );
}

/// Returns the reason analysis or a change to the list cannot happen right now
/// because files are being normalized or processed, or an empty string when it can.
juce::String getFileOperationBlockedReason(const BackgroundWork& work)
{
    return std::visit(
        Overloaded {
            [](const IdleWork& /*idle*/) { return juce::String(); },
            [](const AnalyzingWork& /*analyzing*/) { return juce::String(); },
            [](const NormalizingWork& /*normalizing*/) { return juce::String("Normalization in progress"); },
            [](const ProcessingWork& /*processing*/) { return juce::String("Plugin processing in progress"); },
        },
        work
    );
}

/// Returns the reason a normalization pass cannot start right now because other work is running,
/// or an empty string when a pass may start.
juce::String getNormalizationBlockedReason(const BackgroundWork& work)
{
    return std::visit(
        Overloaded {
            [](const IdleWork& /*idle*/) { return juce::String(); },
            [](const AnalyzingWork& /*analyzing*/) {
                return juce::String("Wait for analysis to finish before normalizing");
            },
            [](const NormalizingWork& /*normalizing*/) { return juce::String("Normalization already in progress"); },
            [](const ProcessingWork& /*processing*/) {
                return juce::String("Wait for plugin processing to finish before normalizing");
            },
        },
        work
    );
}

/// Returns the reason a processing run cannot start right now because other work is running,
/// or an empty string when a run may start.
juce::String getProcessingBlockedReason(const BackgroundWork& work)
{
    return std::visit(
        Overloaded {
            [](const IdleWork& /*idle*/) { return juce::String(); },
            [](const AnalyzingWork& /*analyzing*/) {
                return juce::String("Wait for analysis to finish before processing");
            },
            [](const NormalizingWork& /*normalizing*/) {
                return juce::String("Wait for normalization to finish before processing");
            },
            [](const ProcessingWork& /*processing*/) { return juce::String("Processing already in progress"); },
        },
        work
    );
}

/// Returns true while a cancelled normalization or processing pass waits for its files in progress.
bool isBackgroundWorkCancelling(const BackgroundWork& work)
{
    if (const auto* normalizing = std::get_if<NormalizingWork>(&work)) {
        return normalizing->cancelling;
    }
    if (const auto* processing = std::get_if<ProcessingWork>(&work)) {
        return processing->cancelling;
    }
    return false;
}

/// Returns the progress text of the running pass for the status bar,
/// or an empty string while idle or while a pass does not know its file count yet,
/// in which case the status bar shows the summary of the list instead.
juce::String getProgressText(const BackgroundWork& work)
{
    return std::visit(
        Overloaded {
            [](const IdleWork& /*idle*/) { return juce::String(); },
            [](const AnalyzingWork& analyzing) {
                return analyzing.planReceived
                    ? utils::format("Analyzing {}/{}", analyzing.completed, analyzing.expected)
                    : juce::String("Scanning for audio files...");
            },
            [](const NormalizingWork& normalizing) {
                if (normalizing.cancelling) {
                    return juce::String("Cancelling normalization...");
                }
                return normalizing.expected > 0
                    ? utils::format("Normalizing {}/{}", normalizing.completed, normalizing.expected)
                    : juce::String();
            },
            [](const ProcessingWork& processing) {
                if (processing.cancelling) {
                    return juce::String("Cancelling plugin processing...");
                }
                return processing.expected > 0
                    ? utils::format("Processing {}/{}", processing.completed, processing.expected)
                    : juce::String();
            },
        },
        work
    );
}
}  // namespace audiobatch
