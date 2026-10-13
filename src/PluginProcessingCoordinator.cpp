/// Implementation of PluginProcessingCoordinator.
/// Covers queuing one thread-pool job per file,
/// the per-run free pool of pre-instantiated plugin chains that workers acquire and release between files,
/// run-id based cancellation, and publishing results and completion to the message thread.
/// Cancellation also releases workers waiting for plugin setup on the message thread,
/// so cancelAndWait can block the message thread without deadlocking.
/// Plugin instances are always destroyed on the message thread to satisfy VST3
/// and Audio Unit hosting requirements.

#include "PluginProcessingCoordinator.h"

#include "AudioNormalizationService.h"
#include "StringFormat.h"

#include <algorithm>
#include <map>
#include <utility>

/// How long cancelAndWait waits for running plugin processing jobs to finish before giving up, in milliseconds.
static constexpr int jobShutdownTimeoutMilliseconds = 60000;

/// How long a worker waits for a plugin chain to be released before checking for cancellation again, in milliseconds.
static constexpr int chainWaitIntervalMilliseconds = 100;

PluginProcessingCoordinator::PluginProcessingCoordinator(const int workerCount) :
    threadPool(juce::jmax(1, juce::jmin(4, workerCount)))
{}

PluginProcessingCoordinator::~PluginProcessingCoordinator()
{
    cancelAndWait();
    threadPool.removeAllJobs(true, -1);
    currentChains.reset();
}

PluginProcessingCoordinator::RunChains::RunChains(std::vector<PluginChainInstances> chainInstances, const bool usable) :
    chains(std::move(chainInstances))
{
    if (usable) {
        freeIndices.reserve(chains.size());

        for (int chainIndex = 0; std::cmp_less(chainIndex, chains.size()); ++chainIndex) {
            freeIndices.push_back(chainIndex);
        }
    }
}

PluginProcessingCoordinator::RunChains::~RunChains()
{
    if (chains.empty() || juce::MessageManager::existsAndIsCurrentThread()) {
        return;
    }

    juce::MessageManager::callAsync([chainsCaptured = std::make_shared<decltype(chains)>(std::move(chains))] {
        chainsCaptured->clear();
    });
}

int PluginProcessingCoordinator::RunChains::acquire()
{
    const juce::ScopedLock scopedLock(lock);

    if (freeIndices.empty()) {
        return -1;
    }

    const auto chainIndex = freeIndices.back();
    freeIndices.pop_back();
    return chainIndex;
}

void PluginProcessingCoordinator::RunChains::release(const int chainIndex)
{
    if (chainIndex < 0) {
        return;
    }

    {
        const juce::ScopedLock scopedLock(lock);
        freeIndices.push_back(chainIndex);
    }

    available.signal();
}

std::vector<juce::AudioPluginInstance*> PluginProcessingCoordinator::RunChains::getChainView(const int chainIndex) const
{
    const auto& chain = chains[static_cast<std::size_t>(chainIndex)];
    std::vector<juce::AudioPluginInstance*> chainView;
    chainView.reserve(chain.size());

    for (const auto& instance : chain) {
        chainView.push_back(instance.get());
    }

    return chainView;
}

void PluginProcessingCoordinator::setResultCallback(ResultCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    resultCallback = std::move(callback);
}

void PluginProcessingCoordinator::setCompletionCallback(CompletionCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    completionCallback = std::move(callback);
}

void PluginProcessingCoordinator::setStartErrorCallback(StartErrorCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    startErrorCallback = std::move(callback);
}

void PluginProcessingCoordinator::publishResult(const PluginProcessingResult& result, const int runId) const
{
    if (runId != currentRunId.load()) {
        return;
    }

    ResultCallback callbackCopy;
    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = resultCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(result);
    }
}

void PluginProcessingCoordinator::publishCompletion(const int totalFiles, const int runId) const
{
    if (runId != currentRunId.load()) {
        return;
    }

    CompletionCallback callbackCopy;
    {
        const juce::ScopedLock lock(callbackLock);
        callbackCopy = completionCallback;
    }

    if (callbackCopy != nullptr) {
        callbackCopy(totalFiles);
    }
}

void PluginProcessingCoordinator::cancelAndWait()
{
    ++currentRunId;

    // Wake any workers blocked waiting for a chain.
    if (currentChains != nullptr) {
        currentChains->available.signal();
    }

    threadPool.removeAllJobs(true, jobShutdownTimeoutMilliseconds);
}

