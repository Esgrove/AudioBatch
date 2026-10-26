/// Implementation of BackgroundWorkController.
/// Covers moving the coordinator callbacks to the message thread,
/// dropping the messages of replaced runs and of rows removed during a run,
/// carrying the inputs of a replaced analysis run over to the next one,
/// and the blocked reasons and progress text derived from the state of the running pass.

#include "gui/BackgroundWorkController.h"

#include "gui/FileActivityTracker.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <functional>
#include <utility>
#include <variant>

namespace audiobatch
{
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
/// A start arriving after its pass ended belongs to no current pass, so it does not mark the file busy again.
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

    const auto postStarting = [weakThis](const FileActivity activity, auto isCurrentRun) {
        return [weakThis, activity, isCurrentRun](const juce::File& file, const int runId) {
            juce::MessageManager::callAsync([weakThis, file, activity, isCurrentRun, runId] {
                if (const auto* controller = weakThis.get();
                    controller != nullptr && (controller->*isCurrentRun)(runId))
                {
                    controller->fileActivity.setActive(file.getFullPathName(), activity);
                }
            });
        };
    };

    const auto postProgress = [weakThis](const juce::File& file, const float progress, const int /*runId*/) {
        juce::MessageManager::callAsync([weakThis, file, progress] {
            if (auto* controller = weakThis.get()) {
                controller->fileActivity.setProgress(file.getFullPathName(), progress);
            }
        });
    };

    normalizeCoordinator.setStartingCallback(
        postStarting(FileActivity::normalizing, &BackgroundWorkController::isCurrentNormalizationRun)
    );
    normalizeCoordinator.setProgressCallback(postProgress);
    normalizeCoordinator.setResultCallback(postToMessageThread(&BackgroundWorkController::handleNormalizeResult));
    normalizeCoordinator.setCompletionCallback(postToMessageThread(&BackgroundWorkController::handleNormalizeComplete));

    pluginCoordinator.setStartingCallback(
        postStarting(FileActivity::processing, &BackgroundWorkController::isCurrentProcessingRun)
    );
    pluginCoordinator.setProgressCallback(postProgress);
    pluginCoordinator.setResultCallback(postToMessageThread(&BackgroundWorkController::handleProcessingResult));
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
/// A pass that started no files has already ended,
/// and other work, even another normalization pass, may run by the time its completion arrives.
void BackgroundWorkController::handleNormalizeComplete(const int totalFiles, const int runId)
{
    if (!isCurrentNormalizationRun(runId)) {
        return;
    }

    const auto pass = std::get<NormalizingWork>(work);
    work = IdleWork {};
    listener.normalizeCompleted(pass, totalFiles);
}

/// Counts one normalization result towards its pass and passes it on.
/// A result is passed on even after its pass ended, because its output file was already written.
void BackgroundWorkController::handleNormalizeResult(const AudioNormalizationResult& result, const int runId)
{
    if (isCurrentNormalizationRun(runId)) {
        ++std::get<NormalizingWork>(work).completed;
    }
    fileActivity.clearActive(result.fullPath);
    listener.normalizeResultReceived(result);
}

/// Ends the plugin processing pass and reports its completion.
/// A pass that started no files or failed to start has already ended,
/// and another pass may run by the time its completion arrives.
void BackgroundWorkController::handleProcessingComplete(const int totalFiles, const int runId)
{
    if (!isCurrentProcessingRun(runId)) {
        return;
    }

    const auto pass = std::get<ProcessingWork>(work);
    work = IdleWork {};
    listener.processingCompleted(pass, totalFiles);
}

/// Counts one plugin processing result towards its pass and passes it on.
/// A result is passed on even after its pass ended, because its output file was already written.
void BackgroundWorkController::handleProcessingResult(const PluginProcessingResult& result, const int runId)
{
    if (isCurrentProcessingRun(runId)) {
        ++std::get<ProcessingWork>(work).completed;
    }
    fileActivity.clearActive(result.originalFullPath);
    listener.processingResultReceived(result);
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
                return normalizing.expected > 0
                    ? utils::format("Normalizing {}/{}", normalizing.completed, normalizing.expected)
                    : juce::String();
            },
            [](const ProcessingWork& processing) {
                return processing.expected > 0
                    ? utils::format("Processing {}/{}", processing.completed, processing.expected)
                    : juce::String();
            },
        },
        work
    );
}
}  // namespace audiobatch
