/// Tests for the source overload history of an analysis.
/// Covers the builder's bounded merging, its validation and cache blob format,
/// the column projection that keeps short overloads visible when zoomed out,
/// the flags analyzeFile collects from generated 32-bit float files,
/// and the cache round trip with old and malformed rows triggering reanalysis.

#include "analysis/AnalysisCache.h"
#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/OverloadHistory.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/StringFormat.h"

#include <JuceHeader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ebur128.h>
#include <functional>
#include <memory>
#include <numbers>
#include <span>
#include <sqlite3.h>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Sample rate of the generated files, which gives analysis blocks of 4410 frames.
constexpr double GENERATED_SAMPLE_RATE {44'100.0};
/// Source frames of one analysis block at the generated sample rate.
constexpr int GENERATED_BLOCK_SIZE {4410};
/// The block size as a sample count, for history lengths.
constexpr std::int64_t GENERATED_BLOCK_LENGTH {GENERATED_BLOCK_SIZE};

/// Writes a 32-bit float WAV file whose samples the generator returns for each channel and frame.
bool writeFloatWav(
    const juce::File& file,
    const int channels,
    const int frames,
    const std::function<float(int channel, int frame)>& generator
)
{
    juce::AudioBuffer<float> buffer(channels, frames);
    for (int channel = 0; channel < channels; ++channel) {
        for (int frame = 0; frame < frames; ++frame) {
            buffer.setSample(channel, frame, generator(channel, frame));
        }
    }
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    juce::WavAudioFormat format;
    const auto writer = format.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions()
            .withSampleRate(GENERATED_SAMPLE_RATE)
            .withNumChannels(channels)
            .withBitsPerSample(32)
            .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)
    );
    return writer != nullptr && writer->writeFromAudioSampleBuffer(buffer, 0, frames);
}

/// Runs an SQL statement against a database file through a separate connection.
bool runSql(const juce::File& databaseFile, const char* sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open(databaseFile.getFullPathName().toRawUTF8(), &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    sqlite3_close(database);
    return result == SQLITE_OK;
}

/// Releases a reference meter used to pin block-local true-peak coverage.
void destroyReferenceMeter(ebur128_state* meter)
{
    ebur128_destroy(&meter);
}

/// Builds a history from per-block flags of a single channel.
std::shared_ptr<const OverloadHistory> buildMono(const std::vector<std::uint8_t>& blocks, const std::int64_t length)
{
    OverloadHistoryBuilder builder(1, GENERATED_SAMPLE_RATE, length, GENERATED_BLOCK_SIZE);
    for (const auto flags : blocks) {
        builder.addBlock(std::array {flags});
    }
    return builder.finish();
}
}  // namespace

/// Checks the overload history from block flags to cached blobs and waveform columns.
class OverloadHistoryTests final : public juce::UnitTest
{
public:
    OverloadHistoryTests() : juce::UnitTest("Overload history", "AudioBatch") {}

    void runTest() override;

private:
    void runAnalysisTests();
    void runBlobTests();
    void runBoundaryTests();
    void runBuilderTests();
    void runCacheTests();
    void runProjectionTests();
};

/// Runs the overload history checks.
void OverloadHistoryTests::runTest()
{
    runBuilderTests();
    runBlobTests();
    runProjectionTests();
    runAnalysisTests();
    runBoundaryTests();
    runCacheTests();
}

