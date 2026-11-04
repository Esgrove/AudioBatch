/// Focused admission and combined audio-artifact tests without windows, rendering, or source-backed thumbnails.
/// Deterministic worker gates check the completion boundary, selection promotion and cooperative cancellation.
/// Native JUCE serialization is the independent reference for thumbnail quantization and final-bucket geometry.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/DecodeFailureTracker.h"
#include "analysis/NativeWaveformBuilder.h"
#include "analysis/NativeWaveformData.h"
#include "analysis/SpectrogramData.h"
#include "gui/PreviewArtifactDecoder.h"
#include "gui/PreviewEnrichmentScheduler.h"
#include "serato/SeratoPreviewLoader.h"
#include "serato/SeratoTagCache.h"
#include "serato/SeratoTagTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include "utils/ThreadUtils.h"
#include <condition_variable>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// One admitted request, captured before its deterministic worker gate.
struct EnrichmentCall {
    juce::File file;
    bool cancelled {false};
    bool readMissing {false};
};

/// Supplies deterministic float PCM and decoder failures without disk I/O or a source-backed thumbnail.
class WaveformTestReader final : public juce::AudioFormatReader
{
public:
    /// Creates a mono stream long enough to exercise consecutive block failure handling.
    WaveformTestReader() : juce::AudioFormatReader(nullptr, "Waveform test")
    {
        sampleRate = 48'000.0;
        bitsPerSample = 32;
        lengthInSamples = 65'536;
        numChannels = 1;
        usesFloatingPointData = true;
    }

    /// Fills float sample bit patterns through JUCE's reader ABI and optionally reports a failed block.
    bool readSamples(
        int* const* destination,
        const int channelCount,
        const int destinationOffset,
        juce::int64 /*sourcePosition*/,
        const int sampleCount
    ) override
    {
        ++reads;
        framesRead += sampleCount;
        maximumReadSize = std::max(maximumReadSize, sampleCount);
        for (auto* channel : std::span(destination, static_cast<std::size_t>(channelCount))) {
            if (channel != nullptr) {
                const auto samples = std::span(channel, static_cast<std::size_t>(destinationOffset + sampleCount))
                                         .subspan(static_cast<std::size_t>(destinationOffset));
                std::ranges::fill(samples, std::bit_cast<int>(sample));
            }
        }
        return readSucceeded;
    }

    /// Result reported after supplying PCM for a block.
    bool readSucceeded {true};
    /// PCM value delivered for every decoded sample, including nonfinite rejection cases.
    float sample {0.25f};
    /// Total frames requested, proving combined artifacts share exactly one decode pass.
    juce::int64 framesRead {0};
    /// Largest requested block, proving PCM working memory stays bounded.
    int maximumReadSize {0};
    /// Blocks requested, proving invalid geometry is rejected before decoding.
    int reads {0};
};

/// Holds worker input by shared ownership, so scheduler shutdown never leaves dangling test captures.
class EnrichmentProbe final
{
public:
    /// Returns a stable snapshot while the sole worker may still be running.
    std::vector<EnrichmentCall> snapshot() const
    {
        const std::scoped_lock lock(mutex);
        return calls;
    }

    /// Reports actual overlap rather than inferring it from queued task handles.
    int getMaximumActive() const
    {
        const std::scoped_lock lock(mutex);
        return maximumActive;
    }

    /// Waits for admission. The timeout is a deadlock watchdog rather than a responsiveness threshold.
    bool waitForCalls(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, watchdog, [&] { return calls.size() >= count; });
    }

    /// Waits until the injected work has reached its final cancellation check.
    bool waitForFinished(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, watchdog, [&] { return finished >= count; });
    }

    /// Opens a fixed number of worker gates, without sleeps or message-loop dispatch.
    void release(const std::size_t count)
    {
        const std::scoped_lock lock(mutex);
        released = count;
        condition.notify_all();
    }

    /// Records admission and waits for an explicit release, resembling a decoder between cancellation checks.
    void work(
        const PreviewEnrichmentCandidate& candidate,
        const bool readMissing,
        const utils::CancellationCheck& cancelled
    )
    {
        std::unique_lock lock(mutex);
        const auto index = calls.size();
        calls.push_back({.file = candidate.file, .readMissing = readMissing});
        ++active;
        maximumActive = std::max(maximumActive, active);
        condition.notify_all();
        condition.wait_for(lock, watchdog, [&] { return released > index; });
        calls[index].cancelled = cancelled();
        --active;
        ++finished;
        condition.notify_all();
    }

private:
    static constexpr auto watchdog {std::chrono::milliseconds(500)};

    std::condition_variable condition;
    mutable std::mutex mutex;
    std::vector<EnrichmentCall> calls;

    int active {0};
    int maximumActive {0};

    std::size_t finished {0};
    std::size_t released {0};
};

/// Captures independently published metadata without requiring message-thread delivery.
class EnrichmentDeliveries final
{
public:
    /// Returns owned snapshots so the detached worker cannot race test assertions.
    std::vector<PreviewEnrichmentResult> snapshot() const
    {
        const std::scoped_lock lock(mutex);
        return results;
    }

