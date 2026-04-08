#pragma once

#include "AnalysisCache.h"
#include "AudioAnalysisService.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <vector>

/// Coordinates background audio analysis jobs and marshals result callbacks to the UI layer.
class AnalysisCoordinator
{
public:
    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles)>;

    /// Called for each analysed file as results become available.
    using ResultCallback = std::function<void(const AudioAnalysisRecord& result)>;

    /// Creates a coordinator with a thread pool sized for the current machine.
    explicit AnalysisCoordinator(AnalysisCache& analysisCache, int workerCount = juce::SystemStats::getNumCpus());

    /// Cancels any in-flight work and destroys the worker pool.
    ~AnalysisCoordinator();

    /// Runs a full analysis synchronously and returns all collected results.
    std::vector<AudioAnalysisRecord> analyzeBlocking(const AudioAnalysisOptions& options);

    /// Cancels queued work and waits for active jobs to finish.
    void cancelAndWait();

    /// Sets the callback invoked when a run completes.
    void setCompletionCallback(CompletionCallback callback);

    /// Sets the callback invoked for each published result.
    void setResultCallback(ResultCallback callback);

    /// Starts a background analysis run and returns its monotonically increasing run id.
    int start(const AudioAnalysisOptions& options);

    /// Starts a background analysis run using a precomputed file list.
    int start(const AudioAnalysisOptions& options, const juce::Array<juce::File>& files);

private:
    void publishCompletion(int totalFiles, int runId) const;
    void publishResult(const AudioAnalysisRecord& result, int runId);

    AnalysisCache& cache;
    CompletionCallback completionCallback;
    ResultCallback resultCallback;
    juce::CriticalSection callbackLock;
    juce::ThreadPool threadPool;
    std::atomic<int> currentRunId {0};
    std::atomic<int> pendingJobs {0};
};