/// Checks one bucket per block, the final partial bucket, and bounded merging that keeps isolated flags.
void OverloadHistoryTests::runBuilderTests()
{
    beginTest("Each block becomes one bucket, including a final partial block");
    {
        const auto history = buildMono({0, OverloadHistory::SAMPLE_OVER, 0}, 2 * GENERATED_BLOCK_SIZE + 10);
        expectEquals(static_cast<int>(history->bucketCount()), 3);
        expectEquals(history->samplesPerBucket, static_cast<std::int64_t>(GENERATED_BLOCK_SIZE));
        expectEquals(static_cast<int>(history->flagsAt(1, 0)), static_cast<int>(OverloadHistory::SAMPLE_OVER));
        expect(isValidOverloadHistory(*history));
        expectWithinAbsoluteError(
            history->bucketEndTime(2), (2.0 * GENERATED_BLOCK_SIZE + 10.0) / GENERATED_SAMPLE_RATE, 0.0000001
        );
    }

    beginTest("An all-clear history is valid and has no overloads");
    {
        const auto history = buildMono({0, 0}, 2 * GENERATED_BLOCK_LENGTH);
        expect(isValidOverloadHistory(*history));
        expect(!history->hasOverloads());
    }

    beginTest("Long histories merge neighbouring buckets and keep isolated overloads");
    {
        constexpr auto blocks = static_cast<std::int64_t>(OverloadHistory::MAXIMUM_BUCKETS) * 3 + 5;
        std::vector<std::uint8_t> flags(static_cast<std::size_t>(blocks), 0);
        flags[1] = OverloadHistory::TRUE_PEAK_OVER;
        flags.back() = OverloadHistory::SAMPLE_OVER;
        const auto history = buildMono(flags, blocks * GENERATED_BLOCK_SIZE);
        expect(history->bucketCount() <= OverloadHistory::MAXIMUM_BUCKETS);
        expectEquals(history->samplesPerBucket, 4 * GENERATED_BLOCK_LENGTH);
        expectEquals(static_cast<int>(history->flagsAt(0, 0)), static_cast<int>(OverloadHistory::TRUE_PEAK_OVER));
        expectEquals(
            static_cast<int>(history->flagsAt(history->bucketCount() - 1, 0)),
            static_cast<int>(OverloadHistory::SAMPLE_OVER)
        );
        expect(isValidOverloadHistory(*history));
    }

    beginTest("Exactly the maximum number of blocks needs no merging");
    {
        const std::vector<std::uint8_t> flags(OverloadHistory::MAXIMUM_BUCKETS, 0);
        const auto history
            = buildMono(flags, static_cast<std::int64_t>(OverloadHistory::MAXIMUM_BUCKETS) * GENERATED_BLOCK_SIZE);
        expectEquals(history->bucketCount(), OverloadHistory::MAXIMUM_BUCKETS);
        expectEquals(history->samplesPerBucket, static_cast<std::int64_t>(GENERATED_BLOCK_SIZE));
        expect(isValidOverloadHistory(*history));
    }

    beginTest("Stereo flags stay in their own channel");
    {
        OverloadHistoryBuilder builder(2, GENERATED_SAMPLE_RATE, GENERATED_BLOCK_SIZE, GENERATED_BLOCK_SIZE);
        builder.addBlock(std::array<std::uint8_t, 2> {0, OverloadHistory::SAMPLE_OVER});
        const auto history = builder.finish();
        expectEquals(static_cast<int>(history->flagsAt(0, 0)), 0);
        expectEquals(static_cast<int>(history->flagsAt(0, 1)), static_cast<int>(OverloadHistory::SAMPLE_OVER));
    }
}

/// Checks the blob round trip and that malformed blobs and inconsistent histories are rejected.
void OverloadHistoryTests::runBlobTests()
{
    const auto history
        = buildMono({OverloadHistory::SAMPLE_OVER, 0, OverloadHistory::ALL_FLAGS}, 3 * GENERATED_BLOCK_LENGTH);

    beginTest("An overload history blob round trips");
    {
        const auto data = serializeOverloadHistory(*history);
        const auto restored
            = deserializeOverloadHistory(std::span(static_cast<const std::byte*>(data.getData()), data.getSize()));
        expect(restored != nullptr);
        if (restored) {
            expect(restored->flags == history->flags);
            expectEquals(restored->samplesPerBucket, history->samplesPerBucket);
            expectEquals(restored->channelCount, 1);
        }
    }

    beginTest("Malformed overload history blobs are rejected");
    {
        const auto data = serializeOverloadHistory(*history);
        const auto rejects = [this](juce::MemoryBlock blob, const juce::String& reason) {
            expect(
                deserializeOverloadHistory(std::span(static_cast<const std::byte*>(blob.getData()), blob.getSize()))
                    == nullptr,
                reason
            );
        };
        rejects({}, "empty");
        auto truncated = data;
        truncated.setSize(data.getSize() - 1);
        rejects(truncated, "truncated");
        auto extended = data;
        extended.append("\0", 1);
        rejects(extended, "extended");
        auto badMagic = data;
        badMagic[0] = 0;
        rejects(badMagic, "magic");
        auto badVersion = data;
        badVersion[4] = 2;
        rejects(badVersion, "version");
        auto illegalBits = data;
        illegalBits[data.getSize() - 1] = static_cast<char>(0x04);
        rejects(illegalBits, "flag bits");
        auto badStride = data;
        // The samples per bucket field follows the magic, version, sample rate, and length.
        badStride[24] = static_cast<char>(badStride[24] + 1);
        rejects(badStride, "stride");
        auto badChannels = data;
        badChannels[32] = 2;
        rejects(badChannels, "channels");
    }

    beginTest("A history that is not bounded with the smallest stride is invalid");
    {
        auto unmerged = *history;
        unmerged.samplesPerBucket *= 2;
        expect(!isValidOverloadHistory(unmerged));
        expect(serializeOverloadHistory(unmerged).isEmpty());
    }
}