    /// Waits for independent artifact publication, with a deadlock watchdog only.
    bool waitForResults(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::milliseconds(500), [&] { return results.size() >= count; });
    }

    /// Waits for the explicit post-persistence spectral completion stage, not a result count or elapsed delay.
    /// FFT work normally finishes well below one second, but concurrent builds can starve this detached worker.
    /// The five-second deadline detects deadlock only, without turning CPU contention into a performance assertion.
    bool waitForSpectrogram()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&] { return spectrogramCompleted; });
    }

    /// Waits for a cached waveform independently of any preceding metadata publication.
    bool waitForWaveform()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return std::ranges::any_of(results, [](const auto& result) { return result.waveform.has_value(); });
        });
    }

    /// Records worker completion under a test-owned lock, never waiting for the GUI thread.
    void record(PreviewEnrichmentResult result)
    {
        const std::scoped_lock lock(mutex);
        spectrogramCompleted = spectrogramCompleted || result.spectrogramReady;
        results.push_back(std::move(result));
        condition.notify_all();
    }

private:
    std::condition_variable condition;
    mutable std::mutex mutex;
    std::vector<PreviewEnrichmentResult> results;
    bool spectrogramCompleted {false};
};

/// Captures lightweight source geometry without retaining measurements or waveform blobs.
PreviewEnrichmentCandidate candidateFor(const juce::File& file)
{
    const utils::FileIdentity identity(file);
    return {.file = file, .fileSize = identity.fileSize, .modifiedTimeMs = identity.modifiedTimeMs};
}

/// Returns work whose probe outlives every detached callback, including timeout and early-return paths.
PreviewEnrichmentScheduler::Work workFor(std::shared_ptr<EnrichmentProbe> probe)
{
    return [ownedProbe = std::move(probe)](const auto& candidate, const bool readMissing, const auto& cancelled) {
        ownedProbe->work(candidate, readMissing, cancelled);
    };
}
}  // namespace

/// Tests scheduling policy and serializer equivalence without constructing GUI components.
class PreviewEnrichmentTests final : public juce::UnitTest
{
public:
    /// Registers the lightweight preview-artifact suite.
    PreviewEnrichmentTests() : juce::UnitTest("Preview enrichment", "AudioBatch") {}

    void runTest() override;

private:
    void testArtifacts(const juce::File& first);
    void testMetadata(const juce::File& first);
    void testPersistence(const juce::File& first);
    void testProgress(const juce::File& first, const juce::File& second);
    void testScheduling(const juce::File& first, const juce::File& second);
    void testStreamingWaveformChunks();
    void testStreamingWaveformInvalidInputs();
    void testStreamingWaveformSnapshots();
    void testStreamingWaveforms();
    void testWaveforms(const juce::File& first);
};

/// Exercises completion gating, one-worker admission, selected priority and native waveform bytes.
void PreviewEnrichmentTests::runTest()
{
    const auto first = fixtureFile("sine_mono_44100_16.wav");
    const auto second = fixtureFile("silence_stereo_44100_16.wav");
    testScheduling(first, second);
    testProgress(first, second);
    testPersistence(first);
    testMetadata(first);
    testArtifacts(first);
    testWaveforms(first);
    testStreamingWaveforms();
}

/// Checks admission, selection generations and cancellation with deterministic worker gates.
void PreviewEnrichmentTests::testScheduling(const juce::File& first, const juce::File& second)
{
    beginTest("Selected cache reads precede the gate, but missing repair waits for completion");
    {
        const TemporaryDirectory directory;
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), {}, workFor(probe));
        scheduler.beginForeground();
        scheduler.select(utils::FileIdentity(first), 1);
        expect(probe->waitForCalls(1));
        scheduler.completeForeground({candidateFor(second), candidateFor(first), candidateFor(first)});
        probe->release(1);
        expect(probe->waitForCalls(2));
        const auto calls = probe->snapshot();
        expect(calls.size() >= 2);
        if (calls.size() >= 2) {
            expect(!calls[0].readMissing);
            expect(calls[1].readMissing);
            expect(calls[1].file == first);
        }
        probe->release(2);
        expect(probe->waitForCalls(3));
        const auto afterSelection = probe->snapshot();
        expect(afterSelection.size() == 3 && afterSelection[2].file == second);
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(3));
        expectEquals(probe->getMaximumActive(), 1);
    }

    beginTest("Selection interrupts unselected work and waits for its exit before promotion");
    {
        const TemporaryDirectory directory;
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), {}, workFor(probe));
        scheduler.completeForeground({candidateFor(first), candidateFor(second)});
        expect(probe->waitForCalls(1));
        scheduler.select(utils::FileIdentity(second), 2);
        expectEquals(static_cast<int>(probe->snapshot().size()), 1);
        probe->release(1);
        expect(probe->waitForCalls(2));
        auto calls = probe->snapshot();
        expect(calls.size() >= 2 && calls[0].cancelled && calls[1].file == second);
        scheduler.select(utils::FileIdentity(second), 2);
        probe->release(2);
        expect(probe->waitForCalls(3));
        calls = probe->snapshot();
        expect(calls.size() == 3 && calls[2].file == first);
        expectEquals(probe->getMaximumActive(), 1);
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(3));
    }

    beginTest("A newer same-path selection cannot join an already partially delivered generation");
    {
        const TemporaryDirectory directory;
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), {}, workFor(probe));
        scheduler.select(utils::FileIdentity(first), 1);
        expect(probe->waitForCalls(1));
        scheduler.select(utils::FileIdentity(first), 3);
        probe->release(1);
        expect(probe->waitForCalls(2));
        const auto calls = probe->snapshot();
        expect(calls.size() == 2 && calls[1].file == first);
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(2));
    }

    beginTest("Foreground replacement cancels active work and invalidates queued deliveries");
    {
        const TemporaryDirectory directory;
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), {}, workFor(probe));
        expect(scheduler.isCurrentLoad(1));
        scheduler.completeForeground({candidateFor(first), candidateFor(second)});
        expect(probe->waitForCalls(1));
        scheduler.beginForeground();
        expect(!scheduler.isCurrentLoad(1));
        scheduler.completeForeground({candidateFor(second)});
        probe->release(1);
        expect(probe->waitForCalls(2));
        const auto calls = probe->snapshot();
        expect(calls.size() == 2 && calls[0].cancelled && calls[1].file == second);
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(2));
    }

    beginTest("Removing an unrelated row preserves active repair and invalidates old load delivery");
    {
        const TemporaryDirectory directory;
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), {}, workFor(probe));
        scheduler.completeForeground({candidateFor(first), candidateFor(second)});
        expect(probe->waitForCalls(1));
        scheduler.remove({second.getFullPathName()});
        expect(!scheduler.isCurrentLoad(1));
        probe->release(1);
        expect(probe->waitForCalls(2));
        const auto calls = probe->snapshot();
        expect(calls.size() == 2 && calls[0].cancelled && calls[1].file == first);
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(2));
    }
}