/// Returns whether every chain aligns index-for-index with options.plugins and contains no null instances.
/// At least one chain is required.
static bool areChainsUsable(
    const std::vector<PluginProcessingCoordinator::PluginChainInstances>& chains,
    const PluginProcessingOptions& options
)
{
    const auto isUsableChain = [&options](const PluginProcessingCoordinator::PluginChainInstances& chain) {
        return !chain.empty() && chain.size() == options.plugins.size()
            && std::ranges::none_of(chain, [](const auto& instance) { return instance == nullptr; });
    };

    return !chains.empty() && std::ranges::all_of(chains, isUsableChain);
}

/// Returns the existing records ordered and deduplicated by full path, so each file is processed once.
static std::vector<AudioAnalysisRecord> collectQueuedRecords(const std::vector<AudioAnalysisRecord>& records)
{
    std::map<juce::String, AudioAnalysisRecord> recordsByPath;
    for (const auto& record : records) {
        if (record.file.existsAsFile()) {
            recordsByPath.insert_or_assign(record.fullPath, record);
        }
    }

    std::vector<AudioAnalysisRecord> queuedRecords;
    queuedRecords.reserve(recordsByPath.size());
    for (auto& [path, record] : recordsByPath) {
        juce::ignoreUnused(path);
        queuedRecords.push_back(std::move(record));
    }

    return queuedRecords;
}

void PluginProcessingCoordinator::reportStartError(const juce::String& message) const
{
    StartErrorCallback errorCallback;
    {
        const juce::ScopedLock lock(callbackLock);
        errorCallback = startErrorCallback;
    }

    if (errorCallback != nullptr) {
        errorCallback(message);
    }
}

void PluginProcessingCoordinator::runFileJob(
    const AudioAnalysisRecord& record,
    const PluginProcessingOptions& options,
    const int runId,
    const int totalFiles,
    RunChains& runChains,
    std::atomic<int>& pendingFiles
)
{
    if (runId != currentRunId.load()) {
        return;
    }

    auto chainIndex = runChains.acquire();

    // Wait for a chain to become free (worker count > chain count is unusual).
    while (chainIndex < 0 && runId == currentRunId.load()) {
        runChains.available.wait(chainWaitIntervalMilliseconds);
        chainIndex = runChains.acquire();
    }

    if (runId != currentRunId.load()) {
        runChains.release(chainIndex);
        return;
    }

    // Cancellation lets the worker stop waiting for the message thread,
    // which cancelAndWait blocks while it waits for the workers to finish.
    const auto result
        = PluginProcessingService::processFile(record, options, runChains.getChainView(chainIndex), [this, runId] {
              return runId != currentRunId.load();
          });
    runChains.release(chainIndex);

    publishResult(result, runId);

    if (pendingFiles.fetch_sub(1) == 1) {
        publishCompletion(totalFiles, runId);
    }
}

int PluginProcessingCoordinator::start(
    const std::vector<AudioAnalysisRecord>& records,
    const PluginProcessingOptions& options,
    std::vector<PluginChainInstances> chainInstances
)
{
    cancelAndWait();

    const auto chainsUsable = areChainsUsable(chainInstances, options);
    // Replacing the previous run's chains destroys them here on the message thread,
    // unless a job that outlived the cancellation timeout still holds them.
    currentChains = std::make_shared<RunChains>(std::move(chainInstances), chainsUsable);

    if (!chainsUsable) {
        reportStartError("No plugin chain instances were available to start processing.");
        publishCompletion(0, currentRunId.load());
        return 0;
    }

    const auto runId = currentRunId.load();
    const auto queuedRecords = collectQueuedRecords(records);
    const auto totalFiles = static_cast<int>(queuedRecords.size());

    if (queuedRecords.empty()) {
        publishCompletion(0, runId);
        return 0;
    }

    const auto conflicts = AudioNormalizationService::findBatchOutputConflicts(queuedRecords);
    // The counter belongs to this run, so a job that outlives a cancellation timeout cannot change a later run's count.
    const auto pendingFiles = std::make_shared<std::atomic<int>>(totalFiles);

    for (const auto& record : queuedRecords) {
        if (conflicts.contains(record.file.getFullPathName())) {
            threadPool.addJob([this, record, runId, totalFiles, pendingFiles] {
                if (runId != currentRunId.load()) {
                    return;
                }

                publishResult(
                    PluginProcessingResult::failure(
                        record.file, AudioNormalizationService::batchOutputConflictMessage(record.file)
                    ),
                    runId
                );

                if (pendingFiles->fetch_sub(1) == 1) {
                    publishCompletion(totalFiles, runId);
                }
            });
            continue;
        }

        threadPool.addJob([this, record, options, runId, totalFiles, runChains = currentChains, pendingFiles] {
            runFileJob(record, options, runId, totalFiles, *runChains, *pendingFiles);
        });
    }

    return totalFiles;
}
