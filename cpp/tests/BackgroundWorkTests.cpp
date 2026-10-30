/// Tests for the functions that describe the background work of the main view,
/// such as the reasons new work cannot start and the progress shown in the status bar and window title.

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/AudioFileTableModel.h"
#include "gui/BackgroundWorkController.h"
#include "gui/FileActivityTracker.h"
#include "normalization/AudioNormalization.h"
#include "plugins/PluginProcessing.h"
#include "plugins/PluginProcessingCoordinator.h"
#include "ProcessingTestPlugins.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include <condition_variable>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Records operation delivery without constructing a main window.
class WorkListener final : public BackgroundWorkListener
{
public:
    void analysisPlanReceived(const AnalysisPlan& /*plan*/, const AnalyzingWork& /*run*/) override {}
    void analysisResultsReceived(std::vector<AudioAnalysisRecord> /*records*/) override {}
    void analysisCompleted(const AnalyzingWork& /*run*/, int /*totalFiles*/) override {}
    void normalizeResultsReceived(std::vector<AudioNormalizationResult> results) override
    {
        receivedResults += static_cast<int>(results.size());
    }
    void normalizeCompleted(const NormalizingWork& /*pass*/, int /*totalFiles*/) override
    {
        ++completions;
    }
    void processingResultsReceived(std::vector<PluginProcessingResult> results) override
    {
        processingResults.insert(processingResults.end(), results.begin(), results.end());
    }
    void processingCompleted(const ProcessingWork& pass, int /*totalFiles*/) override
    {
        completedProcessing = pass;
        ++completions;
    }
    void processingStartFailed(const juce::String& errorMessage) override
    {
        startErrors.add(errorMessage);
    }

    int receivedResults {0};
    int completions {0};
    std::vector<PluginProcessingResult> processingResults;
    std::optional<ProcessingWork> completedProcessing;
    juce::StringArray startErrors;
};

/// Builds an analysis run that has received its plan, with the given progress.
AnalyzingWork makeAnalyzingWork(const int completed, const int expected)
{
    AnalyzingWork run;
    run.planReceived = true;
    run.completed = completed;
    run.expected = expected;
    return run;
}
}  // namespace

/// Checks the blocked reasons and the progress derived from each kind of background work.
class BackgroundWorkTests final : public juce::UnitTest
{
public:
    BackgroundWorkTests() : juce::UnitTest("Background work", "AudioBatch") {}