/// Uses subsequent admissions as retirement barriers, without polling sleeps or GUI delivery.
void PreviewEnrichmentTests::testProgress(const juce::File& first, const juce::File& second)
{
    beginTest("Preview progress counts unique retirements, not cache-only reads or interrupted attempts");
    {
        const TemporaryDirectory directory;
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), {}, workFor(probe));
        scheduler.beginForeground();
        scheduler.select(utils::FileIdentity(first), 1);
        expect(probe->waitForCalls(1));
        auto progress = scheduler.getProgress();
        expect(!progress.active);
        expectEquals(progress.completedFiles, 0);
        expectEquals(progress.totalFiles, 0);
        expect(progress.activity.isEmpty());
        scheduler.completeForeground({candidateFor(first), candidateFor(second), candidateFor(first)});
        probe->release(1);
        expect(probe->waitForCalls(2));
        progress = scheduler.getProgress();
        expect(progress.active);
        expectEquals(progress.completedFiles, 0);
        expectEquals(progress.totalFiles, 2);
        expectEquals(progress.activity, juce::String("preview analysis"));

        scheduler.select(utils::FileIdentity(second), 2);
        probe->release(2);
        expect(probe->waitForCalls(3));
        progress = scheduler.getProgress();
        expect(progress.active);
        expectEquals(progress.completedFiles, 0);
        expectEquals(progress.totalFiles, 2);
        const auto interrupted = probe->snapshot();
        expect(interrupted.size() >= 2 && interrupted[1].cancelled);
        probe->release(3);
        expect(probe->waitForCalls(4));
        progress = scheduler.getProgress();
        expect(progress.active);
        expectEquals(progress.completedFiles, 1);
        expectEquals(progress.totalFiles, 2);

        // A later selection of the same path forces another admission after this file retires.
        scheduler.select(utils::FileIdentity(first), 3);
        probe->release(4);
        expect(probe->waitForCalls(5));
        progress = scheduler.getProgress();
        expect(!progress.active);
        expectEquals(progress.completedFiles, 2);
        expectEquals(progress.totalFiles, 2);
        expect(progress.activity.isEmpty());
        scheduler.select(utils::FileIdentity(first), 4);
        probe->release(5);
        expect(probe->waitForCalls(6));
        expectEquals(scheduler.getProgress().completedFiles, 2);

        scheduler.beginForeground();
        progress = scheduler.getProgress();
        expect(!progress.active);
        expectEquals(progress.completedFiles, 0);
        expectEquals(progress.totalFiles, 0);
        scheduler.completeForeground({candidateFor(second)});
        probe->release(6);
        expect(probe->waitForCalls(7));
        // Selection outside the directory pass is useful work but does not inflate its counts.
        progress = scheduler.getProgress();
        expectEquals(progress.completedFiles, 0);
        expectEquals(progress.totalFiles, 1);
        probe->release(7);
        expect(probe->waitForCalls(8));
        progress = scheduler.getProgress();
        expectEquals(progress.completedFiles, 0);
        expectEquals(progress.totalFiles, 1);
        scheduler.stop();
        expect(!scheduler.getProgress().active);
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(8));
    }

    beginTest("Removing candidates adjusts totals without counting cancelled work as completed");
    {
        const TemporaryDirectory directory;
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), {}, workFor(probe));
        scheduler.completeForeground({candidateFor(first), candidateFor(second)});
        expect(probe->waitForCalls(1));
        scheduler.remove({second.getFullPathName()});
        auto progress = scheduler.getProgress();
        expect(progress.active);
        expectEquals(progress.completedFiles, 0);
        expectEquals(progress.totalFiles, 1);
        probe->release(1);
        expect(probe->waitForCalls(2));
        expectEquals(scheduler.getProgress().completedFiles, 0);
        scheduler.remove({first.getFullPathName()});
        progress = scheduler.getProgress();
        expect(!progress.active);
        expectEquals(progress.completedFiles, 0);
        expectEquals(progress.totalFiles, 0);
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(2));
    }
}

