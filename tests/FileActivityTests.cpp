/// Tests for FileActivityTracker, which holds the activities, progress, and operation messages of the results table.
/// Covers marking files busy, clearing activities by kind, progress reporting,
/// and how a new operation replaces the last message.

#include "gui/FileActivityTracker.h"
#include "TestSuites.h"

namespace audiobatch::tests
{
/// Checks the activities, the progress, the operation messages, and the repaint callback.
class FileActivityTests final : public juce::UnitTest
{
public:
    FileActivityTests() : juce::UnitTest("File activity", "AudioBatch") {}

    /// Runs the activity tracker checks.
    void runTest() override
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory);
        const auto first = root.getChildFile("first.wav");
        const auto second = root.getChildFile("second.wav");
        int repaints = 0;
        juce::Component component;
        FileActivityTracker tracker(component, [&repaints] { ++repaints; });

        beginTest("Marked files are busy with their activity until cleared");
        {
            tracker.markActive({first, second}, FileActivity::waiting);
            expect(tracker.isAnyActive());
            expect(tracker.getActivity(first.getFullPathName()) == FileActivity::waiting);
            expectEquals(tracker.getActiveLabel(first.getFullPathName()), juce::String("Waiting"));
            expect(repaints > 0);

            tracker.setActive(first.getFullPathName(), FileActivity::analyzing);
            expectEquals(tracker.getActiveLabel(first.getFullPathName()), juce::String("Analyzing"));

            tracker.clearActive(first.getFullPathName());
            expect(!tracker.getActivity(first.getFullPathName()).has_value());
            expectEquals(tracker.getActiveLabel(first.getFullPathName()), juce::String());
            expect(tracker.isAnyActive());

            tracker.clearAllActive();
            expect(!tracker.isAnyActive());
        }

        beginTest("Only the given activities are cleared");
        {
            tracker.markActive({first}, FileActivity::analyzing);
            tracker.markActive({second}, FileActivity::normalizing);
            tracker.clearActiveWithActivities({FileActivity::analyzing, FileActivity::waiting});

            expect(!tracker.getActivity(first.getFullPathName()).has_value());
            expect(tracker.getActivity(second.getFullPathName()) == FileActivity::normalizing);
            tracker.clearAllActive();
        }

        beginTest("Progress is kept for started files only and summed across them");
        {
            // Returns -1 for an idle file or indeterminate progress.
            const auto progressOf = [&tracker](const juce::File& file) {
                const auto state = tracker.getActivityState(file.getFullPathName());
                return state.has_value() ? state->progress.value_or(-1.0f) : -1.0f;
            };

            tracker.markActive({first, second}, FileActivity::waiting);
            tracker.setProgress(first.getFullPathName(), 0.5f);
            expectEquals(progressOf(first), 0.0f);

            tracker.setActive(first.getFullPathName(), FileActivity::processing);
            expectEquals(progressOf(first), 0.0f);
            tracker.setProgress(first.getFullPathName(), 0.25f);
            tracker.setActive(second.getFullPathName(), FileActivity::processing);
            tracker.setProgress(second.getFullPathName(), 1.5f);

            expectWithinAbsoluteError(progressOf(first), 0.25f, 1.0e-6f);
            expectWithinAbsoluteError(progressOf(second), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError(tracker.getProgressTotal(), 1.25f, 1.0e-6f);

            tracker.clearActive(first.getFullPathName());
            tracker.setProgress(first.getFullPathName(), 0.75f);
            expect(!tracker.getActivityState(first.getFullPathName()).has_value());
            expectWithinAbsoluteError(tracker.getProgressTotal(), 1.0f, 1.0e-6f);
            tracker.clearAllActive();
        }

        beginTest("Each new activity starts empty before its first repaint");
        {
            FileActivityTracker* observedTracker = nullptr;
            bool observedEmptyProgress = false;
            FileActivityTracker observed(component, [&] {
                if (observedTracker != nullptr) {
                    const auto state = observedTracker->getActivityState(first.getFullPathName());
                    observedEmptyProgress = state.has_value() && state->progress == 0.0f;
                }
            });
            observedTracker = &observed;

            for (const auto activity :
                 {FileActivity::analyzing, FileActivity::waiting, FileActivity::normalizing, FileActivity::processing})
            {
                observed.setActive(first.getFullPathName(), FileActivity::analyzing);
                observed.setProgress(first.getFullPathName(), 1.0f);
                observedEmptyProgress = false;
                observed.setActive(first.getFullPathName(), activity);
                expect(observedEmptyProgress);
                expectEquals(observed.getProgressTotal(), 0.0f);
            }

            observed.setActive(first.getFullPathName(), FileActivity::processing);
            observed.setProgress(first.getFullPathName(), 1.0f);
            observedEmptyProgress = false;
            observed.markActive({first}, FileActivity::waiting);
            expect(observedEmptyProgress);
            observed.setProgress(first.getFullPathName(), 1.0f);
            expectEquals(observed.getProgressTotal(), 0.0f);
            observed.setActive(first.getFullPathName(), FileActivity::processing);
            expect(observedEmptyProgress);
            observed.setProgress(first.getFullPathName(), 0.25f);
            expectEquals(observed.getProgressTotal(), 0.25f);
        }

        beginTest("A new operation replaces the message of the last one");
        {
            tracker.setMessage(first.getFullPathName(), {.text = "Reduced 1.0 dB to avoid clipping", .isError = false});
            tracker.setMessage(second.getFullPathName(), {.text = "Could not write", .isError = true});
            expect(tracker.getMessage(second.getFullPathName()).isError);

            tracker.markActive({first}, FileActivity::processing);
            expectEquals(tracker.getMessage(first.getFullPathName()).text, juce::String());
            expectEquals(tracker.getMessage(second.getFullPathName()).text, juce::String("Could not write"));

            tracker.clearMessage(second.getFullPathName());
            expectEquals(tracker.getMessage(second.getFullPathName()).text, juce::String());

            tracker.setMessage(second.getFullPathName(), {.text = "Notice", .isError = false});
            tracker.clearAllMessages();
            expectEquals(tracker.getMessage(second.getFullPathName()).text, juce::String());
            tracker.clearAllActive();
        }

        beginTest("The animation phase stays within one turn");
        {
            const auto phase = FileActivityTracker::getAnimationPhase();
            expect(phase >= 0.0f && phase < 1.0f, juce::String(phase));
        }
    }
};

std::unique_ptr<juce::UnitTest> createFileActivityTests()
{
    return std::make_unique<FileActivityTests>();
}
}  // namespace audiobatch::tests
