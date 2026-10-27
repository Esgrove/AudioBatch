/// Tests for NormalizeCoordinator, which normalizes a batch of files on a thread pool.
/// Covers which files a run queues, what it publishes for them, a source that loses its output path,
/// and that a cancelled run goes quiet while a stopped run reports its unfinished files as cancelled.
/// Only in-place `.aif` sources are normalized successfully here,
/// because converting other formats moves the original to the system trash.

#include "analysis/AudioAnalysis.h"
#include "normalization/AudioNormalization.h"
#include "normalization/NormalizeCoordinator.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include <condition_variable>

#include <algorithm>
#include <chrono>
#include <memory>
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
/// Allowed difference between the normalized peak and the target, in dB.
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

/// Returns the published result for the given source file, or nothing when the run published none.
std::optional<AudioNormalizationResult> findResult(const CoordinatorRun& run, const juce::File& file)
{
    const auto result = std::ranges::find(run.results, file, &AudioNormalizationResult::file);
    return result != run.results.end() ? std::optional(*result) : std::nullopt;
}
}  // namespace

/// Checks the files a normalization run queues, what it publishes, and its cancellation and stop requests.
class NormalizeCoordinatorTests final : public juce::UnitTest
{
public:
    NormalizeCoordinatorTests() : juce::UnitTest("Normalization coordinator", "AudioBatch") {}

    /// Runs the coordinator checks.
    void runTest() override
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
            records.push_back(AudioAnalysisRecord::fromFile(missingFile));
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

        beginTest("A stopped run reports its unfinished files as cancelled, leaves them unchanged, and completes");
        testCoordinatorStop(directory);
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

    /// Holds the first file of a run in its starting callback, asks the run to stop,
    /// and checks that every file is published as cancelled, the run completes, and no output is written.
    void testCoordinatorStop(const TemporaryDirectory& directory)
    {
        std::vector<AudioAnalysisRecord> records;
        std::vector<juce::MemoryBlock> originalBytes;
        for (const auto* fileName : {"stop_held.aif", "stop_queued.aif"}) {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", fileName);
            records.push_back(analyzeFile(source));
            expect(source.loadFileAsData(originalBytes.emplace_back()));
        }

        std::mutex mutex;
        std::condition_variable condition;
        int startedFiles = 0;
        std::vector<AudioNormalizationResult> results;
        int completedFiles = -1;
        bool released = false;
        NormalizeCoordinator coordinator(1);
        coordinator.setStartingCallback([&](const juce::File& /*file*/, int /*runId*/) {
            std::unique_lock lock(mutex);
            ++startedFiles;
            condition.notify_all();
            condition.wait_for(lock, coordinatorTimeout, [&] { return released; });
        });
        coordinator.setResultCallback([&](const AudioNormalizationResult& result, int /*runId*/) {
            const std::scoped_lock lock(mutex);
            results.push_back(result);
        });
        coordinator.setCompletionCallback([&](const int totalFiles, int /*runId*/) {
            const std::scoped_lock lock(mutex);
            completedFiles = totalFiles;
            condition.notify_all();
        });

        auto options = testNormalizationOptions();
        options.targetPeakDbfs = -1.0;
        expectEquals(coordinator.start(records, options), 2);
        std::unique_lock lock(mutex);
        expect(condition.wait_for(lock, coordinatorTimeout, [&] { return startedFiles == 1; }));
        coordinator.requestStop();
        released = true;
        condition.notify_all();
        expect(condition.wait_for(lock, coordinatorTimeout, [&] { return completedFiles >= 0; }));

        expectEquals(completedFiles, 2);
        expectEquals(startedFiles, 1);
        expectEquals(static_cast<int>(results.size()), 2);
        for (const auto& result : results) {
            const auto* failed = std::get_if<OperationFailed>(&result.outcome);
            expect(failed != nullptr && failed->cancelled, result.fileName);
        }
        for (const auto& [record, original] : std::views::zip(records, originalBytes)) {
            juce::MemoryBlock remaining;
            expect(record.file.loadFileAsData(remaining));
            expect(remaining == original, record.fileName);
        }
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
};

/// Creates the normalization coordinator suite.
std::unique_ptr<juce::UnitTest> createNormalizeCoordinatorTests()
{
    return std::make_unique<NormalizeCoordinatorTests>();
}
}  // namespace audiobatch::tests
