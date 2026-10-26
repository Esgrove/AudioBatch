/// Deterministic lifecycle tests for bounded asynchronous Serato preview loading.
/// Injected readers and queued deliveries exercise cancellation, identity validation and cache restart.
/// All files and databases live in temporary directories,
/// with no local music or sibling repository dependency.

#include "serato/SeratoPreviewLoader.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include <condition_variable>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <vector>

namespace audiobatch::tests
{
using serato::ReadStatus;
using serato::SeratoPreviewLoader;

namespace
{
/// Captures a worker completion without adopting it, like a queued message-thread callback.
struct PreviewDelivery {
    FileIdentity identity;
    std::uint64_t generation;
    std::shared_ptr<const serato::SeratoData> data;
};

/// Builds representative owned values for every independently cached kind.
serato::TagReadResult previewResult(const serato::TagKind kind, const ReadStatus status = ReadStatus::present)
{
    if (status != ReadStatus::present) {
        return serato::TagReadResult::missing(kind, status);
    }
    switch (kind) {
        case serato::TagKind::overview: {
            serato::Overview overview;
            overview.slices.resize(1);
            overview.slices.front().fill(32);
            return serato::TagReadResult::present(std::move(overview));
        }
        case serato::TagKind::analysis:
            return serato::TagReadResult::present(serato::Analysis {.majorVersion = 2, .minorVersion = 5});
        case serato::TagKind::autotags:
            return serato::TagReadResult::present(serato::Autotags {.bpm = 128.0, .autoGain = -3.0, .gain = 1.5});
        case serato::TagKind::beatGrid:
            return serato::TagReadResult::present(
                serato::BeatGrid {{{.positionSeconds = 0.5, .beatsUntilNext = 0, .bpm = 128.0}}}
            );
        case serato::TagKind::markers:
            return serato::TagReadResult::present(serato::Markers {
                .cues = {
                    {.positionMilliseconds = 1000,
                        .index = 2,
                        .colour = juce::Colours::red,
                        .name = "Intro",
                    },
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
            });
        case serato::TagKind::trackInfo:
            return serato::TagReadResult::present(makeTrackInfo(128.0, "Am"));
    }
    return serato::TagReadResult::missing(kind, ReadStatus::unsupported);
}
}  // namespace

/// Gates the first read and records worker activity without sleeps or message-thread dispatch.
class PreviewProbe final
{
public:
    /// Chooses the injected read outcome and whether the first read waits for explicit release.
    explicit PreviewProbe(const ReadStatus resultStatus = ReadStatus::present, const bool gateFirstRead = false) :
        status(resultStatus),
        gateFirst(gateFirstRead)
    {}

    /// Returns an owned-input reader with an optional noninterruptible gate resembling a TagLib call.
    SeratoPreviewLoader::Reader reader()
    {
        return [this](
                   const juce::File& file,
                   const std::span<const serato::TagKind> kinds,
                   const utils::CancellationCheck& cancelled
               ) {
            {
                std::unique_lock lock(mutex);
                paths.push_back(file.getFullPathName());
                batches.emplace_back(kinds.begin(), kinds.end());
                condition.notify_all();
                if (gateFirst && paths.size() == 1) {
                    condition.wait_for(lock, watchdog, [this] { return released; });
                }
                observedCancellation = observedCancellation || cancelled();
            }
            std::vector<serato::TagReadResult> results;
            for (const auto kind : kinds) {
                results.push_back(previewResult(kind, status));
            }
            return results;
        };
    }

    /// Records completions without waiting for GUI adoption.
    SeratoPreviewLoader::Completion completion()
    {
        return [this](
                   FileIdentity identity, const std::uint64_t generation, std::shared_ptr<const serato::SeratoData> data
               ) {
            const std::scoped_lock lock(mutex);
            deliveries.push_back({.identity = std::move(identity), .generation = generation, .data = std::move(data)});
            condition.notify_all();
        };
    }

    /// Waits for a reader entry. The timeout is only a deadlock watchdog, not a timing assertion.
    bool waitForReads(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, watchdog, [this, count] { return paths.size() >= count; });
    }

    /// Waits for a completion to be queued, without running a message loop.
    bool waitForDeliveries(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, watchdog, [this, count] { return deliveries.size() >= count; });
    }