/// Verifies explicit edits and committed outputs are persisted before optional repair resumes.
void PreviewEnrichmentTests::testPersistence(const juce::File& first)
{
    beginTest("Explicit gains coalesce off-thread and interrupted selected cache reads resume");
    {
        const TemporaryDirectory directory;
        const auto database = directory.getChildFile("cache.db");
        AnalysisCache cache(database);
        const auto record = analyzeFile(first);
        expect(!record.hasError());
        expect(cache.storeAnalysis(record));
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(database, {}, workFor(probe));
        scheduler.select(utils::FileIdentity(first), 1);
        expect(probe->waitForCalls(1));
        const auto edit = [&](const std::optional<Decibels> gain) {
            const CustomGainUpdate update {
                .file = first,
                .customGain = gain,
                .fileSize = record.fileSize,
                .modifiedTimeMs = record.modifiedTimeMs,
            };
            scheduler.storeCustomGains({update});
        };
        edit(Decibels {1.0f});
        edit(Decibels {3.5f});
        probe->release(1);
        expect(probe->waitForCalls(2));
        const auto gain = cache.getCustomGain(first, record.fileSize, record.modifiedTimeMs);
        expect(gain.has_value());
        if (gain) {
            expectWithinAbsoluteError(gain->value, 3.5f, 0.0001f);
        }
        edit(std::nullopt);
        probe->release(2);
        expect(probe->waitForCalls(3));
        expect(!cache.getCustomGain(first, record.fileSize, record.modifiedTimeMs));
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(3));
        expectEquals(probe->getMaximumActive(), 1);
    }

    beginTest("Queued cached no-ops preserve newer same-source measurements, provenance and cleared gains");
    {
        const TemporaryDirectory directory;
        const auto database = directory.getChildFile("cache.db");
        AnalysisCache cache(database);
        auto original = analyzeFile(first);
        original.customGain = Decibels {2.0f};
        expect(cache.storeAnalysis(original));
        auto captured = cache.getAnalysis(first);
        expect(captured.has_value());
        if (captured) {
            expect(captured->fromCache);
            expect(captured->measurementProducer == MeasurementProducer::cpp);
            const auto probe = std::make_shared<EnrichmentProbe>();
            PreviewEnrichmentScheduler scheduler(database, {}, workFor(probe));
            scheduler.select(utils::FileIdentity(first), 1);
            expect(probe->waitForCalls(1));
            // No-op publication may change its presentation status without producing fresh measurements.
            captured->status = AudioAnalysisStatus::analyzed;
            scheduler.persistOperation(*captured);
            auto replacement = original;
            replacement.measurementProducer = MeasurementProducer::rust;
            replacement.overallPeak = replacement.peakLeft = replacement.peakRight = 0.25f;
            replacement.overallTruePeak = replacement.truePeakLeft = replacement.truePeakRight = 0.3;
            replacement.integratedLufs = -18.0;
            replacement.maxShortTermLufs = -17.0;
            expect(cache.storeAnalysis(replacement));
            expect(cache.storeCustomGain(first, std::nullopt));
            // The next selected admission is a barrier after the queued persistence job has retired.
            scheduler.select(utils::FileIdentity(first), 2);
            probe->release(1);
            expect(probe->waitForCalls(2));
            AnalysisCache observer(database);
            const auto retained = observer.getAnalysis(first);
            expect(retained.has_value());
            if (retained) {
                expect(retained->measurementProducer == MeasurementProducer::rust);
                expectWithinAbsoluteError(retained->overallPeak, replacement.overallPeak, 1.0e-6f);
                expectWithinAbsoluteError(retained->overallTruePeak, replacement.overallTruePeak, 1.0e-12);
                expectWithinAbsoluteError(retained->integratedLufs, replacement.integratedLufs, 1.0e-12);
                expectWithinAbsoluteError(retained->maxShortTermLufs, replacement.maxShortTermLufs, 1.0e-12);
                expect(!retained->customGain);
            }
            scheduler.stop();
            probe->release(std::numeric_limits<std::size_t>::max());
            expect(probe->waitForFinished(2));
        }
    }

    beginTest("Queued output rows never retain transient spectra from direct scheduler callers");
    {
        const TemporaryDirectory directory;
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), {}, workFor(probe));
        scheduler.select(utils::FileIdentity(first), 1);
        expect(probe->waitForCalls(1));
        AudioAnalysisRecord output;
        AudioAnalysisRecord source;
        output.spectrogram = std::make_shared<SpectrogramData>();
        source.spectrogram = std::make_shared<SpectrogramData>();
        const std::weak_ptr<const SpectrogramData> outputSpectrum(output.spectrogram);
        const std::weak_ptr<const SpectrogramData> sourceSpectrum(source.spectrogram);
        scheduler.persistOperation(std::move(output), std::move(source));
        expect(outputSpectrum.expired());
        expect(sourceSpectrum.expired());
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(1));
    }

    beginTest("Committed conversion persists its output and removes its captured source before repair");
    {
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("sine_mono_44100_16.wav", "source.wav");
        const auto outputFile = directory.copyFixture("sine_stereo_44100_16.aif", "output.aif");
        const auto database = directory.getChildFile("cache.db");
        const auto sourceRecord = analyzeFile(source);
        const auto outputRecord = analyzeFile(outputFile);
        expect(!sourceRecord.hasError());
        expect(!outputRecord.hasError());
        AnalysisCache cache(database);
        expect(cache.storeAnalysis(sourceRecord));
        const auto probe = std::make_shared<EnrichmentProbe>();
        PreviewEnrichmentScheduler scheduler(database, {}, workFor(probe));
        scheduler.completeForeground({candidateFor(source), candidateFor(outputFile)});
        expect(probe->waitForCalls(1));
        scheduler.persistOperation(outputRecord, sourceRecord);
        probe->release(1);
        expect(probe->waitForCalls(2));
        {
            // A separate connection proves the writes committed before optional output repair was admitted.
            AnalysisCache persisted(database);
            const auto cached = persisted.getAnalysis(outputFile);
            expect(cached.has_value());
            if (cached) {
                expectEquals(cached->lengthInSamples, outputRecord.lengthInSamples);
                expectEquals(cached->channels, outputRecord.channels);
                expectWithinAbsoluteError(cached->overallPeak, outputRecord.overallPeak, 1.0e-6f);
            }
            expect(!persisted.getAnalysis(source));
            expect(source.existsAsFile());
        }
        const auto calls = probe->snapshot();
        expect(calls.size() == 2 && calls[1].file == outputFile && calls[1].readMissing);
        scheduler.stop();
        probe->release(std::numeric_limits<std::size_t>::max());
        expect(probe->waitForFinished(2));
        expectEquals(probe->getMaximumActive(), 1);
    }
}

