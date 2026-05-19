#include "PluginProcessingCoordinator.h"

#include "utils.h"

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
        if (!ownedInstances.empty()) {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread()) {
                ownedInstances.clear();
                freeInstances.clear();
            } else {
                // Move instances out so they can be cleaned up on the message thread.
                auto instances = std::move(ownedInstances);
                ownedInstances.clear();
                freeInstances.clear();
                juce::MessageManager::callAsync(
                    [instancesCaptured = std::make_shared<decltype(instances)>(std::move(instances))]() mutable {
                        instancesCaptured->clear();
                    }
                );
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
    instanceAvailable.signal();  // Wake any workers blocked waiting for an instance.
    threadPool.removeAllJobs(true, 60000);
    pendingJobs.store(0);
}

juce::AudioPluginInstance* PluginProcessingCoordinator::acquireInstance()
{
    const juce::ScopedLock lock(instanceLock);

    if (freeInstances.empty()) {
        return nullptr;
    }

    auto* instance = freeInstances.back();
    freeInstances.pop_back();
    return instance;
}

void PluginProcessingCoordinator::releaseInstance(juce::AudioPluginInstance* instance)
{
    if (instance == nullptr) {
        return;
    }

    {
        const juce::ScopedLock lock(instanceLock);
        freeInstances.push_back(instance);
    }

    instanceAvailable.signal();
}

int PluginProcessingCoordinator::start(
    const std::vector<AudioAnalysisRecord>& records,
    const PluginProcessingOptions& options,
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> pluginInstances
)
{
    cancelAndWait();

    {
        const juce::ScopedLock lock(instanceLock);
        ownedInstances = std::move(pluginInstances);
        freeInstances.clear();
        freeInstances.reserve(ownedInstances.size());
        for (auto& instance : ownedInstances) {
            if (instance != nullptr) {
                freeInstances.push_back(instance.get());
            }
        }
    }

    if (ownedInstances.empty()) {
        StartErrorCallback errorCallback;
        {
            const juce::ScopedLock lock(callbackLock);
            errorCallback = startErrorCallback;
        }

        if (errorCallback != nullptr) {
            errorCallback("No plugin instances were available to start processing.");
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

            auto* instance = acquireInstance();

            // Wait for an instance to become free (worker count > instance count is unusual).
            while (instance == nullptr && runId == currentRunId.load()) {
                instanceAvailable.wait(100);
                instance = acquireInstance();
            }

            if (runId != currentRunId.load()) {
                releaseInstance(instance);
                return;
            }

            const auto result = PluginProcessingService::processFile(record, options, instance);
            releaseInstance(instance);

            publishResult(result, runId);

            if (pendingJobs.fetch_sub(1) == 1) {
                publishCompletion(totalFiles, runId);
            }
        });
    }

    return static_cast<int>(queuedRecords.size());
}
