/// Tests for the normalization functions and NormalizeCoordinator.
/// Covers output path resolution, batch output conflicts, refusal to overwrite unrelated files, cancellation, the level
/// and sample rate of the written output, and the tags and art carried over to it.
/// The coordinator tests check which files a run queues, what it publishes, and that a cancelled run goes quiet.
/// Only in-place `.aif` sources are normalized successfully here,
/// because converting other formats moves the original to the system trash.

#include "analysis/AudioAnalysis.h"
#include "metadata/AudioMetadata.h"
#include "normalization/AudioNormalization.h"
#include "normalization/NormalizeCoordinator.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/StringFormat.h"
#include <condition_variable>
#include <type_traits>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <thread>
#include <variant>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Allowed difference between the normalized peak and 0 dBFS, in dB.
constexpr double peakTolerance = 0.01;

/// How long a coordinator test waits for a run to reach the expected state before it fails.
constexpr auto coordinatorTimeout {std::chrono::seconds(10)};

/// What a coordinator published during one run, collected from its callbacks.
struct CoordinatorRun {
    /// Number of files the run queued, as returned by start.
    int queuedFiles {0};
    /// Total the completion callback reported, or -1 when the run never completed.
    int completedFiles {-1};
    /// Files in the order their normalization started.
    std::vector<juce::File> startedFiles;
    /// Results in the order they were published.
    std::vector<AudioNormalizationResult> results;
    /// Every run id the callbacks carried.
    std::set<int> runIds;
};

/// Builds an analysis record for a file path without analyzing it.
AudioAnalysisRecord recordForPath(const juce::String& path)
{
    return AudioAnalysisRecord::fromFile(juce::File(path));
}

/// Returns the published result for the given source file, or nothing when the run published none.
std::optional<AudioNormalizationResult> findResult(const CoordinatorRun& run, const juce::File& file)
{
    const auto result = std::ranges::find(run.results, file, &AudioNormalizationResult::file);
    return result != run.results.end() ? std::optional(*result) : std::nullopt;
}
}  // namespace

/// Checks normalization paths, conflicts, and output.
class NormalizationTests final : public juce::UnitTest
{
public:
    NormalizationTests() : juce::UnitTest("Normalization", "AudioBatch") {}

    /// Runs the normalization checks.
    void runTest() override
    {
        runOutputPathTests();
        runConflictTests();
        runFileTests();
        runCoordinatorTests();
    }

private:
    /// Runs a batch through a new coordinator and collects its callbacks until the completion callback.
    CoordinatorRun runToCompletion(
        const std::vector<AudioAnalysisRecord>& records,
        const AudioNormalizationOptions& options,
        const int workerCount
    )
    {
        CoordinatorRun run;
        std::mutex mutex;
        std::condition_variable condition;
        NormalizeCoordinator coordinator(workerCount);
        coordinator.setStartingCallback([&](const juce::File& file, const int runId) {
            const std::scoped_lock lock(mutex);
            run.startedFiles.push_back(file);
            run.runIds.insert(runId);
        });
        coordinator.setResultCallback([&](const AudioNormalizationResult& result, const int runId) {
            const std::scoped_lock lock(mutex);
            run.results.push_back(result);
            run.runIds.insert(runId);
        });
        coordinator.setCompletionCallback([&](const int totalFiles, const int runId) {
            const std::scoped_lock lock(mutex);
            run.completedFiles = totalFiles;
            run.runIds.insert(runId);
            condition.notify_all();
        });

        run.queuedFiles = coordinator.start(records, options);
        std::unique_lock lock(mutex);
        expect(condition.wait_for(lock, coordinatorTimeout, [&] { return run.completedFiles >= 0; }));
        return run;
    }

