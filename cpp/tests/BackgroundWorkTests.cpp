/// Tests for the functions that describe the background work of the main view,
/// such as the reasons new work cannot start and the progress shown in the status bar and window title.

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentState.h"
#include "gui/BackgroundWorkController.h"
#include "gui/FileActivityTracker.h"
#include "normalization/AudioNormalization.h"
#include "plugins/PluginProcessing.h"
#include "plugins/PluginProcessingCoordinator.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include <condition_variable>

#include <chrono>
#include <memory>
#include <mutex>
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
    void processingResultsReceived(std::vector<PluginProcessingResult> /*results*/) override {}
    void processingCompleted(const ProcessingWork& /*pass*/, int /*totalFiles*/) override
    {
        ++completions;
    }
    void processingStartFailed(const juce::String& /*errorMessage*/) override {}

    int receivedResults {0};
    int completions {0};
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
        auto options = testNormalizationOptions();
        options.disposeFile = [&](const juce::File& file) {
            {
                std::unique_lock lock(mutex);
                entered = true;
                condition.notify_all();
                condition.wait(lock, [&] { return released; });
            }
            return file.deleteFile();
        };
        expectEquals(controller.startNormalization({record}, options), 1);
        const auto runId = std::get<NormalizingWork>(controller.work).runId;
        {
            std::unique_lock lock(mutex);
            expect(condition.wait_for(lock, std::chrono::seconds(1), [&] { return entered; }));
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
        // Waiting is only part of the test. Clear itself never waits for a worker.
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
