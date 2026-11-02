/// Tests for ResultsModel, the rows of the results table.
/// Covers merging analysis and operation results, explicit gain edits versus inherited persisted values,
/// the path index after removing and sorting rows, and non-rendering artifact admission checks.

#include "analysis/AudioAnalysisTypes.h"
#include "gui/AudioBatchComponentConstants.h"
#include "gui/NativeWaveformBuilder.h"
#include "gui/PreviewEnrichmentScheduler.h"
#include "gui/ResultsModel.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"
#include <condition_variable>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Barrier state retained until a detached admission-test worker exits.
struct EnrichmentProbe {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<juce::String> paths;
    std::vector<bool> missing;
    bool release {false};
    int active {0};
    int maximum {0};
};

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
        testNativeWaveform();
        testEnrichmentAdmission();

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

        beginTest("Inherited gains never override another process's persisted edit or clear");
        {
            ResultsModel results;
            results.merge(makeRecord("a.wav", 1000, Decibels {-3.0f}));

            const auto unchanged = results.merge(makeRecord("a.wav"));
            expect(!unchanged.customGainCarriedOver);
            expect(!results[0].customGain);

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
            results.beginLoad();
            results.merge(stale);
            expect(sameCustomGain(results[0].customGain, Decibels {-3.0f}));

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
            for (const auto* fileName : {"a.wav", "a.aif", "b.wav", "c.wav"}) {
                results.merge(makeRecord(fileName));
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
            const juce::StringArray expected {
                makeRecord("a.aif").fullPath, makeRecord("b.aif").fullPath, makeRecord("c.wav").fullPath
            };
            expectEquals(mapped.joinIntoString(", "), expected.joinIntoString(", "));
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

private:
    /// Compares complete PCM buckets against JUCE encoding without starting a source-backed decoder.
    void testNativeWaveform()
    {
        beginTest("Waveform-only bytes match native JUCE geometry and quantization without padded final buckets");
        auto reader = utils::createAudioReader(fixtureFile("sine_stereo_44100_16.wav"));
        expect(reader != nullptr);
        if (!reader) {
            return;
        }
        const auto waveform = buildNativeWaveform(*reader);
        expect(waveform.has_value());
        if (!waveform) {
            return;
        }
        juce::MemoryInputStream input(*waveform, false);
        expectEquals(input.readInt(), 0x6d74616a);
        const auto detail = input.readInt();
        expectEquals(detail, samplesPerThumbnailPoint(reader->lengthInSamples));
        expectEquals(input.readInt64(), reader->lengthInSamples);
        const auto completeLength = reader->lengthInSamples / detail * detail;
        expectEquals(input.readInt64(), completeLength);
        const auto points = input.readInt();
        expectEquals(points, static_cast<int>(reader->lengthInSamples / detail + 1));
        expectEquals(input.readInt(), 2);
        expectEquals(input.readInt(), 44'100);

        juce::AudioBuffer<float> pcm(2, static_cast<int>(reader->lengthInSamples));
        expect(utils::readFloatSamples(*reader, utils::writePointers(pcm), 0, pcm.getNumSamples()));
        juce::AudioFormatManager formats;
        juce::AudioThumbnailCache cache(1);
        juce::AudioThumbnail native(detail, formats, cache);
        native.reset(2, reader->sampleRate, reader->lengthInSamples);
        native.addBlock(0, pcm, 0, static_cast<int>(completeLength));
        juce::MemoryBlock expected;
        juce::MemoryOutputStream output(expected, false);
        native.saveTo(output);
        output.flush();
        expect(*waveform == expected);
        expectWithinAbsoluteError(
            native.getTotalLength(), static_cast<double>(reader->lengthInSamples) / reader->sampleRate, 1.0e-12
        );
        expect(!buildNativeWaveform(*reader, [] { return true; }));
    }

    /// Exercises post-load gating, single admission, selection promotion, cancellation and dedup with barriers.
    void testEnrichmentAdmission()
    {
        beginTest("Enrichment gates missing work, joins the selected job, and never admits a second worker");
        auto probe = std::make_shared<EnrichmentProbe>();
        const TemporaryDirectory directory;
        const auto first = directory.copyFixture("silence_stereo_44100_16.wav", "first.wav");
        const auto selected = directory.copyFixture("silence_stereo_44100_16.wav", "selected.wav");
        const utils::FileIdentity identity(selected);
        PreviewEnrichmentScheduler scheduler(
            directory.getChildFile("enrichment.db"),
            {},
            [probe](
                const PreviewEnrichmentCandidate& candidate,
                const bool missing,
                const utils::CancellationCheck& cancelled
            ) {
                std::unique_lock lock(probe->mutex);
                ++probe->active;
                probe->maximum = std::max(probe->maximum, probe->active);
                probe->paths.push_back(candidate.file.getFileName());
                probe->missing.push_back(missing);
                probe->condition.notify_all();
                probe->condition.wait_for(lock, std::chrono::milliseconds(250), [&] {
                    return probe->release || cancelled();
                });
                --probe->active;
                probe->condition.notify_all();
            }
        );
        scheduler.beginForeground();
        scheduler.select(identity, 1);
        {
            std::unique_lock lock(probe->mutex);
            expect(probe->condition.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return !probe->paths.empty();
            }));
            expect(!probe->missing.empty() && !probe->missing.front());
            probe->release = true;
            probe->condition.notify_all();
            expect(probe->condition.wait_for(lock, std::chrono::milliseconds(250), [&] { return probe->active == 0; }));
            probe->release = false;
        }
        const auto candidate = [](const juce::File& file) {
            return PreviewEnrichmentCandidate {
                .file = file,
                .fileSize = file.getSize(),
                .modifiedTimeMs = file.getLastModificationTime().toMilliseconds(),
            };
        };
        scheduler.completeForeground({candidate(first), candidate(selected), candidate(selected)});
        {
            std::unique_lock lock(probe->mutex);
            expect(probe->condition.wait_for(lock, std::chrono::milliseconds(250), [&] {
                return probe->paths.size() >= 2;
            }));
            expectEquals(probe->paths.back(), juce::String("selected.wav"));
            expect(probe->missing.back());
        }
        scheduler.select(identity, 2);
        scheduler.beginForeground();
        scheduler.stop();
        {
            std::unique_lock lock(probe->mutex);
            probe->release = true;
            probe->condition.notify_all();
            expect(probe->condition.wait_for(lock, std::chrono::milliseconds(250), [&] { return probe->active == 0; }));
            expectEquals(probe->maximum, 1);
            expectEquals(probe->paths.size(), std::size_t {2});
        }
    }
};

std::unique_ptr<juce::UnitTest> createResultsModelTests()
{
    return std::make_unique<ResultsModelTests>();
}
}  // namespace audiobatch::tests
