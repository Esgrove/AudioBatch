/// Background work of the main view.
/// Declares BackgroundWorkController, which owns the analysis, normalization, and plugin processing coordinators
/// and the state of the one pass that runs at a time,
/// and BackgroundWorkListener, which receives the plans, results, and completions of that work on the message thread.
/// Also declares the free functions that describe the state for the status bar and the window title.

#pragma once

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "gui/AudioBatchComponentState.h"
#include "normalization/NormalizeCoordinator.h"
#include "plugins/PluginProcessingCoordinator.h"
#include "utils/MessageThreadBatcher.h"

#include <JuceHeader.h>

#include <optional>
#include <variant>
#include <vector>

namespace audiobatch
{
class FileActivityTracker;

namespace tests
{
class BackgroundWorkTests;
}

/// The name of the running pass and how far along it is, shown in the window title and the status bar.
struct BackgroundProgress {
    /// Name of the running pass, such as "Analyzing", or empty while idle.
    juce::String activityName;
    /// Completed fraction from 0 to 1, or empty while the pass does not know its file count yet.
    std::optional<float> fraction;
};

/// A worker started on a file of a normalization or processing pass.
struct FileStartedEvent {
    /// The file the worker started on.
    juce::File file;
};

/// A worker reported how far along it is with a file of a normalization or processing pass.
struct FileProgressEvent {
    /// The file the worker is on.
    juce::File file;
    /// Completed fraction of the file from 0 to 1.
    float progress {0.0f};
};

/// One event of a normalization or processing pass on its way to the message thread, with the run it belongs to.
/// The starts, progress reports, and results of a pass travel through one batcher,
/// so they arrive in the order the workers sent them, and a start never arrives after the result of its file.
template<typename Result>
struct OperationEvent {
    /// What happened to the file.
    std::variant<FileStartedEvent, FileProgressEvent, Result> event;
    /// Id of the coordinator run the event belongs to.
    int runId {0};
};

/// Receives the events of the background work on the message thread.
/// Messages of replaced analysis runs and of passes that already ended never reach the listener.
class BackgroundWorkListener
{
public:
    BackgroundWorkListener() = default;
    virtual ~BackgroundWorkListener() = default;

    BackgroundWorkListener(const BackgroundWorkListener&) = delete;
    BackgroundWorkListener& operator=(const BackgroundWorkListener&) = delete;
    BackgroundWorkListener(BackgroundWorkListener&&) = delete;
    BackgroundWorkListener& operator=(BackgroundWorkListener&&) = delete;

    /// Called once the running analysis has scanned its inputs, with the run already updated from the plan.
    virtual void analysisPlanReceived(const AnalysisPlan& plan, const AnalyzingWork& run) = 0;

    /// Called with a batch of results of the running analysis, without the results of rows removed during the run.
    virtual void analysisResultsReceived(std::vector<AudioAnalysisRecord> records) = 0;

    /// Called when the running analysis has finished, after its last results and once the controller is idle again.
    virtual void analysisCompleted(const AnalyzingWork& run, int totalFiles) = 0;

    /// Called with a batch of normalization results in the order they arrived, after the completed count went up.
    /// A result is passed on even after its pass ended, because its output file was already written.
    virtual void normalizeResultsReceived(std::vector<AudioNormalizationResult> results) = 0;

    /// Called when the running normalization pass has finished, once the controller is idle again.
    virtual void normalizeCompleted(const NormalizingWork& pass, int totalFiles) = 0;

    /// Called with a batch of plugin processing results in the order they arrived, after the completed count went up.
    /// A result is passed on even after its pass ended, because its output file was already written.
    virtual void processingResultsReceived(std::vector<PluginProcessingResult> results) = 0;

    /// Called when the running processing pass has finished, once the controller is idle again.
    virtual void processingCompleted(const ProcessingWork& pass, int totalFiles) = 0;

    /// Called when a processing pass could not start its workers, once the controller is idle again.
    virtual void processingStartFailed(const juce::String& errorMessage) = 0;
};

/// Runs one analysis, normalization, or plugin processing pass at a time, and tracks its progress.
/// Coordinator callbacks arrive on worker threads.
/// The controller moves them to the message thread, marks the files busy in the activity tracker,
/// and forwards the results to the listener.
/// Must only be used on the message thread.
class BackgroundWorkController
{
public:
    BackgroundWorkController(
        AnalysisCache& analysisCache,
        FileActivityTracker& activityTracker,
        BackgroundWorkListener& workListener
    );

    ~BackgroundWorkController();

    BackgroundWorkController(const BackgroundWorkController&) = delete;
    BackgroundWorkController& operator=(const BackgroundWorkController&) = delete;
    BackgroundWorkController(BackgroundWorkController&&) = delete;
    BackgroundWorkController& operator=(BackgroundWorkController&&) = delete;

