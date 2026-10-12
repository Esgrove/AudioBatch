/// Tests for the results table sorting in ResultsSorting.

#include "AudioFileTableModel.h"
#include "ResultsSorting.h"
#include "TestSuites.h"

#include <cmath>
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
    SortingTests() : juce::UnitTest("Results sorting", "AudioBatch") { }

    /// Runs the sorting checks.
    void runTest() override
    {
        beginTest("Peaks compare by magnitude regardless of polarity");
        {
            const auto quiet = makeRecord("quiet", -0.25f, -20.0);
            const auto loud = makeRecord("loud", 0.5f, -10.0);
            expect(results_sorting::compareRecordsByColumn(quiet, loud, AudioFileTableModel::columnOverallPeak) < 0);
        }

        beginTest("Peaks that differ by the smallest step still order strictly");
        {
            const auto lower = makeRecord("lower", 0.5f, -20.0);
            const auto higher = makeRecord("higher", std::nextafter(0.5f, 1.0f), -20.0);
            expect(results_sorting::compareRecordsByColumn(lower, higher, AudioFileTableModel::columnOverallPeak) < 0);
            expect(results_sorting::compareRecordsByColumn(higher, lower, AudioFileTableModel::columnOverallPeak) > 0);
            expectEquals(
                results_sorting::compareRecordsByColumn(lower, lower, AudioFileTableModel::columnOverallPeak), 0
            );
        }

        beginTest("Silent loudness sorts below every measurable value");
        {
            const auto silent = makeRecord("silent", 0.0f, AudioAnalysisRecord::negativeInfinityLoudness);
            const auto quiet = makeRecord("quiet", 0.1f, -40.0);
            expect(
                results_sorting::compareRecordsByColumn(silent, quiet, AudioFileTableModel::columnIntegratedLufs) < 0
            );
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
                    .primaryColumnId = AudioFileTableModel::columnOverallPeak,
                    .primaryForwards = false,
                    .secondaryColumnId = AudioFileTableModel::columnIntegratedLufs,
                    .secondaryForwards = true,
                }
            );

            expectEquals(namesOf(records).joinIntoString(","), juce::String("a,b,c,d,failed"));
        }
    }
};

std::unique_ptr<juce::UnitTest> createSortingTests()
{
    return std::make_unique<SortingTests>();
}
}  // namespace audiobatch::tests