/// Checks that columns merge subpixel overloads and that true peaks cover the interpolation delay.
void OverloadHistoryTests::runProjectionTests()
{
    constexpr auto blocks = 1000;
    std::vector<std::uint8_t> flags(blocks, 0);
    flags[500] = OverloadHistory::SAMPLE_OVER;
    flags[700] = OverloadHistory::TRUE_PEAK_OVER;
    const auto history = buildMono(flags, static_cast<std::int64_t>(blocks) * GENERATED_BLOCK_SIZE);
    const auto duration = static_cast<double>(blocks) * GENERATED_BLOCK_SIZE / GENERATED_SAMPLE_RATE;

    beginTest("A subpixel overload still marks its column when zoomed out");
    {
        const auto columns = projectOverloadColumns(*history, 0, {0.0, duration}, 100);
        expectEquals(static_cast<int>(columns[50]), static_cast<int>(OverloadHistory::SAMPLE_OVER));
        expect((columns[70] & OverloadHistory::TRUE_PEAK_OVER) != 0);
        int marked = 0;
        for (const auto value : columns) {
            marked += value != 0 ? 1 : 0;
        }
        expect(marked <= 3, utils::format("{} marked columns", marked));
    }

    beginTest("A sample over keeps its exact bucket while a true peak over also covers the filter delay");
    {
        const auto bucketSeconds = GENERATED_BLOCK_SIZE / GENERATED_SAMPLE_RATE;
        const auto sampleStart = 500 * bucketSeconds;
        const auto columns
            = projectOverloadColumns(*history, 0, {sampleStart - bucketSeconds, sampleStart + 2 * bucketSeconds}, 3);
        expectEquals(static_cast<int>(columns[0]), 0);
        expectEquals(static_cast<int>(columns[1]), static_cast<int>(OverloadHistory::SAMPLE_OVER));
        expectEquals(static_cast<int>(columns[2]), 0);

        const auto peakStart = 700 * bucketSeconds;
        const auto peakColumns
            = projectOverloadColumns(*history, 0, {peakStart - bucketSeconds, peakStart + 2 * bucketSeconds}, 3);
        expectEquals(static_cast<int>(peakColumns[0]), static_cast<int>(OverloadHistory::TRUE_PEAK_OVER));
        expectEquals(static_cast<int>(peakColumns[1]), static_cast<int>(OverloadHistory::TRUE_PEAK_OVER));
        expectEquals(static_cast<int>(peakColumns[2]), 0);
    }

    beginTest("Ranges outside the file and unknown channels project nothing");
    {
        for (const auto value : projectOverloadColumns(*history, 0, {duration + 1.0, duration + 2.0}, 10)) {
            expectEquals(static_cast<int>(value), 0);
        }
        for (const auto value : projectOverloadColumns(*history, 1, {0.0, duration}, 10)) {
            expectEquals(static_cast<int>(value), 0);
        }
    }
}

