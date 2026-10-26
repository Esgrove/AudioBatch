/// Background orchestration for normalize runs.
/// NormalizeCoordinator runs normalize jobs on a thread pool
/// and publishes starting, progress, per-result, and completion callbacks tagged with a run id
/// so results from cancelled runs are ignored.

#pragma once

#include "normalization/AudioNormalization.h"
#include "utils/Synchronized.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace audiobatch
{
/// The callbacks a NormalizeCoordinator publishes to, each of which may be empty.
struct NormalizeCallbacks {
    /// Called once after a run has published all queued results.
    std::function<void(int totalFiles)> completion;
    /// Called from the worker thread with the normalized fraction of a file, throttled to a few updates per second.
    std::function<void(const juce::File& file, float progress)> progress;
    /// Called for each processed file as normalize results become available.
    std::function<void(const AudioNormalizationResult& result)> result;
    /// Called from the worker thread right before a file's normalization begins.
    std::function<void(const juce::File& file)> starting;
};

/// Coordinates background normalize jobs and marshals results back to the UI layer.
class NormalizeCoordinator
{
public:
    /// Type of the completion callback in NormalizeCallbacks.
    using CompletionCallback = decltype(NormalizeCallbacks::completion);

    /// Type of the progress callback in NormalizeCallbacks.
    using ProgressCallback = decltype(NormalizeCallbacks::progress);

    /// Type of the result callback in NormalizeCallbacks.
    using ResultCallback = decltype(NormalizeCallbacks::result);

    /// Type of the starting callback in NormalizeCallbacks.
    using StartingCallback = decltype(NormalizeCallbacks::starting);

    explicit NormalizeCoordinator(int workerCount = juce::SystemStats::getNumCpus());

    ~NormalizeCoordinator();

    NormalizeCoordinator(const NormalizeCoordinator&) = delete;
    NormalizeCoordinator& operator=(const NormalizeCoordinator&) = delete;
    NormalizeCoordinator(NormalizeCoordinator&&) = delete;
    NormalizeCoordinator& operator=(NormalizeCoordinator&&) = delete;

    [[nodiscard]] int start(const std::vector<AudioAnalysisRecord>& records, const AudioNormalizationOptions& options);

    void cancelAndWait();
    void setCompletionCallback(CompletionCallback callback);
    void setProgressCallback(ProgressCallback callback);
    void setResultCallback(ResultCallback callback);
    void setStartingCallback(StartingCallback callback);

private:
    void publishCompletion(int totalFiles, int runId) const;
    void publishProgress(const juce::File& file, float progress, int runId) const;
    void publishResult(const AudioNormalizationResult& result, int runId) const;
    void publishStarting(const juce::File& file, int runId) const;

    CompletionCallback completionCallback;
    ProgressCallback progressCallback;
    ResultCallback resultCallback;
    utils::Synchronized<NormalizeCallbacks> callbacks;
    juce::ThreadPool threadPool;
    std::atomic<int> currentRunId {0};
};
}  // namespace audiobatch
