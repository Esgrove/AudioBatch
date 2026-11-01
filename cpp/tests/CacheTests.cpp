/// Tests for AnalysisCache against a temporary SQLite database.
/// Covers the round trip of a stored record and its waveform,
/// invalidation when the size or the modification time of the file changes on disk,
/// the migration of a database written by an older version,
/// and the loudness history, including a cached graph that the coordinator serves.
/// SeratoTagCacheTests.cpp covers the Serato tag rows.
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
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

        beginTest("An explicit clear remains persisted after a stale analysis is stored");
        {
            const auto clearFile = directory.copyFixture("sine_mono_44100_16.wav", "cleared_gain.wav");
            auto stale = analyzeFile(clearFile);
            stale.customGain = Decibels {-3.0f};
            expect(cache.storeAnalysis(stale));
            expect(cache.storeCustomGain(stale.file, std::nullopt));
            expect(cache.storeAnalysis(stale));
            const auto cached = cache.getAnalysis(stale.file);
            expect(cached.has_value());
            expect(cached && !cached->customGain);
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

            const auto waveform = validWaveform();
            expect(cache.storeWaveformData(utils::FileIdentity(waveformFile), waveform));

            const auto loaded = cache.getWaveformData(waveformFile);
            expect(loaded.has_value() && *loaded == waveform);

            expect(cache.storeAnalysis(analyzeFile(waveformFile)));
            expect(!cache.getWaveformData(waveformFile).has_value());
        }

        beginTest("Malformed waveform headers and geometry are rejected before allocation");
        {
            const auto valid = validWaveform();
            expect(isValidWaveformData(valid));
            juce::AudioFormatManager formats;
            juce::AudioThumbnailCache thumbnails(1);
            juce::AudioThumbnail thumbnail(16, formats, thumbnails);
            juce::MemoryInputStream roundTrip(valid, false);
            expect(thumbnail.loadFrom(roundTrip));
            for (std::size_t length = 0; length < valid.getSize(); ++length) {
                const juce::MemoryBlock truncated(valid.getData(), length);
                expect(!isValidWaveformData(truncated));
                juce::MemoryInputStream input(truncated, false);
                expect(!thumbnail.loadFrom(input));
            }
            for (const auto& [offset, value] :
                 std::array<std::pair<juce::int64, int>, 7> {
                     {{4, 0}, {4, -1}, {24, 0x7fffffff}, {24, -1}, {28, 3}, {28, 0x7fffffff}, {32, 0}},
                 })
            {
                auto corrupt = valid;
                juce::MemoryOutputStream output(corrupt, true);
                expect(output.setPosition(offset));
                output.writeInt(value);
                expect(!isValidWaveformData(corrupt));
                juce::MemoryInputStream input(corrupt, false);
                if (value != 3) {
                    expect(!thumbnail.loadFrom(input));
                }
            }
            for (const auto& [offset, value] :
                 std::array<std::pair<juce::int64, juce::int64>, 3> {{{8, 16}, {16, 33}, {16, -1}}})
            {
                auto corrupt = valid;
                juce::MemoryOutputStream output(corrupt, true);
                expect(output.setPosition(offset));
                output.writeInt64(value);
                expect(!isValidWaveformData(corrupt));
                juce::MemoryInputStream input(corrupt, false);
                expect(!thumbnail.loadFrom(input));
            }
            auto incomplete = valid;
            juce::MemoryOutputStream output(incomplete, true);
            expect(output.setPosition(16));
            output.writeInt64(0);
            expect(!isValidWaveformData(incomplete));
            auto trailingBytes = valid;
            trailingBytes.append("extra", 5);
            expect(!isValidWaveformData(trailingBytes));

            thumbnail.clear();
            juce::MemoryBlock empty;
            juce::MemoryOutputStream emptyOutput(empty, false);
            thumbnail.saveTo(emptyOutput);
            emptyOutput.flush();
            juce::MemoryInputStream emptyInput(empty, false);
            expect(thumbnail.loadFrom(emptyInput));
            expect(!isValidWaveformData(empty));
        }

        beginTest("Obsolete waveform publication cannot attach peaks to newer analysis");
        {
            const auto waveformFile = directory.copyFixture("sine_stereo_44100_16.wav", "late_waveform.wav");
            const utils::FileIdentity oldIdentity(waveformFile);
            expect(cache.storeAnalysis(analyzeFile(waveformFile)));
            const auto oldKey = waveformCacheKey(oldIdentity, 16);
            juce::AudioFormatManager formats;
            juce::AudioThumbnailCache thumbnails(2);
            juce::AudioThumbnail thumbnail(16, formats, thumbnails);
            const auto waveform = validWaveform();
            juce::MemoryInputStream peaks(waveform, false);
            expect(thumbnail.loadFrom(peaks));
            thumbnails.storeThumb(thumbnail, oldKey);
            expect(thumbnails.loadThumb(thumbnail, oldKey));
            expect(fixtureFile("sine_stereo_48000_24.wav").copyFileTo(waveformFile));
            expect(waveformFile.setLastModificationTime(juce::Time(oldIdentity.modifiedTimeMs + 10'000)));
            const utils::FileIdentity newIdentity(waveformFile);
            expect(oldKey != waveformCacheKey(newIdentity, 16));
            expect(!thumbnails.loadThumb(thumbnail, waveformCacheKey(newIdentity, 16)));
            expect(oldKey != waveformCacheKey(oldIdentity, 32));
            auto replacedIdentity = oldIdentity;
            replacedIdentity.platformId = std::pair<std::uint64_t, std::uint64_t> {1, 2};
            const auto replacedKey = waveformCacheKey(replacedIdentity, 16);
            ++replacedIdentity.platformId->second;
            expect(replacedKey != waveformCacheKey(replacedIdentity, 16));
            const auto preciseKey = waveformCacheKey(replacedIdentity, 16);
            ++replacedIdentity.preciseTimes.first;
            expect(preciseKey != waveformCacheKey(replacedIdentity, 16));
            expect(cache.storeAnalysis(analyzeFile(waveformFile)));
            expect(!cache.storeWaveformData(oldIdentity, validWaveform()));
            expect(!cache.getWaveformData(waveformFile));
            expect(cache.storeWaveformData(newIdentity, validWaveform()));
            expect(cache.getWaveformData(waveformFile).has_value());
            expect(executeSql(
                *this, directory.getChildFile("analysis.db"), "UPDATE file_analysis SET waveform_data = X'6a61746d';"
            ));
            expect(!cache.getWaveformData(waveformFile));
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
        expect(executeSql(
            *this, databaseFile, "UPDATE file_analysis SET analysis_version = 8, short_term_loudness = NULL;"
        ));
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
            expect(executeSql(*this, databaseFile, sql.toRawUTF8()));
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
};

std::unique_ptr<juce::UnitTest> createCacheTests()
{
    return std::make_unique<CacheTests>();
}
}  // namespace audiobatch::tests
