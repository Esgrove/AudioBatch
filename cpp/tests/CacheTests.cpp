/// Tests for AnalysisCache against a temporary SQLite database.
/// Covers the round trip of a stored record and its waveform,
/// invalidation when the size or the modification time of the file changes on disk,
/// the migration of a database written by an older version, the Serato tag rows,
/// and the loudness history, including a cached graph that the coordinator serves.
/// AnalysisCoordinatorTests.cpp covers the plans and the replaced runs of the coordinator.
/// The cached files are copies of the committed fixtures.

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisCoordinator.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/Sqlite.h"
#include "analysis/SqliteTransaction.h"
#include "serato/SeratoOverview.h"
#include "serato/SeratoTagCache.h"
#include "serato/SeratoTags.h"
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <utility>

namespace audiobatch::tests
{
/// Checks that cached analysis is returned for unchanged files and dropped for changed ones.
class CacheTests final : public juce::UnitTest
{
public:
    CacheTests() : juce::UnitTest("Analysis cache", "AudioBatch") {}

    /// Runs the cache checks.
    void runTest() override
    {
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("analysis.db"));

        beginTest("The cache database opens in a custom location");
        expect(cache.open());
        expect(directory.getChildFile("analysis.db").existsAsFile());

        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "tone.wav");
        const auto analyzed = analyzeFile(file);
        expect(!analyzed.hasError(), analyzed.errorMessage);

        beginTest("Numeric Overview data round trips without an analysis row");
        {
            const auto tagFile = directory.copyFixture("sine_stereo_44100_16.wav", "tag_only.wav");
            const utils::FileIdentity identity(tagFile);
            serato::Overview overview;
            overview.slices.resize(2);
            overview.slices.front().fill(7);
            overview.slices.back().fill(19);
            const auto result = serato::TagReadResult::present(overview);
            expect(serato::storeCachedTag(cache, identity, result));
            const auto loaded = serato::getCachedTag(cache, identity);
            expect(loaded.has_value());
            const auto loadedOverview = loaded ? tagValue<serato::Overview>(*loaded) : std::nullopt;
            expect(loadedOverview && loadedOverview->slices == overview.slices);
            expect(!cache.getAnalysis(tagFile).has_value());
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
            expect(migratedCache.storeAnalysis(analyzeFile(migratedFile)));
            expect(migratedCache.storeCustomGain(migratedFile, Decibels {1.5f}));

            const auto cached = migratedCache.getAnalysis(migratedFile);
            expect(cached.has_value() && cached->customGain.has_value());
            expectEquals(cached.value_or(AudioAnalysisRecord {}).customGain.value_or(Decibels {}).value, 1.5f);

            const utils::FileIdentity identity(migratedFile);
            expect(serato::storeCachedTag(migratedCache, identity, {}));
            expect(serato::getCachedTag(migratedCache, identity).has_value());
        }

        beginTest("A transaction rolls back unless it is committed");
        {
            sqlite3* opened = nullptr;
            expectEquals(
                sqlite3_open(directory.getChildFile("transaction.db").getFullPathName().toRawUTF8(), &opened), SQLITE_OK
            );
            const sqlite::Database connection(opened);
            expect(sqlite::execute(*connection, "CREATE TABLE numbers (value INTEGER);"));

            const auto countRows = [&connection] {
                const auto statement = sqlite::prepare(*connection, "SELECT COUNT(*) FROM numbers;");
                return statement != nullptr && sqlite3_step(statement.get()) == SQLITE_ROW
                    ? sqlite3_column_int(statement.get(), 0)
                    : -1;
            };

            {
                const sqlite::Transaction transaction(*connection);
                expect(transaction.isActive());
                expect(sqlite::execute(*connection, "INSERT INTO numbers VALUES (1);"));
            }
            expectEquals(countRows(), 0);

            {
                sqlite::Transaction transaction(*connection);
                expect(sqlite::execute(*connection, "INSERT INTO numbers VALUES (2);"));
                expect(transaction.commit());
                expect(!transaction.isActive());
            }
            expectEquals(countRows(), 1);
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

            const auto cached = cache.getAnalysis(file).value_or(AudioAnalysisRecord {});
            expectEquals(cached.overallPeak, analyzed.overallPeak);
            expectEquals(cached.sampleRate, analyzed.sampleRate);
        }

        beginTest("A changed file is not served from the cache");
        {
            expect(fixtureFile("sine_stereo_48000_24.wav").copyFileTo(file));
            expect(!cache.getAnalysis(file).has_value());
        }