/// Checks independent metadata publication and cache-only handling of missing and stable negative tags.
void PreviewEnrichmentTests::testMetadata(const juce::File& first)
{
    beginTest("Successful metadata is published even when independent waveform decoding fails");
    {
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("broken.wav", "broken.wav");
        const utils::FileIdentity identity(source);
        const auto database = directory.getChildFile("cache.db");
        {
            AnalysisCache cache(database);
            for (const auto kind : serato::allTagKinds) {
                const auto result = kind == serato::TagKind::analysis
                    ? serato::TagReadResult::present(serato::Analysis {.majorVersion = 2, .minorVersion = 5})
                    : serato::TagReadResult::missing(kind, serato::ReadStatus::absent);
                expect(serato::storeCachedTag(cache, identity, result));
            }
        }
        const auto deliveries = std::make_shared<EnrichmentDeliveries>();
        PreviewEnrichmentScheduler scheduler(database, [deliveries](PreviewEnrichmentResult result) {
            deliveries->record(std::move(result));
        });
        scheduler.select(identity, 1);
        scheduler.completeForeground({candidateFor(source)});
        expect(deliveries->waitForResults(1));
        const auto results = deliveries->snapshot();
        expect(!results.empty());
        if (!results.empty()) {
            expect(results.front().tagsReady);
            expect(results.front().seratoData && results.front().seratoData->analysis.has_value());
            expect(!results.front().waveform.has_value());
            expectEquals(static_cast<int>(results.front().selectionGeneration), 1);
        }
        scheduler.stop();
    }

    beginTest("Cache-only metadata never parses missing kinds and stable negatives are not retried");
    {
        const TemporaryDirectory directory;
        AnalysisCache cache(directory.getChildFile("cache.db"));
        const utils::FileIdentity identity(first);
        int reads = 0;
        const serato::SeratoPreviewLoader::Reader reader
            = [&reads](const auto&, const std::span<const serato::TagKind> kinds, const auto&) {
                  ++reads;
                  std::vector<serato::TagReadResult> results;
                  for (std::size_t index = 0; index < kinds.size(); ++index) {
                      constexpr std::array statuses {
                          serato::ReadStatus::absent,
                          serato::ReadStatus::unsupported,
                          serato::ReadStatus::invalid,
                      };
                      const auto status = statuses[index % statuses.size()];
                      results.push_back(serato::TagReadResult::missing(kinds[index], status));
                  }
                  return results;
              };
        expect(serato::loadPreviewData(cache, identity, {}, false, reader) == nullptr);
        expectEquals(reads, 0);
        expect(serato::loadPreviewData(cache, identity, {}, true, reader) == nullptr);
        expectEquals(reads, 1);
        expect(serato::loadPreviewData(cache, identity, {}, true, reader) == nullptr);
        expectEquals(reads, 1);
        const auto cached = serato::getCachedTags(cache, identity, serato::allTagKinds);
        expect(std::ranges::all_of(cached, [](const auto& result) { return result.has_value(); }));
    }
}