    /// Runs the background work checks.
    void runTest() override
    {
        beginTest("Clear retains writer exclusion until terminal delivery without adopting old rows");
        testClearDuringCommit();
        testStaleProcessingStartError();
        testProcessingAfterCommit(false);
        testProcessingAfterCommit(true);

        beginTest("Analysis blocks normalization and processing but not changes to the list");
        {
            const BackgroundWork analyzing {makeAnalyzingWork(0, 4)};
            expect(getFileOperationBlockedReason(analyzing).isEmpty());
            expect(getNormalizationBlockedReason(analyzing).isNotEmpty());
            expect(getProcessingBlockedReason(analyzing).isNotEmpty());

            const BackgroundWork idle {IdleWork {}};
            expect(getFileOperationBlockedReason(idle).isEmpty());
            expect(getNormalizationBlockedReason(idle).isEmpty());
            expect(getProcessingBlockedReason(idle).isEmpty());
        }

        beginTest("Normalization and processing block every other change");
        {
            for (const BackgroundWork& work : {BackgroundWork {NormalizingWork {}}, BackgroundWork {ProcessingWork {}}})
            {
                expect(getFileOperationBlockedReason(work).isNotEmpty());
                expect(getNormalizationBlockedReason(work).isNotEmpty());
                expect(getProcessingBlockedReason(work).isNotEmpty());
            }
        }

        beginTest("Progress text counts files once the count is known");
        {
            expectEquals(getProgressText(BackgroundWork {IdleWork {}}), juce::String());
            expectEquals(
                getProgressText(BackgroundWork {AnalyzingWork {}}), juce::String("Scanning for audio files...")
            );
            expectEquals(getProgressText(BackgroundWork {makeAnalyzingWork(3, 8)}), juce::String("Analyzing 3/8"));

            NormalizingWork normalizing;
            expectEquals(getProgressText(BackgroundWork {normalizing}), juce::String());
            normalizing.completed = 1;
            normalizing.expected = 2;
            expectEquals(getProgressText(BackgroundWork {normalizing}), juce::String("Normalizing 1/2"));
        }

        beginTest("Progress fraction adds the files in progress and stays within the run");
        {
            const auto idle = getBackgroundProgress(BackgroundWork {IdleWork {}}, 0.0f);
            expect(idle.activityName.isEmpty());
            expect(!idle.fraction.has_value());

            const auto scanning = getBackgroundProgress(BackgroundWork {AnalyzingWork {}}, 0.0f);
            expectEquals(scanning.activityName, juce::String("Scanning"));
            expect(!scanning.fraction.has_value());

            const auto analyzing = getBackgroundProgress(BackgroundWork {makeAnalyzingWork(1, 4)}, 0.5f);
            expectEquals(analyzing.activityName, juce::String("Analyzing"));
            expectWithinAbsoluteError(analyzing.fraction.value_or(-1.0f), 0.375f, 1.0e-6f);

            const auto overshoot = getBackgroundProgress(BackgroundWork {makeAnalyzingWork(4, 4)}, 2.0f);
            expectWithinAbsoluteError(overshoot.fraction.value_or(-1.0f), 1.0f, 1.0e-6f);
        }
    }

private:
    /// Delivers a failed attempt after a newer run starts, without dispatching unrelated JUCE messages.
    void testStaleProcessingStartError()
    {
        beginTest("A queued processing start error cannot end or notify a newer run");
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("analysis.db"));
        juce::Component component;
        FileActivityTracker activity(component, [] {});
        WorkListener listener;
        BackgroundWorkController controller(cache, activity, listener);
        juce::String queuedError;
        int queuedRunId {-1};
        controller.pluginCoordinator.setStartErrorCallback([&](const juce::String& message, const int runId) {
            queuedError = message;
            queuedRunId = runId;
        });
        const PluginProcessingOptions invalidOptions;
        expectEquals(controller.startProcessing({}, invalidOptions, {}, 0.0), 0);
        expect(queuedError.isNotEmpty());
        expectEquals(queuedRunId, controller.pluginCoordinator.getCurrentRunId());
        controller.handleProcessingStartError(queuedError, queuedRunId);
        expectEquals(listener.startErrors.size(), 1);
        listener.startErrors.clear();
        expectEquals(controller.startProcessing({}, invalidOptions, {}, 0.0), 0);
        const auto failedRunId = queuedRunId;