        beginTest("A custom gain is kept while the file is unchanged");
        {
            const auto gainFile = directory.copyFixture("sine_stereo_44100_16.wav", "gain.wav");
            expect(cache.storeAnalysis(analyzeFile(gainFile)));
            expect(cache.storeCustomGain(gainFile, Decibels {-3.5f}));

            // Analyzing the unchanged file again must not drop the gain.
            expect(cache.storeAnalysis(analyzeFile(gainFile)));

            const auto cached = cache.getAnalysis(gainFile);
            expect(cached.has_value() && cached->customGain.has_value());
            expectEquals(cached.value_or(AudioAnalysisRecord {}).customGain.value_or(Decibels {}).value, -3.5f);
        }

        beginTest("A custom gain for a file without a cached row reports that nothing was stored");
        {
            const auto waitingFile = directory.copyFixture("sine_stereo_44100_16.wav", "waiting.wav");
            expect(!cache.storeCustomGain(waitingFile, Decibels {1.0f}));
        }

        beginTest("Custom gains of several files are stored together, skipping files without a cached row");
        {
            const auto first = directory.copyFixture("sine_stereo_44100_16.wav", "batch_first.wav");
            const auto second = directory.copyFixture("sine_stereo_44100_16.wav", "batch_second.wav");
            const auto uncached = directory.copyFixture("sine_stereo_44100_16.wav", "batch_uncached.wav");
            expect(cache.storeAnalysis(analyzeFile(first)));
            expect(cache.storeAnalysis(analyzeFile(second)));

            expectEquals(cache.storeCustomGains({}), 0);
            expectEquals(
                cache.storeCustomGains({
                    {.file = first, .customGain = Decibels {1.5f}},
                    {.file = uncached, .customGain = Decibels {2.0f}},
                    {.file = second, .customGain = std::nullopt},
                }),
                2
            );

            const auto firstCached = cache.getAnalysis(first);
            expect(firstCached.has_value() && firstCached->customGain.has_value());
            expectEquals(firstCached.value_or(AudioAnalysisRecord {}).customGain.value_or(Decibels {}).value, 1.5f);
            const auto secondCached = cache.getAnalysis(second);
            expect(secondCached.has_value() && !secondCached->customGain);
        }

        beginTest("A custom gain is cleared when the file changes");
        {
            const auto gainFile = directory.getChildFile("gain.wav");
            expect(fixtureFile("sine_mono_44100_16.wav").copyFileTo(gainFile));
            expect(cache.storeAnalysis(analyzeFile(gainFile)));

            const auto cached = cache.getAnalysis(gainFile);
            expect(cached.has_value() && !cached->customGain);
        }

        beginTest("Pruning removes rows of deleted files only");
        {
            const auto keptFile = directory.copyFixture("sine_stereo_44100_16.wav", "kept.wav");
            const auto deletedFile = directory.copyFixture("sine_stereo_44100_16.wav", "deleted.wav");
            expect(cache.storeAnalysis(analyzeFile(keptFile)));
            expect(cache.storeAnalysis(analyzeFile(deletedFile)));
            expect(deletedFile.deleteFile());

            expectEquals(cache.pruneMissingFiles(), 1);
            expect(cache.getAnalysis(keptFile).has_value());
        }

        beginTest("Pruning keeps rows of files whose folder is missing, such as an unmounted drive");
        {
            const auto folder = directory.getChildFile("unmounted");
            expect(folder.createDirectory());
            const auto unmountedFile = folder.getChildFile("tone.wav");
            expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(unmountedFile));
            expect(cache.storeAnalysis(analyzeFile(unmountedFile)));
            expect(cache.storeCustomGain(unmountedFile, Decibels {1.5f}));
            expect(folder.deleteRecursively());

