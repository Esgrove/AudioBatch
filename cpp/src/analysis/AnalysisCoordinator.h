/// Background orchestration for audio analysis runs.
/// AnalysisCoordinator runs analysis jobs on a thread pool, serves cached results first,
/// and publishes starting, progress, per-result, and completion callbacks tagged with a run id
/// so results from cancelled runs are ignored.

#pragma once

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "utils/BackgroundRunner.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace audiobatch
{
/// The files of a run, published once the input paths are expanded and checked against the cache.
struct AnalysisPlan {
    /// The run the plan belongs to.
    int runId {0};
    /// Every supported file found in the input paths.
    juce::Array<juce::File> files;
    /// The files without a current cache entry, which are analyzed in this run.
    juce::Array<juce::File> staleFiles;
};

/// Coordinates background audio analysis jobs and marshals result callbacks to the UI layer.
class AnalysisCoordinator
{
public:
    // Every callback carries the id of the run it belongs to,
    // so a receiver that handles them asynchronously can drop messages from a run that was replaced meanwhile.

    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles, int runId)>;

    /// Called for each analysed file as results become available.
    using ResultCallback = std::function<void(const AudioAnalysisRecord& result, int runId)>;

    /// Called right before a file's analysis begins, from the worker thread.
    using StartingCallback = std::function<void(const juce::File& file, int runId)>;

    /// Called from the worker thread with the analyzed fraction of a file, throttled to a few updates per second.
    using ProgressCallback = std::function<void(const juce::File& file, float progress, int runId)>;

    /// Called once per run from a worker thread, before any of its results.
    using PlanCallback = std::function<void(const AnalysisPlan& plan)>;

    /// Called by analyzeBlocking for each result as it arrives, from a worker thread.
    using ResultObserver = std::function<void(const AudioAnalysisRecord& result)>;

    explicit AnalysisCoordinator(AnalysisCache& analysisCache, int workerCount = juce::SystemStats::getNumCpus());

    ~AnalysisCoordinator();

    AnalysisCoordinator(const AnalysisCoordinator&) = delete;
    AnalysisCoordinator& operator=(const AnalysisCoordinator&) = delete;
    AnalysisCoordinator(AnalysisCoordinator&&) = delete;
    AnalysisCoordinator& operator=(AnalysisCoordinator&&) = delete;

    std::vector<AudioAnalysisRecord> analyzeBlocking(
        const AudioAnalysisOptions& options,
        const ResultObserver& onResult = nullptr
    );

    [[nodiscard]] int start(const AudioAnalysisOptions& options);

    void cancel();
    void cancelAndWait();
    void setCompletionCallback(CompletionCallback callback);
    void setPlanCallback(PlanCallback callback);
    void setProgressCallback(ProgressCallback callback);
    void setResultCallback(ResultCallback callback);
    void setStartingCallback(StartingCallback callback);

private:
    void planRun(const AudioAnalysisOptions& options, int runId);

    void analyzeStaleFile(
        const juce::File& file,
        int runId,
        int totalFiles,
        const std::shared_ptr<std::atomic<int>>& pendingFiles
    ) const;

    /// The cache the runs look files up in and store their results in, which outlives the coordinator.
    AnalysisCache& cache;
    /// Receives the completion of each run.
    RunCallback<int, int> completionCallback;
    /// Receives the files and stale files each run found.
    RunCallback<const AnalysisPlan&> planCallback;
    /// Receives the analyzed fraction of each file.
    RunCallback<const juce::File&, float, int> progressCallback;
    /// Receives each cached and fresh result.
    RunCallback<const AudioAnalysisRecord&, int> resultCallback;
    /// Receives each file a worker starts to analyze.
    RunCallback<const juce::File&, int> startingCallback;

    /// Declared last, so the callbacks the jobs publish to outlive it.
    BackgroundRunner runner;
};
}  // namespace audiobatch
