#pragma once

#include "AudioNormalizationService.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <vector>

/// Coordinates background normalize jobs and marshals results back to the UI layer.
class NormalizeCoordinator
{
public:
    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles)>;

    /// Called for each processed file as normalize results become available.
    using ResultCallback = std::function<void(const AudioNormalizationResult& result)>;

    /// Creates a coordinator with a thread pool sized for the current machine.
    explicit NormalizeCoordinator(int workerCount = juce::SystemStats::getNumCpus());

    /// Cancels any in-flight work and destroys the worker pool.
    ~NormalizeCoordinator();

    /// Cancels queued work and waits for active jobs to finish.
    void cancelAndWait();

    /// Sets the callback that is invoked when a run completes.
    void setCompletionCallback(CompletionCallback callback);

    /// Sets the callback that is invoked for each published result.
    void setResultCallback(ResultCallback callback);

    /// Starts a background normalize run and returns the number of queued files.
    int start(const std::vector<AudioAnalysisRecord>& records);

private:
    void publishCompletion(int totalFiles, int runId);
    void publishResult(const AudioNormalizationResult& result, int runId);

    juce::ThreadPool threadPool;
    juce::CriticalSection callbackLock;
    ResultCallback resultCallback;
    CompletionCallback completionCallback;
    std::atomic<int> currentRunId {0};
    std::atomic<int> pendingJobs {0};
};
