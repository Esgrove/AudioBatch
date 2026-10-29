/// Tests for the functions that describe the background work of the main view,
/// such as the reasons new work cannot start and the progress shown in the status bar and window title.

#include "gui/AudioBatchComponentState.h"
#include "gui/BackgroundWorkController.h"
#include "TestSuites.h"

#include <memory>

namespace audiobatch::tests
{
namespace
{
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
};

std::unique_ptr<juce::UnitTest> createBackgroundWorkTests()
{
    return std::make_unique<BackgroundWorkTests>();
}
}  // namespace audiobatch::tests
