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

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <numbers>
#include <sqlite3.h>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Sample rate of the generated files, which gives analysis blocks of 4410 frames.
constexpr double generatedSampleRate {44'100.0};
/// Source frames of one analysis block at the generated sample rate.
constexpr int generatedBlockSize {4410};
/// The block size as a sample count, for history lengths.
constexpr std::int64_t generatedBlockLength {generatedBlockSize};

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
    auto writer = format.createWriterFor(
        stream,
        juce::AudioFormatWriterOptions()
            .withSampleRate(generatedSampleRate)
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

/// Builds a history from per-block flags of a single channel.
std::shared_ptr<const OverloadHistory> buildMono(const std::vector<std::uint8_t>& blocks, const std::int64_t length)
{
    OverloadHistoryBuilder builder(1, generatedSampleRate, length, generatedBlockSize);
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

    /// Runs the overload history checks.
    void runTest() override
    {
        runBuilderTests();
        runBlobTests();
        runProjectionTests();
        runAnalysisTests();
        runCacheTests();
    }

private:
    /// Checks one bucket per block, the final partial bucket, and bounded merging that keeps isolated flags.
    void runBuilderTests()
    {
        beginTest("Each block becomes one bucket, including a final partial block");
        {
            const auto history = buildMono({0, OverloadHistory::sampleOver, 0}, 2 * generatedBlockSize + 10);
            expectEquals(static_cast<int>(history->bucketCount()), 3);
            expectEquals(history->samplesPerBucket, static_cast<std::int64_t>(generatedBlockSize));
            expectEquals(static_cast<int>(history->flagsAt(1, 0)), static_cast<int>(OverloadHistory::sampleOver));
            expect(isValidOverloadHistory(*history));
            expectWithinAbsoluteError(
                history->bucketEndTime(2), (2.0 * generatedBlockSize + 10.0) / generatedSampleRate, 0.0000001
            );
        }

        beginTest("An all-clear history is valid and has no overloads");
        {
            const auto history = buildMono({0, 0}, 2 * generatedBlockLength);
            expect(isValidOverloadHistory(*history));
            expect(!history->hasOverloads());
        }

        beginTest("Long histories merge neighbouring buckets and keep isolated overloads");
        {
            constexpr auto blocks = static_cast<std::int64_t>(OverloadHistory::maximumBuckets) * 3 + 5;
            std::vector<std::uint8_t> flags(static_cast<std::size_t>(blocks), 0);
            flags[1] = OverloadHistory::truePeakOver;
            flags.back() = OverloadHistory::sampleOver;
            const auto history = buildMono(flags, blocks * generatedBlockSize);
            expect(history->bucketCount() <= OverloadHistory::maximumBuckets);
            expectEquals(history->samplesPerBucket, 4 * generatedBlockLength);
            expectEquals(static_cast<int>(history->flagsAt(0, 0)), static_cast<int>(OverloadHistory::truePeakOver));
            expectEquals(
                static_cast<int>(history->flagsAt(history->bucketCount() - 1, 0)),
                static_cast<int>(OverloadHistory::sampleOver)
            );
            expect(isValidOverloadHistory(*history));
        }

        beginTest("Exactly the maximum number of blocks needs no merging");
        {
            const std::vector<std::uint8_t> flags(OverloadHistory::maximumBuckets, 0);
            const auto history
                = buildMono(flags, static_cast<std::int64_t>(OverloadHistory::maximumBuckets) * generatedBlockSize);
            expectEquals(history->bucketCount(), OverloadHistory::maximumBuckets);
            expectEquals(history->samplesPerBucket, static_cast<std::int64_t>(generatedBlockSize));
            expect(isValidOverloadHistory(*history));
        }

        beginTest("Stereo flags stay in their own channel");
        {
            OverloadHistoryBuilder builder(2, generatedSampleRate, generatedBlockSize, generatedBlockSize);
            builder.addBlock(std::array<std::uint8_t, 2> {0, OverloadHistory::sampleOver});
            const auto history = builder.finish();
            expectEquals(static_cast<int>(history->flagsAt(0, 0)), 0);
            expectEquals(static_cast<int>(history->flagsAt(0, 1)), static_cast<int>(OverloadHistory::sampleOver));
        }
    }

    /// Checks the blob round trip and that malformed blobs and inconsistent histories are rejected.
    void runBlobTests()
    {
        const auto history
            = buildMono({OverloadHistory::sampleOver, 0, OverloadHistory::allFlags}, 3 * generatedBlockLength);

        beginTest("An overload history blob round trips");
        {
            const auto data = serializeOverloadHistory(*history);
            const auto restored = deserializeOverloadHistory(data.getData(), data.getSize());
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
                expect(deserializeOverloadHistory(blob.getData(), blob.getSize()) == nullptr, reason);
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
    void runProjectionTests()
    {
        constexpr auto blocks = 1000;
        std::vector<std::uint8_t> flags(blocks, 0);
        flags[500] = OverloadHistory::sampleOver;
        flags[700] = OverloadHistory::truePeakOver;
        const auto history = buildMono(flags, static_cast<std::int64_t>(blocks) * generatedBlockSize);
        const auto duration = static_cast<double>(blocks) * generatedBlockSize / generatedSampleRate;

        beginTest("A subpixel overload still marks its column when zoomed out");
        {
            const auto columns = projectOverloadColumns(*history, 0, {0.0, duration}, 100);
            expectEquals(static_cast<int>(columns[50]), static_cast<int>(OverloadHistory::sampleOver));
            expect((columns[70] & OverloadHistory::truePeakOver) != 0);
            int marked = 0;
            for (const auto value : columns) {
                marked += value != 0 ? 1 : 0;
            }
            expect(marked <= 3, utils::format("{} marked columns", marked));
        }

        beginTest("A sample over keeps its exact bucket while a true peak over also covers the filter delay");
        {
            const auto bucketSeconds = generatedBlockSize / generatedSampleRate;
            const auto sampleStart = 500 * bucketSeconds;
            const auto columns = projectOverloadColumns(
                *history, 0, {sampleStart - bucketSeconds, sampleStart + 2 * bucketSeconds}, 3
            );
            expectEquals(static_cast<int>(columns[0]), 0);
            expectEquals(static_cast<int>(columns[1]), static_cast<int>(OverloadHistory::sampleOver));
            expectEquals(static_cast<int>(columns[2]), 0);

            const auto peakStart = 700 * bucketSeconds;
            const auto peakColumns
                = projectOverloadColumns(*history, 0, {peakStart - bucketSeconds, peakStart + 2 * bucketSeconds}, 3);
            expectEquals(static_cast<int>(peakColumns[0]), static_cast<int>(OverloadHistory::truePeakOver));
            expectEquals(static_cast<int>(peakColumns[1]), static_cast<int>(OverloadHistory::truePeakOver));
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
    void runAnalysisTests()
    {
        const TemporaryDirectory directory;

        beginTest("Samples above full scale are sample overs per channel, and samples exactly at full scale are not");
        {
            // Four full blocks and a partial one, with spikes in the middle of their blocks.
            constexpr int frames = 4 * generatedBlockSize + 100;
            constexpr int middle = generatedBlockSize / 2;
            const auto file = directory.getChildFile("overs.wav");
            expect(writeFloatWav(file, 2, frames, [](const int channel, const int frame) {
                if (channel == 0 && frame == generatedBlockSize + middle) {
                    return 1.0f;
                }
                if (channel == 0 && frame == generatedBlockSize + middle + 10) {
                    return -1.0f;
                }
                if (channel == 1 && frame == 2 * generatedBlockSize + middle) {
                    return 1.25f;
                }
                if (channel == 0 && frame == 3 * generatedBlockSize + middle) {
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
                    return (history.flagsAt(bucket, channel) & OverloadHistory::sampleOver) != 0;
                };
                expect(!sampleOver(0, 0) && !sampleOver(0, 1));
                expect(!sampleOver(1, 0), "exactly full scale");
                expect(!sampleOver(2, 0) && sampleOver(2, 1));
                expect(sampleOver(3, 0) && !sampleOver(3, 1));
                expect(!sampleOver(4, 0) && !sampleOver(4, 1));
                expect((history.flagsAt(2, 1) & OverloadHistory::truePeakOver) != 0);
                expect(overloadHistoryMatches(history, record));
            }
            expectWithinAbsoluteError(record.overallPeak, -1.5f, 0.0001f);
        }

        beginTest("A true peak above full scale between samples below it is a true peak over only");
        {
            // A quarter sample rate sine sampled 45 degrees from its peaks reaches 1.2 between samples of 0.85.
            constexpr int frames = 2 * generatedBlockSize;
            const auto file = directory.getChildFile("intersample.wav");
            expect(writeFloatWav(file, 1, frames, [](int, const int frame) {
                const auto phase = std::numbers::pi / 2.0 * frame + std::numbers::pi / 4.0;
                return static_cast<float>(1.2 * std::sin(phase));
            }));
            const auto record = analyzeFile(file);
            expect(!record.hasError(), record.errorMessage);
            expect(record.overloads != nullptr);
            if (record.overloads) {
                for (std::size_t bucket = 0; bucket < record.overloads->bucketCount(); ++bucket) {
                    expectEquals(
                        static_cast<int>(record.overloads->flagsAt(bucket, 0)),
                        static_cast<int>(OverloadHistory::truePeakOver)
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

    /// Checks the cache round trip, and that old or malformed rows become misses that keep the custom gain.
    void runCacheTests()
    {
        const TemporaryDirectory directory;
        const auto databaseFile = directory.getChildFile("overloads.db");
        const auto file = directory.getChildFile("cached_overs.wav");
        expect(writeFloatWav(file, 2, 3 * generatedBlockSize, [](const int channel, const int frame) {
            return channel == 1 && frame == generatedBlockSize + 7 ? 2.0f : 0.0f;
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
};

/// Creates the overload history test suite.
std::unique_ptr<juce::UnitTest> createOverloadHistoryTests()
{
    return std::make_unique<OverloadHistoryTests>();
}
}  // namespace audiobatch::tests
