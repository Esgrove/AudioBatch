/// Tests for the shared helpers in AppInfo, FileUtils, AudioFormatUtils, ThreadUtils, ConsolePrint,
/// and DecodeFailureTracker.

#include "analysis/DecodeFailureTracker.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AppInfo.h"
#include "utils/AudioFormatUtils.h"
#include "utils/ConsolePrint.h"
#include "utils/FileUtils.h"
#include "utils/Log.h"
#include "utils/MessageThreadBatcher.h"
#include "utils/StringFormat.h"
#include "utils/ThreadUtils.h"
#include <string_view>

#include <algorithm>
#include <atomic>
#include <bit>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

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

        beginTest("Startup flags recognize complete options and their short aliases");
        {
            expect(utils::isVersionRequest("--version"));
            expect(utils::isVersionRequest("-V"));
            expect(!utils::isVersionRequest({}));
            expect(!utils::isVersionRequest("--versions"));
            expect(!utils::isVersionRequest("track--version.wav"));
            expect(!utils::isVersionRequest("--headless"));
            expect(utils::isHeadlessRequest("--headless"));
            expect(utils::isHeadlessRequest("-H"));
            expect(!utils::isHeadlessRequest({}));
            expect(!utils::isHeadlessRequest("--headless-mode"));
            expect(!utils::isHeadlessRequest("track--headless.wav"));
            expect(!utils::isHeadlessRequest("--version"));
        }

        beginTest("System information retains indented details without padding one-line messages");
        {
            const ScopedLogCapture capture;
            utils::logSystemInfo();
            utils::logInfo("Processing log check");
            const auto messages = capture.getMessages();
            expect(messages.size() >= 3);
            if (messages.size() >= 3) {
                expect(messages[0].contains("[INFO]: System info\n    AudioBatch"), messages[0]);
                for (int index = 1; index < messages.size() - 1; ++index) {
                    expect(messages[index].startsWith("    "), messages[index]);
                }
                expect(messages[messages.size() - 1].endsWith("[INFO]: Processing log check"));
            }
        }

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

            const auto validationError =
                [&formatManager](
                    const juce::File& file, const int channels, const double sampleRate, const juce::int64 samples
                ) {
                    return utils::validateWrittenAudio(formatManager, file, channels, sampleRate, samples).error_or({});
                };

            expect(utils::validateWrittenAudio(formatManager, fixture, 2, 44100.0, length).has_value());
            expect(validationError(fixture, 1, 44100.0, length).contains("channels"));
            expect(validationError(fixture, 2, 48000.0, length).contains("sample rate"));
            expect(validationError(fixture, 2, 44100.0, length + 1).contains("samples"));
            expect(validationError(root.getChildFile("missing.aif"), 2, 44100.0, length).contains("cannot be read"));
        }

        beginTest("A temporary output replaces its source in place");
        {
            const TemporaryDirectory directory;
            const auto source = directory.getChildFile("song.aif");
            expect(source.replaceWithText("old"));

            const juce::TemporaryFile temporaryFile(source);
            expect(temporaryFile.getFile().replaceWithText("new"));

            expect(utils::commitTemporaryOutput(temporaryFile, source, source, false).has_value());
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

            const auto error = utils::commitTemporaryOutput(temporaryFile, source, output, false).error_or({});
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

            const auto error = utils::commitTemporaryOutput(temporaryFile, source, output, true).error_or({});
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
            const auto commit
                = utils::commitTemporaryOutput(temporaryFile, source, output, true, [&](const juce::File& file) {
                      disposedContents.add(file.loadFileAsString());
                      return file.deleteFile();
                  });

            expect(commit.has_value(), commit.error_or({}));
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
                               }).error_or({});

            expect(error.contains("original audio file"), error);
            expectEquals(output.loadFileAsString(), juce::String("previous"));
            expectEquals(source.loadFileAsString(), juce::String("source"));
            expectEquals(output.getParentDirectory().findChildFiles(juce::File::findFiles, false).size(), 2);
        }

        runDisposalTests();

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

        beginTest("Values posted from a worker are delivered in one batch, in posting order");
        {
            std::vector<std::vector<int>> batches;
            utils::MessageThreadBatcher<int> batcher([&batches](std::vector<int> batch) {
                batches.push_back(std::move(batch));
            });

            // The test runs on the message thread without dispatching messages, so delivery is triggered by hand.
            batcher.deliverPending();
            expect(batches.empty());

            std::thread worker([&batcher] {
                for (int value = 1; value <= 3; ++value) {
                    batcher.post(value);
                }
            });
            worker.join();

            batcher.deliverPending();
            batcher.deliverPending();
            expectEquals(static_cast<int>(batches.size()), 1);
            expect(!batches.empty() && batches.front() == std::vector {1, 2, 3});
        }

        beginTest("An exception being handled is described by its message");
        {
            try {
                throw std::runtime_error("worker failure");
            } catch (...) {
                expectEquals(utils::describeCurrentException(), juce::String("worker failure"));
            }
        }

        beginTest("An empty cancellation check never cancels");
        {
            expect(!utils::isCancellationRequested({}));
            expect(utils::isCancellationRequested([] { return true; }));
        }

        beginTest("A progress range maps a phase into part of the whole operation");
        {
            std::vector<float> reported;
            const utils::ProgressCallback onProgress
                = [&reported](const float progress) { reported.push_back(progress); };
            const auto phase = utils::makeProgressRange(onProgress, 0.2f, 0.6f);
            phase(0.0f);
            phase(0.5f);
            phase(2.0f);

            expectEquals(static_cast<int>(reported.size()), 3);
            expectWithinAbsoluteError(reported[0], 0.2f, 1.0e-6f);
            expectWithinAbsoluteError(reported[1], 0.4f, 1.0e-6f);
            expectWithinAbsoluteError(reported[2], 0.6f, 1.0e-6f);
            expect(utils::makeProgressRange({}, 0.0f, 1.0f) == nullptr);
            expectWithinAbsoluteError(utils::progressFraction(1, 4), 0.25f, 1.0e-6f);
            expectEquals(utils::progressFraction(0, 0), 1.0f);
        }

        beginTest("Throttled progress skips small steps but always reports completion");
        {
            std::vector<float> reported;
            const auto onProgress
                = utils::makeThrottledProgress([&reported](const float progress) { reported.push_back(progress); });

            for (int step = 0; step <= 1000; ++step) {
                onProgress(static_cast<float>(step) / 1000.0f);
            }

            expect(!reported.empty());
            expect(reported.size() < 1000, juce::String(static_cast<int>(reported.size())));
            expectEquals(reported.back(), 1.0f);
            expect(std::ranges::is_sorted(reported));
            expect(utils::makeThrottledProgress({}) == nullptr);
        }

        beginTest("Padded MP3 frames without a frame-count header have an exact readable length");
        {
            const TemporaryDirectory temporary;
            const auto file = temporary.copyFixture("sine_stereo_44100_320k.mp3");
            juce::MemoryBlock data;
            expect(file.loadFileAsData(data));
            const std::span<char> bytes(static_cast<char*>(data.getData()), data.getSize());

            for (const std::string_view marker : {"Info", "Xing"}) {
                const auto found = std::ranges::search(bytes, marker);
                if (!found.empty()) {
                    std::ranges::fill(found, '\0');
                }
            }

            expect(file.replaceWithData(data.getData(), data.getSize()));
            auto reader = utils::createAudioReader(file);
            expect(reader != nullptr);

            if (reader != nullptr) {
                expectEquals(reader->lengthInSamples, static_cast<juce::int64>(41 * 1152));
                juce::AudioBuffer<float> buffer(2, 512);
                for (juce::int64 position = 0; position < reader->lengthInSamples; position += 512) {
                    const auto samples
                        = static_cast<int>(juce::jmin<juce::int64>(512, reader->lengthInSamples - position));
                    expect(utils::readFloatSamples(*reader, buffer.getArrayOfWritePointers(), 2, position, samples));
                }
            }

            reader.reset();
            juce::MemoryBlock trailingTag(128, true);
            std::copy_n("TAG", 3, static_cast<char*>(trailingTag.getData()));
            data.append(trailingTag.getData(), trailingTag.getSize());
            expect(file.replaceWithData(data.getData(), data.getSize()));
            reader = utils::createAudioReader(file);
            expect(reader != nullptr);
            if (reader != nullptr) {
                expectEquals(reader->lengthInSamples, static_cast<juce::int64>(41 * 1152));
            }
        }

        beginTest("Readers retain indexed MP3 and PCM lengths and reject unreadable files");
        {
            for (const auto* fixtureName : {"sine_stereo_44100_320k.mp3", "sine_stereo_44100_16.aif"}) {
                const auto file = fixtureFile(fixtureName);
                const std::unique_ptr<juce::AudioFormatReader> original(
                    utils::threadLocalFormatManager().createReaderFor(file)
                );
                const auto reader = utils::createAudioReader(file);
                expect(original != nullptr && reader != nullptr);
                if (original != nullptr && reader != nullptr) {
                    expectEquals(reader->lengthInSamples, original->lengthInSamples);
                }
            }
            expect(utils::createAudioReader(fixtureFile("broken.wav")) == nullptr);
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

private:
    /// Checks moving to the trash and the backups of originals, without touching the system trash.
    void runDisposalTests()
    {
        beginTest("Moving a missing file to the trash reports it as already missing");
        {
            const TemporaryDirectory directory;
            const auto missing = directory.getChildFile("missing.aif");
            expect(utils::moveToTrash(missing) == utils::TrashResult::alreadyMissing);
            expect(utils::trashFile(missing));
        }

        beginTest("Concurrent backups of files with the same name each dispose of their own copy");
        {
            const TemporaryDirectory directory;
            const auto source = directory.getChildFile("song.aif");
            expect(source.replaceWithText("original"));
            constexpr int workerCount {4};
            constexpr int backupsPerWorker {10};
            std::atomic<int> failedBackups {0};
            std::atomic<int> foreignCopies {0};

            const auto backUp = [&] {
                for (int backup = 0; backup < backupsPerWorker; ++backup) {
                    const auto disposed = utils::disposeOfCopy(source, [&](const juce::File& copy) {
                        if (copy.loadFileAsString() != "original") {
                            ++foreignCopies;
                        }
                        return copy.deleteFile();
                    });

                    if (!disposed) {
                        ++failedBackups;
                    }
                }
            };

            {
                std::vector<std::jthread> workers;
                workers.reserve(workerCount);

                for (int worker = 0; worker < workerCount; ++worker) {
                    workers.emplace_back(backUp);
                }
            }

            expectEquals(failedBackups.load(), 0);
            expectEquals(foreignCopies.load(), 0);
            expectEquals(source.loadFileAsString(), juce::String("original"));
        }
    }
};

std::unique_ptr<juce::UnitTest> createUtilsTests()
{
    return std::make_unique<UtilsTests>();
}
}  // namespace audiobatch::tests
