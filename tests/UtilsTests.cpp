/// Tests for the shared helpers in FileUtils, AudioFormatUtils, Cancellation, MessageThreadCall, ConsolePrint,
/// and DecodeFailureTracker.

#include "AudioFormatUtils.h"
#include "Cancellation.h"
#include "ConsolePrint.h"
#include "DecodeFailureTracker.h"
#include "FileUtils.h"
#include "MessageThreadCall.h"
#include "StringFormat.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <span>
#include <thread>

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
        for (auto* const channel : std::span(destSamples, static_cast<std::size_t>(numDestChannels))) {
            const std::span samples(channel, static_cast<std::size_t>(startOffsetInDestBuffer + numSamples));
            std::ranges::fill(
                samples.subspan(static_cast<std::size_t>(startOffsetInDestBuffer)), std::bit_cast<int>(0.5f)
            );
        }

        return false;
    }
};

/// Checks the generic file, format, and cancellation helpers.
class UtilsTests final : public juce::UnitTest
{
public:
    UtilsTests() : juce::UnitTest("Shared helpers", "AudioBatch") {}

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
            metadata.set("MetaDataSource", "AIFF");
            metadata.set("key", "value");
            expect(utils::aiffWriterMetadata(metadata).contains("key"));

            metadata.set("MetaDataSource", "WAV");
            expect(utils::aiffWriterMetadata(metadata).empty());
            expectEquals(utils::peakMagnitude(-0.5f), 0.5f);
        }

        beginTest("Written audio is validated against the expected layout and length");
        {
            auto& formatManager = utils::threadLocalFormatManager();
            const auto fixture = fixtureFile("sine_stereo_44100_16.wav");
            const std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(fixture));
            expect(reader != nullptr);
            const auto length = reader->lengthInSamples;

            expectEquals(utils::validateWrittenAudio(formatManager, fixture, 2, 44100.0, length), juce::String());
            expect(utils::validateWrittenAudio(formatManager, fixture, 1, 44100.0, length).contains("channels"));
            expect(utils::validateWrittenAudio(formatManager, fixture, 2, 48000.0, length).contains("sample rate"));
            expect(utils::validateWrittenAudio(formatManager, fixture, 2, 44100.0, length + 1).contains("samples"));
            expect(
                utils::validateWrittenAudio(formatManager, root.getChildFile("missing.aif"), 2, 44100.0, length)
                    .contains("cannot be read")
            );
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

        beginTest("Force overwrite replaces the existing output and disposes of it and the source");
        {
            const TemporaryDirectory directory;
            const auto source = directory.getChildFile("song.wav");
            const auto output = directory.getChildFile("song.aif");
            expect(source.replaceWithText("source"));
            expect(output.replaceWithText("previous"));

            const juce::TemporaryFile temporaryFile(output);
            expect(temporaryFile.getFile().replaceWithText("new"));

            juce::StringArray disposedContents;
            const auto error
                = utils::commitTemporaryOutput(temporaryFile, source, output, true, [&](const juce::File& file) {
                      disposedContents.add(file.loadFileAsString());
                      return file.deleteFile();
                  });

            expectEquals(error, juce::String());
            expectEquals(output.loadFileAsString(), juce::String("new"));
            expect(!source.exists());
            expectEquals(disposedContents.joinIntoString(","), juce::String("source,previous"));
            expectEquals(output.getParentDirectory().findChildFiles(juce::File::findFiles, false).size(), 1);
        }

        beginTest("A forced commit that cannot dispose of the source restores the previous output");
        {
            const TemporaryDirectory directory;
            const auto source = directory.getChildFile("song.wav");
            const auto output = directory.getChildFile("song.aif");
            expect(source.replaceWithText("source"));
            expect(output.replaceWithText("previous"));

            const juce::TemporaryFile temporaryFile(output);
            expect(temporaryFile.getFile().replaceWithText("new"));

            const auto error = utils::commitTemporaryOutput(temporaryFile, source, output, true, [](const juce::File&) {
                return false;
            });

            expect(error.contains("original audio file"), error);
            expectEquals(output.loadFileAsString(), juce::String("previous"));
            expectEquals(source.loadFileAsString(), juce::String("source"));
            expectEquals(output.getParentDirectory().findChildFiles(juce::File::findFiles, false).size(), 2);
        }

        beginTest("Names match case-insensitively and CSV fields are escaped");
        {
            expectEquals(utils::normalizedToken("  JSON "), juce::String("json"));
            expectEquals(utils::escapeCsvField("plain"), juce::String("plain"));
            expectEquals(utils::escapeCsvField("a,b"), juce::String(R"("a,b")"));
            expectEquals(utils::escapeCsvField(R"(say "hi")"), juce::String(R"("say ""hi""")"));
        }

        beginTest("Display width counts terminal columns");
        {
            expectEquals(console::displayWidth("plain"), 5);
            expectEquals(console::displayWidth(juce::String()), 0);
            expectEquals(console::displayWidth(juce::String::fromUTF8("\xe6\x97\xa5\xe6\x9c\xac")), 4);
        }

        beginTest("A message thread call runs directly on the message thread");
        {
            bool ran = false;
            expect(utils::callOnMessageThread([&ran] { ran = true; }, {}));
            expect(ran);
        }

        beginTest("A queued message thread call from a worker is abandoned when cancelled");
        {
            // The test runs on the message thread and does not dispatch messages,
            // so the call stays queued until the worker gives up on it.
            std::atomic<bool> cancelled {false};
            std::atomic<bool> ran {false};
            std::atomic<int> outcome {-1};

            std::thread worker([&] {
                const auto completed
                    = utils::callOnMessageThread([&ran] { ran = true; }, [&cancelled] { return cancelled.load(); });
                outcome = completed ? 1 : 0;
            });

            juce::Thread::sleep(50);
            cancelled = true;
            worker.join();

            expectEquals(outcome.load(), 0);
            expect(!ran.load());
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
            buffer.setSample(0, 0, 0.25f);

            expect(!utils::readFloatSamples(reader, buffer.getArrayOfWritePointers(), 1, 0, 64));
            expectEquals(buffer.getMagnitude(0, 0, 64), 0.0f);
        }

        beginTest("Decode failures are tolerated in isolation and reported in a notice");
        {
            constexpr int blockSize = 1024;
            const auto blockStart = [](const juce::int64 block) { return block * blockSize; };
            DecodeFailureTracker tracker(blockStart(10));
            expect(tracker.recordBlock(true, 0, blockSize));
            expectEquals(tracker.getNotice(), juce::String());

            expect(tracker.recordBlock(false, blockStart(1), blockSize));
            expect(tracker.recordBlock(true, blockStart(2), blockSize));
            // A failed final block is expected for MP3 files and is not reported.
            expect(tracker.recordBlock(false, blockStart(9), blockSize));
            expectEquals(tracker.getFailedBlockCount(), 1);
            expectEquals(tracker.getNotice(), juce::String("1 audio block(s) failed to decode, first at sample 1024"));

            for (int block = 3; block < 3 + DecodeFailureTracker::maximumConsecutiveFailures - 1; ++block) {
                expect(tracker.recordBlock(false, blockStart(block), blockSize));
            }

            expect(!tracker.recordBlock(false, blockStart(5), blockSize));
            expect(tracker.getErrorMessage().contains("5120"), tracker.getErrorMessage());
        }
    }
};

std::unique_ptr<juce::UnitTest> createUtilsTests()
{
    return std::make_unique<UtilsTests>();
}
}  // namespace audiobatch::tests
