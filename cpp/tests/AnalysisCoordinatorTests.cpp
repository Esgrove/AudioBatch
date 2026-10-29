/// Tests for AnalysisCoordinator runs on top of an analysis cache in a temporary folder.
/// Covers the plan published before the results, the custom gain of a changed and of an unchanged file,
/// and a run replaced while its file is still being analyzed, which must publish nothing more.
/// The analyzed files are copies of the committed fixtures.

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/StringFormat.h"
#include <condition_variable>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// How long a coordinator test waits for a run to reach the expected state before it fails.
constexpr auto coordinatorTimeout {std::chrono::seconds(10)};
}  // namespace

/// Checks what an analysis run publishes, in which order, and that a replaced run goes quiet.
class AnalysisCoordinatorTests final : public juce::UnitTest
{
public:
    AnalysisCoordinatorTests() : juce::UnitTest("Analysis coordinator", "AudioBatch") {}

    /// Runs the coordinator checks against a fresh cache.
    void runTest() override
    {
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("analysis.db"));

        beginTest("The coordinator publishes the plan before results and drops the gain of a changed file");
        {
            expect(cache.open());
            const auto plannedFile = directory.copyFixture("sine_stereo_44100_16.wav", "planned.wav");
            expect(cache.storeAnalysis(analyzeFile(plannedFile)));
            expect(cache.storeCustomGain(plannedFile, Decibels {2.0f}));
            expect(fixtureFile("sine_stereo_48000_24.wav").copyFileTo(plannedFile));

            AnalysisCoordinator coordinator(cache, 2);
            int planFileCount = -1;
            bool planCameFirst = false;
            coordinator.setPlanCallback([&](const AnalysisPlan& plan) { planFileCount = plan.files.size(); });
            coordinator.setStartingCallback([&](const juce::File&, int) { planCameFirst = planFileCount >= 0; });

            AudioAnalysisOptions options;
            options.inputPaths.add(plannedFile);
            const auto results = coordinator.analyzeBlocking(options);

            expectEquals(planFileCount, 1);
            expect(planCameFirst);
            expectEquals(static_cast<int>(results.size()), 1);
            if (!results.empty()) {
                expect(!results.front().customGain);
                expectEquals(results.front().sampleRate, 48'000);
            }
        }

        beginTest("A refresh rescan keeps the custom gain of an unchanged file");
        {
            const auto unchangedFile = directory.copyFixture("sine_stereo_44100_16.wav", "unchanged.wav");
            expect(cache.storeAnalysis(analyzeFile(unchangedFile)));
            expect(cache.storeCustomGain(unchangedFile, Decibels {-2.5f}));

            AnalysisCoordinator coordinator(cache, 2);
            AudioAnalysisOptions options;
            options.inputPaths.add(unchangedFile);
            options.refresh = true;
            const auto results = coordinator.analyzeBlocking(options);

            expectEquals(static_cast<int>(results.size()), 1);
            expect(!results.empty() && results.front().customGain.has_value());
            expectEquals(results.empty() ? 0.0f : results.front().customGain.value_or(Decibels {}).value, -2.5f);
        }

        beginTest("Results of a replaced run are not delivered after the new run started");
        testReplacedRun(cache, directory);
    }

private:
    /// Holds the only job of a first run in its starting callback until a second run has replaced it,
    /// so the first run is still analyzing when it goes stale instead of racing the second one.
    /// Checks that the replaced run publishes no result and no completion, while the second run completes.
    void testReplacedRun(AnalysisCache& cache, const TemporaryDirectory& directory)
    {
        const auto heldFile = directory.copyFixture("sine_stereo_44100_16.wav", "held.wav");
        const auto currentFile = directory.copyFixture("sine_stereo_48000_24.wav", "current.wav");
        std::mutex mutex;
        std::condition_variable condition;
        int heldRunId {-1};
        bool released {false};
        std::vector<int> resultRunIds;
        std::vector<int> completedRunIds;

        AnalysisCoordinator coordinator(cache, 2);
        coordinator.setStartingCallback([&](const juce::File& startedFile, const int runId) {
            std::unique_lock lock(mutex);
            if (startedFile != heldFile) {
                return;
            }
            heldRunId = runId;
            condition.notify_all();
            condition.wait_for(lock, coordinatorTimeout, [&] { return released; });
        });
        coordinator.setResultCallback([&](const AudioAnalysisRecord& /*record*/, const int runId) {
            const std::scoped_lock lock(mutex);
            resultRunIds.push_back(runId);
        });
        coordinator.setCompletionCallback([&](int /*totalFiles*/, const int runId) {
            const std::scoped_lock lock(mutex);
            completedRunIds.push_back(runId);
            condition.notify_all();
        });

        AudioAnalysisOptions replacedRun;
        replacedRun.inputPaths.add(heldFile);
        replacedRun.refresh = true;
        const auto replacedRunId = coordinator.start(replacedRun);
        {
            std::unique_lock lock(mutex);
            expect(condition.wait_for(lock, coordinatorTimeout, [&] { return heldRunId >= 0; }));
            expectEquals(heldRunId, replacedRunId);
        }

        AudioAnalysisOptions currentRun;
        currentRun.inputPaths.add(currentFile);
        currentRun.refresh = true;
        const auto currentRunId = coordinator.start(currentRun);
        expect(currentRunId != replacedRunId);

        {
            std::unique_lock lock(mutex);
            released = true;
            condition.notify_all();
            expect(condition.wait_for(lock, coordinatorTimeout, [&] {
                return std::ranges::contains(completedRunIds, currentRunId);
            }));
        }
        // Waits for the released job too, so everything it could still publish has been published.
        coordinator.cancelAndWait();

        const std::scoped_lock lock(mutex);
        expect(resultRunIds == std::vector {currentRunId}, utils::format("{} results", resultRunIds.size()));
        expect(completedRunIds == std::vector {currentRunId}, utils::format("{} completions", completedRunIds.size()));
        expect(!cache.getAnalysis(heldFile).has_value(), "the replaced run must not cache its cancelled analysis");
    }
};

/// Creates the analysis coordinator suite.
std::unique_ptr<juce::UnitTest> createAnalysisCoordinatorTests()
{
    return std::make_unique<AnalysisCoordinatorTests>();
}
}  // namespace audiobatch::tests