            expectEquals(cache.pruneMissingFiles(), 0);
        }

        beginTest("Waveform data round trips and is dropped when the analysis is stored again");
        {
            const auto waveformFile = directory.copyFixture("sine_stereo_44100_16.wav", "waveform.wav");
            expect(cache.storeAnalysis(analyzeFile(waveformFile)));

            const juce::String waveformText("waveform bytes");
            const juce::MemoryBlock waveform(waveformText.toRawUTF8(), waveformText.getNumBytesAsUTF8());
            expect(cache.storeWaveformData(waveformFile, waveform));

            const auto loaded = cache.getWaveformData(waveformFile);
            expect(loaded.has_value() && *loaded == waveform);

            expect(cache.storeAnalysis(analyzeFile(waveformFile)));
            expect(!cache.getWaveformData(waveformFile).has_value());
        }

        beginTest("A cached record is dropped when only the modification time of the file changes");
        {
            const auto touchedFile = directory.copyFixture("sine_stereo_44100_16.wav", "touched.wav");
            const auto record = analyzeFile(touchedFile);
            expect(cache.storeAnalysis(record));
            expect(cache.storeCustomGain(touchedFile, Decibels {1.0f}));
            expect(cache.getAnalysis(touchedFile).has_value());

            const auto sizeBefore = touchedFile.getSize();
            expect(touchedFile.setLastModificationTime(juce::Time(record.modifiedTimeMs + 10'000)));
            expectEquals(touchedFile.getSize(), sizeBefore);
            expect(!cache.getAnalysis(touchedFile).has_value());
            expect(!cache.getCustomGain(touchedFile, record.fileSize, record.modifiedTimeMs + 10'000).has_value());

            // Restoring the original time makes the stored row match the file again.
            expect(touchedFile.setLastModificationTime(juce::Time(record.modifiedTimeMs)));
            const auto restored = cache.getAnalysis(touchedFile);
            expect(restored.has_value());
            expect(restored.has_value() && restored->customGain.has_value());
        }

        beginTest("A removed record is no longer returned");
        {
            const auto refreshed = analyzeFile(file);
            expect(cache.storeAnalysis(refreshed));
            expect(cache.removeAnalysis(file));
            expect(!cache.getAnalysis(file).has_value());
        }

        runSeratoTests();
        runMultiKindSeratoTests();
        runLoudnessHistoryTests();
    }

private:
    /// Exercises persistent history, automatic legacy regeneration, and corrupt payload recovery without GUI work.
    void runLoudnessHistoryTests()
    {
        const TemporaryDirectory directory;
        const auto databaseFile = directory.getChildFile("loudness.db");
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav");
        const auto record = analyzeFile(file);
        {
            AnalysisCache writer(databaseFile);
            expect(writer.storeAnalysis(record));
            expect(writer.storeCustomGain(file, Decibels {-2.5f}));
        }
        AnalysisCache cache(databaseFile);
        AudioAnalysisRecord cached;
        // Loads the cached record of a file into `cached`, or a default record when there is none.
        const auto loadCached = [&cache, &cached](const juce::File& source) {
            const auto loaded = cache.getAnalysis(source);
            cached = loaded.value_or(AudioAnalysisRecord {});
            return loaded.has_value();
        };
        beginTest("Short-term loudness survives reopening the database with identical timing and readings");
        expect(loadCached(file));
        expect(cached.shortTermLoudness != nullptr);
        if (cached.shortTermLoudness && record.shortTermLoudness) {
            expect(cached.shortTermLoudness->values == record.shortTermLoudness->values);
            expectEquals(cached.shortTermLoudness->samplesPerPoint, record.shortTermLoudness->samplesPerPoint);
            expectWithinAbsoluteError(cached.shortTermLoudness->timeAt(9), 1.0, 0.000001);
        }

        beginTest("A cached loudness graph is delivered without starting an analysis job");
        std::atomic<int> started {0};
        AnalysisCoordinator coordinator(cache, 1);
        coordinator.setStartingCallback([&started](const juce::File&, int) { ++started; });
        AudioAnalysisOptions options;
        options.inputPaths.add(file);
        const auto cachedResults = coordinator.analyzeBlocking(options);
        expectEquals(started.load(), 0);
        expectEquals(static_cast<int>(cachedResults.size()), 1);
        expect(
            !cachedResults.empty() && cachedResults.front().fromCache
            && cachedResults.front().shortTermLoudness != nullptr
        );

        beginTest("Version-eight scalar analysis regenerates history once and preserves custom gain");
        expect(executeSql(databaseFile, "UPDATE file_analysis SET analysis_version = 8, short_term_loudness = NULL;"));
        expect(!loadCached(file));
        const auto regenerated = coordinator.analyzeBlocking(options);
        expectEquals(started.load(), 1);
        expectEquals(static_cast<int>(regenerated.size()), 1);
        expect(!regenerated.empty() && regenerated.front().shortTermLoudness && !regenerated.front().fromCache);
        expect(loadCached(file));
        expect(cached.customGain.has_value());
        expectEquals(cached.customGain.value_or(Decibels {}).value, -2.5f);

        beginTest("Invalid headers, counts, timestamps and readings become cache misses");
        for (const auto* mutation :
             {
                 "short_term_loudness = NULL",
                 "short_term_loudness = X'4C554653'",
                 "short_term_loudness = zeroblob(72)",
                 "short_term_loudness = CAST(short_term_loudness AS TEXT)",
                 "short_term_loudness = substr(short_term_loudness, 1, length(short_term_loudness) - 1)",
                 "short_term_loudness = CAST(short_term_loudness || X'00' AS BLOB)",
                 "short_term_loudness = CAST(substr(short_term_loudness,1,4) || X'02000000' || "
                 "substr(short_term_loudness,9) AS BLOB)",
                 "short_term_loudness = CAST(substr(short_term_loudness,1,8) || zeroblob(8) || "
                 "substr(short_term_loudness,17) AS BLOB)",
                 "short_term_loudness = CAST(substr(short_term_loudness,1,28) || X'FFFFFFFF' || "
                 "substr(short_term_loudness,33) AS BLOB)",
                 "short_term_loudness = CAST(substr(short_term_loudness,1,28) || X'FFFFFF7F' || "
                 "substr(short_term_loudness,33) AS BLOB)",
                 "short_term_loudness = CAST(substr(short_term_loudness,1,32) || X'0000C07F' || "
                 "substr(short_term_loudness,37) AS BLOB)",
                 "short_term_loudness = CAST(substr(short_term_loudness,1,32) || X'0000807F' || "
                 "substr(short_term_loudness,37) AS BLOB)",
                 "short_term_loudness = CAST(substr(short_term_loudness,1,32) || X'0000FAC4' || "
                 "substr(short_term_loudness,37) AS BLOB)",
                 "short_term_loudness = CAST(substr(short_term_loudness,1,32) || X'FFFF7F7F' || "
                 "substr(short_term_loudness,37) AS BLOB)",
                 "length_in_samples = length_in_samples + 1",
                 "duration_seconds = duration_seconds + 1",
                 // Status values outside AudioAnalysisStatus.
                 "status = 4",
                 "status = -1",
             })
        {
            expect(cache.storeAnalysis(record));
            const auto sql = utils::format("UPDATE file_analysis SET {};", mutation);
            expect(executeSql(databaseFile, sql.toRawUTF8()));
            expect(!loadCached(file), mutation);
        }

        beginTest("Silent history round trips and failed analysis needs no history");
        const auto silent = directory.copyFixture("silence_stereo_44100_16.wav");
        expect(cache.storeAnalysis(analyzeFile(silent)));
        expect(loadCached(silent));
        expect(cached.shortTermLoudness != nullptr);
        if (cached.shortTermLoudness && !cached.shortTermLoudness->values.empty()) {
            expectEquals(
                cached.shortTermLoudness->values.front(),
                static_cast<float>(AudioAnalysisRecord::negativeInfinityLoudness)
            );
        }
        const auto broken = directory.copyFixture("broken.wav");
        expect(cache.storeAnalysis(analyzeFile(broken)));
        expect(loadCached(broken));
        expect(cached.hasError() && !cached.shortTermLoudness);
        expect(cached.error == AnalysisError::unsupported);

        beginTest("A failure that may not repeat on the same bytes leaves the cache as it was");
        const auto cancelledFile = directory.copyFixture("sine_stereo_44100_16.wav", "cancelled.wav");
        const auto cancelled = analyzeFile(cancelledFile, [] { return true; });
        expect(cancelled.error == AnalysisError::cancelled);
        expect(cache.storeAnalysis(cancelled));
        expect(!loadCached(cancelledFile));
        expect(cache.storeAnalysis(analyzeFile(cancelledFile)));
        expect(cache.storeAnalysis(cancelled));
        expect(loadCached(cancelledFile));
        expect(!cached.hasError());
    }

    /// Exercises tag-only lifecycle, stable outcomes, corruption and independence using fixture copies.
    void runSeratoTests()
    {
        const TemporaryDirectory directory;
        const auto databaseFile = directory.getChildFile("tag_cache.db");
        AnalysisCache cache(databaseFile);
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "tag.wav");
        const utils::FileIdentity identity(file);
        serato::Overview overview;
        overview.slices.resize(3);
        overview.slices.front().fill(3);
        overview.slices.back().fill(255);
        const auto positive = serato::TagReadResult::present(overview);

        beginTest("Tag results survive a database reopen without an analysis row");
        {
            AnalysisCache writer(databaseFile);
            expect(serato::storeCachedTag(writer, identity, positive));
        }
        const auto reopened = serato::getCachedTag(cache, identity);
        expect(
            reopened && reopened->status() == serato::ReadStatus::present
            && tagValue<serato::Overview>(*reopened).value_or(serato::Overview {}).slices == overview.slices
        );

        beginTest("All stable negatives round trip with SQL NULL data");
        for (const auto status :
             {serato::ReadStatus::absent, serato::ReadStatus::invalid, serato::ReadStatus::unsupported})
        {
            expect(
                serato::storeCachedTag(
                    cache, identity, serato::TagReadResult::missing(serato::TagKind::overview, status)
                )
            );
            const auto loaded = serato::getCachedTag(cache, identity);
            expect(loaded && loaded->status() == status && !loaded->value);
            expectSqlScalar(databaseFile, "SELECT decoded_data IS NULL FROM serato_tag_cache;", 1);
        }

        beginTest("Transient and inconsistent results never overwrite a stable result");
        expect(serato::storeCachedTag(cache, identity, positive));
        for (const auto status :
             {
                 serato::ReadStatus::ioError,
                 serato::ReadStatus::cancelled,
                 std::bit_cast<serato::ReadStatus>(std::uint8_t {99}),
             })
        {
            expect(!serato::storeCachedTag(
                cache, identity, serato::TagReadResult::missing(serato::TagKind::overview, status)
            ));
        }
        expect(!serato::storeCachedTag(cache, identity, serato::TagReadResult::present(serato::Overview {})));
        expect(!serato::storeCachedTag(
            cache, identity, serato::TagReadResult {.kind = serato::TagKind::analysis, .value = overview}
        ));
        serato::Overview oversized;
        oversized.slices.resize(serato::Overview::maxSlices + 1);
        expect(!serato::storeCachedTag(cache, identity, serato::TagReadResult::present(std::move(oversized))));
        const auto retained = serato::getCachedTag(cache, identity);
        expect(
            retained && tagValue<serato::Overview>(*retained).value_or(serato::Overview {}).slices == overview.slices
        );
        expect(cache.removeAnalysis(file));
        expect(!serato::storeCachedTag(
            cache, identity, serato::TagReadResult::missing(serato::TagKind::overview, serato::ReadStatus::ioError)
        ));
        expect(!serato::storeCachedTag(
            cache, identity, serato::TagReadResult::missing(serato::TagKind::overview, serato::ReadStatus::cancelled)
        ));
        expect(!serato::getCachedTag(cache, identity));
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

        beginTest("Several kinds are looked up together in their order, and unknown kinds among them miss");
        {
            expect(serato::storeCachedTag(cache, identity, positive));
            expect(
                serato::storeCachedTag(
                    cache,
                    identity,
                    serato::TagReadResult::missing(serato::TagKind::markers, serato::ReadStatus::absent)
                )
            );
            const std::array kinds {
                serato::TagKind::markers,
                std::bit_cast<serato::TagKind>(std::uint8_t {99}),
                serato::TagKind::beatGrid,
                serato::TagKind::overview,
            };
            const auto results = serato::getCachedTags(cache, identity, kinds);
            expectEquals(static_cast<int>(results.size()), 4);
            if (results.size() == 4) {
                const auto& markers = results[0];
                const auto& overviewResult = results[3];
                expect(markers.has_value() && markers->status() == serato::ReadStatus::absent);
                expect(!results[1]);
                expect(!results[2]);
                expect(overviewResult.has_value() && overviewResult->status() == serato::ReadStatus::present);
            }
            expect(cache.removeAnalysis(file));
        }

        beginTest("Unknown tag kinds miss and cannot be stored");
        const auto unknownKind = std::bit_cast<serato::TagKind>(std::uint8_t {99});
        expect(!serato::getCachedTag(cache, identity, unknownKind));
        expect(!serato::storeCachedTag(
            cache, identity, serato::TagReadResult::missing(unknownKind, serato::ReadStatus::absent)
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
            expect(serato::storeCachedTag(cache, identity, positive));
            expect(executeSql(databaseFile, sql));
            expect(!serato::getCachedTag(cache, identity), sql);
        }
        expect(executeSql(databaseFile, "DELETE FROM serato_tag_cache;"));

        beginTest("Analysis and waveform writes leave independently versioned tags untouched");
        expect(serato::storeCachedTag(cache, identity, positive));
        const auto analyzed = analyzeFile(file);
        expect(cache.storeAnalysis(analyzed));
        const juce::MemoryBlock waveform("cached peaks", 12);
        expect(cache.storeWaveformData(file, waveform));
        expectSqlScalar(databaseFile, "SELECT analysis_version FROM file_analysis;", 12);
        expectSqlScalar(databaseFile, "SELECT waveform_version FROM file_analysis;", 2);
        expect(serato::getCachedTag(cache, identity).has_value());
        const auto loadedWaveform = cache.getWaveformData(file);
        expect(loadedWaveform.has_value() && *loadedWaveform == waveform);
        expect(cache.storeAnalysis(analyzed));
        expect(!cache.getWaveformData(file).has_value());
        expect(serato::getCachedTag(cache, identity).has_value());
        expect(executeSql(databaseFile, "UPDATE file_analysis SET analysis_version = 0;"));
        expect(serato::getCachedTag(cache, identity).has_value());

        beginTest("Explicit eviction removes analysis, waveform and all tag kinds");
        expect(cache.storeAnalysis(analyzed));
        expect(cache.storeWaveformData(file, waveform));
        expect(executeSql(databaseFile, R"SQL(
            INSERT INTO serato_tag_cache
            SELECT file_path, 2, file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms
            FROM serato_tag_cache;
        )SQL"));
        expect(cache.removeAnalysis(file));
        expect(!serato::getCachedTag(cache, identity));
        expect(!cache.getWaveformData(file).has_value());
        expect(!cache.getAnalysis(file).has_value());
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

        beginTest("Modification time alone invalidates captured identities and rejects stale writes");
        expect(serato::storeCachedTag(cache, identity, positive));
        expect(file.setLastModificationTime(juce::Time(identity.modifiedTimeMs + 10'000)));
        const utils::FileIdentity changedTime(file);
        expect(!identity.matches(file));
        expect(!serato::getCachedTag(cache, identity));
        expect(!serato::getCachedTag(cache, changedTime));
        expect(!serato::storeCachedTag(cache, identity, positive));
        expect(
            serato::storeCachedTag(
                cache,
                changedTime,
                serato::TagReadResult::missing(serato::TagKind::overview, serato::ReadStatus::absent)
            )
        );
        expect(!serato::storeCachedTag(cache, identity, positive));
        expect(serato::getCachedTag(cache, changedTime).has_value());

        beginTest("Size alone invalidates captured identities even if the time is restored");
        expect(fixtureFile("sine_mono_44100_16.wav").copyFileTo(file));
        expect(file.setLastModificationTime(juce::Time(changedTime.modifiedTimeMs)));
        const utils::FileIdentity changedSize(file);
        expect(changedSize.fileSize != changedTime.fileSize);
        expect(!serato::getCachedTag(cache, changedTime));
        expect(!serato::getCachedTag(cache, changedSize));
        expect(!serato::storeCachedTag(cache, changedTime, positive));
        expect(serato::storeCachedTag(cache, changedSize, positive));
        expect(serato::getCachedTag(cache, changedSize).has_value());

        beginTest("Missing files and directories cannot be looked up or stored");
        const utils::FileIdentity missing(directory.getChildFile("missing.wav"));
        const utils::FileIdentity folder(file.getParentDirectory());
        expect(!serato::storeCachedTag(cache, missing, positive));
        expect(!serato::storeCachedTag(cache, folder, positive));
        expect(!serato::getCachedTag(cache, missing));
        expect(!serato::getCachedTag(cache, folder));
        expect(!changedSize.matches(directory.getChildFile("other.wav")));

        beginTest("Pruning deletes tag-only rows but can stop without deleting them");
        expect(file.deleteFile());
        expect(!serato::getCachedTag(cache, changedSize));
        expect(!serato::storeCachedTag(cache, changedSize, positive));
        expectEquals(cache.pruneMissingFiles([] { return true; }), 0);
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 1);
        expectEquals(cache.pruneMissingFiles(), 1);
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

        beginTest("Pruning preserves tag-only rows under a missing parent folder");
        const auto parent = directory.getChildFile("detached");
        expect(parent.createDirectory());
        const auto detachedFile = parent.getChildFile("tone.wav");
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(detachedFile));
        const utils::FileIdentity detachedIdentity(detachedFile);
        expect(serato::storeCachedTag(cache, detachedIdentity, positive));
        expect(parent.deleteRecursively());
        expectEquals(cache.pruneMissingFiles(), 0);
        expect(parent.createDirectory());
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(detachedFile));
        expect(detachedFile.setLastModificationTime(juce::Time(detachedIdentity.modifiedTimeMs)));
        expect(serato::getCachedTag(cache, detachedIdentity).has_value());
        expect(cache.removeAnalysis(detachedFile));

        beginTest("A tag row replaced after the pruning snapshot retains its new identity");
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(file));
        const utils::FileIdentity beforePrune(file);
        expect(serato::storeCachedTag(cache, beforePrune, positive));
        expect(file.deleteFile());
        expectEquals(
            cache.pruneMissingFiles([&] {
                expect(fixtureFile("sine_stereo_48000_24.wav").copyFileTo(file));
                expect(serato::storeCachedTag(cache, utils::FileIdentity(file), positive));
                expect(file.deleteFile());
                return false;
            }),
            0
        );
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 1);
        expectEquals(cache.pruneMissingFiles(), 1);

        beginTest("Pruning counts analysis and tag rows independently for the same deleted file");
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(file));
        expect(cache.storeAnalysis(analyzeFile(file)));
        expect(serato::storeCachedTag(cache, utils::FileIdentity(file), positive));
        expect(file.deleteFile());
        expectEquals(cache.pruneMissingFiles(), 2);

        beginTest("Malformed cache schemas and database failures cannot produce tag hits");
        const auto malformedDatabase = directory.getChildFile("malformed.db");
        expect(executeSql(malformedDatabase, "CREATE TABLE serato_tag_cache (file_path TEXT);"));
        AnalysisCache malformed(malformedDatabase);
        expect(!serato::getCachedTag(malformed, detachedIdentity));
        expect(!serato::storeCachedTag(malformed, detachedIdentity, positive));
        const auto brokenDatabase = directory.getChildFile("broken.db");
        expect(brokenDatabase.replaceWithText("not an SQLite database"));
        AnalysisCache broken(brokenDatabase);
        expect(!serato::getCachedTag(broken, detachedIdentity));
        expect(!serato::storeCachedTag(broken, detachedIdentity, positive));
    }

    /// Verifies independently versioned owned values, negatives and tag-only coexistence for every supported kind.
    void runMultiKindSeratoTests()
    {
        const TemporaryDirectory directory;
        const auto databaseFile = directory.getChildFile("multi_kind.db");
        AnalysisCache cache(databaseFile);
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "multi_kind.wav");
        const utils::FileIdentity identity(file);
        serato::Overview overview;
        overview.slices.resize(2);
        overview.slices.front().fill(7);
        overview.slices.back().fill(19);
        const std::array positives {
            serato::TagReadResult::present(overview),
            serato::TagReadResult::present(serato::Analysis {.majorVersion = 2, .minorVersion = 5}),
            serato::TagReadResult::present(serato::Autotags {.bpm = 128.25, .autoGain = -3.5, .gain = 1.25}),
            serato::TagReadResult::present(serato::BeatGrid {
                .markers = {
                    {.positionSeconds = 0.5, .beatsUntilNext = 16, .bpm = std::nullopt},
                    {.positionSeconds = 8.0, .beatsUntilNext = 0, .bpm = 128.25},
                },
            }),
            serato::TagReadResult::present(serato::Markers {
                .cues = {
                    {.positionMilliseconds = 1500, .index = 3, .colour = juce::Colours::red, .name = "Intro"},
                },
                .loops = {
                    {
                        .startMilliseconds = 2000,
                        .endMilliseconds = 4000,
                        .index = 1,
                        .colour = juce::Colours::green,
                        .locked = true,
                        .name = "Loop",
                    },
                },
                .colour = juce::Colours::blue,
                .bpmLocked = false,
            }),
            serato::TagReadResult::present(makeTrackInfo(128.25, "Am")),
        };

        runMultiKindRoundTripTests(cache, databaseFile, file, identity, positives);
        runMultiKindNegativeTests(cache, databaseFile, identity, positives);
        runMultiKindVersionTests(cache, databaseFile, identity, positives);
        runMultiKindRetentionTests(cache, databaseFile, identity, positives);
        runMultiKindCoexistenceTests(cache, databaseFile, file, identity, positives);
    }

    /// Reopens positive tag-only rows and checks owned values and row counts for every supported kind.
    void runMultiKindRoundTripTests(
        AnalysisCache& cache,
        const juce::File& databaseFile,
        const juce::File& file,
        const utils::FileIdentity& identity,
        const std::span<const serato::TagReadResult> positives
    )
    {
        beginTest("All positive kinds round trip independently after reopening without analysis rows");
        {
            AnalysisCache writer(databaseFile);
            for (const auto& positive : positives) {
                expect(serato::storeCachedTag(writer, identity, positive));
            }
        }
        for (const auto& positive : positives) {
            const auto loaded = serato::getCachedTag(cache, identity, positive.kind);
            expect(loaded && loaded->kind == positive.kind && loaded->status() == serato::ReadStatus::present);
            if (loaded) {
                expect(serato::serializeTag(*loaded) == serato::serializeTag(positive));
                expect(!serato::serializeTag(*loaded).isEmpty());
            }
        }
        expect(!cache.getAnalysis(file).has_value());
        expectSqlScalar(databaseFile, "SELECT count(*) FROM file_analysis;", 0);
        expectSqlScalar(
            databaseFile, "SELECT count(*) FROM serato_tag_cache;", static_cast<int>(serato::allTagKinds.size())
        );
    }

    /// Replaces each kind with stable negatives while preserving the other positive rows and SQL NULL payloads.
    void runMultiKindNegativeTests(
        AnalysisCache& cache,
        const juce::File& databaseFile,
        const utils::FileIdentity& identity,
        const std::span<const serato::TagReadResult> positives
    )
    {
        beginTest("Each stable negative replaces only its own kind and has SQL NULL data");
        for (const auto kind : serato::allTagKinds) {
            for (const auto status :
                 {serato::ReadStatus::absent, serato::ReadStatus::invalid, serato::ReadStatus::unsupported})
            {
                expect(serato::storeCachedTag(cache, identity, serato::TagReadResult::missing(kind, status)));
                const auto negative = serato::getCachedTag(cache, identity, kind);
                expect(negative && negative->kind == kind && negative->status() == status);
                expect(negative && !negative->value);
                const auto sql = utils::format(
                    "SELECT decoded_data IS NULL FROM serato_tag_cache WHERE tag_kind = {};",
                    static_cast<std::uint32_t>(kind)
                );
                expectSqlScalar(databaseFile, sql.toRawUTF8(), 1);
                for (const auto& positive : positives) {
                    if (positive.kind != kind) {
                        const auto retained = serato::getCachedTag(cache, identity, positive.kind);
                        expect(retained && serato::serializeTag(*retained) == serato::serializeTag(positive));
                    } else {
                        expect(serato::storeCachedTag(cache, identity, positive));
                    }
                }
            }
        }
    }

    /// Invalidates positive and negative rows independently when one kind's parser version changes.
    void runMultiKindVersionTests(
        AnalysisCache& cache,
        const juce::File& databaseFile,
        const utils::FileIdentity& identity,
        const std::span<const serato::TagReadResult> positives
    )
    {
        beginTest("A parser version mismatch invalidates only the matching positive or negative kind");
        for (const auto kind : serato::allTagKinds) {
            for (const auto status : {serato::ReadStatus::present, serato::ReadStatus::absent}) {
                if (status == serato::ReadStatus::absent) {
                    expect(serato::storeCachedTag(cache, identity, serato::TagReadResult::missing(kind, status)));
                }
                const auto mismatch = utils::format(
                    "UPDATE serato_tag_cache SET parser_version = {} WHERE tag_kind = {};",
                    serato::parserVersion(kind) + 1,
                    static_cast<std::uint32_t>(kind)
                );
                expect(executeSql(databaseFile, mismatch.toRawUTF8()));
                expect(!serato::getCachedTag(cache, identity, kind));
                for (const auto& positive : positives) {
                    if (positive.kind == kind) {
                        expect(serato::storeCachedTag(cache, identity, positive));
                    } else {
                        const auto retained = serato::getCachedTag(cache, identity, positive.kind);
                        expect(retained && serato::serializeTag(*retained) == serato::serializeTag(positive));
                    }
                }
            }
        }
    }

    /// Ensures corruption stays local to one kind and transient results never overwrite stable positive values.
    void runMultiKindRetentionTests(
        AnalysisCache& cache,
        const juce::File& databaseFile,
        const utils::FileIdentity& identity,
        const std::span<const serato::TagReadResult> positives
    )
    {
        beginTest("Corrupt marker data does not lose overview or any other kind");
        expect(executeSql(databaseFile, "UPDATE serato_tag_cache SET decoded_data = X'0105' WHERE tag_kind = 5;"));
        expect(!serato::getCachedTag(cache, identity, serato::TagKind::markers));
        for (const auto& positive : positives) {
            if (positive.kind != serato::TagKind::markers) {
                const auto retained = serato::getCachedTag(cache, identity, positive.kind);
                expect(retained && serato::serializeTag(*retained) == serato::serializeTag(positive));
            } else {
                expect(serato::storeCachedTag(cache, identity, positive));
            }
        }

        beginTest("Transient results cannot overwrite any independently cached positive kind");
        for (const auto& positive : positives) {
            for (const auto status : {serato::ReadStatus::ioError, serato::ReadStatus::cancelled}) {
                expect(!serato::storeCachedTag(cache, identity, serato::TagReadResult::missing(positive.kind, status)));
                const auto retained = serato::getCachedTag(cache, identity, positive.kind);
                expect(retained && serato::serializeTag(*retained) == serato::serializeTag(positive));
            }
        }
    }

    /// Checks shared analysis and waveform lifecycles, explicit eviction and cancellable tag-only pruning.
    void runMultiKindCoexistenceTests(
        AnalysisCache& cache,
        const juce::File& databaseFile,
        const juce::File& file,
        const utils::FileIdentity& identity,
        const std::span<const serato::TagReadResult> positives
    )
    {
        beginTest("All kinds coexist with analysis and waveform writes and independent version changes");
        const auto analyzed = analyzeFile(file);
        expect(cache.storeAnalysis(analyzed));
        const juce::MemoryBlock waveform("cached peaks", 12);
        expect(cache.storeWaveformData(file, waveform));
        for (const auto& positive : positives) {
            expect(serato::storeCachedTag(cache, identity, positive));
        }
        const auto loadedWaveform = cache.getWaveformData(file);
        expect(loadedWaveform.has_value() && *loadedWaveform == waveform);
        expect(cache.getAnalysis(file).has_value());
        expect(cache.storeAnalysis(analyzed));
        expect(!cache.getWaveformData(file).has_value());
        expect(executeSql(databaseFile, "UPDATE file_analysis SET analysis_version = 0;"));
        for (const auto& positive : positives) {
            const auto retained = serato::getCachedTag(cache, identity, positive.kind);
            expect(retained && serato::serializeTag(*retained) == serato::serializeTag(positive));
        }

        beginTest("Explicit eviction removes every independently populated kind");
        expect(cache.removeAnalysis(file));
        for (const auto kind : serato::allTagKinds) {
            expect(!serato::getCachedTag(cache, identity, kind));
        }
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

        beginTest("Pruning counts all tag-only kinds and cancellation preserves every row");
        for (const auto& positive : positives) {
            expect(serato::storeCachedTag(cache, identity, positive));
        }
        expect(file.deleteFile());
        expectEquals(cache.pruneMissingFiles([] { return true; }), 0);
        expectSqlScalar(
            databaseFile, "SELECT count(*) FROM serato_tag_cache;", static_cast<int>(serato::allTagKinds.size())
        );
        expectEquals(cache.pruneMissingFiles(), static_cast<int>(serato::allTagKinds.size()));
        expectSqlScalar(databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);
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
