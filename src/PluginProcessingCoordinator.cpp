/// Implementation of PluginProcessingCoordinator.
/// Covers queuing one thread-pool job per file,
/// the free pool of pre-instantiated plugin chains that workers acquire and release between files,
/// run-id based cancellation, and publishing results and completion to the message thread.
/// Plugin instances are always destroyed on the message thread
/// to satisfy VST3 and Audio Unit hosting requirements.

#include "PluginProcessingCoordinator.h"

#include "utils.h"

#include <algorithm>
#include <map>

PluginProcessingCoordinator::PluginProcessingCoordinator(const int workerCount) :
    threadPool(juce::jmax(1, juce::jmin(4, workerCount)))
{ }

PluginProcessingCoordinator::~PluginProcessingCoordinator()
{
    cancelAndWait();

    // Destroy plugin instances on the message thread to keep VST3/AU happy.
    {
        const juce::ScopedLock lock(instanceLock);
        if (!ownedChains.empty()) {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
                ownedChains.clear();
                freeChainIndices.clear();
            } else {
                // Move instances out so they can be cleaned up on the message thread.
                auto chains = std::move(ownedChains);
                ownedChains.clear();
                freeChainIndices.clear();
                juce::MessageManager::callAsync([chainsCaptured
                                                 = std::make_shared<decltype(chains)>(std::move(chains))]() mutable {
                    chainsCaptured->clear();
                });
            }
        }
    }
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
    // Wake any workers blocked waiting for an instance.
    instanceAvailable.signal();
    threadPool.removeAllJobs(true, 60000);
    pendingJobs.store(0);
}

int PluginProcessingCoordinator::acquireChain()
{
    const juce::ScopedLock lock(instanceLock);

    if (freeChainIndices.empty()) {
        return -1;
    }

    const auto chainIndex = freeChainIndices.back();
    freeChainIndices.pop_back();
    return chainIndex;
}

void PluginProcessingCoordinator::releaseChain(const int chainIndex)
{
    if (chainIndex < 0) {
        return;
    }

    {
        const juce::ScopedLock lock(instanceLock);
        freeChainIndices.push_back(chainIndex);
    }

    instanceAvailable.signal();
}

int PluginProcessingCoordinator::start(
    const std::vector<AudioAnalysisRecord>& records,
    const PluginProcessingOptions& options,
    std::vector<PluginChainInstances> chainInstances
)
{
    cancelAndWait();

    // A usable chain must align index-for-index with options.plugins and contain no null instances.
    const auto isUsableChain = [&options](const PluginChainInstances& chainToCheck) {
        if (chainToCheck.size() != options.plugins.size() || chainToCheck.empty()) {
            return false;
        }

        return std::ranges::none_of(chainToCheck, [](const auto& instance) { return instance == nullptr; });
    };

    bool chainsUsable = !chainInstances.empty();
    for (const auto& chainToCheck : chainInstances) {
        chainsUsable = chainsUsable && isUsableChain(chainToCheck);
    }

    {
        const juce::ScopedLock lock(instanceLock);
        ownedChains = std::move(chainInstances);
        freeChainIndices.clear();
        freeChainIndices.reserve(ownedChains.size());
        if (chainsUsable) {
            for (int chainIndex = 0; chainIndex < static_cast<int>(ownedChains.size()); ++chainIndex) {
                freeChainIndices.push_back(chainIndex);
            }
        }
    }

    if (!chainsUsable) {
        StartErrorCallback errorCallback;
        {
            const juce::ScopedLock lock(callbackLock);
            errorCallback = startErrorCallback;
        }

        if (errorCallback != nullptr) {
            errorCallback("No plugin chain instances were available to start processing.");
        }

        publishCompletion(0, currentRunId.load());
        return 0;
    }

    const auto runId = currentRunId.load();

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

    pendingJobs.store(static_cast<int>(queuedRecords.size()));

    if (queuedRecords.empty()) {
        publishCompletion(0, runId);
        return 0;
    }

    for (const auto& record : queuedRecords) {
        threadPool.addJob([this, record, options, runId, totalFiles = static_cast<int>(queuedRecords.size())] {
            if (runId != currentRunId.load()) {
                return;
            }

            auto chainIndex = acquireChain();

            // Wait for a chain to become free (worker count > chain count is unusual).
            while (chainIndex < 0 && runId == currentRunId.load()) {
                instanceAvailable.wait(100);
                chainIndex = acquireChain();
            }

            if (runId != currentRunId.load()) {
                releaseChain(chainIndex);
                return;
            }

            // Build a raw-pointer view of the chain for the service call.
            std::vector<juce::AudioPluginInstance*> chainView;
            {
                const juce::ScopedLock lock(instanceLock);
                const auto& chain = ownedChains[static_cast<std::size_t>(chainIndex)];
                chainView.reserve(chain.size());
                for (const auto& instance : chain) {
                    chainView.push_back(instance.get());
                }
            }

            const auto result = PluginProcessingService::processFile(record, options, chainView);
            releaseChain(chainIndex);

            publishResult(result, runId);

            if (pendingJobs.fetch_sub(1) == 1) {
                publishCompletion(totalFiles, runId);
            }
        });
    }

    return static_cast<int>(queuedRecords.size());
}