    /// Checks the files a coordinator run queues, what it publishes for them, and cancellation.
    void runCoordinatorTests()
    {
        const TemporaryDirectory directory;

        beginTest("The coordinator normalizes each file once, fails a missing file, and then reports completion");
        {
            std::vector<AudioAnalysisRecord> records;
            for (const auto* fileName : {"first.aif", "second.aif", "third.aif"}) {
                records.push_back(analyzeFile(directory.copyFixture("sine_stereo_44100_16.aif", fileName)));
            }
            records.push_back(records.front());
            const auto missingFile = directory.getChildFile("missing.aif");
            records.push_back(recordForPath(missingFile.getFullPathName()));
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;

            const auto run = runToCompletion(records, options, 2);
            expectEquals(run.queuedFiles, 4);
            expectEquals(run.completedFiles, 4);
            expectEquals(static_cast<int>(run.startedFiles.size()), 4);
            expectEquals(static_cast<int>(run.results.size()), 4);
            expectEquals(static_cast<int>(run.runIds.size()), 1);
            const auto missingResult = findResult(run, missingFile);
            expect(missingResult.has_value());
            if (missingResult) {
                expectEquals(outcomeError(missingResult->outcome), juce::String("File does not exist"));
            }
            for (const auto& record : std::span(records).first(3)) {
                const auto result = findResult(run, record.file);
                expect(result.has_value(), record.fileName);
                if (result) {
                    expect(!result->hasError(), outcomeError(result->outcome));
                    if (!result->hasError()) {
                        expectWithinAbsoluteError(peakDecibels(outcomeRecord(result->outcome)), -1.0, peakTolerance);
                    }
                }
            }
        }

        beginTest("A source that loses its output path fails without being normalized");
        {
            const auto converted = directory.copyFixture("sine_stereo_44100_16.wav", "song.wav");
            const auto inPlace = directory.copyFixture("sine_stereo_44100_16.aif", "song.aif");

            const auto run
                = runToCompletion({analyzeFile(converted), analyzeFile(inPlace)}, testNormalizationOptions(), 1);
            expectEquals(run.queuedFiles, 2);
            expectEquals(run.completedFiles, 2);
            expect(run.startedFiles == std::vector {inPlace});
            const auto result = findResult(run, converted);
            expect(result.has_value());
            if (result) {
                expect(result->hasError());
                expectEquals(outcomeError(result->outcome), batchOutputConflictMessage(converted));
            }
            expect(converted.existsAsFile());
        }

        beginTest("A run without files completes at once with no files");
        {
            const auto run = runToCompletion({}, testNormalizationOptions(), 1);
            expectEquals(run.queuedFiles, 0);
            expectEquals(run.completedFiles, 0);
            expect(run.results.empty());
        }

        beginTest("A cancelled run publishes nothing more and leaves its files unchanged");
        testCoordinatorCancellation(directory);
    }

    /// Holds the first file of a run in its starting callback, cancels the run from another thread,
    /// and checks that the run publishes no result or completion and writes no output.
    void testCoordinatorCancellation(const TemporaryDirectory& directory)
    {
        std::vector<AudioAnalysisRecord> records;
        std::vector<juce::MemoryBlock> originalBytes;
        for (const auto* fileName : {"held.aif", "queued.aif"}) {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", fileName);
            records.push_back(analyzeFile(source));
            expect(source.loadFileAsData(originalBytes.emplace_back()));
        }

        std::mutex mutex;
        std::condition_variable condition;
        int startedFiles = 0;
        int publishedResults = 0;
        bool completed = false;
        bool released = false;
        NormalizeCoordinator coordinator(1);
        coordinator.setStartingCallback([&](const juce::File& /*file*/, int /*runId*/) {
            std::unique_lock lock(mutex);
            ++startedFiles;
            condition.notify_all();
            condition.wait_for(lock, coordinatorTimeout, [&] { return released; });
        });
        coordinator.setResultCallback([&](const AudioNormalizationResult& /*result*/, int /*runId*/) {
            const std::scoped_lock lock(mutex);
            ++publishedResults;
        });
        coordinator.setCompletionCallback([&](int /*totalFiles*/, int /*runId*/) {
            const std::scoped_lock lock(mutex);
            completed = true;
        });

        auto options = testNormalizationOptions();
        options.targetPeakDbfs = -1.0;
        expectEquals(coordinator.start(records, options), 2);
        const auto runId = coordinator.getCurrentRunId();
        {
            std::unique_lock lock(mutex);
            expect(condition.wait_for(lock, coordinatorTimeout, [&] { return startedFiles == 1; }));
        }

        {
            // The run id changes before cancelAndWait starts waiting for the held job,
            // so the job sees the cancellation as soon as it is released.
            const std::jthread canceller([&coordinator] { coordinator.cancelAndWait(); });
            const auto deadline = std::chrono::steady_clock::now() + coordinatorTimeout;
            while (coordinator.getCurrentRunId() == runId && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            expect(coordinator.getCurrentRunId() != runId);
            const std::scoped_lock lock(mutex);
            released = true;
            condition.notify_all();
        }

        const std::scoped_lock lock(mutex);
        expectEquals(startedFiles, 1);
        expectEquals(publishedResults, 0);
        expect(!completed);
        for (const auto& [record, original] : std::views::zip(records, originalBytes)) {
            juce::MemoryBlock remaining;
            expect(record.file.loadFileAsData(remaining));
            expect(remaining == original, record.fileName);
        }
    }

    /// Checks that every source maps to a sibling `.aif` file.
    void runOutputPathTests()
    {
        beginTest("Output is always a sibling .aif file");

        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory);
        expectEquals(getNormalizationOutputFile(root.getChildFile("song.mp3")).getFileName(), juce::String("song.aif"));
        expectEquals(
            getNormalizationOutputFile(root.getChildFile("song.aiff")).getFileName(), juce::String("song.aif")
        );
        expectEquals(getNormalizationOutputFile(root.getChildFile("song.aif")).getFileName(), juce::String("song.aif"));

        beginTest("The format summary describes .aif sources as rewritten in place");
        {
            const auto summary = getFormatSupportSummary();
            expect(summary.contains("A .aif file is rewritten in place"), summary);
            expect(summary.contains("including other AIFF extensions, to a .aif file"), summary);
            expect(!summary.contains("when they are already AIFF"), summary);
        }
    }

