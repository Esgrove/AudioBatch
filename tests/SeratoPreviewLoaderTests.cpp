/// Deterministic lifecycle tests for bounded asynchronous Serato preview loading.
/// Injected readers and queued deliveries exercise cancellation, identity validation and cache restart.
/// All files and databases live in temporary directories, with no local music or sibling repository dependency.

#include "SeratoPreviewLoader.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include <condition_variable>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <vector>

namespace audiobatch::tests
{
using serato::ReadStatus;
using serato::SeratoPreviewLoader;

/// Captures a worker completion without adopting it, like a queued message-thread callback.
struct PreviewDelivery {
    AnalysisCache::FileIdentity identity;
    SeratoPreviewLoader::Generation generation;
    SeratoPreviewLoader::Bands bands;
};

/// Gates the first read and records worker activity without sleeps or message-thread dispatch.
class PreviewProbe final
{
public:
    /// Chooses the injected read outcome and whether the first read waits for explicit release.
    explicit PreviewProbe(const ReadStatus resultStatus = ReadStatus::present, const bool gateFirstRead = false) :
        status(resultStatus),
        gateFirst(gateFirstRead)
    { }

    /// Returns an owned-input reader with an optional noninterruptible gate resembling a TagLib call.
    SeratoPreviewLoader::Reader reader()
    {
        return [this](const juce::File& file, const serato::TagKind kind, const utils::CancellationCheck& cancelled) {
            {
                std::unique_lock lock(mutex);
                paths.push_back(file.getFullPathName());
                condition.notify_all();
                if (gateFirst && paths.size() == 1) {
                    condition.wait_for(lock, watchdog, [this] { return released; });
                }
                observedCancellation = observedCancellation || cancelled();
            }
            serato::TagReadResult result {.kind = kind, .status = status, .overview = std::nullopt};
            if (status == ReadStatus::present) {
                result.overview.emplace();
                result.overview->slices.resize(1);
                result.overview->slices.front().fill(32);
            }
            return result;
        };
    }

    /// Records completions without waiting for GUI adoption.
    SeratoPreviewLoader::Completion completion()
    {
        return [this](
                   AnalysisCache::FileIdentity identity,
                   const SeratoPreviewLoader::Generation generation,
                   SeratoPreviewLoader::Bands bands
               ) {
            const std::scoped_lock lock(mutex);
            deliveries.push_back(
                {.identity = std::move(identity), .generation = generation, .bands = std::move(bands)}
            );
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
    bool waitForGeneration(const SeratoPreviewLoader::Generation generation)
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
    std::vector<PreviewDelivery> deliveries;
};

/// Checks selection generations, bounded work, cancellation and independent tag caching.
class SeratoPreviewLoaderTests final : public juce::UnitTest
{
public:
    /// Registers the focused lifecycle suite in the standard AudioBatch category.
    SeratoPreviewLoaderTests() : juce::UnitTest("Serato preview lifecycle", "AudioBatch") { }

    /// Runs lifecycle scenarios with explicit read gates and completion barriers.
    void runTest() override
    {
        boundedSelection();
        queuedDelivery();
        mutationDuringRead();
        clearAndShutdown();
        cacheRestart();
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
        expect(!loader.canAdopt(AnalysisCache::FileIdentity(first), original, first));
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
            expect(results.front().bands != nullptr);
        }
        expect(probe.wasCancelled());
        expect(!cache.getSeratoTag(AnalysisCache::FileIdentity(second)));
        expect(!cache.getSeratoTag(AnalysisCache::FileIdentity(third)));
    }

    /// Exercises delivery-time generation and identity checks after the worker has already queued a result.
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
        expect(loader.canAdopt(original.identity, original.generation, first));
        expect(!loader.canAdopt(original.identity, original.generation, second));
        loader.request(second, 1.0);
        const auto latest = loader.request(first, 1.0);
        expect(!loader.canAdopt(original.identity, original.generation, first));
        expect(probe.waitForGeneration(latest));
        loader.cancel();
        expect(!loader.canAdopt(AnalysisCache::FileIdentity(first), latest, first));
        const auto afterClear = loader.request(first, 1.0);
        expect(probe.waitForGeneration(afterClear));
        results = probe.queued();
        const auto mutated = results.back();
        expect(first.appendText("changed"));
        expect(!loader.canAdopt(mutated.identity, mutated.generation, first));
        loader.cancelAndWait();
        expect(!loader.canAdopt(original.identity, original.generation, first));
    }

    /// Uses a subsequent completion as a barrier proving a mutated active read finished without delivery or storage.
    void mutationDuringRead()
    {
        beginTest("Mutation during a noninterruptible read discards the old bytes before cache storage");
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cache.db"));
        const auto first = directory.copyFixture("sine_stereo_44100_16.wav", "first.wav");
        const auto second = directory.copyFixture("sine_stereo_44100_16.wav", "second.wav");
        const AnalysisCache::FileIdentity original(first);
        PreviewProbe probe(ReadStatus::present, true);
        SeratoPreviewLoader loader(cache, probe.completion(), probe.reader());
        loader.request(first, 1.0);
        expect(probe.waitForReads(1));
        expect(first.appendText("changed"));
        probe.release();
        loader.request(second, 1.0);
        expect(probe.waitForDeliveries(1));
        loader.cancelAndWait();
        const auto results = probe.queued();
        expectEquals(static_cast<int>(results.size()), 1);
        if (!results.empty()) {
            expectEquals(results.front().identity.path, second.getFullPathName());
        }
        expect(!cache.getSeratoTag(original));
        expect(!cache.getSeratoTag(AnalysisCache::FileIdentity(first)));
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
        expect(!cache.getSeratoTag(AnalysisCache::FileIdentity(first)));
        expect(!cache.getSeratoTag(AnalysisCache::FileIdentity(second)));
    }

    /// Reopens real SQLite databases and verifies positive and stable-negative rows avoid another read.
    void cacheRestart()
    {
        for (const auto status :
             {ReadStatus::present, ReadStatus::absent, ReadStatus::unsupported, ReadStatus::invalid})
        {
            beginTest("Restart reuses numeric bands or stable negative tag outcomes");
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
                    if (status == ReadStatus::present) {
                        expect(results[0].bands && results[1].bands);
                        if (results[0].bands && results[1].bands) {
                            expect(results[0].bands->slices == results[1].bands->slices);
                        }
                    } else {
                        expect(!results[0].bands && !results[1].bands);
                    }
                }
                AudioAnalysisRecord record;
                expect(!cache.getAnalysis(file, record));
            }
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
                expect(!cache.getSeratoTag(AnalysisCache::FileIdentity(file)));
            }
            expectEquals(static_cast<int>(probe.readPaths().size()), 2);
            for (const auto& result : probe.queued()) {
                expect(result.bands == nullptr);
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
        loader.request(file, 0.0);
        loader.request(file, -1.0);
        loader.request(file, std::numeric_limits<double>::quiet_NaN());
        loader.request(file, std::numeric_limits<double>::infinity());
        loader.request(directory.getChildFile("missing.wav"), 1.0);
        loader.cancelAndWait();
        loader.request(file, 1.0);
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
            expect(results.front().bands != nullptr);
        }
    }
};

std::unique_ptr<juce::UnitTest> createSeratoPreviewLoaderTests()
{
    return std::make_unique<SeratoPreviewLoaderTests>();
}
}  // namespace audiobatch::tests
