/// Tests for the Serato tag rows of AnalysisCache against a temporary SQLite database.
/// Covers the tag-only lifecycle, stable negatives and corruption, pruning and eviction,
/// and the independent versions and coexistence of every supported tag kind.
/// CacheTests.cpp covers the analysis and waveform rows.
/// The cached files are copies of the committed fixtures.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/Sqlite.h"
#include "gui/NativeWaveformBuilder.h"
#include "serato/SeratoOverview.h"
#include "serato/SeratoTagCache.h"
#include "serato/SeratoTags.h"
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/FileUtils.h"
#include "utils/StringFormat.h"
#include <string_view>

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sqlite3.h>
#include <utility>

namespace audiobatch::tests
{
/// Checks the tag rows of the cache for every supported kind.
class SeratoTagCacheTests final : public juce::UnitTest
{
public:
    SeratoTagCacheTests() : juce::UnitTest("Serato tag cache", "AudioBatch") {}

    /// Runs the tag-only and multi-kind checks.
    void runTest() override
    {
        runSeratoTests();
        runMultiKindSeratoTests();
        runDelayedWriteTests();
    }

private:
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
            expectSqlScalar(*this, databaseFile, "SELECT decoded_data IS NULL FROM serato_tag_cache;", 1);
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
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

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
            expect(executeSql(*this, databaseFile, "DELETE FROM serato_tag_cache;"));
            expect(serato::storeCachedTag(cache, identity, positive));
            expect(executeSql(*this, databaseFile, sql));
            expect(!serato::getCachedTag(cache, identity), sql);
        }
        expect(executeSql(*this, databaseFile, "DELETE FROM serato_tag_cache;"));

        beginTest("Analysis and waveform writes leave independently versioned tags untouched");
        expect(serato::storeCachedTag(cache, identity, positive));
        const auto analyzed = analyzeFile(file);
        expect(cache.storeAnalysis(analyzed));
        const auto waveform = buildNativeWaveform(identity).value_or(juce::MemoryBlock {});
        expect(cache.storeWaveformData(utils::FileIdentity(file), waveform));
        expectSqlScalar(*this, databaseFile, "SELECT analysis_version FROM file_analysis;", 13);
        expectSqlScalar(*this, databaseFile, "SELECT waveform_version FROM file_analysis;", 3);
        expect(serato::getCachedTag(cache, identity).has_value());
        const auto loadedWaveform = cache.getWaveformData(file);
        expect(loadedWaveform.has_value() && *loadedWaveform == waveform);
        expect(cache.storeAnalysis(analyzed));
        expect(cache.getWaveformData(file) == std::optional(waveform));
        expect(serato::getCachedTag(cache, identity).has_value());
        expect(executeSql(*this, databaseFile, "UPDATE file_analysis SET analysis_version = 0;"));
        expect(serato::getCachedTag(cache, identity).has_value());

        beginTest("Explicit eviction removes analysis, waveform and all tag kinds");
        expect(cache.storeAnalysis(analyzed));
        expect(cache.storeWaveformData(utils::FileIdentity(file), waveform));
        expect(executeSql(*this, databaseFile, R"SQL(
            INSERT INTO serato_tag_cache
            SELECT file_path, 2, file_size, modified_time_ms, parser_version, result_state, decoded_data, updated_at_ms
            FROM serato_tag_cache;
        )SQL"));
        expect(cache.removeAnalysis(file));
        expect(!serato::getCachedTag(cache, identity));
        expect(!cache.getWaveformData(file).has_value());
        expect(!cache.getAnalysis(file).has_value());
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

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
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", 1);
        expectEquals(cache.pruneMissingFiles(), 1);
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

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
        expect(serato::getCachedTag(cache, utils::FileIdentity(detachedFile)).has_value());
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
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", 1);
        expectEquals(cache.pruneMissingFiles(), 1);

        beginTest("Pruning counts analysis and tag rows independently for the same deleted file");
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(file));
        expect(cache.storeAnalysis(analyzeFile(file)));
        expect(serato::storeCachedTag(cache, utils::FileIdentity(file), positive));
        expect(file.deleteFile());
        expectEquals(cache.pruneMissingFiles(), 2);