    /// Checks which records lose when several sources map to the same output.
    void runConflictTests()
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory);

        beginTest("An in-place .aif source wins its output path");
        {
            const std::vector records {
                recordForPath(root.getChildFile("song.mp3").getFullPathName()),
                recordForPath(root.getChildFile("song.aif").getFullPathName()),
                recordForPath(root.getChildFile("song.flac").getFullPathName()),
                recordForPath(root.getChildFile("other.wav").getFullPathName()),
            };

            const auto conflicts = findBatchOutputConflicts(records);
            expectEquals(conflicts.size(), 2);
            expect(conflicts.contains(root.getChildFile("song.mp3").getFullPathName()));
            expect(conflicts.contains(root.getChildFile("song.flac").getFullPathName()));
        }

        beginTest(
            "Without an in-place source, the first path wins regardless of "
            "input order"
        );
        {
            const std::vector records {
                recordForPath(root.getChildFile("song.mp3").getFullPathName()),
                recordForPath(root.getChildFile("song.flac").getFullPathName()),
            };

            const auto conflicts = findBatchOutputConflicts(records);
            expectEquals(conflicts.size(), 1);
            expect(conflicts.contains(root.getChildFile("song.mp3").getFullPathName()));
        }

        if (!juce::File::areFileNamesCaseSensitive()) {
            beginTest(
                "Outputs that differ only in case conflict where file names "
                "ignore case"
            );
            {
                const std::vector records {
                    recordForPath(root.getChildFile("song.mp3").getFullPathName()),
                    recordForPath(root.getChildFile("SONG.flac").getFullPathName()),
                };

                const auto conflicts = findBatchOutputConflicts(records);
                expectEquals(conflicts.size(), 1);
            }
        }
    }

    /// Normalizes generated files and checks the results on disk.
    void runFileTests()
    {
        const TemporaryDirectory directory;

        beginTest("No-op normalization logs the action before unpadded time and filename");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "log_noop.aif");
            const auto record = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(record);
            const ScopedLogCapture capture;
            const auto result = normalizeFile(record, options);
            expect(!result.hasError(), outcomeError(result.outcome));
            const auto messages = capture.getMessages();
            expect(std::ranges::any_of(messages, [&](const auto& message) {
                return message.contains("[INFO]: Normalized: ")
                    && message.endsWith(utils::format(" s, {} (already at target)", source.getFileName().quoted()));
            }));
        }

        beginTest("An unrelated existing output file is not overwritten");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "collision.wav");
            const auto existingOutput = directory.getChildFile("collision.aif");
            expect(existingOutput.replaceWithText("unrelated"));

            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(result.hasError());
            expect(std::holds_alternative<OperationFailed>(result.outcome));
            expect(outcomeError(result.outcome).contains("already exists"), outcomeError(result.outcome));
            expect(source.existsAsFile());
            expectEquals(existingOutput.loadFileAsString(), juce::String("unrelated"));
        }

        beginTest(
            "A folder at the output path fails before rendering, even with "
            "force overwrite"
        );
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "folder_output.wav");
            const auto folder = directory.getChildFile("folder_output.aif");
            expect(folder.createDirectory());

            auto options = testNormalizationOptions();
            options.overwriteExisting = true;
            int cancellationChecks = 0;
            const auto result = normalizeFile(analyzeFile(source), options, [&] {
                ++cancellationChecks;
                return false;
            });

            expect(result.hasError());
            expect(outcomeError(result.outcome).contains("not a file"), outcomeError(result.outcome));
            expectEquals(cancellationChecks, 0);
            expect(folder.isDirectory());
            expect(source.existsAsFile());
        }

        beginTest("A cancelled run leaves the source untouched");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled.aif");
            const auto sizeBefore = source.getSize();
            const auto record = analyzeFile(source);

            const auto options = testNormalizationOptions();
            const auto result = normalizeFile(record, options, [] { return true; });
            expect(result.hasError());
            expectEquals(source.getSize(), sizeBefore);
            expectEquals(analyzeFile(source).overallPeak, record.overallPeak);
        }

        checkInPlaceNormalization(directory, "sine_mono_24000_16.aif", 1, 24'000, 16);
        checkInPlaceNormalization(directory, "sine_mono_48000_24.aif", 1, 48'000, 24);
        checkInPlaceNormalization(directory, "sine_stereo_44100_16.aif", 2, 44'100, 16);

        beginTest("Tags and embedded art survive in-place normalization");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "tagged.aif");
            AudioMetadata metadata;
            metadata.properties.push_back({.key = "TITLE", .values = {"Tagged tone"}});
            metadata.properties.push_back({.key = "ARTIST", .values = {"AudioBatch"}});
            const juce::String pictureText("not really a picture");
            metadata.pictures.push_back({
                .data = juce::MemoryBlock(pictureText.toRawUTF8(), pictureText.getNumBytesAsUTF8()),
                .mimeType = "image/jpeg",
                .description = "cover",
                .pictureType = "Front Cover",
            });
            expect(writeMetadata(source, metadata));

            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(!result.hasError(), outcomeError(result.outcome));

            AudioMetadata written;
            expect(readMetadata(source, written));
            const auto title
                = std::ranges::find_if(written.properties, [](const auto& entry) { return entry.key == "TITLE"; });
            expect(title != written.properties.end() && title->values[0] == "Tagged tone");
            expectEquals(static_cast<int>(written.pictures.size()), 1);
            expect(!written.pictures.empty() && written.pictures.front().data == metadata.pictures.front().data);
        }

        if (!juce::File::areFileNamesCaseSensitive()) {
            beginTest(
                "A source whose extension differs only in case is normalized "
                "in place"
            );
            {
                const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "Upper.AIF");
                const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());

                expect(!result.hasError(), outcomeError(result.outcome));
                expect(outcomeRecord(result.outcome).file == source);
                expect(source.existsAsFile());
                expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), 0.0, peakTolerance);
            }
        }

        beginTest(
            "A file that changed since analysis is analyzed again before "
            "computing the gain"
        );
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "changed.aif");
            auto staleRecord = analyzeFile(source);
            // Pretend the file was a quieter one when it was analyzed.
            // Trusting this peak would apply 12 dB of gain and clip the output.
            staleRecord.overallPeak = 0.125f;
            staleRecord.modifiedTimeMs -= 1000;

            const auto result = normalizeFile(staleRecord, testNormalizationOptions());
            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(
                outcomeRecord(result.outcome).integratedLufs, fixtureLoudness - fixturePeakDecibels, 0.1
            );
        }

        beginTest("Cancellation stops changed-source analysis before an in-place no-op");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled_refresh.aif");
            auto staleRecord = analyzeFile(source);
            juce::MemoryBlock originalBytes;
            expect(source.loadFileAsData(originalBytes));
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(staleRecord);
            staleRecord.modifiedTimeMs -= 1000;

            const auto result = normalizeFile(staleRecord, options, [] { return true; });
            expect(result.hasError());
            expect(std::holds_alternative<OperationFailed>(result.outcome));
            expect(outcomeError(result.outcome).containsIgnoreCase("cancel"), outcomeError(result.outcome));
            juce::MemoryBlock remainingBytes;
            expect(source.loadFileAsData(remainingBytes));
            expect(remainingBytes == originalBytes);
        }

        beginTest(
            "A successful in-place no-op returns refreshed analysis without "
            "committing output"
        );
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "refreshed_no_op.aif");
            auto staleRecord = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(staleRecord);
            staleRecord.modifiedTimeMs -= 1000;
            staleRecord.overallPeak = 0.125f;
            const auto modifiedBefore = source.getLastModificationTime();
            const auto sizeBefore = source.getSize();

            const auto result = normalizeFile(staleRecord, options);
            expect(!result.hasError(), outcomeError(result.outcome));
            expect(source.getLastModificationTime() == modifiedBefore);
            expectEquals(source.getSize(), sizeBefore);
            expect(outcomeRecord(result.outcome).matchesFileOnDisk());
            expectWithinAbsoluteError(
                peakDecibels(outcomeRecord(result.outcome)), options.targetPeakDbfs, peakTolerance
            );
        }

        beginTest("Cancellation stops re-analysis of an unchanged in-place no-op");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled_no_op.aif");
            const auto record = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(record);

            const auto result = normalizeFile(record, options, [] { return true; });
            expect(result.hasError());
            expect(std::holds_alternative<OperationFailed>(result.outcome));
            expect(outcomeError(result.outcome).containsIgnoreCase("cancel"), outcomeError(result.outcome));
        }

        beginTest(
            "Loudness mode reaches the target loudness unless the true peak "
            "ceiling is lower"
        );
        {
            const auto record = analyzeFile(fixtureFile("sine_stereo_44100_16.wav"));
            auto options = testNormalizationOptions();
            options.mode = AudioNormalizationMode::loudness;

            options.targetLufs = -14.0;
            auto gain = getNormalizationGain(record, options);
            expect(gain.has_value(), gain.error_or({}));
            expectWithinAbsoluteError(
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(LinearGain {0.0f}).value)),
                -14.0 - record.integratedLufs,
                0.01
            );

            // Reaching 0 LUFS would put the true peak at about 0 dBTP,
            // above the ceiling, so the gain stops at the ceiling.
            options.targetLufs = 0.0;
            gain = getNormalizationGain(record, options);
            expect(gain.has_value(), gain.error_or({}));
            expectWithinAbsoluteError(
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(LinearGain {0.0f}).value)),
                AudioNormalizationOptions::defaultTruePeakCeilingDbtp
                    - juce::Decibels::gainToDecibels(record.overallTruePeak),
                0.01
            );
        }

        beginTest("In-place loudness normalization reaches the target");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "loudness.aif");
            auto options = testNormalizationOptions();
            options.mode = AudioNormalizationMode::loudness;
            options.targetLufs = -14.0;

            const auto result = normalizeFile(analyzeFile(source), options);
            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(outcomeRecord(result.outcome).integratedLufs, -14.0, 0.1);
        }

        beginTest("Peak mode reaches a target below full scale");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "peak.aif");
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;

            const auto result = normalizeFile(analyzeFile(source), options);
            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), -1.0, peakTolerance);
        }

        beginTest("A silent file cannot be normalized");
        {
            const auto source = directory.copyFixture("silence_stereo_44100_16.wav");
            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(result.hasError());
            expect(source.existsAsFile());
        }

        beginTest("A gain in decibels converts to its linear factor and keeps the JUCE floor");
        {
            static_assert(!std::is_convertible_v<float, Decibels>);
            static_assert(!std::is_convertible_v<float, LinearGain>);
            expectWithinAbsoluteError(Decibels {}.toLinearGain().value, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError(Decibels {-6.0206f}.toLinearGain().value, 0.5f, 1.0e-4f);
            expectWithinAbsoluteError(Decibels {6.0206f}.toLinearGain().value, 2.0f, 1.0e-4f);
            expectEquals(Decibels {-120.0f}.toLinearGain().value, 0.0f);
        }

        beginTest("A nearly silent file needs more gain than normalization applies");
        {
            auto record = analyzeFile(fixtureFile("sine_stereo_44100_16.aif"));
            // -60 dBFS, so reaching full scale would need about 60 dB of gain.
            record.overallPeak = 0.001f;
            record.overallTruePeak = 0.001;
            record.integratedLufs = -80.0;
            auto options = testNormalizationOptions();

            const auto peakGain = getNormalizationGain(record, options);
            expect(!peakGain.has_value());
            expect(peakGain.error_or({}).contains("dB of gain"), peakGain.error_or({}));

            options.mode = AudioNormalizationMode::loudness;
            const auto loudnessGain = getNormalizationGain(record, options);
            expect(!loudnessGain.has_value());
            expect(loudnessGain.error_or({}).contains("dB of gain"), loudnessGain.error_or({}));

            // 30 dB of gain stays within the limit.
            options.mode = AudioNormalizationMode::peak;
            options.targetPeakDbfs = -30.0;
            const auto gain = getNormalizationGain(record, options);
            expect(gain.has_value(), gain.error_or({}));
            expectWithinAbsoluteError(
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(LinearGain {0.0f}).value)), 30.0, 0.01
            );
        }

        beginTest("Converting an MP3 file keeps the start of the audio in place");
        {
            const auto source = directory.copyFixture("click_mono_44100_320k.mp3", "click.mp3");
            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(outcomeRecord(result.outcome).file.getFileName(), juce::String("click.aif"));
            expectEquals(outcomeRecord(result.outcome).lengthInSamples, clickFixtureLength);
            expectEquals(peakSamplePosition(outcomeRecord(result.outcome).file), clickFixturePosition);
        }

        beginTest("An .aif source normalized in place disposes of a copy of the original first");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "backup.aif");
            const auto sourceSize = source.getSize();
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;
            juce::StringArray disposedNames;
            juce::Array<juce::int64> disposedSizes;
            options.disposeFile = [&](const juce::File& file) {
                disposedNames.add(file.getFileName());
                disposedSizes.add(file.getSize());
                return file.deleteFile();
            };

            const auto result = normalizeFile(analyzeFile(source), options);
            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(disposedNames.joinIntoString(","), juce::String("backup.aif"));
            expectEquals(disposedSizes[0], sourceSize);
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), -1.0, peakTolerance);
        }

        beginTest("An in-place no-op disposes of nothing");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "no_backup.aif");
            const auto record = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(record);
            int disposedCount = 0;
            options.disposeFile = [&disposedCount](const juce::File& file) {
                ++disposedCount;
                return file.deleteFile();
            };

            const auto result = normalizeFile(record, options);
            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(disposedCount, 0);
        }

        beginTest("An .aif normalized to 0 dBFS is left as it is when normalized to 0 dBFS again");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "full_scale.aif");
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = 0.0;
            const auto first = normalizeFile(analyzeFile(source), options);
            expect(!first.hasError(), outcomeError(first.outcome));
            const auto normalizedSize = source.getSize();

            int disposedCount = 0;
            options.disposeFile = [&disposedCount](const juce::File& file) {
                ++disposedCount;
                return file.deleteFile();
            };
            const auto second = normalizeFile(analyzeFile(source), options);
            expect(!second.hasError(), outcomeError(second.outcome));
            expectEquals(disposedCount, 0);
            expectEquals(source.getSize(), normalizedSize);
        }
    }

    /// Normalizes a copy of an `.aif` fixture in place,
    /// and checks that it reaches 0 dBFS and keeps its channel count, sample rate, and bit depth.
    void checkInPlaceNormalization(
        const TemporaryDirectory& directory,
        const juce::String& fixtureName,
        const int channels,
        const int sampleRate,
        const int bitsPerSample
    )
    {
        beginTest(utils::format("In-place normalization of {}", fixtureName));

        const auto source = directory.copyFixture(fixtureName);
        const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
        expect(!result.hasError(), outcomeError(result.outcome));
        expectEquals(outcomeRecord(result.outcome).fullPath, source.getFullPathName());
        expectEquals(outcomeRecord(result.outcome).channels, channels);
        expectEquals(outcomeRecord(result.outcome).sampleRate, sampleRate);
        expectEquals(outcomeRecord(result.outcome).bitsPerSample, bitsPerSample);
        expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), 0.0, peakTolerance);
    }
};

std::unique_ptr<juce::UnitTest> createNormalizationTests()
{
    return std::make_unique<NormalizationTests>();
}
}  // namespace audiobatch::tests
