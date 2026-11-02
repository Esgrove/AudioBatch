/// Background job coordination for batch plugin processing.
/// Declares PluginProcessingCoordinator,
/// which runs files through pre-instantiated plugin chains on a worker thread pool
/// and publishes per-file starts, progress, results and completion from workers.
/// BackgroundWorkController marshals those callbacks to the message thread.
/// Start errors are published on the message thread where a run is started.

#pragma once

#include "plugins/PluginProcessing.h"
#include "plugins/PluginRunChains.h"
#include "utils/BackgroundRunner.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace audiobatch
{
int getPluginProcessingWorkerCount(
    int physicalCoreCount = juce::SystemStats::getNumPhysicalCpus(),
    int logicalCoreCount = juce::SystemStats::getNumCpus()
);

/// Coordinates background plugin-processing jobs and publishes callbacks for the caller to marshal to the UI layer.
class PluginProcessingCoordinator
{
public:
    // Every callback of a run carries the id of the run it belongs to,
    // so a receiver that handles them asynchronously can drop messages from a run that ended meanwhile.

    /// Called once after a run has published all queued results.
    using CompletionCallback = std::function<void(int totalFiles, int runId)>;

    /// Called from the worker thread with the processed fraction of a file, throttled to a few updates per second.
    using ProgressCallback = std::function<void(const juce::File& file, float progress, int runId)>;

    /// Called for each processed file as plugin-processing results become available.
    using ResultCallback = std::function<void(const PluginProcessingResult& result, int runId)>;

    /// Called from the worker thread once a file has a plugin chain and its processing begins.
    using StartingCallback = std::function<void(const juce::File& file, int runId)>;

    /// Called on the message thread when the coordinator could not start a run, with its attempted run id.
    /// Receivers handling it asynchronously must discard errors from attempts superseded by a later run.
    using StartErrorCallback = std::function<void(juce::String errorMessage, int runId)>;

    explicit PluginProcessingCoordinator(int workerCount = getPluginProcessingWorkerCount());

    ~PluginProcessingCoordinator();

    PluginProcessingCoordinator(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator& operator=(const PluginProcessingCoordinator&) = delete;
    PluginProcessingCoordinator(PluginProcessingCoordinator&&) = delete;
    PluginProcessingCoordinator& operator=(PluginProcessingCoordinator&&) = delete;

    [[nodiscard]] int getCurrentRunId() const noexcept;
    [[nodiscard]] int start(
        const std::vector<AudioAnalysisRecord>& records,
        const PluginProcessingOptions& options,
        std::vector<PluginChainInstances> chainInstances
    );

    void cancel();
    void cancelAndWait();
    void requestStop();

    void setCompletionCallback(CompletionCallback callback);
    void setProgressCallback(ProgressCallback callback);
    void setResultCallback(ResultCallback callback);
    void setStartErrorCallback(StartErrorCallback callback);
    void setStartingCallback(StartingCallback callback);

private:
    [[nodiscard]] PluginProcessingResult processLeasedFile(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        int runId,
        std::span<juce::AudioPluginInstance* const> chainView
    ) const;

    void runFileJob(
        const AudioAnalysisRecord& record,
        const PluginProcessingOptions& options,
        int runId,
        int totalFiles,
        PluginRunChains& runChains,
        std::atomic<int>& pendingFiles
    ) const;

    /// Receives the completion of each run.
    RunCallback<int, int> completionCallback;
    /// Receives the processed fraction of each file.
    RunCallback<const juce::File&, float, int> progressCallback;
    /// Receives the result of each file.
    RunCallback<const PluginProcessingResult&, int> resultCallback;
    /// Receives the reason a run could not start its workers.
    RunCallback<juce::String, int> startErrorCallback;
    /// Receives each file a worker starts on.
    RunCallback<const juce::File&, int> startingCallback;

    /// Chains of the current run. Only accessed on the message thread.
    std::shared_ptr<PluginRunChains> currentChains;

    /// Declared last, so the callbacks and chains the jobs use outlive it.
    BackgroundRunner runner;
};
}  // namespace audiobatch