    /// Waits for the exact latest generation even if an intervening completion was also queued.
    bool waitForGeneration(const std::uint64_t generation)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, watchdog, [this, generation] {
            return std::ranges::any_of(deliveries, [generation](const PreviewDelivery& result) {
                return result.generation == generation;
            });
        });
    }

    /// Releases the noninterruptible read and wakes its condition variable.
    void release()
    {
        const std::scoped_lock lock(mutex);
        released = true;
        condition.notify_all();
    }

    /// Returns a snapshot of queued results while the worker may still be alive.
    [[nodiscard]] std::vector<PreviewDelivery> queued() const
    {
        const std::scoped_lock lock(mutex);
        return deliveries;
    }

    /// Returns a snapshot of paths opened by the injected reader.
    [[nodiscard]] std::vector<juce::String> readPaths() const
    {
        const std::scoped_lock lock(mutex);
        return paths;
    }

    /// Returns the exact kinds requested in each single-open batch.
    [[nodiscard]] std::vector<std::vector<serato::TagKind>> readBatches() const
    {
        const std::scoped_lock lock(mutex);
        return batches;
    }

    /// Reports whether the reader saw cancellation at its return boundary.
    [[nodiscard]] bool wasCancelled() const
    {
        const std::scoped_lock lock(mutex);
        return observedCancellation;
    }

private:
    static constexpr auto watchdog = std::chrono::seconds(10);
    ReadStatus status;
    bool gateFirst;
    bool released = false;
    bool observedCancellation = false;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<juce::String> paths;
    std::vector<std::vector<serato::TagKind>> batches;
    std::vector<PreviewDelivery> deliveries;
};

/// Checks selection generations, bounded work, cancellation and independent tag caching.
class SeratoPreviewLoaderTests final : public juce::UnitTest
{
public:
    /// Registers the focused lifecycle suite in the standard AudioBatch category.
    SeratoPreviewLoaderTests() : juce::UnitTest("Serato preview lifecycle", "AudioBatch") {}

    /// Runs lifecycle scenarios with explicit read gates and completion barriers.
    void runTest() override
    {
        boundedSelection();
        queuedDelivery();
        mutationDuringRead();
        clearAndShutdown();
        cacheRestart();
        independentCacheMisses();
        mixedBatchResults();
        missingOverview();
        transientResults();
        invalidInputs();
        unavailableDatabase();
    }

private:
    /// Proves that intermediate pending selections are replaced and cancelled reads are not delivered or stored.
    void boundedSelection()
    {
        beginTest("One active read and only the latest pending selection, including A to B to A");
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cache.db"));
        const auto first = directory.copyFixture("sine_stereo_44100_16.wav", "first.wav");
        const auto second = directory.copyFixture("sine_stereo_44100_16.wav", "second.wav");
        const auto third = directory.copyFixture("sine_stereo_44100_16.wav", "third.wav");
        PreviewProbe probe(ReadStatus::present, true);
        SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
        const auto original = loader.request(first, 1.0);
        expect(probe.waitForReads(1));
        loader.request(second, 1.0);
        loader.request(third, 1.0);
        const auto latest = loader.request(first, 1.0);
        expect(!loader.canAdopt(FileIdentity(first), original, first));
        expectEquals(static_cast<int>(probe.readPaths().size()), 1);
        probe.release();
        expect(probe.waitForDeliveries(1));
        loader.cancelAndWait();
        const auto paths = probe.readPaths();
        expect(paths == std::vector<juce::String> {first.getFullPathName(), first.getFullPathName()});
        const auto results = probe.queued();
        expectEquals(static_cast<int>(results.size()), 1);
        if (!results.empty()) {
            expect(results.front().generation == latest);
            expect(results.front().data != nullptr);
        }
        expect(probe.wasCancelled());
        const auto batches = probe.readBatches();
        expectEquals(static_cast<int>(batches.size()), 2);
        for (const auto& batch : batches) {
            expect(batch == std::vector<serato::TagKind>(serato::allTagKinds.begin(), serato::allTagKinds.end()));
        }
        for (const auto kind : serato::allTagKinds) {
            expect(!cache.getSeratoTag(FileIdentity(second), kind));
            expect(!cache.getSeratoTag(FileIdentity(third), kind));
        }
    }

