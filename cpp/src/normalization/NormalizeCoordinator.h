/// Background orchestration for normalize runs.
/// NormalizeCoordinator runs normalize jobs on a thread pool
/// and publishes starting, progress, per-result, and completion callbacks tagged with a run id
/// so results from cancelled runs are ignored.

#pragma once

#include "normalization/AudioNormalization.h"
#include "utils/BackgroundRunner.h"

#include <JuceHeader.h>

#include <functional>
#include <vector>

namespace audiobatch
{
/// Coordinates background normalize jobs and marshals results back to the UI layer.
class NormalizeCoordinator
{
public:
    // Every callback carries the id of the run it belongs to,
    // so a receiver that handles them asynchronously can drop messages from a run that ended meanwhile.

    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles, int runId)>;

    /// Called from the worker thread with the normalized fraction of a file, throttled to a few updates per second.
    using ProgressCallback = std::function<void(const juce::File& file, float progress, int runId)>;

    /// Called for each processed file as normalize results become available.
    using ResultCallback = std::function<void(const AudioNormalizationResult& result, int runId)>;

    /// Called from the worker thread right before a file's normalization begins.
    using StartingCallback = std::function<void(const juce::File& file, int runId)>;

    explicit NormalizeCoordinator(int workerCount = juce::SystemStats::getNumCpus());

    ~NormalizeCoordinator();

    NormalizeCoordinator(const NormalizeCoordinator&) = delete;
    NormalizeCoordinator& operator=(const NormalizeCoordinator&) = delete;
    NormalizeCoordinator(NormalizeCoordinator&&) = delete;
    NormalizeCoordinator& operator=(NormalizeCoordinator&&) = delete;

    [[nodiscard]] int getCurrentRunId() const noexcept;
    [[nodiscard]] int start(const std::vector<AudioAnalysisRecord>& records, const AudioNormalizationOptions& options);

    void cancel();
    void cancelAndWait();
    void requestStop();
    void setCompletionCallback(CompletionCallback callback);
    void setProgressCallback(ProgressCallback callback);
    void setResultCallback(ResultCallback callback);
    void setStartingCallback(StartingCallback callback);

private:
    RunCallback<int, int> completionCallback;
    RunCallback<const juce::File&, float, int> progressCallback;
    RunCallback<const AudioNormalizationResult&, int> resultCallback;
    RunCallback<const juce::File&, int> startingCallback;

    /// Declared last, so the callbacks the jobs publish to outlive it.
    BackgroundRunner runner;
};
}  // namespace audiobatch
