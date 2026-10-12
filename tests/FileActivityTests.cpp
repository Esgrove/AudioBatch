/// Tests for FileActivityTracker, which holds the activity labels and operation messages of the results table.
/// Covers marking files busy, clearing labels by name, and how a new operation replaces the last message.

#include "FileActivityTracker.h"
#include "TestSuites.h"

namespace audiobatch::tests
{
/// Checks the activity labels, the operation messages, and the repaint callback.
class FileActivityTests final : public juce::UnitTest
{
public:
    FileActivityTests() : juce::UnitTest("File activity", "AudioBatch") { }

    /// Runs the activity tracker checks.
    void runTest() override
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory);
        const auto first = root.getChildFile("first.wav");
        const auto second = root.getChildFile("second.wav");
        int repaints = 0;
        FileActivityTracker tracker([&repaints] { ++repaints; });

        beginTest("Marked files are busy with their label until cleared");
        {
            tracker.markActive({first, second}, "Waiting");
            expect(tracker.isAnyActive());
            expectEquals(tracker.getActiveLabel(first.getFullPathName()), juce::String("Waiting"));
            expect(repaints > 0);

            tracker.setActive(first.getFullPathName(), "Analyzing");
            expectEquals(tracker.getActiveLabel(first.getFullPathName()), juce::String("Analyzing"));

            tracker.clearActive(first.getFullPathName());
            expectEquals(tracker.getActiveLabel(first.getFullPathName()), juce::String());
            expect(tracker.isAnyActive());

            tracker.clearAllActive();
            expect(!tracker.isAnyActive());
        }

        beginTest("Only the given labels are cleared");
        {
            tracker.markActive({first}, "Analyzing");
            tracker.markActive({second}, "Normalizing");
            tracker.clearActiveWithLabels({"Analyzing", "Waiting"});

            expectEquals(tracker.getActiveLabel(first.getFullPathName()), juce::String());
            expectEquals(tracker.getActiveLabel(second.getFullPathName()), juce::String("Normalizing"));
            tracker.clearAllActive();
        }

        beginTest("A new operation replaces the message of the last one");
        {
            tracker.setMessage(first.getFullPathName(), {.text = "Reduced 1.0 dB to avoid clipping", .isError = false});
            tracker.setMessage(second.getFullPathName(), {.text = "Could not write", .isError = true});
            expect(tracker.getMessage(second.getFullPathName()).isError);

            tracker.markActive({first}, "Processing");
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
