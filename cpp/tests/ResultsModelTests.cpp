/// Tests for ResultsModel, the rows of the results table.
/// Covers merging analysis and operation results, the custom gain carried over for unchanged files,
/// and the path index after removing and sorting rows.

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/ResultsModel.h"
#include "TestSuites.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace audiobatch::tests
{
namespace
{
/// Builds an analyzed record for a file with the given name, size, and custom gain.
AudioAnalysisRecord makeRecord(
    const juce::String& name,
    const std::int64_t fileSize = 1000,
    const std::optional<Decibels> customGain = std::nullopt
)
{
    AudioAnalysisRecord record;
    record.file = juce::File("/music").getChildFile(name);
    record.fileName = name;
    record.fullPath = record.file.getFullPathName();
    record.fileSize = fileSize;
    record.modifiedTimeMs = 1'700'000'000'000;
    record.customGain = customGain;
    record.status = AudioAnalysisStatus::analyzed;
    return record;
}

/// Returns the file names of the rows in their current order, separated by commas.
juce::String namesOf(const ResultsModel& results)
{
    juce::StringArray names;
    for (const auto& record : results) {
        names.add(record.fileName);
    }
    return names.joinIntoString(", ");
}
}  // namespace

/// Checks that the results keep one row per path and find every row by its path.
class ResultsModelTests final : public juce::UnitTest
{
public:
    ResultsModelTests() : juce::UnitTest("Results model", "AudioBatch") {}

    /// Runs the results model checks.
    void runTest() override
    {
        beginTest("Merging appends new paths and replaces the row of a known path in place");
        {
            ResultsModel results;
            expectEquals(results.merge(makeRecord("a.wav")).row, 0);
            expectEquals(results.merge(makeRecord("b.wav")).row, 1);

            auto changed = makeRecord("a.wav");
            changed.overallPeak = 0.5f;
            expectEquals(results.merge(changed).row, 0);

            expectEquals(results.size(), 2);
            expect(results.findRow(changed.fullPath) == 0);
            expectWithinAbsoluteError(results[0].overallPeak, 0.5f, 1.0e-6f);
            expect(!results.findRow("/music/missing.wav").has_value());
            expect(!results.containsRow(2));
        }

        beginTest("A custom gain is carried over only while the file is unchanged");
        {
            ResultsModel results;
            results.merge(makeRecord("a.wav", 1000, Decibels {-3.0f}));

            const auto unchanged = results.merge(makeRecord("a.wav"));
            expect(unchanged.customGainCarriedOver);
            expect(sameCustomGain(results[0].customGain, Decibels {-3.0f}));

            const auto sameGain = results.merge(makeRecord("a.wav", 1000, Decibels {-3.0f}));
            expect(!sameGain.customGainCarriedOver);

            const auto resized = results.merge(makeRecord("a.wav", 2000));
            expect(!resized.customGainCarriedOver);
            expect(!results[0].customGain.has_value());
        }

        beginTest("Explicit clears override stale gains only for the edited file identity");
        {
            ResultsModel results;
            results.merge(makeRecord("a.wav", 1000, Decibels {-3.0f}));
            expect(results.setCustomGain(0, std::nullopt));
            const auto stale = results.merge(makeRecord("a.wav", 1000, Decibels {-3.0f}));
            expect(stale.customGainCarriedOver);
            expect(!results[0].customGain);
            expect(!results.setCustomGain(0, std::nullopt));
            expect(results.merge(makeRecord("a.wav", 1000, Decibels {2.0f})).customGainCarriedOver);
            expect(!results[0].customGain);

            auto changed = makeRecord("a.wav", 1000, Decibels {4.0f});
            ++changed.modifiedTimeMs;
            expect(!results.merge(changed).customGainCarriedOver);
            expect(sameCustomGain(results[0].customGain, Decibels {4.0f}));
        }

        beginTest("Unchanged operation and reconciliation records preserve explicit clears");
        {
            ResultsModel results;
            results.merge(makeRecord("a.aif", 1000, Decibels {-3.0f}));
            results.setCustomGain(0, std::nullopt);
            const auto stale = makeRecord("a.aif", 1000, Decibels {-3.0f});
            expect(results.replaceAfterOperation(stale.fullPath, stale).customGainCarriedOver);
            expect(!results[0].customGain);
            results.setRecord(0, stale);
            expect(!results[0].customGain);

            results.removePaths({stale.fullPath});
            results.merge(stale);
            expect(sameCustomGain(results[0].customGain, Decibels {-3.0f}));
            results.clear();
            results.merge(stale);
            expect(sameCustomGain(results[0].customGain, Decibels {-3.0f}));
        }

        beginTest("An output with a new path replaces the row of its source");
        {
            ResultsModel results;
            results.merge(makeRecord("a.wav"));
            results.merge(makeRecord("b.aif"));
            results.merge(makeRecord("c.wav"));

            const auto merged = results.replaceAfterOperation(makeRecord("a.wav").fullPath, makeRecord("a.aif"));

            expectEquals(namesOf(results), juce::String("b.aif, c.wav, a.aif"));
            expectEquals(merged.row, 2);
            expect(!results.findRow(makeRecord("a.wav").fullPath).has_value());

            // A conversion onto a path that already has a row replaces that row instead of adding a second one.
            results.replaceAfterOperation(makeRecord("c.wav").fullPath, makeRecord("b.aif"));
            expectEquals(namesOf(results), juce::String("b.aif, a.aif"));
        }

        beginTest("Rows are found by path after removing and sorting");
        {
            ResultsModel results;
            for (const auto* fileName : {"d.wav", "b.wav", "a.wav", "c.wav"}) {
                results.merge(makeRecord(fileName));
            }

            results.removePaths(juce::StringArray {makeRecord("b.wav").fullPath});
            expect(results.findRow(makeRecord("a.wav").fullPath) == 1);

            results.sort({
                .primaryColumn = AudioFileTableColumn::name,
                .secondaryColumn = std::nullopt,
                .statusText = {},
            });
            expectEquals(namesOf(results), juce::String("a.wav, c.wav, d.wav"));

            for (int row = 0; row < results.size(); ++row) {
                expect(results.findRow(results[row].fullPath) == row);
            }
        }

        beginTest("A pre-mutation selection follows multiple conversions and an existing output row");
        {
            ResultsModel results;
            for (const auto* name : {"a.wav", "a.aif", "b.wav", "c.wav"}) {
                results.merge(makeRecord(name));
            }
            const juce::StringArray selected {
                results[0].fullPath, results[1].fullPath, results[2].fullPath, results[3].fullPath
            };
            results.replaceAfterOperation(makeRecord("a.wav").fullPath, makeRecord("a.aif"));
            results.replaceAfterOperation(makeRecord("b.wav").fullPath, makeRecord("b.aif"));
            results.sort({
                .primaryColumn = AudioFileTableColumn::name,
                .secondaryColumn = std::nullopt,
                .statusText = {},
            });
            const auto mapped = remapSelectedPaths(
                selected,
                {
                    {makeRecord("a.wav").fullPath, makeRecord("a.aif").fullPath},
                    {makeRecord("b.wav").fullPath, makeRecord("b.aif").fullPath},
                }
            );
            expectEquals(mapped.joinIntoString(", "), juce::String("/music/a.aif, /music/b.aif, /music/c.wav"));
            for (const auto& path : mapped) {
                expect(results.findRow(path).has_value());
            }
        }

        beginTest("Selection paths survive an earlier-sorting analysis insertion");
        {
            ResultsModel results;
            results.merge(makeRecord("b.wav"));
            const juce::StringArray selected {results[0].fullPath};
            results.addPending(juce::File("/music/a.wav"));
            results.sort({
                .primaryColumn = AudioFileTableColumn::name,
                .secondaryColumn = std::nullopt,
                .statusText = {},
            });
            expect(results.findRow(selected[0]) == 1);
            expectEquals(selected[0], makeRecord("b.wav").fullPath);
        }

        beginTest("A pending placeholder is only added for a path without a row");
        {
            ResultsModel results;
            results.merge(makeRecord("a.wav"));

            expect(!results.addPending(juce::File("/music/a.wav")));
            expect(results.addPending(juce::File("/music/b.wav")));
            expectEquals(results.size(), 2);
            expect(results[0].status == AudioAnalysisStatus::analyzed);
            expect(results[1].status == AudioAnalysisStatus::pending);
        }
    }
};

std::unique_ptr<juce::UnitTest> createResultsModelTests()
{
    return std::make_unique<ResultsModelTests>();
}
}  // namespace audiobatch::tests
