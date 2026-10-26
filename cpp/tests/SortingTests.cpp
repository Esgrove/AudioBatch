/// Tests for the results table sorting in ResultsSorting.

#include "gui/GuiLayout.h"
#include "gui/ResultsSorting.h"
#include "TestSuites.h"
#include "utils/StringFormat.h"

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
namespace
{
static_assert(!tableColumnFromId(0).has_value());
static_assert(tableColumnFromId(1) == AudioFileTableColumn::name);
static_assert(tableColumnFromId(std::to_underlying(AudioFileTableColumn::length)) == AudioFileTableColumn::length);
static_assert(!tableColumnFromId(std::to_underlying(AudioFileTableColumn::length) + 1).has_value());
static_assert(toColumnId(AudioFileTableColumn::status) == std::to_underlying(AudioFileTableColumn::status));
static_assert(toColumnId(std::nullopt) == 0);
static_assert(!fileMenuItemFromId(0).has_value());
static_assert(fileMenuItemFromId(std::to_underlying(FileMenuItemId::reanalyze)) == FileMenuItemId::reanalyze);
static_assert(!fileMenuItemFromId(std::to_underlying(FileMenuItemId::reanalyze) + 1).has_value());

/// Builds an analyzed record with the given name, peak, and integrated loudness.
AudioAnalysisRecord makeRecord(const juce::String& name, const float peak, const double integratedLufs)
{
    AudioAnalysisRecord record;
    record.fileName = name;
    record.fullPath = "/music/" + name;
    record.overallPeak = peak;
    record.integratedLufs = integratedLufs;
    record.status = AudioAnalysisStatus::analyzed;
    return record;
}

/// Returns the file names of the records in their current order.
juce::StringArray namesOf(const std::vector<AudioAnalysisRecord>& records)
{
    juce::StringArray names;
    for (const auto& record : records) {
        names.add(record.fileName);
    }
    return names;
}
}  // namespace

/// Checks the column comparators and the primary and secondary sort order.
class SortingTests final : public juce::UnitTest
{
public:
    SortingTests() : juce::UnitTest("Results sorting", "AudioBatch") {}

