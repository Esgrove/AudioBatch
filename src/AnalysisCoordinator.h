#pragma once

#include "AnalysisCache.h"
#include "AudioAnalysisService.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>
#include <vector>

class AnalysisCoordinator
{
public:
    using CompletionCallback = std::function<void(int totalFiles)>;
    using ResultCallback = std::function<void(const AudioAnalysisRecord& result)>;

    explicit AnalysisCoordinator(AnalysisCache& cache, int workerCount = juce::SystemStats::getNumCpus());
    ~AnalysisCoordinator();

    std::vector<AudioAnalysisRecord> analyzeBlocking(const AudioAnalysisOptions& options);
    void cancelAndWait();
    void setCompletionCallback(CompletionCallback callback);
    void setResultCallback(ResultCallback callback);
    int start(const AudioAnalysisOptions& options);

private:
    void publishCompletion(int totalFiles, int runId);
    void publishResult(const AudioAnalysisRecord& result, int runId);

    AnalysisCache& cache;
    juce::ThreadPool threadPool;
    juce::CriticalSection callbackLock;
    ResultCallback resultCallback;
    CompletionCallback completionCallback;
    std::atomic<int> currentRunId {0};
    std::atomic<int> pendingJobs {0};
};