    /// Distinguishes generation cancellation from path and identity rejection after a result is queued.
    void queuedDelivery()
    {
        beginTest("Queued completions reject path mismatch, A to B to A, mutation and clear");
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cache.db"));
        const auto first = directory.copyFixture("sine_stereo_44100_16.wav", "first.wav");
        const auto second = directory.copyFixture("sine_stereo_44100_16.wav", "second.wav");
        PreviewProbe probe;
        SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
        loader.request(first, 1.0);
        expect(probe.waitForDeliveries(1));
        auto results = probe.queued();
        if (results.empty()) {
            return;
        }
        const auto original = results.front();
        expect(loader.isCurrentGeneration(original.generation));
        expect(loader.canAdopt(original.identity, original.generation, first));
        expect(!loader.canAdopt(original.identity, original.generation, second));
        expect(loader.isCurrentGeneration(original.generation));
        loader.request(second, 1.0);
        const auto latest = loader.request(first, 1.0);
        expect(!loader.canAdopt(original.identity, original.generation, first));
        expect(!loader.isCurrentGeneration(original.generation));
        expect(probe.waitForGeneration(latest));
        expect(loader.isCurrentGeneration(latest));
        loader.cancel();
        expect(!loader.canAdopt(FileIdentity(first), latest, first));
        expect(!loader.isCurrentGeneration(latest));
        const auto afterClear = loader.request(first, 1.0);
        expect(probe.waitForGeneration(afterClear));
        results = probe.queued();
        const auto mutated = results.back();
        expect(first.appendText("changed"));
        expect(!loader.canAdopt(mutated.identity, mutated.generation, first));
        expect(loader.isCurrentGeneration(mutated.generation));
        loader.cancelAndWait();
        expect(!loader.canAdopt(original.identity, original.generation, first));
        expect(!loader.isCurrentGeneration(mutated.generation));
    }

    /// Waits for null completion of an unchanged generation before requesting another file.
    /// Mutated bytes cannot be adopted or cached, and a subsequent valid request still completes.
    void mutationDuringRead()
    {
        beginTest("Mutation during a noninterruptible read completes without metadata or cache storage");
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cache.db"));
        const auto first = directory.copyFixture("sine_stereo_44100_16.wav", "first.wav");
        const auto second = directory.copyFixture("sine_stereo_44100_16.wav", "second.wav");
        const FileIdentity original(first);
        PreviewProbe probe(ReadStatus::present, true);
        SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
        const auto originalGeneration = loader.request(first, 1.0);
        expect(probe.waitForReads(1));
        expect(first.appendText("changed"));
        probe.release();
        expect(probe.waitForGeneration(originalGeneration));
        auto results = probe.queued();
        expectEquals(static_cast<int>(results.size()), 1);
        if (!results.empty()) {
            const auto& rejected = results.front();
            expectEquals(rejected.identity.path, original.path);
            expect(rejected.generation == originalGeneration);
            expect(rejected.data == nullptr);
            expect(!loader.canAdopt(rejected.identity, rejected.generation, first));
            expect(loader.isCurrentGeneration(rejected.generation));
        }
        expect(loader.isCurrentGeneration(originalGeneration));
        expect(!probe.wasCancelled());
        for (const auto kind : serato::allTagKinds) {
            expect(!cache.getSeratoTag(original, kind));
            expect(!cache.getSeratoTag(FileIdentity(first), kind));
            expect(!cache.getSeratoTag(FileIdentity(second), kind));
        }

        const auto latest = loader.request(second, 1.0);
        expect(probe.waitForGeneration(latest));
        results = probe.queued();
        expectEquals(static_cast<int>(results.size()), 2);
        if (results.size() == 2) {
            expectEquals(results.back().identity.path, second.getFullPathName());
            expect(results.back().generation == latest);
            expectEveryKind(results.back().data);
            expect(loader.canAdopt(results.back().identity, results.back().generation, second));
            expect(loader.isCurrentGeneration(results.back().generation));
        }
        expect(!loader.isCurrentGeneration(originalGeneration));
        loader.cancelAndWait();
    }

    /// Clears pending work and joins an in-flight reader without running queued GUI callbacks.
    void clearAndShutdown()
    {
        beginTest("Clear drops pending work and shutdown joins without GUI adoption");
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cache.db"));
        const auto first = directory.copyFixture("sine_stereo_44100_16.wav", "first.wav");
        const auto second = directory.copyFixture("sine_stereo_44100_16.wav", "second.wav");
        PreviewProbe probe(ReadStatus::present, true);
        auto loader = std::make_unique<SeratoPreviewLoader>(cache, probe.completion(), probe.reader());
        loader->request(first, 1.0);
        expect(probe.waitForReads(1));
        loader->request(second, 1.0);
        loader->cancel();
        probe.release();
        loader.reset();
        expect(probe.wasCancelled());
        expect(probe.queued().empty());
        expectEquals(static_cast<int>(probe.readPaths().size()), 1);
        for (const auto kind : serato::allTagKinds) {
            expect(!cache.getSeratoTag(FileIdentity(first), kind));
            expect(!cache.getSeratoTag(FileIdentity(second), kind));
        }
    }