/// Checks the flags analyzeFile collects from generated float files.
void OverloadHistoryTests::runAnalysisTests()
{
    const TemporaryDirectory directory;

    beginTest("Samples above full scale are sample overs per channel, and samples exactly at full scale are not");
    {
        // Four full blocks and a partial one, with spikes in the middle of their blocks.
        constexpr int frames = 4 * GENERATED_BLOCK_SIZE + 100;
        constexpr int middle = GENERATED_BLOCK_SIZE / 2;
        const auto file = directory.getChildFile("overs.wav");
        expect(writeFloatWav(file, 2, frames, [](const int channel, const int frame) {
            if (channel == 0 && frame == GENERATED_BLOCK_SIZE + middle) {
                return 1.0f;
            }
            if (channel == 0 && frame == GENERATED_BLOCK_SIZE + middle + 10) {
                return -1.0f;
            }
            if (channel == 1 && frame == 2 * GENERATED_BLOCK_SIZE + middle) {
                return 1.25f;
            }
            if (channel == 0 && frame == 3 * GENERATED_BLOCK_SIZE + middle) {
                return -1.5f;
            }
            return 0.0f;
        }));
        const auto record = analyzeFile(file);
        expect(!record.hasError(), record.errorMessage);
        expect(record.overloads != nullptr);
        if (record.overloads) {
            const auto& history = *record.overloads;
            expectEquals(static_cast<int>(history.bucketCount()), 5);
            expectEquals(history.channelCount, 2);
            const auto sampleOver = [&history](const std::size_t bucket, const int channel) {
                return (history.flagsAt(bucket, channel) & OverloadHistory::SAMPLE_OVER) != 0;
            };
            expect(!sampleOver(0, 0) && !sampleOver(0, 1));
            expect(!sampleOver(1, 0), "exactly full scale");
            expect(!sampleOver(2, 0) && sampleOver(2, 1));
            expect(sampleOver(3, 0) && !sampleOver(3, 1));
            expect(!sampleOver(4, 0) && !sampleOver(4, 1));
            expect((history.flagsAt(2, 1) & OverloadHistory::TRUE_PEAK_OVER) != 0);
            expect(overloadHistoryMatches(history, record));
        }
        expectWithinAbsoluteError(record.overallPeak, -1.5f, 0.0001f);
    }

    beginTest("A true peak above full scale between samples below it is a true peak over only");
    {
        // A quarter sample rate sine sampled 45 degrees from its peaks reaches 1.2 between samples of 0.85.
        constexpr int frames = 2 * GENERATED_BLOCK_SIZE;
        const auto file = directory.getChildFile("intersample.wav");
        expect(writeFloatWav(file, 1, frames, [](int, const int frame) {
            const auto phase = std::numbers::pi * 0.5 * frame + std::numbers::pi / 4.0;
            return static_cast<float>(1.2 * std::sin(phase));
        }));
        const auto record = analyzeFile(file);
        expect(!record.hasError(), record.errorMessage);
        expect(record.overloads != nullptr);
        if (record.overloads) {
            for (std::size_t bucket = 0; bucket < record.overloads->bucketCount(); ++bucket) {
                expectEquals(
                    static_cast<int>(record.overloads->flagsAt(bucket, 0)),
                    static_cast<int>(OverloadHistory::TRUE_PEAK_OVER)
                );
            }
        }
        expect(std::abs(record.overallPeak) < 1.0f);
        expect(record.overallTruePeak > 1.0);
    }

    beginTest("Digital silence has a valid all-clear history");
    {
        const auto record = analyzeFile(fixtureFile("silence_stereo_44100_16.wav"));
        expect(record.overloads != nullptr);
        if (record.overloads) {
            expect(!record.overloads->hasOverloads());
            expect(overloadHistoryMatches(*record.overloads, record));
        }
    }
}

