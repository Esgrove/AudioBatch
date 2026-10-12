/// Tests for AnalysisCache against a temporary SQLite database, and for AnalysisCoordinator runs on top of it.
/// Covers the round trip of a stored record and its waveform, invalidation when the file changes on disk,
/// the migration of a database written by an older version, and the delivery of results of replaced runs.
/// The cached files are copies of the committed fixtures.

#include "AnalysisCache.h"
#include "AnalysisCoordinator.h"
#include "AudioAnalysisService.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <atomic>
#include <sqlite3.h>

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

        beginTest("A database written by an older version gets the missing columns");
        {
            const auto oldDatabaseFile = directory.getChildFile("old_analysis.db");
            sqlite3* oldDatabase = nullptr;
            expectEquals(sqlite3_open(oldDatabaseFile.getFullPathName().toRawUTF8(), &oldDatabase), SQLITE_OK);
            expectEquals(
                sqlite3_exec(
                    oldDatabase,
                    R"SQL(
                        CREATE TABLE file_analysis (
                            file_path TEXT PRIMARY KEY,
                            file_name TEXT NOT NULL,
                            format_name TEXT,
                            file_size INTEGER NOT NULL,
                            modified_time_ms INTEGER NOT NULL,
                            length_in_samples INTEGER NOT NULL,
                            duration_seconds REAL NOT NULL,
                            peak_left REAL NOT NULL,
                            peak_right REAL NOT NULL,
                            overall_peak REAL NOT NULL,
                            sample_rate INTEGER NOT NULL,
                            channels INTEGER NOT NULL,
                            bits_per_sample INTEGER NOT NULL,
                            status INTEGER NOT NULL,
                            error_message TEXT,
                            analysis_version INTEGER NOT NULL,
                            updated_at_ms INTEGER NOT NULL
                        );
                    )SQL",
                    nullptr,
                    nullptr,
                    nullptr
                ),
                SQLITE_OK
            );
            sqlite3_close(oldDatabase);

            AnalysisCache migratedCache(oldDatabaseFile);
            expect(migratedCache.open());

            const auto migratedFile = directory.copyFixture("sine_stereo_44100_16.wav", "migrated.wav");
            expect(migratedCache.storeAnalysis(AudioAnalysisService::analyzeFile(migratedFile)));
            expect(migratedCache.storeCustomGain(migratedFile, 1.5f, true));

            AudioAnalysisRecord cached;
            expect(migratedCache.getAnalysis(migratedFile, cached));
            expect(cached.hasCustomGain);
            expectEquals(cached.customGainDb, 1.5f);
        }

        beginTest("A database whose schema cannot be prepared is not left half open");
        {
            const auto notDatabase = directory.getChildFile("not_a_database.db");
            expect(notDatabase.replaceWithText("This text is not an SQLite database, so preparing the schema fails."));
            AnalysisCache brokenCache(notDatabase);

            expect(!brokenCache.open());
            // A connection left open by the first attempt would make the second one report success.
            expect(!brokenCache.open());
        }

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

        beginTest("A custom gain for a file without a cached row reports that nothing was stored");
        {
            const auto waitingFile = directory.copyFixture("sine_stereo_44100_16.wav", "waiting.wav");
            expect(!cache.storeCustomGain(waitingFile, 1.0f, true));
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

        beginTest("Pruning keeps rows of files whose folder is missing, such as an unmounted drive");
        {
            const auto folder = directory.getChildFile("unmounted");
            expect(folder.createDirectory());
            const auto unmountedFile = folder.getChildFile("tone.wav");
            expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(unmountedFile));
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(unmountedFile)));
            expect(cache.storeCustomGain(unmountedFile, 1.5f, true));
            expect(folder.deleteRecursively());

            expectEquals(cache.pruneMissingFiles(), 0);
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
            coordinator.setStartingCallback([&](const juce::File&, int) { planCameFirst = planFileCount >= 0; });

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

        beginTest("Waveform data round trips and is dropped when the analysis is stored again");
        {
            const auto waveformFile = directory.copyFixture("sine_stereo_44100_16.wav", "waveform.wav");
            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(waveformFile)));

            const juce::String waveformText("waveform bytes");
            const juce::MemoryBlock waveform(waveformText.toRawUTF8(), waveformText.getNumBytesAsUTF8());
            expect(cache.storeWaveformData(waveformFile, waveform));

            juce::MemoryBlock loaded;
            expect(cache.getWaveformData(waveformFile, loaded));
            expect(loaded == waveform);

            expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(waveformFile)));
            expect(!cache.getWaveformData(waveformFile, loaded));
        }

        beginTest("Results of a replaced run are not delivered after the new run started");
        {
            std::atomic<int> latestRunId {-1};
            std::atomic<int> lastCompletedRunId {-1};
            std::atomic<int> staleDeliveries {0};
            // Declared after the counters its callbacks use, so it is destroyed first.
            AnalysisCoordinator coordinator(cache, 2);

            coordinator.setResultCallback([&](const AudioAnalysisRecord&, const int runId) {
                if (const auto latest = latestRunId.load(); latest >= 0 && runId != latest) {
                    ++staleDeliveries;
                }
            });
            coordinator.setCompletionCallback([&](int, const int runId) { lastCompletedRunId = runId; });

            AudioAnalysisOptions replacedRun;
            replacedRun.inputPaths.add(fixtureFile("sine_stereo_44100_16.wav").getParentDirectory());
            replacedRun.refresh = true;
            coordinator.start(replacedRun);

            AudioAnalysisOptions currentRun;
            currentRun.inputPaths.add(file);
            currentRun.refresh = true;
            latestRunId = coordinator.start(currentRun);

            constexpr int maximumWaitSteps = 300;
            for (int step = 0; step < maximumWaitSteps && lastCompletedRunId.load() != latestRunId.load(); ++step) {
                juce::Thread::sleep(100);
            }

            expectEquals(lastCompletedRunId.load(), latestRunId.load());
            expectEquals(staleDeliveries.load(), 0);
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
