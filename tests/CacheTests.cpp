/// Tests for AnalysisCache against a temporary SQLite database.
/// Covers the round trip of a stored record and invalidation when the file changes on disk.
/// The cached files are copies of the committed fixtures.

#include "AnalysisCache.h"
#include "AnalysisCoordinator.h"
#include "AudioAnalysisService.h"
#include "TestHelpers.h"
#include "TestSuites.h"

namespace audiobatch::tests
{
/// Checks that cached analysis is returned for unchanged files and dropped for changed ones.
class CacheTests final : public juce::UnitTest
{
public:
    CacheTests() : juce::UnitTest("Analysis cache", "AudioBatch") { }

    /// Runs the cache checks.
    void runTest() override
    {
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("analysis.db"));

        beginTest("The cache database opens in a custom location");
        expect(cache.open());
        expect(directory.getChildFile("analysis.db").existsAsFile());

        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "tone.wav");
        const auto analyzed = AudioAnalysisService::analyzeFile(file);
        expect(!analyzed.hasError(), analyzed.errorMessage);

        beginTest("A stored record is returned for the unchanged file");
        {
            expect(cache.storeAnalysis(analyzed));

            AudioAnalysisRecord cached;
            expect(cache.getAnalysis(file, cached));
            expectEquals(cached.overallPeak, analyzed.overallPeak);
            expectEquals(cached.sampleRate, analyzed.sampleRate);
        }

        beginTest("A changed file is not served from the cache");
        {
            expect(fixtureFile("sine_stereo_48000_24.wav").copyFileTo(file));

            AudioAnalysisRecord cached;
            expect(!cache.getAnalysis(file, cached));
        }

        beginTest("A custom gain is kept while the file is unchanged");
        {
            const auto gainFile = directory.copyFixture("sine_stereo_44100_16.wav", "gain.wav");
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(gainFile)));
            expect(cache.storeCustomGain(gainFile, -3.5f, true));

            // Analyzing the unchanged file again must not drop the gain.
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(gainFile)));

            AudioAnalysisRecord cached;
            expect(cache.getAnalysis(gainFile, cached));
            expect(cached.hasCustomGain);
            expectEquals(cached.customGainDb, -3.5f);
        }

        beginTest("A custom gain is cleared when the file changes");
        {
            const auto gainFile = directory.getChildFile("gain.wav");
            expect(fixtureFile("sine_mono_44100_16.wav").copyFileTo(gainFile));
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(gainFile)));

            AudioAnalysisRecord cached;
            expect(cache.getAnalysis(gainFile, cached));
            expect(!cached.hasCustomGain);
        }

        beginTest("Pruning removes rows of deleted files only");
        {
            const auto keptFile = directory.copyFixture("sine_stereo_44100_16.wav", "kept.wav");
            const auto deletedFile = directory.copyFixture("sine_stereo_44100_16.wav", "deleted.wav");
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(keptFile)));
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(deletedFile)));
            expect(deletedFile.deleteFile());

            expectEquals(cache.pruneMissingFiles(), 1);

            AudioAnalysisRecord cached;
            expect(cache.getAnalysis(keptFile, cached));
        }

        beginTest("The coordinator publishes the plan before results and drops the gain of a changed file");
        {
            const auto plannedFile = directory.copyFixture("sine_stereo_44100_16.wav", "planned.wav");
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(plannedFile)));
            expect(cache.storeCustomGain(plannedFile, 2.0f, true));
            expect(fixtureFile("sine_stereo_48000_24.wav").copyFileTo(plannedFile));

            AnalysisCoordinator coordinator(cache, 2);
            int planFileCount = -1;
            bool planCameFirst = false;
            coordinator.setPlanCallback([&](const AnalysisCoordinator::AnalysisPlan& plan) {
                planFileCount = plan.files.size();
            });
            coordinator.setStartingCallback([&](const juce::File&) { planCameFirst = planFileCount >= 0; });

            AudioAnalysisOptions options;
            options.inputPaths.add(plannedFile);
            const auto results = coordinator.analyzeBlocking(options);

            expectEquals(planFileCount, 1);
            expect(planCameFirst);
            expectEquals(static_cast<int>(results.size()), 1);
            expect(!results.front().hasCustomGain);
            expectEquals(results.front().sampleRate, 48000);
        }

        beginTest("A refresh rescan keeps the custom gain of an unchanged file");
        {
            const auto unchangedFile = directory.copyFixture("sine_stereo_44100_16.wav", "unchanged.wav");
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(unchangedFile)));
            expect(cache.storeCustomGain(unchangedFile, -2.5f, true));

            AnalysisCoordinator coordinator(cache, 2);
            AudioAnalysisOptions options;
            options.inputPaths.add(unchangedFile);
            options.refresh = true;
            const auto results = coordinator.analyzeBlocking(options);

            expectEquals(static_cast<int>(results.size()), 1);
            expect(!results.empty() && results.front().hasCustomGain);
            expectEquals(results.empty() ? 0.0f : results.front().customGainDb, -2.5f);
        }

        beginTest("A removed record is no longer returned");
        {
            const auto refreshed = AudioAnalysisService::analyzeFile(file);
            expect(cache.storeAnalysis(refreshed));
            expect(cache.removeAnalysis(file));

            AudioAnalysisRecord cached;
            expect(!cache.getAnalysis(file, cached));
        }
    }
};

std::unique_ptr<juce::UnitTest> createCacheTests()
{
    return std::make_unique<CacheTests>();
}
}  // namespace audiobatch::tests
