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

        beginTest("Numeric Overview data round trips without an analysis row");
        {
            const auto tagFile = directory.copyFixture("sine_stereo_44100_16.wav", "tag_only.wav");
            const AnalysisCache::FileIdentity identity(tagFile);
            audiobatch::serato::Overview overview;
            overview.slices.resize(2);
            overview.slices.front().fill(7);
            overview.slices.back().fill(19);
            const audiobatch::serato::TagReadResult result {
                .status = audiobatch::serato::ReadStatus::present, .overview = overview
            };
            expect(cache.storeSeratoTag(identity, result));
            const auto loaded = cache.getSeratoTag(identity);
            expect(loaded.has_value());
            expect(loaded && loaded->overview && loaded->overview->slices == overview.slices);
            AudioAnalysisRecord cached;
            expect(!cache.getAnalysis(tagFile, cached));
        }

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

            const AnalysisCache::FileIdentity identity(migratedFile);
            expect(migratedCache.storeSeratoTag(identity, {}));
            expect(migratedCache.getSeratoTag(identity).has_value());
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

        runSeratoTests();
    }

private:
    /// Exercises tag-only lifecycle, stable outcomes, corruption and independence using fixture copies.
    void runSeratoTests()
    {
        using namespace audiobatch::serato;
        const TemporaryDirectory directory;
        const auto databaseFile = directory.getChildFile("tag_cache.db");
        AnalysisCache cache(databaseFile);
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "tag.wav");
        const AnalysisCache::FileIdentity identity(file);
        Overview overview;
        overview.slices.resize(3);
        overview.slices.front().fill(3);
        overview.slices.back().fill(255);
        const TagReadResult positive {.status = ReadStatus::present, .overview = overview};

        beginTest("Tag results survive a database reopen without an analysis row");
        {
            AnalysisCache writer(databaseFile);
            expect(writer.storeSeratoTag(identity, positive));
        }
        const auto reopened = cache.getSeratoTag(identity);
        expect(
            reopened && reopened->status == ReadStatus::present && reopened->overview
            && reopened->overview->slices == overview.slices
        );

        beginTest("All stable negatives round trip with SQL NULL data");
        for (const auto status : {ReadStatus::absent, ReadStatus::invalid, ReadStatus::unsupported}) {
            expect(cache.storeSeratoTag(identity, {.status = status, .overview = std::nullopt}));
            const auto loaded = cache.getSeratoTag(identity);
            expect(loaded && loaded->status == status && !loaded->overview);
            expectSqlScalar(databaseFile, "SELECT decoded_data IS NULL FROM serato_tag_cache;", 1);
        }

        beginTest("Transient and inconsistent results never overwrite a stable result");
        expect(cache.storeSeratoTag(identity, positive));
        for (const auto status : {ReadStatus::ioError, ReadStatus::cancelled, static_cast<ReadStatus>(99)}) {
            expect(!cache.storeSeratoTag(identity, {.status = status, .overview = std::nullopt}));
        }
        expect(!cache.storeSeratoTag(identity, {.status = ReadStatus::present, .overview = std::nullopt}));
        expect(!cache.storeSeratoTag(identity, {.status = ReadStatus::present, .overview = Overview {}}));
        Overview oversized;
        oversized.slices.resize(Overview::maxSlices + 1);
        expect(!cache.storeSeratoTag(identity, {.status = ReadStatus::present, .overview = std::move(oversized)}));
        expect(!cache.storeSeratoTag(identity, {.status = ReadStatus::absent, .overview = overview}));
        const auto retained = cache.getSeratoTag(identity);
        expect(retained && retained->overview && retained->overview->slices == overview.slices);
        expect(cache.removeAnalysis(file));
        expect(!cache.storeSeratoTag(identity, {.status = ReadStatus::ioError, .overview = std::nullopt}));
        expect(!cache.storeSeratoTag(identity, {.status = ReadStatus::cancelled, .overview = std::nullopt}));
        expect(!cache.getSeratoTag(identity));
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

        beginTest("Unknown tag kinds miss and cannot be stored");
        const auto unknownKind = static_cast<TagKind>(2);
        expect(!cache.getSeratoTag(identity, unknownKind));
        expect(!cache.storeSeratoTag(
            identity, {.kind = unknownKind, .status = ReadStatus::absent, .overview = std::nullopt}
        ));

        beginTest("Corrupt state, blob, parser, path and stat fields are cache misses");
        for (const auto* sql :
             {
                 "UPDATE serato_tag_cache SET result_state = 0;",
                 "UPDATE serato_tag_cache SET result_state = 257;",
                 "UPDATE serato_tag_cache SET result_state = 5, decoded_data = NULL;",
                 "UPDATE serato_tag_cache SET result_state = 6, decoded_data = NULL;",
                 "UPDATE serato_tag_cache SET result_state = 'invalid';",
                 "UPDATE serato_tag_cache SET decoded_data = NULL;",
                 "UPDATE serato_tag_cache SET decoded_data = X'0105';",
                 "UPDATE serato_tag_cache SET decoded_data = zeroblob(1048593);",
                 "UPDATE serato_tag_cache SET decoded_data = 'not numeric bands';",
                 "UPDATE serato_tag_cache SET decoded_data = X'534F5652000000010000000100000000';",
                 "UPDATE serato_tag_cache SET decoded_data = X'534F5652000000010000000100010001';",
                 "UPDATE serato_tag_cache SET result_state = 2;",
                 "UPDATE serato_tag_cache SET parser_version = 0;",
                 "UPDATE serato_tag_cache SET parser_version = 4294967297;",
                 "UPDATE serato_tag_cache SET parser_version = 'unknown';",
                 "UPDATE serato_tag_cache SET file_size = -1;",
                 "UPDATE serato_tag_cache SET file_size = 'unknown';",
                 "UPDATE serato_tag_cache SET modified_time_ms = 0;",
                 "UPDATE serato_tag_cache SET modified_time_ms = 'unknown';",
                 "UPDATE serato_tag_cache SET updated_at_ms = 0;",
                 "UPDATE serato_tag_cache SET updated_at_ms = 'unknown';",
                 "UPDATE serato_tag_cache SET file_path = 'relative.wav';",
                 "UPDATE serato_tag_cache SET file_path = CAST(file_path AS BLOB);",
                 "UPDATE serato_tag_cache SET tag_kind = 2;",
                 "UPDATE serato_tag_cache SET tag_kind = 'unknown';",
             })
        {
            expect(cache.removeAnalysis(file));
            expect(executeSql(databaseFile, "DELETE FROM serato_tag_cache;"));
            expect(cache.storeSeratoTag(identity, positive));
            expect(executeSql(databaseFile, sql));
            expect(!cache.getSeratoTag(identity), sql);
        }
        expect(executeSql(databaseFile, "DELETE FROM serato_tag_cache;"));

        beginTest("Analysis and waveform writes leave independently versioned tags untouched");
        expect(cache.storeSeratoTag(identity, positive));
        const auto analyzed = AudioAnalysisService::analyzeFile(file);
        expect(cache.storeAnalysis(analyzed));
        const juce::MemoryBlock waveform("cached peaks", 12);
        expect(cache.storeWaveformData(file, waveform));
        expectSqlScalar(databaseFile, "SELECT analysis_version FROM file_analysis;", 8);
        expectSqlScalar(databaseFile, "SELECT waveform_version FROM file_analysis;", 1);
        expect(cache.getSeratoTag(identity).has_value());
        juce::MemoryBlock loadedWaveform;
        expect(cache.getWaveformData(file, loadedWaveform));
        expect(loadedWaveform == waveform);
        expect(cache.storeAnalysis(analyzed));
        expect(!cache.getWaveformData(file, loadedWaveform));
        expect(cache.getSeratoTag(identity).has_value());
        expect(executeSql(databaseFile, "UPDATE file_analysis SET analysis_version = 0;"));
        expect(cache.getSeratoTag(identity).has_value());

        beginTest("Explicit eviction removes analysis, waveform and all tag kinds");
        expect(cache.storeAnalysis(analyzed));
        expect(cache.storeWaveformData(file, waveform));
        expect(executeSql(databaseFile, R"SQL(
            INSERT INTO serato_tag_cache
            SELECT file_path, 2, file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms
            FROM serato_tag_cache;
        )SQL"));
        expect(cache.removeAnalysis(file));
        expect(!cache.getSeratoTag(identity));
        expect(!cache.getWaveformData(file, loadedWaveform));
        AudioAnalysisRecord loadedAnalysis;
        expect(!cache.getAnalysis(file, loadedAnalysis));
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

        beginTest("Modification time alone invalidates captured identities and rejects stale writes");
        expect(cache.storeSeratoTag(identity, positive));
        expect(file.setLastModificationTime(juce::Time(identity.modifiedTimeMs + 10000)));
        const AnalysisCache::FileIdentity changedTime(file);
        expect(!identity.matches(file));
        expect(!cache.getSeratoTag(identity));
        expect(!cache.getSeratoTag(changedTime));
        expect(!cache.storeSeratoTag(identity, positive));
        expect(cache.storeSeratoTag(changedTime, {.status = ReadStatus::absent, .overview = std::nullopt}));
        expect(!cache.storeSeratoTag(identity, positive));
        expect(cache.getSeratoTag(changedTime).has_value());

        beginTest("Size alone invalidates captured identities even if the time is restored");
        expect(fixtureFile("sine_mono_44100_16.wav").copyFileTo(file));
        expect(file.setLastModificationTime(juce::Time(changedTime.modifiedTimeMs)));
        const AnalysisCache::FileIdentity changedSize(file);
        expect(changedSize.fileSize != changedTime.fileSize);
        expect(!cache.getSeratoTag(changedTime));
        expect(!cache.getSeratoTag(changedSize));
        expect(!cache.storeSeratoTag(changedTime, positive));
        expect(cache.storeSeratoTag(changedSize, positive));
        expect(cache.getSeratoTag(changedSize).has_value());

        beginTest("Missing files and directories cannot be looked up or stored");
        const AnalysisCache::FileIdentity missing(directory.getChildFile("missing.wav"));
        const AnalysisCache::FileIdentity folder(file.getParentDirectory());
        expect(!cache.storeSeratoTag(missing, positive));
        expect(!cache.storeSeratoTag(folder, positive));
        expect(!cache.getSeratoTag(missing));
        expect(!cache.getSeratoTag(folder));
        expect(!changedSize.matches(directory.getChildFile("other.wav")));

        beginTest("Pruning deletes tag-only rows but can stop without deleting them");
        expect(file.deleteFile());
        expect(!cache.getSeratoTag(changedSize));
        expect(!cache.storeSeratoTag(changedSize, positive));
        expectEquals(cache.pruneMissingFiles([] { return true; }), 0);
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 1);
        expectEquals(cache.pruneMissingFiles(), 1);
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

        beginTest("Pruning preserves tag-only rows under a missing parent folder");
        const auto parent = directory.getChildFile("detached");
        expect(parent.createDirectory());
        const auto detachedFile = parent.getChildFile("tone.wav");
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(detachedFile));
        const AnalysisCache::FileIdentity detachedIdentity(detachedFile);
        expect(cache.storeSeratoTag(detachedIdentity, positive));
        expect(parent.deleteRecursively());
        expectEquals(cache.pruneMissingFiles(), 0);
        expect(parent.createDirectory());
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(detachedFile));
        expect(detachedFile.setLastModificationTime(juce::Time(detachedIdentity.modifiedTimeMs)));
        expect(cache.getSeratoTag(detachedIdentity).has_value());
        expect(cache.removeAnalysis(detachedFile));

        beginTest("A tag row replaced after the pruning snapshot retains its new identity");
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(file));
        const AnalysisCache::FileIdentity beforePrune(file);
        expect(cache.storeSeratoTag(beforePrune, positive));
        expect(file.deleteFile());
        expectEquals(
            cache.pruneMissingFiles([&] {
                expect(fixtureFile("sine_stereo_48000_24.wav").copyFileTo(file));
                expect(cache.storeSeratoTag(AnalysisCache::FileIdentity(file), positive));
                expect(file.deleteFile());
                return false;
            }),
            0
        );
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 1);
        expectEquals(cache.pruneMissingFiles(), 1);

        beginTest("Pruning counts analysis and tag rows independently for the same deleted file");
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(file));
        expect(cache.storeAnalysis(AudioAnalysisService::analyzeFile(file)));
        expect(cache.storeSeratoTag(AnalysisCache::FileIdentity(file), positive));
        expect(file.deleteFile());
        expectEquals(cache.pruneMissingFiles(), 2);

        beginTest("Malformed cache schemas and database failures cannot produce tag hits");
        const auto malformedDatabase = directory.getChildFile("malformed.db");
        expect(executeSql(malformedDatabase, "CREATE TABLE serato_tag_cache (file_path TEXT);"));
        AnalysisCache malformed(malformedDatabase);
        expect(!malformed.getSeratoTag(detachedIdentity));
        expect(!malformed.storeSeratoTag(detachedIdentity, positive));
        const auto brokenDatabase = directory.getChildFile("broken.db");
        expect(brokenDatabase.replaceWithText("not an SQLite database"));
        AnalysisCache broken(brokenDatabase);
        expect(!broken.getSeratoTag(detachedIdentity));
        expect(!broken.storeSeratoTag(detachedIdentity, positive));
    }

    /// Mutates an isolated test database to exercise rows that normal cache APIs cannot write.
    bool executeSql(const juce::File& databaseFile, const char* sql)
    {
        sqlite3* database = nullptr;
        const auto opened = sqlite3_open(databaseFile.getFullPathName().toRawUTF8(), &database);
        const auto completed
            = opened == SQLITE_OK && sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
        if (!completed && database != nullptr) {
            logMessage(juce::String::fromUTF8(sqlite3_errmsg(database)));
        }
        sqlite3_close(database);
        return completed;
    }

    /// Checks persisted SQL state without relying on the cache's decoder or identity validation.
    void expectSqlScalar(const juce::File& databaseFile, const char* sql, const int expected)
    {
        sqlite3* database = nullptr;
        const auto opened = sqlite3_open(databaseFile.getFullPathName().toRawUTF8(), &database);
        expectEquals(opened, SQLITE_OK);
        sqlite3_stmt* statement = nullptr;
        if (opened == SQLITE_OK && sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK) {
            expectEquals(sqlite3_step(statement), SQLITE_ROW);
            expectEquals(sqlite3_column_int(statement, 0), expected);
        } else {
            expect(false, "Cannot read test database state");
        }
        sqlite3_finalize(statement);
        sqlite3_close(database);
    }
};

std::unique_ptr<juce::UnitTest> createCacheTests()
{
    return std::make_unique<CacheTests>();
}
}  // namespace audiobatch::tests