        beginTest("Malformed cache schemas and database failures cannot produce tag hits");
        const auto malformedDatabase = directory.getChildFile("malformed.db");
        expect(executeSql(*this, malformedDatabase, "CREATE TABLE serato_tag_cache (file_path TEXT);"));
        AnalysisCache malformed(malformedDatabase);
        expect(!serato::getCachedTag(malformed, detachedIdentity));
        expect(!serato::storeCachedTag(malformed, detachedIdentity, positive));
        const auto brokenDatabase = directory.getChildFile("broken.db");
        expect(brokenDatabase.replaceWithText("not an SQLite database"));
        AnalysisCache broken(brokenDatabase);
        expect(!serato::getCachedTag(broken, detachedIdentity));
        expect(!serato::storeCachedTag(broken, detachedIdentity, positive));
    }

    /// Interleaves another connection immediately before the write transaction acquires its database lock.
    /// Also recognizes an unguarded INSERT so this regression fails without the transaction and source predicates.
    bool storeWithConcurrentWrite(
        AnalysisCache& cache,
        const utils::FileIdentity& identity,
        const serato::TagReadResult& result,
        const std::function<void()>& concurrentWrite
    )
    {
        bool interleaved = false;
        std::function<void()> pendingWrite = [&] {
            interleaved = true;
            concurrentWrite();
        };
        cache.withDatabase([&](sqlite3& database, sqlite::StatementCache&) {
            expectEquals(
                sqlite3_trace_v2(
                    &database,
                    SQLITE_TRACE_STMT,
                    [](unsigned int, void* context, void* statement, void*) {
                        auto& pending = *static_cast<std::function<void()>*>(context);
                        const auto* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
                        if (pending && sql != nullptr) {
                            const std::string_view text(sql);
                            if (text.starts_with("BEGIN IMMEDIATE") || text.contains("INSERT INTO serato_tag_cache")) {
                                const auto write = std::exchange(pending, {});
                                write();
                            }
                        }
                        return 0;
                    },
                    &pendingWrite
                ),
                SQLITE_OK
            );
        });
        const bool stored = serato::storeCachedTag(cache, identity, result);
        cache.withDatabase([&](sqlite3& database, sqlite::StatementCache&) {
            expectEquals(sqlite3_trace_v2(&database, 0, nullptr, nullptr), SQLITE_OK);
        });
        expect(interleaved);
        return stored;
    }

    /// Checks delayed tag work against newer source and parser rows using two connections without sleeps.
    void runDelayedWriteTests()
    {
        const TemporaryDirectory directory;
        const auto databaseFile = directory.getChildFile("delayed_tags.db");
        AnalysisCache cache(databaseFile);
        AnalysisCache concurrent(databaseFile);
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "delayed.wav");
        const auto absent = serato::TagReadResult::missing(serato::TagKind::overview, serato::ReadStatus::absent);
        const auto invalid = serato::TagReadResult::missing(serato::TagKind::overview, serato::ReadStatus::invalid);

        beginTest("A source published after filesystem validation cannot be replaced by delayed tag work");
        for (const bool existingRow : {false, true}) {
            expect(cache.removeAnalysis(file));
            const utils::FileIdentity identity(file);
            if (existingRow) {
                expect(serato::storeCachedTag(cache, identity, absent));
            }
            expect(!storeWithConcurrentWrite(cache, identity, invalid, [&] {
                expect(file.setLastModificationTime(juce::Time(identity.modifiedTimeMs + 10'000)));
                expect(serato::storeCachedTag(concurrent, utils::FileIdentity(file), absent));
            }));
            const utils::FileIdentity changed(file);
            const auto retained = serato::getCachedTag(cache, changed);
            expect(retained && retained->status() == serato::ReadStatus::absent);
            expect(!serato::getCachedTag(cache, identity));
        }

        beginTest("A newer analysis identity rejects tag work that passed filesystem validation earlier");
        expect(cache.removeAnalysis(file));
        const utils::FileIdentity beforeAnalysis(file);
        expect(serato::storeCachedTag(cache, beforeAnalysis, absent));
        expect(!storeWithConcurrentWrite(cache, beforeAnalysis, invalid, [&] {
            expect(file.setLastModificationTime(juce::Time(beforeAnalysis.modifiedTimeMs + 10'000)));
            auto analyzed = AudioAnalysisRecord::fromFile(file);
            analyzed.status = AudioAnalysisStatus::failed;
            analyzed.error = AnalysisError::unsupported;
            expect(concurrent.storeAnalysis(analyzed));
        }));
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM file_analysis;", 1);

        beginTest("A newer parser published during a delayed write is not downgraded");
        expect(cache.removeAnalysis(file));
        const utils::FileIdentity identity(file);
        expect(serato::storeCachedTag(cache, identity, absent));
        expect(!storeWithConcurrentWrite(cache, identity, invalid, [&] {
            concurrent.withDatabase([&](sqlite3& database, sqlite::StatementCache&) {
                expect(sqlite::execute(database, "UPDATE serato_tag_cache SET parser_version = parser_version + 1;"));
            });
        }));
        expect(!serato::storeCachedTag(cache, identity, invalid));
        expectSqlScalar(
            *this,
            databaseFile,
            "SELECT parser_version FROM serato_tag_cache;",
            static_cast<int>(serato::parserVersion(serato::TagKind::overview) + 1)
        );
        expectSqlScalar(*this, databaseFile, "SELECT result_state FROM serato_tag_cache;", 2);
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
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM file_analysis;", 0);
        expectSqlScalar(
            *this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", static_cast<int>(serato::allTagKinds.size())
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
                expectSqlScalar(*this, databaseFile, sql.toRawUTF8(), 1);
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
                    serato::parserVersion(kind) - 1,
                    static_cast<std::uint32_t>(kind)
                );
                expect(executeSql(*this, databaseFile, mismatch.toRawUTF8()));
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
        expect(
            executeSql(*this, databaseFile, "UPDATE serato_tag_cache SET decoded_data = X'0105' WHERE tag_kind = 5;")
        );
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
        const auto waveform = buildNativeWaveform(identity).value_or(juce::MemoryBlock {});
        expect(cache.storeWaveformData(utils::FileIdentity(file), waveform));
        for (const auto& positive : positives) {
            expect(serato::storeCachedTag(cache, identity, positive));
        }
        const auto loadedWaveform = cache.getWaveformData(file);
        expect(loadedWaveform.has_value() && *loadedWaveform == waveform);
        expect(cache.getAnalysis(file).has_value());
        expect(cache.storeAnalysis(analyzed));
        expect(cache.getWaveformData(file) == std::optional(waveform));
        expect(executeSql(*this, databaseFile, "UPDATE file_analysis SET analysis_version = 0;"));
        for (const auto& positive : positives) {
            const auto retained = serato::getCachedTag(cache, identity, positive.kind);
            expect(retained && serato::serializeTag(*retained) == serato::serializeTag(positive));
        }

        beginTest("Explicit eviction removes every independently populated kind");
        expect(cache.removeAnalysis(file));
        for (const auto kind : serato::allTagKinds) {
            expect(!serato::getCachedTag(cache, identity, kind));
        }
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);

        beginTest("Pruning counts all tag-only kinds and cancellation preserves every row");
        for (const auto& positive : positives) {
            expect(serato::storeCachedTag(cache, identity, positive));
        }
        expect(file.deleteFile());
        expectEquals(cache.pruneMissingFiles([] { return true; }), 0);
        expectSqlScalar(
            *this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", static_cast<int>(serato::allTagKinds.size())
        );
        expectEquals(cache.pruneMissingFiles(), static_cast<int>(serato::allTagKinds.size()));
        expectSqlScalar(*this, databaseFile, "SELECT count(*) FROM serato_tag_cache;", 0);
    }
};

std::unique_ptr<juce::UnitTest> createSeratoTagCacheTests()
{
    return std::make_unique<SeratoTagCacheTests>();
}
}  // namespace audiobatch::tests