        TestPlugin plugin(TestPluginSettings {});
        const auto options = makeTestPluginOptions(chainOf(plugin));
        PluginChainInstances chain;
        chain.push_back(std::make_unique<TestPlugin>(TestPluginSettings {}));
        std::vector<PluginChainInstances> chains;
        chains.push_back(std::move(chain));
        AudioAnalysisRecord missing;
        missing.file = directory.getChildFile("missing.wav");
        missing.fullPath = missing.file.getFullPathName();
        expectEquals(controller.startProcessing({missing}, options, std::move(chains), 0.0), 1);
        const auto currentRunId = controller.pluginCoordinator.getCurrentRunId();
        expect(currentRunId != failedRunId);
        controller.handleProcessingStartError(queuedError, queuedRunId);
        expect(controller.isProcessingInProgress(), "the old start error ended the newer run");
        expect(listener.startErrors.isEmpty(), "the old start error reached the listener");
        controller.pluginCoordinator.cancelAndWait();
        controller.handleProcessingComplete(1, currentRunId);
    }

    /// Stops processing after the WAV output is published, then delivers its result and terminal separately.
    /// Real rendering runs on the message thread so preparation needs no message loop or audio device.
    void testProcessingAfterCommit(const bool clear)
    {
        beginTest(
            clear ? "Clear excludes writers until processing terminal delivery and drops committed rows"
                  : "Cancel after processing commit retains its result until terminal delivery"
        );
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("analysis.db"));
        juce::Component component;
        FileActivityTracker activity(component, [] {});
        WorkListener listener;
        BackgroundWorkController controller(cache, activity, listener);
        const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "committed.wav");
        const auto record = analyzeFile(source);
        const auto runId = controller.pluginCoordinator.getCurrentRunId();
        auto& pass = controller.work.emplace<ProcessingWork>();
        pass.runId = runId;
        pass.expected = 1;
        activity.setActive(record.fullPath, FileActivity::processing);
        TestPlugin plugin({.gain = 0.5f});
        auto options = makeTestPluginOptions(chainOf(plugin));
        bool disposed = false;
        options.disposeFile = [&](const juce::File& file) {
            disposed = true;
            expect(getProcessingOutputFile(source).existsAsFile());
            if (clear) {
                controller.stopAllWork();
                controller.stopAllWork();
                expect(!activity.isAnyActive());
            } else {
                expect(controller.cancelCurrentWork());
            }
            expect(controller.isProcessingInProgress());
            expect(isBackgroundWorkCancelling(controller.work));
            expectEquals(controller.pluginCoordinator.getCurrentRunId(), runId);
            return file.deleteFile();
        };
        const auto result = processFileWithPlugins(record, options, chainOf(plugin), [&] {
            return isBackgroundWorkCancelling(controller.work);
        });
        expect(disposed);
        expect(std::holds_alternative<OperationSucceeded>(result.outcome), outcomeError(result.outcome));
        expect(!source.exists());
        expect(getProcessingOutputFile(source).existsAsFile());
        controller.processingEventBatcher.post({.event = result, .runId = runId});
        controller.processingEventBatcher.deliverPending();
        expectEquals(static_cast<int>(listener.processingResults.size()), clear ? 0 : 1);
        expect(!controller.isIdle());
        expect(getFileOperationBlockedReason(controller.work).isNotEmpty());
        expectEquals(controller.startNormalization({record}, testNormalizationOptions()), 0);
        expectEquals(controller.startProcessing({}, options, {}, 0.0), 0);
        controller.startAnalysis({source}, false, false, true);
        expect(controller.isProcessingInProgress());
        controller.handleProcessingComplete(1, runId + 1);
        expect(controller.isProcessingInProgress());
        controller.handleProcessingComplete(1, runId);
        controller.handleProcessingComplete(1, runId);
        expect(controller.isIdle());
        expectEquals(listener.completions, 1);
        expect(listener.completedProcessing.has_value());
        if (listener.completedProcessing) {
            expect(listener.completedProcessing->cancelling);
            expectEquals(listener.completedProcessing->completed, clear ? 0 : 1);
        }
        expect(!activity.isAnyActive());
    }

    /// Holds a writer after publication, clears the list, and manually delivers its terminal event.
    /// The test runner does not dispatch JUCE messages, so queued completion is applied explicitly.
    void testClearDuringCommit()
    {
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("analysis.db"));
        juce::Component component;
        FileActivityTracker activity(component, [] {});
        WorkListener listener;
        BackgroundWorkController controller(cache, activity, listener);
        const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "held.wav");
        const auto record = analyzeFile(source);
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false;
        bool released = false;
        bool completed = false;
        controller.normalizeCoordinator.setCompletionCallback([&](int, int) {
            const std::scoped_lock lock(mutex);
            completed = true;
            condition.notify_all();
        });
        auto options = testNormalizationOptions();
        options.disposeFile = [&](const juce::File& file) {
            {
                std::unique_lock lock(mutex);
                entered = true;
                condition.notify_all();
                condition.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
            }
            return file.deleteFile();
        };
        expectEquals(controller.startNormalization({record}, options), 1);
        const auto runId = std::get<NormalizingWork>(controller.work).runId;
        {
            std::unique_lock lock(mutex);
            expect(condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
        }
        controller.stopAllWork();
        controller.stopAllWork();
        expect(!controller.isIdle());
        expect(controller.isFileOperationInProgress());
        expect(isBackgroundWorkCancelling(controller.work));
        expect(getFileOperationBlockedReason(controller.work).isNotEmpty());
        expectEquals(controller.startNormalization({record}, options), 0);
        const PluginProcessingOptions processingOptions;
        expectEquals(
            controller.startProcessing(
                std::vector<AudioAnalysisRecord> {}, processingOptions, std::vector<PluginChainInstances> {}, 0.0
            ),
            0
        );
        controller.startAnalysis({source}, false, false, true);
        expect(controller.isFileOperationInProgress());
        {
            const std::scoped_lock lock(mutex);
            released = true;
        }
        condition.notify_all();
        // Observe actual publication before invalidating the coordinator run during cleanup.
        {
            std::unique_lock lock(mutex);
            expect(condition.wait_for(lock, std::chrono::seconds(5), [&] { return completed; }));
        }
        controller.normalizeCoordinator.cancelAndWait();
        controller.normalizeEventBatcher.deliverPending();
        expectEquals(listener.receivedResults, 0);
        expect(!controller.isIdle());
        controller.handleNormalizeComplete(1, runId);
        expect(controller.isIdle());
        expectEquals(listener.completions, 1);
        expectEquals(listener.receivedResults, 0);
        expect(!activity.isAnyActive());
    }
};

std::unique_ptr<juce::UnitTest> createBackgroundWorkTests()
{
    return std::make_unique<BackgroundWorkTests>();
}
}  // namespace audiobatch::tests
