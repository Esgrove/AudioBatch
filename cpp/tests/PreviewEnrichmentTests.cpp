/// Focused admission and native waveform tests without windows, rendering, or source-backed thumbnails.
/// Deterministic worker gates check the completion boundary, selection promotion and cooperative cancellation.
/// Native JUCE serialization is the independent reference for thumbnail quantization and final-bucket geometry.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/DecodeFailureTracker.h"
#include "gui/NativeWaveformBuilder.h"
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
    bool readMissing {false};
    bool cancelled {false};
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
        for (auto* channel : std::span(destination, static_cast<std::size_t>(channelCount))) {
            if (channel != nullptr) {
                const auto samples = std::span(channel, static_cast<std::size_t>(destinationOffset + sampleCount))
                                         .subspan(static_cast<std::size_t>(destinationOffset));
                std::ranges::fill(samples, std::bit_cast<int>(sample));
            }
        }
        return readSucceeded;
    }

    /// PCM value delivered for every decoded sample, including nonfinite rejection cases.
    float sample {0.25f};
    /// Result reported after supplying PCM for a block.
    bool readSucceeded {true};
    /// Blocks requested, proving invalid geometry is rejected before decoding.
    int reads {0};
};

/// Holds worker input by shared ownership, so scheduler shutdown never leaves dangling test captures.
class EnrichmentProbe final
{
public:
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

private:
    static constexpr auto watchdog {std::chrono::milliseconds(500)};
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<EnrichmentCall> calls;
    std::size_t released {0};
    std::size_t finished {0};
    int active {0};
    int maximumActive {0};
};

/// Captures independently published metadata without requiring message-thread delivery.
class EnrichmentDeliveries final
{
public:
    /// Records worker completion under a test-owned lock, never waiting for the GUI thread.
    void record(PreviewEnrichmentResult result)
    {
        const std::scoped_lock lock(mutex);
        results.push_back(std::move(result));
        condition.notify_all();
    }

    /// Waits for independent artifact publication, with a deadlock watchdog only.
    bool waitForResults(const std::size_t count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::milliseconds(500), [&] { return results.size() >= count; });
    }

    /// Returns owned snapshots so the detached worker cannot race test assertions.
    std::vector<PreviewEnrichmentResult> snapshot() const
    {
        const std::scoped_lock lock(mutex);
        return results;
    }

private:
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<PreviewEnrichmentResult> results;
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

    /// Exercises completion gating, one-worker admission, selected priority and native waveform bytes.
    void runTest() override
    {
        const auto first = fixtureFile("sine_mono_44100_16.wav");
        const auto second = fixtureFile("silence_stereo_44100_16.wav");
        testScheduling(first, second);
        testPersistence(first);
        testMetadata(first);
        testWaveforms(first);
    }

private:
    /// Checks admission, selection generations and cancellation with deterministic worker gates.
    void testScheduling(const juce::File& first, const juce::File& second)
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

    /// Verifies explicit edits and committed outputs are persisted before optional repair resumes.
    void testPersistence(const juce::File& first)
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
    void testMetadata(const juce::File& first)
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

    /// Compares native waveform bytes and rejects invalid geometry, failed decoding and obsolete identities.
    void testWaveforms(const juce::File& first)
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
};

/// Creates the scheduler and native serializer suite without static registration.
std::unique_ptr<juce::UnitTest> createPreviewEnrichmentTests()
{
    return std::make_unique<PreviewEnrichmentTests>();
}
}  // namespace audiobatch::tests
