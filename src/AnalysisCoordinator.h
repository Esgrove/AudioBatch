/// Background orchestration for audio analysis runs.
/// AnalysisCoordinator runs analysis jobs on a thread pool, serves cached results first,
/// and publishes starting, per-result, and completion callbacks tagged with a run id
/// so results from cancelled runs are ignored.

#pragma once

#include "AnalysisCache.h"
#include "AudioAnalysisService.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

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

    /// The files of a run, published once the input paths are expanded and checked against the cache.
    struct AnalysisPlan {
        /// The run the plan belongs to.
        int runId = 0;
        /// Every supported file found in the input paths.
        juce::Array<juce::File> files;
        /// The files without a current cache entry, which are analyzed in this run.
        juce::Array<juce::File> staleFiles;
    };

    /// Called once per run from a worker thread, before any of its results.
    using PlanCallback = std::function<void(const AnalysisPlan& plan)>;

    /// Creates a coordinator with a thread pool sized for the current machine.
    explicit AnalysisCoordinator(AnalysisCache& analysisCache, int workerCount = juce::SystemStats::getNumCpus());

    /// Cancels any in-flight work and destroys the worker pool.
    /// Waits without a timeout so workers cannot access destroyed coordinator state.
    ~AnalysisCoordinator();

    AnalysisCoordinator(const AnalysisCoordinator&) = delete;
    AnalysisCoordinator& operator=(const AnalysisCoordinator&) = delete;
    AnalysisCoordinator(AnalysisCoordinator&&) = delete;
    AnalysisCoordinator& operator=(AnalysisCoordinator&&) = delete;

    /// Runs a full analysis synchronously and returns all collected results.
    /// Replaces the result and completion callbacks for the run and clears them before returning.
    std::vector<AudioAnalysisRecord> analyzeBlocking(const AudioAnalysisOptions& options);

    /// Cancels queued work and waits for active jobs to finish.
    void cancelAndWait();

    /// Sets the callback invoked when a run completes.
    void setCompletionCallback(CompletionCallback callback);

    /// Sets the callback invoked for each published result.
    void setResultCallback(ResultCallback callback);

    /// Sets the callback invoked when a file's analysis is about to start.
    void setStartingCallback(StartingCallback callback);

    /// Sets the callback invoked with the file list of each run.
    void setPlanCallback(PlanCallback callback);

    /// Starts a background analysis run and returns its run id.
    /// Expanding the input paths and the cache lookups also run in the background,
    /// so the caller never waits for the file system.
    /// The plan callback comes first, then cached results, then fresh results, and finally the completion callback,
    /// which is published even when no files are found.
    int start(const AudioAnalysisOptions& options);

private:
    /// Invokes the completion callback when the given run id is still current.
    /// The callback is copied under the lock and invoked without it,
    /// so the callback may safely call back into the coordinator.
    void publishCompletion(int totalFiles, int runId) const;

    /// Invokes the result callback when the given run id is still current.
    /// Runs on the worker thread, so the callback must marshal to the message thread itself if needed.
    void publishResult(const AudioAnalysisRecord& result, int runId) const;

    /// Expands the input paths, publishes the plan and the cached results, and queues one job per stale file.
    /// Runs on a worker thread and stops early when the run is cancelled.
    void planRun(const AudioAnalysisOptions& options, int runId);

    /// Analyzes one file, caches the result, and publishes it.
    /// The stored custom gain is kept while the file is unchanged on disk.
    /// A file whose analysis is cancelled is neither cached nor published.
    /// pendingFiles counts the run's unfinished files, and the job that finishes the last one publishes completion.
    /// It belongs to the run, so a job that outlives a cancellation timeout cannot change a later run's count.
    void analyzeStaleFile(
        const juce::File& file,
        int runId,
        int totalFiles,
        const std::shared_ptr<std::atomic<int>>& pendingFiles
    ) const;

    /// Returns true when the run has been cancelled or replaced by a newer one.
    [[nodiscard]] bool isRunCancelled(int runId) const noexcept;

    /// Invokes the plan callback when the given run id is still current.
    void publishPlan(const AnalysisPlan& plan, int runId) const;

    /// Invokes the starting callback when the given run id is still current.
    /// Runs on the worker thread right before a file's analysis begins.
    void publishStarting(const juce::File& file, int runId) const;

    AnalysisCache& cache;
    CompletionCallback completionCallback;
    ResultCallback resultCallback;
    StartingCallback startingCallback;
    PlanCallback planCallback;
    juce::CriticalSection callbackLock;
    juce::ThreadPool threadPool;
    std::atomic<int> currentRunId {0};
};