/// Pins continuous interpolation across blocks and the analyzer's unflushed EOF coverage.
void OverloadHistoryTests::runBoundaryTests()
{
    beginTest("True-peak interpolation crosses analysis blocks and EOF retains only the existing source coverage");
    const TemporaryDirectory directory;
    constexpr int frames = GENERATED_BLOCK_SIZE + 32;
    const auto sample = [](const int frame) {
        if (frame >= GENERATED_BLOCK_SIZE - 4 && frame < GENERATED_BLOCK_SIZE) {
            return frame % 4 < 2 ? 0.85f : -0.85f;
        }
        return frame == frames - 1 ? 0.99f : 0.0f;
    };
    const auto file = directory.getChildFile("boundary.wav");
    expect(writeFloatWav(file, 1, frames, [sample](int, const int frame) { return sample(frame); }));
    const auto record = analyzeFile(file);
    expect(!record.hasError(), record.errorMessage);
    const std::unique_ptr<ebur128_state, decltype(&destroyReferenceMeter)> meter(
        ebur128_init(1, static_cast<unsigned long>(GENERATED_SAMPLE_RATE), EBUR128_MODE_TRUE_PEAK),
        destroyReferenceMeter
    );
    expect(meter != nullptr);
    expect(record.overloads != nullptr);
    if (!meter || !record.overloads) {
        return;
    }
    std::vector<float> samples(static_cast<std::size_t>(frames));
    for (int frame = 0; frame < frames; ++frame) {
        samples[static_cast<std::size_t>(frame)] = sample(frame);
    }
    for (std::size_t bucket = 0; bucket < 2; ++bucket) {
        const auto offset = bucket * static_cast<std::size_t>(GENERATED_BLOCK_SIZE);
        const auto block = std::span(samples).subspan(offset).first(
            std::min(samples.size() - offset, static_cast<std::size_t>(GENERATED_BLOCK_SIZE))
        );
        expectEquals(
            ebur128_add_frames_float(meter.get(), block.data(), block.size()), static_cast<int>(EBUR128_SUCCESS)
        );
        double peak = 0.0;
        expectEquals(ebur128_prev_true_peak(meter.get(), 0, &peak), static_cast<int>(EBUR128_SUCCESS));
        const auto expected = peak > 1.0 ? OverloadHistory::TRUE_PEAK_OVER : 0;
        expectEquals(static_cast<int>(record.overloads->flagsAt(bucket, 0)), static_cast<int>(expected));
    }
    expect((record.overloads->flagsAt(1, 0) & OverloadHistory::TRUE_PEAK_OVER) != 0);
    expectEquals(record.overloads->lengthInSamples, static_cast<std::int64_t>(frames));
    double unflushedPeak = 0.0;
    expectEquals(ebur128_true_peak(meter.get(), 0, &unflushedPeak), static_cast<int>(EBUR128_SUCCESS));
    expectWithinAbsoluteError(record.overallTruePeak, unflushedPeak, 0.000001);
}

/// Checks the cache round trip, and that old or malformed rows become misses that keep the custom gain.
void OverloadHistoryTests::runCacheTests()
{
    const TemporaryDirectory directory;
    const auto databaseFile = directory.getChildFile("overloads.db");
    const auto file = directory.getChildFile("cached_overs.wav");
    expect(writeFloatWav(file, 2, 3 * GENERATED_BLOCK_SIZE, [](const int channel, const int frame) {
        return channel == 1 && frame == GENERATED_BLOCK_SIZE + 7 ? 2.0f : 0.0f;
    }));
    const auto record = analyzeFile(file);
    expect(record.overloads && record.overloads->hasOverloads());
    AnalysisCache cache(databaseFile);

    beginTest("Overload history survives the analysis cache");
    expect(cache.storeAnalysis(record));
    expect(cache.storeCustomGain(file, Decibels {-1.5f}));
    {
        const auto cached = cache.getAnalysis(file);
        expect(cached.has_value() && cached->overloads != nullptr);
        if (cached && cached->overloads && record.overloads) {
            expect(cached->overloads->flags == record.overloads->flags);
            expectEquals(cached->overloads->samplesPerBucket, record.overloads->samplesPerBucket);
        }
    }

    beginTest("A record without overload history is not cached as all-clear");
    {
        auto missing = record;
        missing.overloads.reset();
        expect(!cache.storeAnalysis(missing));
    }

    beginTest("Missing, old, and malformed overload history rows need reanalysis");
    for (const auto* mutation :
         {
             "overload_history = NULL",
             "overload_history = X'4F564C44'",
             "overload_history = CAST(overload_history AS TEXT)",
             "overload_history = substr(overload_history, 1, length(overload_history) - 1)",
             "overload_history = CAST(overload_history || X'00' AS BLOB)",
             "overload_history = CAST(substr(overload_history, 1, length(overload_history) - 1) || X'08' AS BLOB)",
             "analysis_version = 11",
         })
    {
        expect(cache.storeAnalysis(record));
        expect(runSql(databaseFile, utils::format("UPDATE file_analysis SET {};", mutation).toRawUTF8()));
        expect(!cache.getAnalysis(file).has_value(), mutation);
    }

    beginTest("A reanalyzed row keeps the custom gain of the unchanged file");
    {
        expect(cache.storeAnalysis(analyzeFile(file)));
        const auto cached = cache.getAnalysis(file);
        expect(cached.has_value() && cached->customGain.has_value());
        expectEquals(cached.value_or(AudioAnalysisRecord {}).customGain.value_or(Decibels {}).value, -1.5f);
    }
}

/// Creates the overload history test suite.
std::unique_ptr<juce::UnitTest> createOverloadHistoryTests()
{
    return std::make_unique<OverloadHistoryTests>();
}
}  // namespace audiobatch::tests
