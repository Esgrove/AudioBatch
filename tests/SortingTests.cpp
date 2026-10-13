/// Tests for the results table sorting in ResultsSorting.

#include "AudioFileTableModel.h"
#include "ResultsSorting.h"
#include "StringFormat.h"
#include "TestSuites.h"

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

namespace audiobatch::tests
{
/// Builds an analyzed record with the given name, peak, and integrated loudness.
static AudioAnalysisRecord makeRecord(const juce::String& name, const float peak, const double integratedLufs)
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
static juce::StringArray namesOf(const std::vector<AudioAnalysisRecord>& records)
{
    juce::StringArray names;
    for (const auto& record : records) {
        names.add(record.fileName);
    }
    return names;
}

/// Checks the column comparators and the primary and secondary sort order.
class SortingTests final : public juce::UnitTest
{
public:
    SortingTests() : juce::UnitTest("Results sorting", "AudioBatch") {}

    /// Runs the sorting checks.
    void runTest() override
    {
        beginTest("Peaks compare by magnitude regardless of polarity");
        {
            const auto quiet = makeRecord("quiet", -0.25f, -20.0);
            const auto loud = makeRecord("loud", 0.5f, -10.0);
            expect(results_sorting::compareRecordsByColumn(quiet, loud, columnOverallPeak) < 0);
        }

        beginTest("Peaks that differ by the smallest step still order strictly");
        {
            const auto lower = makeRecord("lower", 0.5f, -20.0);
            const auto higher = makeRecord("higher", std::nextafter(0.5f, 1.0f), -20.0);
            expect(results_sorting::compareRecordsByColumn(lower, higher, columnOverallPeak) < 0);
            expect(results_sorting::compareRecordsByColumn(higher, lower, columnOverallPeak) > 0);
            expectEquals(results_sorting::compareRecordsByColumn(lower, lower, columnOverallPeak), 0);
        }

        beginTest("Silent loudness sorts below every measurable value");
        {
            const auto silent = makeRecord("silent", 0.0f, AudioAnalysisRecord::negativeInfinityLoudness);
            const auto quiet = makeRecord("quiet", 0.1f, -40.0);
            expect(results_sorting::compareRecordsByColumn(silent, quiet, columnIntegratedLufs) < 0);
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
                    .primaryColumnId = columnOverallPeak,
                    .primaryForwards = false,
                    .secondaryColumnId = columnIntegratedLufs,
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
            constexpr auto column = columnOverallPeak;

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
                    .primaryColumnId = columnIntegratedLufs,
                    .primaryForwards = true,
                    .secondaryColumnId = columnOverallPeak,
                    .secondaryForwards = true,
                    .statusText = {},
                }
            );

            expectEquals(namesOf(records).joinIntoString(","), juce::String("quieter silent,louder silent,measured"));
        }

        beginTest("Records without a custom gain sort before gains, which sort by value");
        {
            auto boosted = makeRecord("boosted", 0.5f, -12.0);
            boosted.hasCustomGain = true;
            boosted.customGainDb = 3.0f;
            auto cut = makeRecord("cut", 0.5f, -12.0);
            cut.hasCustomGain = true;
            cut.customGainDb = -3.0f;
            std::vector records {boosted, makeRecord("none", 0.5f, -12.0), cut};

            results_sorting::sortRecords(
                records,
                {
                    .primaryColumnId = columnCustomGain,
                    .primaryForwards = true,
                    .secondaryColumnId = 0,
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
                    .primaryColumnId = columnOverallPeak,
                    .primaryForwards = false,
                    .secondaryColumnId = 0,
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
                    .primaryColumnId = columnStatus,
                    .primaryForwards = true,
                    .secondaryColumnId = 0,
                    .secondaryForwards = true,
                    .statusText
                    = [&shownText](const AudioAnalysisRecord& record) { return shownText.at(record.fileName); },
                }
            );

            expectEquals(namesOf(records).joinIntoString(","), juce::String("second,third,first"));
        }
    }

private:
    /// Checks antisymmetry and transitivity of every column comparator on records with random values,
    /// including NaN, negative zero, silence, and repeated values.
    /// std::ranges::sort has undefined behaviour when its comparator is not a strict weak ordering.
    void checkComparatorsOnRandomRecords()
    {
        constexpr std::array columns {
            columnName,
            columnPath,
            columnSampleRate,
            columnPeakLeft,
            columnPeakRight,
            columnOverallPeak,
            columnOverallTruePeak,
            columnMaxShortTermLufs,
            columnIntegratedLufs,
            columnCustomGain,
            columnStatus,
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
            record.sampleRate = random.nextBool() ? 44100 : 48000;
            record.hasCustomGain = random.nextBool();
            record.customGainDb = static_cast<float>(random.nextInt(3));
            record.status = random.nextBool() ? AudioAnalysisStatus::analyzed : AudioAnalysisStatus::cached;
            records.push_back(record);
        }

        return records;
    }

    /// Returns how many pairs break antisymmetry and how many triples break transitivity for one column.
    static int countOrderingViolations(const std::vector<AudioAnalysisRecord>& records, const int column)
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
