/// Tests for the shared helpers in FileUtils, AudioFormatUtils, and Cancellation.

#include "AudioFormatUtils.h"
#include "Cancellation.h"
#include "FileUtils.h"
#include "TestSuites.h"

namespace audiobatch::tests
{
/// Checks the generic file, format, and cancellation helpers.
class UtilsTests final : public juce::UnitTest
{
public:
    UtilsTests() : juce::UnitTest("Shared helpers", "AudioBatch") { }

    /// Runs the helper checks.
    void runTest() override
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory);

        beginTest("Extensions are lowercased without the leading dot");
        {
            expectEquals(utils::normalizeExtension(" .MP3 "), juce::String("mp3"));
            expectEquals(utils::fileExtension(root.getChildFile("Song.AIFF")), juce::String("aiff"));
            expectEquals(utils::fileExtension(root.getChildFile("no_extension")), juce::String());
        }

        beginTest("Audio file types are recognised by extension");
        {
            expect(utils::isMp3File(root.getChildFile("a.MP3")));
            expect(!utils::isMp3File(root.getChildFile("a.mp4")));
            expect(utils::isAiffFile(root.getChildFile("a.aif")));
            expect(utils::isAiffFile(root.getChildFile("a.aifc")));
            expect(!utils::isAiffFile(root.getChildFile("a.wav")));
            expect(utils::isWavFile(root.getChildFile("a.WAVE")));
        }

        beginTest("The AIFF writer and metadata conversion are available");
        {
            expect(utils::findAiffFormat(utils::threadLocalFormatManager()) != nullptr);

            juce::StringPairArray metadata;
            metadata.set("key", "value");
            const auto copied = utils::copyMetadata(metadata);
            expect(copied.contains("key"));
            expectEquals(utils::peakMagnitude(-0.5f), 0.5f);
        }

        beginTest("An empty cancellation check never cancels");
        {
            expect(!utils::isCancellationRequested({}));
            expect(utils::isCancellationRequested([] { return true; }));
        }
    }
};

std::unique_ptr<juce::UnitTest> createUtilsTests()
{
    return std::make_unique<UtilsTests>();
}
}  // namespace audiobatch::tests
