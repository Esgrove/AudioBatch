/// Background orchestration for normalize runs.
/// NormalizeCoordinator runs normalize jobs on a thread pool
/// and publishes per-result and completion callbacks tagged with a run id
/// so results from cancelled runs are ignored.

#pragma once

#include "AudioNormalizationService.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

/// Coordinates background normalize jobs and marshals results back to the UI layer.
class NormalizeCoordinator
{
public:
    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles)>;

    /// Called for each processed file as normalize results become available.
    using ResultCallback = std::function<void(const AudioNormalizationResult& result)>;

    explicit NormalizeCoordinator(int workerCount = juce::SystemStats::getNumCpus());

    ~NormalizeCoordinator();

    NormalizeCoordinator(const NormalizeCoordinator&) = delete;
    NormalizeCoordinator& operator=(const NormalizeCoordinator&) = delete;
    NormalizeCoordinator(NormalizeCoordinator&&) = delete;
    NormalizeCoordinator& operator=(NormalizeCoordinator&&) = delete;

    void cancelAndWait();

    void setCompletionCallback(CompletionCallback callback);

    void setResultCallback(ResultCallback callback);

    int start(const std::vector<AudioAnalysisRecord>& records, const AudioNormalizationOptions& options);

private:
    void publishCompletion(int totalFiles, int runId) const;

    void publishResult(const AudioNormalizationResult& result, int runId) const;

    juce::ThreadPool threadPool;
    juce::CriticalSection callbackLock;
    ResultCallback resultCallback;
    CompletionCallback completionCallback;
    std::atomic<int> currentRunId {0};
};
