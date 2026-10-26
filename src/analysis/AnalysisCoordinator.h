/// Background orchestration for audio analysis runs.
/// AnalysisCoordinator runs analysis jobs on a thread pool, serves cached results first,
/// and publishes starting, progress, per-result, and completion callbacks tagged with a run id
/// so results from cancelled runs are ignored.

#pragma once

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"

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

    explicit AnalysisCoordinator(AnalysisCache& analysisCache, int workerCount = juce::SystemStats::getNumCpus());

    ~AnalysisCoordinator();

    AnalysisCoordinator(const AnalysisCoordinator&) = delete;
    AnalysisCoordinator& operator=(const AnalysisCoordinator&) = delete;
    AnalysisCoordinator(AnalysisCoordinator&&) = delete;
    AnalysisCoordinator& operator=(AnalysisCoordinator&&) = delete;

    std::vector<AudioAnalysisRecord> analyzeBlocking(const AudioAnalysisOptions& options);

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
    void publishCompletion(int totalFiles, int runId) const;
    void publishPlan(const AnalysisPlan& plan, int runId) const;
    void publishProgress(const juce::File& file, float progress, int runId) const;
    void publishResult(const AudioAnalysisRecord& result, int runId) const;
    void publishStarting(const juce::File& file, int runId) const;

    void analyzeStaleFile(
        const juce::File& file,
        int runId,
        int totalFiles,
        const std::shared_ptr<std::atomic<int>>& pendingFiles
    ) const;

    [[nodiscard]] bool isRunCancelled(int runId) const noexcept;

    AnalysisCache& cache;
    CompletionCallback completionCallback;
    PlanCallback planCallback;
    ProgressCallback progressCallback;
    ResultCallback resultCallback;
    StartingCallback startingCallback;
    juce::CriticalSection callbackLock;
    juce::ThreadPool threadPool;
    std::atomic<int> currentRunId {0};
};
}  // namespace audiobatch