    /// Runs the sorting checks.
    void runTest() override
    {
        beginTest("Bit depth sorts numerically in both directions, with unavailable depths first");
        {
            auto unknown = makeRecord("unknown", 0.5f, -20.0);
            auto eightBit = makeRecord("eight", 0.5f, -20.0);
            auto sixteenBit = makeRecord("sixteen", 0.5f, -20.0);
            auto twentyFourBit = makeRecord("twenty-four", 0.5f, -20.0);
            unknown.bitsPerSample = 0;
            eightBit.bitsPerSample = 8;
            sixteenBit.bitsPerSample = 16;
            twentyFourBit.bitsPerSample = 24;
            expectEquals(
                results_sorting::compareRecordsByColumn(sixteenBit, sixteenBit, AudioFileTableColumn::bitDepth), 0
            );
            expect(results_sorting::compareRecordsByColumn(eightBit, sixteenBit, AudioFileTableColumn::bitDepth) < 0);

            std::vector records {twentyFourBit, sixteenBit, unknown, eightBit};
            results_sorting::SortState state {
                .primaryColumn = AudioFileTableColumn::bitDepth,
                .secondaryColumn = std::nullopt,
                .statusText = {},
            };
            results_sorting::sortRecords(records, state);
            expect(namesOf(records) == juce::StringArray {"unknown", "eight", "sixteen", "twenty-four"});
            state.primaryForwards = false;
            results_sorting::sortRecords(records, state);
            expect(namesOf(records) == juce::StringArray {"twenty-four", "sixteen", "eight", "unknown"});
        }

        beginTest("Peaks compare by magnitude regardless of polarity");
        {
            const auto quiet = makeRecord("quiet", -0.25f, -20.0);
            const auto loud = makeRecord("loud", 0.5f, -10.0);
            expect(results_sorting::compareRecordsByColumn(quiet, loud, AudioFileTableColumn::overallPeak) < 0);
        }

        beginTest("Peaks that differ by the smallest step still order strictly");
        {
            const auto lower = makeRecord("lower", 0.5f, -20.0);
            const auto higher = makeRecord("higher", std::nextafter(0.5f, 1.0f), -20.0);
            expect(results_sorting::compareRecordsByColumn(lower, higher, AudioFileTableColumn::overallPeak) < 0);
            expect(results_sorting::compareRecordsByColumn(higher, lower, AudioFileTableColumn::overallPeak) > 0);
            expectEquals(results_sorting::compareRecordsByColumn(lower, lower, AudioFileTableColumn::overallPeak), 0);
        }

        beginTest("Silent loudness sorts below every measurable value");
        {
            const auto silent = makeRecord("silent", 0.0f, AudioAnalysisRecord::negativeInfinityLoudness);
            const auto quiet = makeRecord("quiet", 0.1f, -40.0);
            expect(results_sorting::compareRecordsByColumn(silent, quiet, AudioFileTableColumn::integratedLufs) < 0);
        }

        beginTest("Primary, secondary, and name ordering, with failed records last");
        {
            auto failed = makeRecord("failed", 0.0f, -10.0);
            failed.status = AudioAnalysisStatus::failed;

            std::vector records {
                failed,
                makeRecord("b", 0.5f, -12.0),
                makeRecord("a", 0.5f, -12.0),
                makeRecord("c", 0.5f, -8.0),
                makeRecord("d", 0.25f, -20.0),
            };

            results_sorting::sortRecords(
                records,
                {
                    .primaryColumn = AudioFileTableColumn::overallPeak,
                    .primaryForwards = false,
                    .secondaryColumn = AudioFileTableColumn::integratedLufs,
                    .secondaryForwards = true,
                    .statusText = {},
                }
            );

            expectEquals(namesOf(records).joinIntoString(","), juce::String("a,b,c,d,failed"));
        }

        beginTest("NaN and negative zero peaks compare consistently");
        {
            const auto nan = makeRecord("nan", std::numeric_limits<float>::quiet_NaN(), -20.0);
            const auto otherNan = makeRecord("other nan", std::numeric_limits<float>::quiet_NaN(), -20.0);
            const auto negativeZero = makeRecord("negative zero", -0.0f, -20.0);
            const auto positiveZero = makeRecord("positive zero", 0.0f, -20.0);
            const auto half = makeRecord("half", 0.5f, -20.0);
            constexpr auto column = AudioFileTableColumn::overallPeak;

            expectEquals(results_sorting::compareRecordsByColumn(nan, otherNan, column), 0);
            expect(results_sorting::compareRecordsByColumn(half, nan, column) < 0);
            expect(results_sorting::compareRecordsByColumn(nan, half, column) > 0);
            expectEquals(results_sorting::compareRecordsByColumn(negativeZero, positiveZero, column), 0);
        }

        beginTest("Silent loudness values tie and fall through to the secondary sort");
        {
            std::vector records {
                makeRecord("louder silent", 0.5f, AudioAnalysisRecord::negativeInfinityLoudness),
                makeRecord("quieter silent", 0.25f, AudioAnalysisRecord::negativeInfinityLoudness * 2.0),
                makeRecord("measured", 0.1f, -30.0),
            };

            results_sorting::sortRecords(
                records,
                {
                    .primaryColumn = AudioFileTableColumn::integratedLufs,
                    .primaryForwards = true,
                    .secondaryColumn = AudioFileTableColumn::overallPeak,
                    .secondaryForwards = true,
                    .statusText = {},
                }
            );

            expectEquals(namesOf(records).joinIntoString(","), juce::String("quieter silent,louder silent,measured"));
        }

        beginTest("Records without a custom gain sort before gains, which sort by value");
        {
            auto boosted = makeRecord("boosted", 0.5f, -12.0);
            boosted.customGain = Decibels {3.0f};
            auto cut = makeRecord("cut", 0.5f, -12.0);
            cut.customGain = Decibels {-3.0f};
            std::vector records {boosted, makeRecord("none", 0.5f, -12.0), cut};

            results_sorting::sortRecords(
                records,
                {
                    .primaryColumn = AudioFileTableColumn::customGain,
                    .primaryForwards = true,
                    .secondaryColumn = std::nullopt,
                    .secondaryForwards = true,
                    .statusText = {},
                }
            );

            expectEquals(namesOf(records).joinIntoString(","), juce::String("none,cut,boosted"));
        }

        beginTest("Descending order keeps ties in ascending name order");
        {
            std::vector records {
                makeRecord("b", 0.5f, -12.0),
                makeRecord("c", 0.25f, -12.0),
                makeRecord("a", 0.5f, -12.0),
            };

            results_sorting::sortRecords(
                records,
                {
                    .primaryColumn = AudioFileTableColumn::overallPeak,
                    .primaryForwards = false,
                    .secondaryColumn = std::nullopt,
                    .secondaryForwards = true,
                    .statusText = {},
                }
            );

            expectEquals(namesOf(records).joinIntoString(","), juce::String("a,b,c"));
        }

        beginTest("Every column comparator is a strict weak ordering on random records");
        {
            checkComparatorsOnRandomRecords();
        }

        beginTest("The status column sorts by the text it shows when one is provided");
        {
            std::vector records {
                makeRecord("first", 0.5f, -12.0),
                makeRecord("second", 0.5f, -12.0),
                makeRecord("third", 0.5f, -12.0),
            };
            const std::map<juce::String, juce::String> shownText {
                {"first", "Reduced 2.0 dB to avoid clipping"},
                {"second", "Analyzed"},
                {"third", "Cached"},
            };

            results_sorting::sortRecords(
                records,
                {
                    .primaryColumn = AudioFileTableColumn::status,
                    .primaryForwards = true,
                    .secondaryColumn = std::nullopt,
                    .secondaryForwards = true,
                    .statusText
                    = [&shownText](const AudioAnalysisRecord& record) { return shownText.at(record.fileName); },
                }
            );

            expectEquals(namesOf(records).joinIntoString(","), juce::String("second,third,first"));
        }

        beginTest("A new primary sort clears a secondary sort on the same column");
        {
            results_sorting::SortState state {
                .primaryColumn = AudioFileTableColumn::overallPeak,
                .secondaryColumn = std::nullopt,
                .statusText = {},
            };
            results_sorting::toggleSecondarySort(state, AudioFileTableColumn::integratedLufs);
            results_sorting::setPrimarySort(state, AudioFileTableColumn::integratedLufs, false);

            expect(state.primaryColumn == AudioFileTableColumn::integratedLufs);
            expect(!state.primaryForwards);
            expect(!state.secondaryColumn.has_value());
            expect(state.secondaryForwards);

            results_sorting::toggleSecondarySort(state, AudioFileTableColumn::overallPeak);
            results_sorting::setPrimarySort(state, AudioFileTableColumn::status, true);
            expect(state.secondaryColumn == AudioFileTableColumn::overallPeak);
        }

        beginTest("Secondary sort clicks set, flip, and clear the secondary column");
        {
            results_sorting::SortState state {
                .primaryColumn = AudioFileTableColumn::overallPeak,
                .secondaryColumn = std::nullopt,
                .statusText = {},
            };

            results_sorting::toggleSecondarySort(state, AudioFileTableColumn::integratedLufs);
            expect(state.secondaryColumn == AudioFileTableColumn::integratedLufs);
            expect(state.secondaryForwards);

            results_sorting::toggleSecondarySort(state, AudioFileTableColumn::integratedLufs);
            expect(!state.secondaryForwards);

            results_sorting::toggleSecondarySort(state, AudioFileTableColumn::status);
            expect(state.secondaryColumn == AudioFileTableColumn::status);
            expect(state.secondaryForwards);

            results_sorting::toggleSecondarySort(state, AudioFileTableColumn::overallPeak);
            expect(!state.secondaryColumn.has_value());
            expect(state.secondaryForwards);
        }
    }

private:
    /// Checks antisymmetry and transitivity of every column comparator on records with random values,
    /// including NaN, negative zero, silence, and repeated values.
    /// std::ranges::sort has undefined behaviour when its comparator is not a strict weak ordering.
    void checkComparatorsOnRandomRecords()
    {
        constexpr std::array columns {
            AudioFileTableColumn::name,
            AudioFileTableColumn::path,
            AudioFileTableColumn::bitDepth,
            AudioFileTableColumn::sampleRate,
            AudioFileTableColumn::length,
            AudioFileTableColumn::peakLeft,
            AudioFileTableColumn::peakRight,
            AudioFileTableColumn::overallPeak,
            AudioFileTableColumn::overallTruePeak,
            AudioFileTableColumn::maxShortTermLufs,
            AudioFileTableColumn::integratedLufs,
            AudioFileTableColumn::customGain,
            AudioFileTableColumn::status,
        };

        const auto records = makeRandomRecords();
        int violations = 0;

        for (const auto column : columns) {
            violations += countOrderingViolations(records, column);
        }

        expectEquals(violations, 0);
    }