/// Checks one-pass audio artifacts, independent cache repair, early delivery and stale-source rejection.
void PreviewEnrichmentTests::testArtifacts(const juce::File& first)
{
    beginTest("Combined artifacts decode every frame once in bounded blocks and preserve waveform bytes");
    {
        WaveformTestReader reader;
        reader.lengthInSamples += 7;
        const auto artifacts = buildPreviewAudioArtifacts(reader, true, true);
        expect(artifacts.waveform.has_value());
        expect(artifacts.spectrogram != nullptr);
        expectEquals(reader.framesRead, reader.lengthInSamples);
        expectEquals(reader.reads, 5);
        expect(reader.maximumReadSize <= 16'384);
        if (artifacts.spectrogram) {
            expect(isValidSpectrogramData(*artifacts.spectrogram));
            expectEquals(artifacts.spectrogram->lengthInSamples, static_cast<std::int64_t>(reader.lengthInSamples));
        }
        WaveformTestReader reference;
        reference.lengthInSamples = reader.lengthInSamples;
        expect(artifacts.waveform == buildNativeWaveform(reference));
    }

    beginTest("Optional artifact requests do no unrequested work and spectral-only input decodes once");
    {
        WaveformTestReader none;
        const auto empty = buildPreviewAudioArtifacts(none, false, false);
        expect(!empty.waveform && !empty.spectrogram);
        expectEquals(none.reads, 0);
        WaveformTestReader spectrum;
        const auto artifacts = buildPreviewAudioArtifacts(spectrum, false, true);
        expect(!artifacts.waveform && artifacts.spectrogram != nullptr);
        expectEquals(spectrum.framesRead, spectrum.lengthInSamples);
        expectEquals(spectrum.reads, 4);
    }

    beginTest("Combined cancellation and failed decoding never expose partial audio artifacts");
    {
        WaveformTestReader reader;
        const auto cancelled = buildPreviewAudioArtifacts(reader, true, true, [&reader] { return reader.reads >= 1; });
        expect(!cancelled.waveform && !cancelled.spectrogram);
        expectEquals(reader.reads, 1);
        WaveformTestReader failed;
        failed.readSucceeded = false;
        const auto artifacts = buildPreviewAudioArtifacts(failed, true, true);
        expect(!artifacts.waveform && !artifacts.spectrogram);
        expectEquals(failed.reads, DecodeFailureTracker::maximumConsecutiveFailures);
        WaveformTestReader nonfinite;
        nonfinite.sample = std::numeric_limits<float>::quiet_NaN();
        const auto invalid = buildPreviewAudioArtifacts(nonfinite, true, true);
        expect(!invalid.waveform && !invalid.spectrogram);
    }

    beginTest("Nonfinite PCM in an omitted final bucket still rejects each optional artifact");
    {
        WaveformTestReader reader;
        reader.lengthInSamples = 7;
        reader.sample = std::numeric_limits<float>::quiet_NaN();
        const auto artifacts = buildPreviewAudioArtifacts(reader, true, true);
        expect(!artifacts.waveform);
        expect(!artifacts.spectrogram);
        WaveformTestReader reference;
        reference.lengthInSamples = reader.lengthInSamples;
        reference.sample = reader.sample;
        expect(artifacts.waveform == buildNativeWaveform(reference));
        expectEquals(reader.reads, 1);
    }

    beginTest("Combined source identity checks reject replacements before and after decoding");
    {
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("sine_mono_44100_16.wav", "source.wav");
        const utils::FileIdentity identity(source);
        int checks = 0;
        const auto replaced = buildPreviewAudioArtifacts(identity, true, true, [&] {
            if (++checks == 6) {
                static_cast<void>(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(source));
            }
            return false;
        });
        expect(!replaced.waveform && !replaced.spectrogram);
        const auto stale = buildPreviewAudioArtifacts(identity, true, true);
        expect(!stale.waveform && !stale.spectrogram);
    }

    beginTest("A spectral cache miss preserves measurement and publishes cached waveform before repair");
    {
        const TemporaryDirectory directory;
        const auto database = directory.getChildFile("cache.db");
        AnalysisCache cache(database);
        auto measured = analyzeFile(first);
        measured.spectrogram.reset();
        expect(!measured.hasError());
        expect(cache.storeAnalysis(measured));
        const utils::FileIdentity identity(first);
        const auto waveform = buildNativeWaveform(identity);
        expect(waveform.has_value());
        if (waveform) {
            expect(cache.storeWaveformData(identity, *waveform));
        }
        expect(!cache.getSpectrogramData(first));
        expect(cache.getAnalysis(first).has_value());
        const auto deliveries = std::make_shared<EnrichmentDeliveries>();
        PreviewEnrichmentScheduler scheduler(database, [deliveries](PreviewEnrichmentResult result) {
            deliveries->record(std::move(result));
        });
        scheduler.select(identity, 7);
        expect(deliveries->waitForWaveform());
        const auto cached = deliveries->snapshot();
        expect(std::ranges::any_of(cached, [](const auto& result) {
            return result.waveform.has_value() && !result.spectrogram && !result.spectrogramReady;
        }));
        expect(!cache.getSpectrogramData(first));
        scheduler.completeForeground({candidateFor(first)});
        expect(deliveries->waitForSpectrogram());
        const auto repaired = deliveries->snapshot();
        expect(std::ranges::any_of(repaired, [](const auto& result) {
            return result.spectrogramReady && result.spectrogram && !result.waveform;
        }));
        expect(cache.getSpectrogramData(first) != nullptr);
        const auto retained = cache.getAnalysis(first);
        expect(retained.has_value());
        if (retained) {
            expectWithinAbsoluteError(retained->overallPeak, measured.overallPeak, 1.0e-6f);
            expectEquals(retained->lengthInSamples, measured.lengthInSamples);
        }
        scheduler.stop();
    }

    beginTest("Unavailable spectra complete independently of successful cached tags");
    {
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("broken.wav", "broken.wav");
        const auto deliveries = std::make_shared<EnrichmentDeliveries>();
        PreviewEnrichmentScheduler scheduler(directory.getChildFile("cache.db"), [deliveries](auto result) {
            deliveries->record(std::move(result));
        });
        scheduler.select(utils::FileIdentity(source), 1);
        scheduler.completeForeground({candidateFor(source)});
        expect(deliveries->waitForSpectrogram());
        const auto results = deliveries->snapshot();
        expect(std::ranges::any_of(results, [](const auto& result) { return result.tagsReady; }));
        expect(std::ranges::any_of(results, [](const auto& result) {
            return result.spectrogramReady && !result.spectrogram;
        }));
        scheduler.stop();
    }
}

/// Compares native waveform bytes and rejects invalid geometry, failed decoding and obsolete identities.
void PreviewEnrichmentTests::testWaveforms(const juce::File& first)
{
    beginTest("Waveform-only serialization matches native JUCE complete buckets and empty final point");
    for (const auto* fixtureName :
         {
             "sine_mono_44100_16.wav",
             "sine_stereo_44100_16.wav",
             "silence_stereo_44100_16.wav",
             "sine_stereo_44100_32f.wav",
             "sine_stereo_96000_16.wav",
             "click_mono_44100_16.wav",
         })
    {
        const auto reader = utils::createAudioReader(fixtureFile(fixtureName));
        expect(reader != nullptr, fixtureName);
        if (!reader) {
            continue;
        }
        const auto actual = buildNativeWaveform(*reader);
        expect(actual.has_value(), fixtureName);
        if (!actual) {
            continue;
        }
        const auto detail = samplesPerThumbnailPoint(reader->lengthInSamples);
        const auto completeSamples = static_cast<int>((reader->lengthInSamples / detail) * detail);
        juce::MemoryInputStream input(*actual, false);
        expectEquals(input.readInt(), 0x6d74616a);
        expectEquals(input.readInt(), detail);
        expectEquals(input.readInt64(), reader->lengthInSamples);
        expectEquals(input.readInt64(), static_cast<juce::int64>(completeSamples));
        expectEquals(input.readInt(), static_cast<int>(reader->lengthInSamples / detail + 1));
        expectEquals(input.readInt(), static_cast<int>(reader->numChannels));
        expectEquals(input.readInt(), static_cast<int>(reader->sampleRate));
        juce::AudioBuffer<float> pcm(static_cast<int>(reader->numChannels), completeSamples);
        expect(utils::readFloatSamples(*reader, utils::writePointers(pcm), 0, completeSamples));
        juce::AudioFormatManager formats;
        juce::AudioThumbnailCache thumbnails(1);
        juce::AudioThumbnail reference(detail, formats, thumbnails);
        reference.reset(pcm.getNumChannels(), reader->sampleRate, reader->lengthInSamples);
        reference.addBlock(0, pcm, 0, completeSamples);
        juce::MemoryBlock expected;
        juce::MemoryOutputStream output(expected, false);
        reference.saveTo(output);
        output.flush();
        expect(*actual == expected, fixtureName);
        expectWithinAbsoluteError(
            reference.getTotalLength(), static_cast<double>(reader->lengthInSamples) / reader->sampleRate, 1.0e-12
        );
        expect(!buildNativeWaveform(*reader, [] { return true; }));
    }

    beginTest("Native thumbnail detail keeps its power-of-two bounds across long and empty sources");
    {
        expectEquals(samplesPerThumbnailPoint(0), 1024);
        expectEquals(samplesPerThumbnailPoint(16LL * 16'384), 16);
        expectEquals(samplesPerThumbnailPoint(16LL * 16'385), 32);
        expectEquals(samplesPerThumbnailPoint(std::numeric_limits<juce::int64>::max()), 1024);
    }

    beginTest("Invalid waveform geometry is rejected before any PCM read");
    {
        for (const auto channels : {0U, 3U}) {
            WaveformTestReader reader;
            reader.numChannels = channels;
            expect(!buildNativeWaveform(reader));
            expectEquals(reader.reads, 0);
        }
        for (const auto rate :
             {
                 0.0,
                 -1.0,
                 768'001.0,
                 std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::quiet_NaN(),
             })
        {
            WaveformTestReader reader;
            reader.sampleRate = rate;
            expect(!buildNativeWaveform(reader));
            expectEquals(reader.reads, 0);
        }
        for (const auto length : {juce::int64 {0}, juce::int64 {-1}, juce::int64 {4'194'304} * 1024}) {
            WaveformTestReader reader;
            reader.lengthInSamples = length;
            expect(!buildNativeWaveform(reader));
            expectEquals(reader.reads, 0);
        }
    }

    beginTest("Nonfinite PCM and repeated decode failures never publish a partial native waveform");
    {
        for (const auto sample :
             {
                 std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity(),
                 std::numeric_limits<float>::quiet_NaN(),
             })
        {
            WaveformTestReader reader;
            reader.sample = sample;
            expect(!buildNativeWaveform(reader));
            expectEquals(reader.reads, 1);
        }
        WaveformTestReader reader;
        reader.readSucceeded = false;
        expect(!buildNativeWaveform(reader));
        expectEquals(reader.reads, DecodeFailureTracker::maximumConsecutiveFailures);
    }

    beginTest("Obsolete filesystem identities never open or publish native waveforms");
    {
        const TemporaryDirectory directory;
        const auto source = directory.copyFixture("sine_mono_44100_16.wav", "replaced.wav");
        const utils::FileIdentity identity(source);
        expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(source));
        expect(!buildNativeWaveform(identity));
        expect(!buildNativeWaveform(utils::FileIdentity(directory.getChildFile("missing.wav"))));
    }

    beginTest("Waveform cancellation is checked between bounded native PCM reads");
    {
        const auto reader = utils::createAudioReader(first);
        expect(reader != nullptr);
        if (reader) {
            int checks = 0;
            expect(!buildNativeWaveform(*reader, [&checks] { return ++checks >= 3; }));
            expectEquals(checks, 3);
        }
    }
}

/// Proves streaming chunks preserve native bytes and display snapshots never expose unfinished buckets.
void PreviewEnrichmentTests::testStreamingWaveforms()
{
    testStreamingWaveformChunks();
    testStreamingWaveformSnapshots();
    testStreamingWaveformInvalidInputs();
}

/// Compares arbitrary streaming chunk boundaries against independent native JUCE serialization.
void PreviewEnrichmentTests::testStreamingWaveformChunks()
{
    beginTest("Arbitrary chunks including 4410-frame meter blocks match independent JUCE native bytes");
    for (const auto* fixtureName : {"sine_mono_44100_16.wav", "sine_stereo_44100_32f.wav"}) {
        const auto reader = utils::createAudioReader(fixtureFile(fixtureName));
        expect(reader != nullptr);
        if (!reader) {
            continue;
        }
        const auto channels = static_cast<int>(reader->numChannels);
        const auto length = static_cast<int>(reader->lengthInSamples);
        const auto detail = samplesPerThumbnailPoint(length);
        const auto completeSamples = (length / detail) * detail;
        juce::AudioBuffer<float> pcm(channels, length);
        expect(utils::readFloatSamples(*reader, utils::writePointers(pcm), 0, length));
        juce::AudioFormatManager formats;
        juce::AudioThumbnailCache thumbnails(1);
        juce::AudioThumbnail reference(detail, formats, thumbnails);
        reference.reset(channels, reader->sampleRate, length);
        reference.addBlock(0, pcm, 0, completeSamples);
        juce::MemoryBlock expected;
        {
            juce::MemoryOutputStream output(expected, false);
            reference.saveTo(output);
            output.flush();
        }
        for (const auto chunkSize : {1, 7, 17, 4410, 16'384}) {
            NativeWaveformBuilder builder(channels, static_cast<int>(reader->sampleRate), length);
            expect(!builder.finish());
            for (int position = 0; position < length; position += chunkSize) {
                const auto count = std::min(chunkSize, length - position);
                builder.push(pcm, position, count);
                expectEquals(builder.framesReceived(), static_cast<juce::int64>(position) + count);
            }
            const auto result = builder.finish();
            expect(result.has_value());
            if (result) {
                expect(*result == expected, juce::String(chunkSize));
            }
            expect(!builder.finish());
            expect(!builder.snapshot());
        }
    }
}

/// Checks full-geometry snapshots remain sparse until complete buckets have arrived.
void PreviewEnrichmentTests::testStreamingWaveformSnapshots()
{
    beginTest("Sparse display snapshots retain full geometry and zero partial and unread buckets");
    {
        constexpr int channels {2};
        constexpr int length {71};
        constexpr int detail {16};
        juce::AudioBuffer<float> pcm(channels, length);
        for (int frame = 0; frame < length; ++frame) {
            pcm.setSample(0, frame, frame % 2 == 0 ? -0.5f : 0.25f);
            pcm.setSample(1, frame, frame % 2 == 0 ? -0.125f : 0.75f);
        }
        NativeWaveformBuilder builder(channels, 44'100, length);
        NativeWaveformBuilder whole(channels, 44'100, length);
        whole.push(pcm, 0, length);
        const auto expected = whole.finish();
        expect(expected.has_value());
        for (const auto end : {0, 15, 17, 35, 64, length}) {
            const auto first = static_cast<int>(builder.framesReceived());
            builder.push(pcm, first, end - first);
            const auto snapshot = builder.snapshot();
            expect(snapshot.has_value());
            expectEquals(builder.framesReceived(), static_cast<juce::int64>(end));
            if (!snapshot || !expected) {
                continue;
            }
            expect(isValidWaveformData(*snapshot));
            expectEquals(snapshot->getSize(), expected->getSize());
            juce::MemoryInputStream header(*snapshot, false);
            expectEquals(header.readInt(), 0x6d74616a);
            expectEquals(header.readInt(), detail);
            expectEquals(header.readInt64(), static_cast<juce::int64>(length));
            expectEquals(header.readInt64(), static_cast<juce::int64>(length));
            const auto completeBytes = static_cast<std::size_t>(end / detail) * channels * 2;
            const auto snapshotBytes
                = std::span(static_cast<const std::byte*>(snapshot->getData()), snapshot->getSize());
            const auto expectedBytes
                = std::span(static_cast<const std::byte*>(expected->getData()), expected->getSize());
            expect(
                std::ranges::equal(snapshotBytes.subspan(52, completeBytes), expectedBytes.subspan(52, completeBytes))
            );
            expect(std::ranges::all_of(snapshotBytes.subspan(52 + completeBytes), [](const auto value) {
                return value == std::byte {0};
            }));
            if (end < length) {
                expect(!builder.finish());
            }
        }
        expect(builder.finish() == expected);
    }
}

/// Rejects malformed streaming input without producing cacheable or provisional artifacts.
void PreviewEnrichmentTests::testStreamingWaveformInvalidInputs()
{
    beginTest("Invalid streaming geometry, slices, excess frames and nonfinite tails cannot finish");
    {
        juce::AudioBuffer<float> pcm(1, 33);
        pcm.clear();
        for (const auto length : {juce::int64 {-1}, juce::int64 {0}, juce::int64 {4'194'304} * 1024}) {
            NativeWaveformBuilder builder(1, 44'100, length);
            expect(!builder.snapshot());
            expect(!builder.finish());
        }
        for (const auto channels : {0, 3}) {
            const NativeWaveformBuilder builder(channels, 44'100, 33);
            expect(!builder.snapshot());
        }
        for (const auto rate : {0, -1, 768'001}) {
            const NativeWaveformBuilder builder(1, rate, 33);
            expect(!builder.snapshot());
        }
        for (const auto slice : {std::pair {-1, 1}, std::pair {0, -1}, std::pair {32, 2}, std::pair {0, 34}}) {
            NativeWaveformBuilder builder(1, 44'100, 33);
            builder.push(pcm, slice.first, slice.second);
            expect(!builder.snapshot());
            expect(!builder.finish());
        }
        NativeWaveformBuilder excess(1, 44'100, 32);
        excess.push(pcm, 0, 33);
        expect(!excess.finish());
        NativeWaveformBuilder wrongChannels(2, 44'100, 33);
        wrongChannels.push(pcm, 0, 33);
        expect(!wrongChannels.snapshot());
        for (const auto value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            NativeWaveformBuilder nonfinite(1, 44'100, 33);
            pcm.setSample(0, 32, value);
            nonfinite.push(pcm, 0, 33);
            expect(!nonfinite.snapshot());
            expect(!nonfinite.finish());
        }
    }
}

/// Creates the scheduler and native serializer suite without static registration.
std::unique_ptr<juce::UnitTest> createPreviewEnrichmentTests()
{
    return std::make_unique<PreviewEnrichmentTests>();
}
}  // namespace audiobatch::tests
