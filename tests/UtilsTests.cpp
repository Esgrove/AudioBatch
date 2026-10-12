/// Tests for the shared helpers in FileUtils, AudioFormatUtils, Cancellation, and DecodeFailureTracker.

#include "AudioFormatUtils.h"
#include "Cancellation.h"
#include "DecodeFailureTracker.h"
#include "FileUtils.h"
#include "StringFormat.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <algorithm>
#include <bit>

namespace audiobatch::tests
{
/// A fixed-point reader whose reads write integer samples and then report a failure,
/// like a decoder that fails part way through a block.
class FailingFixedPointReader final : public juce::AudioFormatReader
{
public:
    /// Creates a mono 16-bit reader with one second of audio.
    FailingFixedPointReader() : juce::AudioFormatReader(nullptr, "Failing")
    {
        sampleRate = 44100.0;
        bitsPerSample = 16;
        lengthInSamples = 44100;
        numChannels = 1;
        usesFloatingPointData = false;
    }

    /// Fills the destination with integers whose bits read as 0.5 when reinterpreted as floats, and fails.
    bool readSamples(
        int* const* destSamples,
        const int numDestChannels,
        const int startOffsetInDestBuffer,
        juce::int64 /*startSampleInFile*/,
        const int numSamples
    ) override
    {
        for (int channel = 0; channel < numDestChannels; ++channel) {
            std::fill_n(destSamples[channel] + startOffsetInDestBuffer, numSamples, std::bit_cast<int>(0.5f));
        }

        return false;
    }
};

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

        beginTest("A temporary output replaces its source in place");
        {
            const TemporaryDirectory directory;
            const auto source = directory.getChildFile("song.aif");
            expect(source.replaceWithText("old"));

            const juce::TemporaryFile temporaryFile(source);
            expect(temporaryFile.getFile().replaceWithText("new"));

            expectEquals(utils::commitTemporaryOutput(temporaryFile, source, source, false), juce::String());
            expectEquals(source.loadFileAsString(), juce::String("new"));
        }

        beginTest("A temporary output never replaces an unrelated file without overwrite");
        {
            const TemporaryDirectory directory;
            const auto source = directory.getChildFile("song.wav");
            const auto output = directory.getChildFile("song.aif");
            expect(source.replaceWithText("source"));
            expect(output.replaceWithText("unrelated"));

            const juce::TemporaryFile temporaryFile(output);
            expect(temporaryFile.getFile().replaceWithText("new"));

            const auto error = utils::commitTemporaryOutput(temporaryFile, source, output, false);
            expect(error.contains("already exists"), error);
            expectEquals(output.loadFileAsString(), juce::String("unrelated"));
            expectEquals(source.loadFileAsString(), juce::String("source"));
        }

        beginTest("Force overwrite never trashes a folder at the output path");
        {
            const TemporaryDirectory directory;
            const auto source = directory.getChildFile("song.wav");
            const auto output = directory.getChildFile("song.aif");
            expect(source.replaceWithText("source"));
            expect(output.createDirectory());
            const auto keptFile = output.getChildFile("kept.txt");
            expect(keptFile.replaceWithText("kept"));

            const juce::TemporaryFile temporaryFile(output);
            expect(temporaryFile.getFile().replaceWithText("new"));

            const auto error = utils::commitTemporaryOutput(temporaryFile, source, output, true);
            expect(error.contains("not a file"), error);
            expect(keptFile.existsAsFile());
            expectEquals(source.loadFileAsString(), juce::String("source"));
        }

        beginTest("Names match case-insensitively and CSV fields are escaped");
        {
            expectEquals(utils::normalizedToken("  JSON "), juce::String("json"));
            expectEquals(utils::escapeCsvField("plain"), juce::String("plain"));
            expectEquals(utils::escapeCsvField("a,b"), juce::String(R"("a,b")"));
            expectEquals(utils::escapeCsvField(R"(say "hi")"), juce::String(R"("say ""hi""")"));
        }

        beginTest("An empty cancellation check never cancels");
        {
            expect(!utils::isCancellationRequested({}));
            expect(utils::isCancellationRequested([] { return true; }));
        }

        beginTest("A failed read from a fixed-point reader leaves silence instead of raw integers");
        {
            FailingFixedPointReader reader;
            juce::AudioBuffer<float> buffer(1, 64);
            buffer.getWritePointer(0)[0] = 0.25f;

            expect(!utils::readFloatSamples(reader, buffer.getArrayOfWritePointers(), 1, 0, 64));
            expectEquals(buffer.getMagnitude(0, 0, 64), 0.0f);
        }

        beginTest("Decode failures are tolerated in isolation and reported in a notice");
        {
            constexpr int blockSize = 1024;
            DecodeFailureTracker tracker(blockSize * 10);
            expect(tracker.recordBlock(true, 0, blockSize));
            expectEquals(tracker.getNotice(), juce::String());

            expect(tracker.recordBlock(false, blockSize, blockSize));
            expect(tracker.recordBlock(true, blockSize * 2, blockSize));
            // A failed final block is expected for MP3 files and is not reported.
            expect(tracker.recordBlock(false, blockSize * 9, blockSize));
            expectEquals(tracker.getFailedBlockCount(), 1);
            expectEquals(tracker.getNotice(), juce::String("1 audio block(s) failed to decode, first at sample 1024"));

            for (int block = 3; block < 3 + DecodeFailureTracker::maximumConsecutiveFailures - 1; ++block) {
                expect(tracker.recordBlock(false, blockSize * block, blockSize));
            }

            expect(!tracker.recordBlock(false, blockSize * 5, blockSize));
            expect(tracker.getErrorMessage().contains("5120"), tracker.getErrorMessage());
        }
    }
};

std::unique_ptr<juce::UnitTest> createUtilsTests()
{
    return std::make_unique<UtilsTests>();
}
}  // namespace audiobatch::tests