    [[nodiscard]] const BackgroundWork& getWork() const noexcept;
    [[nodiscard]] bool isIdle() const noexcept;
    [[nodiscard]] bool isAnalysisInProgress() const noexcept;
    [[nodiscard]] bool isProcessingInProgress() const noexcept;
    [[nodiscard]] bool isFileOperationInProgress() const noexcept;

    [[nodiscard]] int startNormalization(
        const std::vector<AudioAnalysisRecord>& records,
        const AudioNormalizationOptions& options
    );

    [[nodiscard]] int startProcessing(
        const std::vector<AudioAnalysisRecord>& records,
        const PluginProcessingOptions& options,
        std::vector<PluginChainInstances> chainInstances,
        double startedAtMilliseconds
    );

    void cancelAnalysis();
    void cancelAndWait();
    bool cancelCurrentWork();
    void stopAllWork();
    void forgetRemovedPaths(const juce::StringArray& removedPaths, int removedPendingCount);
    void recordFailure(const juce::String& failure);
    void startAnalysis(const juce::Array<juce::File>& inputPaths, bool recursive, bool forceRefresh, bool clearResults);

private:
    friend class tests::BackgroundWorkTests;

    void connectAnalysisCallbacks();
    void connectOperationCallbacks();
    void handleAnalysisComplete(int totalFiles, int runId);
    void handleAnalysisPlan(const AnalysisPlan& plan);
    void handleAnalysisResults(std::vector<AnalysisResultMessage> messages);
    void handleNormalizeComplete(int totalFiles, int runId);
    void handleNormalizeEvents(std::vector<OperationEvent<AudioNormalizationResult>> events);
    void handleProcessingComplete(int totalFiles, int runId);
    void handleProcessingEvents(std::vector<OperationEvent<PluginProcessingResult>> events);
    void handleProcessingStartError(const juce::String& errorMessage);

    [[nodiscard]] bool isCurrentAnalysisRun(int runId) const noexcept;
    [[nodiscard]] bool isCurrentNormalizationRun(int runId) const noexcept;
    [[nodiscard]] bool isCurrentProcessingRun(int runId) const noexcept;

    /// The activities of the table rows, which the passes mark busy and idle, owned by the main component.
    FileActivityTracker& fileActivity;
    /// Receives the plans, results, and completions, owned by the main component.
    BackgroundWorkListener& listener;

    /// The analysis run, normalization pass, or plugin processing pass that is running, if any.
    BackgroundWork work;
    /// Run id of the last normalization pass that stopAllWork discarded, whose events are all dropped,
    /// or -1 when none was discarded.
    int discardedNormalizeRunId {-1};
    /// Run id of the last processing pass that stopAllWork discarded, whose events are all dropped,
    /// or -1 when none was discarded.
    int discardedProcessingRunId {-1};

    /// Hands analysis results from the workers to the message thread, merging each burst into one batch.
    /// Declared before the analysis coordinator, whose workers post to it until the coordinator is destroyed.
    utils::MessageThreadBatcher<AnalysisResultMessage> analysisResultBatcher {
        [this](std::vector<AnalysisResultMessage> messages) { handleAnalysisResults(std::move(messages)); }
    };
    /// Hands the starts, progress reports, and results of normalization to the message thread in batches.
    /// Declared before the normalize coordinator, whose workers post to it until the coordinator is destroyed.
    utils::MessageThreadBatcher<OperationEvent<AudioNormalizationResult>> normalizeEventBatcher {
        [this](std::vector<OperationEvent<AudioNormalizationResult>> events) {
            handleNormalizeEvents(std::move(events));
        }
    };
    /// Hands the starts, progress reports, and results of plugin processing to the message thread in batches.
    /// Declared before the plugin coordinator, whose workers post to it until the coordinator is destroyed.
    utils::MessageThreadBatcher<OperationEvent<PluginProcessingResult>> processingEventBatcher {
        [this](std::vector<OperationEvent<PluginProcessingResult>> events) {
            handleProcessingEvents(std::move(events));
        }
    };

    /// Runs the analysis passes.
    AnalysisCoordinator analysisCoordinator;
    /// Runs the normalization passes.
    NormalizeCoordinator normalizeCoordinator;
    /// Runs the plugin processing passes.
    PluginProcessingCoordinator pluginCoordinator;

    JUCE_DECLARE_WEAK_REFERENCEABLE(BackgroundWorkController)
};

[[nodiscard]] BackgroundProgress getBackgroundProgress(const BackgroundWork& work, float activeFileProgress);
[[nodiscard]] juce::String getFileOperationBlockedReason(const BackgroundWork& work);
[[nodiscard]] juce::String getNormalizationBlockedReason(const BackgroundWork& work);
[[nodiscard]] juce::String getProcessingBlockedReason(const BackgroundWork& work);
[[nodiscard]] juce::String getProgressText(const BackgroundWork& work);
[[nodiscard]] bool isBackgroundWorkCancelling(const BackgroundWork& work);
}  // namespace audiobatch