    /// Reopens real SQLite databases and verifies every positive and stable-negative kind avoids another read.
    void cacheRestart()
    {
        for (const auto status :
             {ReadStatus::present, ReadStatus::absent, ReadStatus::unsupported, ReadStatus::invalid})
        {
            cacheRestartOutcome(status);
        }
    }

    /// Checks a single stable outcome across fresh loader and database instances without another tag read.
    void cacheRestartOutcome(const ReadStatus status)
    {
        beginTest("Restart reuses all independent positive or stable negative tag outcomes");
        const TemporaryDirectory directory;
        const auto database = directory.getChildFile("cache.db");
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "preview.wav");
        PreviewProbe probe(status);
        {
            AnalysisCache cache(database);
            SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
            loader.request(file, 1.0);
            expect(probe.waitForDeliveries(1));
        }
        {
            AnalysisCache cache(database);
            SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
            loader.request(file, 1.0);
            expect(probe.waitForDeliveries(2));
            loader.cancelAndWait();
            const auto results = probe.queued();
            expectEquals(static_cast<int>(probe.readPaths().size()), 1);
            expectEquals(static_cast<int>(results.size()), 2);
            if (results.size() == 2) {
                expectRestartData(results[0].data, results[1].data, status);
            }
            const auto batches = probe.readBatches();
            expectEquals(static_cast<int>(batches.size()), 1);
            if (!batches.empty()) {
                expect(
                    batches.front()
                    == std::vector<serato::TagKind>(serato::allTagKinds.begin(), serato::allTagKinds.end())
                );
            }
            for (const auto kind : serato::allTagKinds) {
                const auto cached = cache.getSeratoTag(FileIdentity(file), kind);
                expect(cached && cached->kind == kind && cached->status() == status);
            }
            expect(!cache.getAnalysis(file).has_value());
        }
    }

    /// Verifies identical owned overview values after restart, or absent aggregates for stable negatives.
    void expectRestartData(
        const std::shared_ptr<const serato::SeratoData>& original,
        const std::shared_ptr<const serato::SeratoData>& restarted,
        const ReadStatus status
    )
    {
        if (status != ReadStatus::present) {
            expect(!original && !restarted);
            return;
        }
        expect(original && restarted);
        expectEveryKind(original);
        expectEveryKind(restarted);
        if (!original || !restarted) {
            return;
        }
        const auto& originalOverview = original->overview;
        const auto& restartedOverview = restarted->overview;
        expect(originalOverview && restartedOverview);
        if (originalOverview && restartedOverview) {
            expect(originalOverview->slices == restartedOverview->slices);
        }
    }

    /// Keeps independent negative hits while batching only missing and corrupt positive rows.
    void independentCacheMisses()
    {
        using serato::TagKind;
        beginTest("Only corrupt markers and missing track metadata are read alongside independent negative hits");
        const TemporaryDirectory directory;
        const auto databaseFile = directory.getChildFile("cache.db");
        AnalysisCache cache(databaseFile);
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "preview.wav");
        const FileIdentity identity(file);
        expect(cache.storeSeratoTag(identity, previewResult(TagKind::overview)));
        expect(cache.storeSeratoTag(identity, previewResult(TagKind::analysis, ReadStatus::absent)));
        expect(cache.storeSeratoTag(identity, previewResult(TagKind::autotags, ReadStatus::unsupported)));
        expect(cache.storeSeratoTag(identity, previewResult(TagKind::beatGrid, ReadStatus::invalid)));
        expect(cache.storeSeratoTag(identity, previewResult(TagKind::markers)));
        sqlite3* database = nullptr;
        expectEquals(sqlite3_open(databaseFile.getFullPathName().toRawUTF8(), &database), SQLITE_OK);
        expectEquals(
            sqlite3_exec(
                database,
                "UPDATE serato_tag_cache SET decoded_data = X'0105' WHERE tag_kind = 5;",
                nullptr,
                nullptr,
                nullptr
            ),
            SQLITE_OK
        );
        sqlite3_close(database);
        PreviewProbe probe;
        SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
        loader.request(file, 1.0);
        expect(probe.waitForDeliveries(1));
        loader.request(file, 1.0);
        expect(probe.waitForDeliveries(2));
        loader.cancelAndWait();
        expect(probe.readBatches() == std::vector<std::vector<TagKind>> {{TagKind::markers, TagKind::trackInfo}});
        for (const auto& delivery : probe.queued()) {
            expect(delivery.data != nullptr);
            if (delivery.data) {
                expect(delivery.data->overview && delivery.data->markers && delivery.data->trackInfo);
                expect(!delivery.data->analysis && !delivery.data->autotags && !delivery.data->beatGrid);
            }
        }
    }

    /// Matches reordered results by kind, preserving valid values when another result is invalid or omitted.
    void mixedBatchResults()
    {
        using serato::TagKind;
        beginTest("Reordered, missing and transient kinds cannot hide valid overview, cues or track metadata");
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cache.db"));
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "preview.wav");
        const FileIdentity identity(file);
        PreviewProbe probe;
        std::vector<std::vector<TagKind>> batches;
        const SeratoPreviewLoader::Reader reader
            = [&](const juce::File&, const std::span<const TagKind> kinds, const utils::CancellationCheck&) {
                  batches.emplace_back(kinds.begin(), kinds.end());
                  std::vector<serato::TagReadResult> results;
                  for (const auto kind : kinds) {
                      if (kind != TagKind::beatGrid) {
                          auto status = ReadStatus::present;
                          if (kind == TagKind::analysis) {
                              status = ReadStatus::invalid;
                          } else if (kind == TagKind::autotags) {
                              status = ReadStatus::ioError;
                          }
                          results.push_back(previewResult(kind, status));
                      }
                  }
                  std::ranges::reverse(results);
                  results.push_back(previewResult(std::bit_cast<TagKind>(std::uint8_t {99}), ReadStatus::absent));
                  return results;
              };
        SeratoPreviewLoader loader(cache, probe.completion(), reader);
        loader.request(file, 1.0);
        expect(probe.waitForDeliveries(1));
        loader.request(file, 1.0);
        expect(probe.waitForDeliveries(2));
        loader.cancelAndWait();
        expectEquals(static_cast<int>(batches.size()), 2);
        if (batches.size() == 2) {
            expect(batches.front() == std::vector<TagKind>(serato::allTagKinds.begin(), serato::allTagKinds.end()));
            expect(batches.back() == std::vector<TagKind> {TagKind::autotags, TagKind::beatGrid});
        }
        for (const auto& delivery : probe.queued()) {
            expect(delivery.data != nullptr);
            if (delivery.data) {
                expect(delivery.data->overview && delivery.data->markers && delivery.data->trackInfo);
                expect(!delivery.data->analysis && !delivery.data->autotags && !delivery.data->beatGrid);
                if (delivery.data->markers) {
                    expectEquals(delivery.data->markers->cues.front().name, juce::String("Intro"));
                }
            }
        }
        const auto negative = cache.getSeratoTag(identity, TagKind::analysis);
        expect(negative && negative->status() == ReadStatus::invalid);
        expect(!cache.getSeratoTag(identity, TagKind::autotags));
        expect(!cache.getSeratoTag(identity, TagKind::beatGrid));
    }

    /// Keeps musical metadata and markers available when overview alone has a stable negative outcome.
    void missingOverview()
    {
        for (const auto status : {ReadStatus::absent, ReadStatus::invalid}) {
            beginTest("Missing or invalid overview does not hide valid metadata from other kinds");
            const TemporaryDirectory directory;
            AnalysisCache cache(directory.getChildFile("cache.db"));
            const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "preview.wav");
            const FileIdentity identity(file);
            expect(cache.storeSeratoTag(identity, previewResult(serato::TagKind::overview, status)));
            PreviewProbe probe;
            SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
            loader.request(file, 1.0);
            expect(probe.waitForDeliveries(1));
            loader.cancelAndWait();
            const auto batches = probe.readBatches();
            expectEquals(static_cast<int>(batches.size()), 1);
            if (!batches.empty()) {
                expect(
                    batches.front()
                    == std::vector<serato::TagKind>(std::next(serato::allTagKinds.begin()), serato::allTagKinds.end())
                );
            }
            for (const auto& delivery : probe.queued()) {
                expect(delivery.data != nullptr);
                if (delivery.data) {
                    expect(!delivery.data->overview);
                    expect(
                        delivery.data->analysis && delivery.data->autotags && delivery.data->beatGrid
                        && delivery.data->markers && delivery.data->trackInfo
                    );
                }
            }
        }
    }

    /// Verifies the complete immutable aggregate and the representative values returned by the batch probe.
    void expectEveryKind(const std::shared_ptr<const serato::SeratoData>& data)
    {
        expect(data != nullptr);
        if (!data) {
            return;
        }
        expect(
            data->overview && data->analysis && data->autotags && data->beatGrid && data->markers && data->trackInfo
        );
        if (data->analysis) {
            expectEquals(static_cast<int>(data->analysis->majorVersion), 2);
            expectEquals(static_cast<int>(data->analysis->minorVersion), 5);
        }
        if (data->autotags) {
            expectEquals(data->autotags->bpm, 128.0);
            expectEquals(data->autotags->autoGain, -3.0);
            expectEquals(data->autotags->gain, 1.5);
        }
        if (data->beatGrid) {
            expectEquals(static_cast<int>(data->beatGrid->markers.size()), 1);
            if (!data->beatGrid->markers.empty()) {
                expectEquals(data->beatGrid->markers.front().positionSeconds, 0.5);
                expect(data->beatGrid->markers.front().bpm == 128.0);
            }
        }
        if (data->markers) {
            expectEquals(static_cast<int>(data->markers->cues.size()), 1);
            expectEquals(static_cast<int>(data->markers->loops.size()), 1);
            expect(data->markers->colour == juce::Colours::blue);
            expect(data->markers->bpmLocked == false);
        }
        if (data->trackInfo) {
            expect(data->trackInfo->bpm == 128.0);
            expectEquals(data->trackInfo->key, juce::String("Am"));
        }
    }

    /// Confirms transient read outcomes remain misses and cannot recolour a preview after cancellation.
    void transientResults()
    {
        for (const auto status : {ReadStatus::ioError, ReadStatus::cancelled}) {
            beginTest("I/O failure and cancelled reads are retried instead of negative-cached");
            const TemporaryDirectory directory;
            AnalysisCache cache(directory.getChildFile("cache.db"));
            const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "preview.wav");
            PreviewProbe probe(status);
            for (std::size_t attempt = 0; attempt < 2; ++attempt) {
                SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
                loader.request(file, 1.0);
                expect(probe.waitForReads(attempt + 1));
                if (status == ReadStatus::ioError) {
                    expect(probe.waitForDeliveries(attempt + 1));
                }
                loader.cancelAndWait();
                for (const auto kind : serato::allTagKinds) {
                    expect(!cache.getSeratoTag(FileIdentity(file), kind));
                }
            }
            expectEquals(static_cast<int>(probe.readPaths().size()), 2);
            for (const auto& result : probe.queued()) {
                expect(result.data == nullptr);
            }
            if (status == ReadStatus::cancelled) {
                expect(probe.queued().empty());
            }
        }
    }

    /// Rejects unusable durations, missing resources and requests submitted after permanent shutdown.
    void invalidInputs()
    {
        beginTest("Invalid preview inputs cancel work without opening tags");
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cache.db"));
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "preview.wav");
        PreviewProbe probe;
        SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
        expect(loader.request(file, 0.0) == 0);
        expect(loader.request(file, -1.0) == 0);
        expect(loader.request(file, std::numeric_limits<double>::quiet_NaN()) == 0);
        expect(loader.request(file, std::numeric_limits<double>::infinity()) == 0);
        expect(loader.request(directory.getChildFile("missing.wav"), 1.0) == 0);
        loader.cancelAndWait();
        expect(loader.request(file, 1.0) == 0);
        loader.cancelAndWait();
        expect(probe.readPaths().empty());
        expect(probe.queued().empty());
    }

    /// Allows a fresh read to complete when the database cannot open, without requiring an analysis row.
    void unavailableDatabase()
    {
        beginTest("A database failure does not prevent fresh preview colours");
        const TemporaryDirectory directory;
        const auto databaseDirectory = directory.getChildFile("database_directory");
        expect(databaseDirectory.createDirectory().wasOk());
        AnalysisCache cache(databaseDirectory);
        const auto file = directory.copyFixture("sine_stereo_44100_16.wav", "preview.wav");
        PreviewProbe probe;
        SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
        loader.request(file, 1.0);
        expect(probe.waitForDeliveries(1));
        loader.cancelAndWait();
        const auto results = probe.queued();
        expectEquals(static_cast<int>(results.size()), 1);
        if (!results.empty()) {
            expect(results.front().data != nullptr);
        }
    }
};

std::unique_ptr<juce::UnitTest> createSeratoPreviewLoaderTests()
{
    return std::make_unique<SeratoPreviewLoaderTests>();
}
}  // namespace audiobatch::tests