    /// Builds records with random values that often repeat or hit NaN, negative zero, and silence.
    static std::vector<AudioAnalysisRecord> makeRandomRecords()
    {
        constexpr int recordCount = 24;
        constexpr std::array specialPeaks {
            0.0f,
            -0.0f,
            0.5f,
            -0.5f,
            1.0f,
            std::numeric_limits<float>::quiet_NaN(),
        };
        constexpr std::array specialLoudness {
            AudioAnalysisRecord::negativeInfinityLoudness,
            AudioAnalysisRecord::negativeInfinityLoudness * 2.0,
            -14.0,
            -23.0,
            std::numeric_limits<double>::quiet_NaN(),
        };

        juce::Random random(1234);
        const auto pickPeak = [&random, &specialPeaks] {
            return random.nextBool()
                ? specialPeaks[static_cast<std::size_t>(random.nextInt(static_cast<int>(specialPeaks.size())))]
                : random.nextFloat() * 2.0f - 1.0f;
        };
        const auto pickLoudness = [&random, &specialLoudness] {
            return random.nextBool()
                ? specialLoudness[static_cast<std::size_t>(random.nextInt(static_cast<int>(specialLoudness.size())))]
                : random.nextDouble() * -40.0;
        };

        std::vector<AudioAnalysisRecord> records;

        for (int index = 0; index < recordCount; ++index) {
            auto record = makeRecord(utils::format("file {}", random.nextInt(4)), pickPeak(), pickLoudness());
            record.fullPath = utils::format("/music/{}/{}", random.nextInt(3), record.fileName);
            record.peakLeft = pickPeak();
            record.peakRight = pickPeak();
            record.overallTruePeak = static_cast<double>(pickPeak());
            record.maxShortTermLufs = pickLoudness();
            record.sampleRate = random.nextBool() ? 44'100 : 48'000;
            record.durationSeconds = random.nextBool() ? 180.0 : random.nextDouble() * 600.0;
            record.bitsPerSample = random.nextInt(5) * 8;
            if (random.nextBool()) {
                record.customGain = Decibels {static_cast<float>(random.nextInt(3))};
            }
            record.status = random.nextBool() ? AudioAnalysisStatus::analyzed : AudioAnalysisStatus::cached;
            records.push_back(record);
        }

        return records;
    }

    /// Returns how many pairs break antisymmetry and how many triples break transitivity for one column.
    static int countOrderingViolations(
        const std::vector<AudioAnalysisRecord>& records,
        const AudioFileTableColumn column
    )
    {
        const auto compare = [column](const AudioAnalysisRecord& lhs, const AudioAnalysisRecord& rhs) {
            return juce::jlimit(-1, 1, results_sorting::compareRecordsByColumn(lhs, rhs, column));
        };

        int violations = 0;

        for (const auto& first : records) {
            for (const auto& second : records) {
                violations += compare(first, second) != -compare(second, first) ? 1 : 0;

                for (const auto& third : records) {
                    const auto ordered = compare(first, second) <= 0 && compare(second, third) <= 0;
                    violations += ordered && compare(first, third) > 0 ? 1 : 0;
                }
            }
        }

        return violations;
    }
};

std::unique_ptr<juce::UnitTest> createSortingTests()
{
    return std::make_unique<SortingTests>();
}
}  // namespace audiobatch::tests
