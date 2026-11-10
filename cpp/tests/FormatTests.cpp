/// Tests for the display formatting helpers in AudioAnalysisFormatting
/// and the record sorting used by the GUI table and the CLI output.

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "analysis/AudioAnalysisTypes.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace audiobatch::tests
{
/// Checks peak, true peak, loudness, sample rate, and bitrate formatting, and record sorting.
class FormatTests final : public juce::UnitTest
{
public:
    FormatTests() : juce::UnitTest("Display formatting", "AudioBatch") {}

    /// Runs the formatting and sorting checks.
    void runTest() override
    {
        beginTest("Folder display matches shared platform expectations");
        {
            const auto cases = juce::StringArray::fromLines(fixtureFile("folder_display.tsv").loadFileAsString());
            int count = 0;
            for (const auto& line : cases) {
                if (line.isEmpty() || line.startsWithChar('#')) {
                    continue;
                }
                const auto fields = juce::StringArray::fromTokens(line, "\t", "");
                expectEquals(fields.size(), 3);
                if (fields.size() == 3) {
                    // Only macOS shortens home folders, so the other platforms keep the path as it is.
                    expectEquals(fields[0] == "macos" ? formatFolderText(fields[1]) : fields[1], fields[2]);
                    ++count;
                }
            }
            expect(count >= 10);
            const auto source = fixtureFile("sine_stereo_44100_16.wav");
            const auto folder = source.getParentDirectory().getFullPathName();
#if JUCE_MAC
            expectEquals(formatFolder(source), formatFolderText(folder));
#else
            expectEquals(formatFolder(source), folder);
#endif
        }

        beginTest("Source statistics keep undefined values distinct from zero and name every channel");
        {
            expectEquals(formatMetric(std::nullopt), juce::String("-"));
            expectEquals(formatMetric(1.0, true), juce::String("+1.00"));
            expectEquals(formatMetric(-0.0001, true), juce::String("+0.00"));
            expectEquals(formatMetric(3.0102999566), juce::String("3.01"));
            SignalMetrics metrics;
            metrics.dcOffsets = {0.25, -0.5};
            metrics.clipCounts = {4, 3};
            expectEquals(formatDcOffsets(metrics), juce::String("Ch 1: +25.0000%, Ch 2: -50.0000%"));
            expectEquals(formatClipCounts(metrics), juce::String("Ch 1: 4, Ch 2: 3"));
            expectEquals(static_cast<juce::int64>(metrics.totalClips()), juce::int64 {7});
            expectWithinAbsoluteError(metrics.maximumDcOffset().value_or(-1.0), 0.5, 0.0);
        }

        beginTest("Peaks format as dBFS, with silence as -INF");
        {
            expectEquals(formatPeakCompact(1.0), juce::String("0.00"));
            expectEquals(formatPeakCompact(0.5), juce::String("-6.02"));
            expectEquals(formatPeakCompact(-0.5), juce::String("-6.02"));
            expectEquals(formatPeakCompact(0.0), juce::String("-INF"));
            expectEquals(formatTruePeakCompact(0.5), juce::String("-6.02"));
            expectEquals(formatTruePeakCompact(0.0), juce::String("-INF"));
        }

        beginTest("Very quiet peaks are floored at -100 dB");
        {
            expectEquals(formatPeakCompact(1.0e-9), juce::String("-100.00"));
        }

        beginTest("Loudness formats as LUFS, with the sentinel as -INF");
        {
            expectEquals(formatLoudnessCompact(-14.25), juce::String("-14.25"));
            expectEquals(formatLoudnessCompact(AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS), juce::String("-INF"));
        }

        beginTest("Sample rates format in kHz, with the unit only outside the results table");
        {
            AudioAnalysisRecord record;
            expectEquals(formatSampleRateCompact(record), juce::String("-"));
            expectEquals(formatSampleRateDisplay(record), juce::String("-"));
            record.sampleRate = 48'000;
            expectEquals(formatSampleRateCompact(record), juce::String("48"));
            expectEquals(formatSampleRateDisplay(record), juce::String("48 kHz"));
            record.sampleRate = 44'100;
            expectEquals(formatSampleRateCompact(record), juce::String("44.1"));
            expectEquals(formatSampleRateDisplay(record), juce::String("44.1 kHz"));
        }

        beginTest("Durations format as minutes and seconds, with hours from one hour on");
        {
            expectEquals(formatDurationClock(0.0), juce::String("-"));
            expectEquals(formatDurationClock(std::numeric_limits<double>::quiet_NaN()), juce::String("-"));
            expectEquals(formatDurationClock(5.4), juce::String("0:05"));
            expectEquals(formatDurationClock(195.0), juce::String("3:15"));
            expectEquals(formatDurationClock(195.6), juce::String("3:16"));
            expectEquals(formatDurationClock(3'599.6), juce::String("1:00:00"));
            expectEquals(formatDurationClock(3'725.0), juce::String("1:02:05"));
            expectEquals(formatDurationClockTenths(0.0), juce::String("-"));
            expectEquals(formatDurationClockTenths(195.44), juce::String("3:15.4"));
            expectEquals(formatDurationClockTenths(59.96), juce::String("1:00.0"));
            expectEquals(formatDurationClockTenths(3'725.25), juce::String("1:02:05.3"));
            expectEquals(formatDurationClockTenths(205.4), juce::String("3:25.4"));
            expectEquals(formatDurationClockTenths(0.01), juce::String("0:00.0"));
            expectEquals(formatDurationClockTenths(0.05), juce::String("0:00.1"));
            expectEquals(formatDurationClockTenths(59.95), juce::String("1:00.0"));
            expectEquals(formatDurationClockTenths(3'599.95), juce::String("1:00:00.0"));
            expectEquals(formatDurationClockTenths(-1.0), juce::String("-"));
            expectEquals(formatDurationClockTenths(std::numeric_limits<double>::quiet_NaN()), juce::String("-"));
            expectEquals(formatDurationClockTenths(std::numeric_limits<double>::infinity()), juce::String("-"));
        }

        beginTest("Bit depth uses the analyzed value or the existing unavailable label");
        {
            AudioAnalysisRecord record;
            expectEquals(formatBitsPerSampleDisplay(record), juce::String("-"));
            record.bitsPerSample = 16;
            expectEquals(formatBitsPerSampleDisplay(record), juce::String("16"));
            record.bitsPerSample = 24;
            expectEquals(formatBitsPerSampleDisplay(record), juce::String("24"));
            record.bitsPerSample = 0;
            record.file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("song.mp3");
            expectEquals(formatBitsPerSampleDisplay(record), juce::String("Unknown"));
        }

        beginTest("Bitrate is derived from file size and duration");
        {
            AudioAnalysisRecord record;
            expectEquals(formatBitrateCompact(record), juce::String("-"));
            expectEquals(formatBitrateDisplay(record), juce::String("-"));
            record.fileSize = 40'000;
            record.durationSeconds = 1.0;
            expectEquals(formatBitrateCompact(record), juce::String("320"));
            expectEquals(formatBitrateDisplay(record), juce::String("320 kbps"));
        }

        beginTest("Per-file gain shows its sign, and no gain shows nothing");
        {
            AudioAnalysisRecord record;
            expectEquals(formatCustomGainCompact(record), juce::String());
            record.customGain = Decibels {3.0f};
            expectEquals(formatCustomGainCompact(record), juce::String("+3.0"));
            record.customGain = Decibels {0.0f};
            expectEquals(formatCustomGainCompact(record), juce::String("+0.0"));
            record.customGain = Decibels {-1.5f};
            expectEquals(formatCustomGainCompact(record), juce::String("-1.5"));
            record.customGain = Decibels {-0.0f};
            expectEquals(formatCustomGainCompact(record), juce::String("+0.0"));
            record.customGain = Decibels {-0.04f};
            expectEquals(formatCustomGainCompact(record), juce::String("+0.0"));
        }

        beginTest("The type label prefers the extension, then the format name");
        {
            AudioAnalysisRecord record;
            record.file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("song.flac");
            expectEquals(formatTypeLabel(record), juce::String("FLAC"));

            record.file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("no_extension");
            record.formatName = "WAV file";
            expectEquals(formatTypeLabel(record), juce::String("WAV"));

            record.formatName = {};
            expectEquals(formatTypeLabel(record), juce::String("Unknown"));
        }

        beginTest("Camelot mapping matches the shared independent key spellings");
        {
            const auto fixture = fixtureFile("camelot_keys.tsv");
            expect(fixture.existsAsFile());
            int cases = 0;
            for (const auto& line : juce::StringArray::fromLines(fixture.loadFileAsString())) {
                if (line.isEmpty() || line.startsWith("#")) {
                    continue;
                }
                const auto separator = line.indexOfChar('\t');
                expect(separator >= 0, line);
                if (separator < 0) {
                    continue;
                }
                const auto input = line.substring(0, separator);
                const auto expected = line.substring(separator + 1);
                const auto actual = camelotKey(input);
                expectEquals(actual.value_or("-"), expected, input);
                ++cases;
            }
            expect(cases >= 140);
        }

        beginTest("File sizes use decimal KB and MB with one decimal place");
        {
            AudioAnalysisRecord record;
            expectEquals(formatFileSizeDisplay(record), juce::String("-"));
            record.fileSize = -1;
            expectEquals(formatFileSizeDisplay(record), juce::String("-"));
            record.fileSize = 1;
            expectEquals(formatFileSizeDisplay(record), juce::String("0.0 KB"));
            record.fileSize = 1000;
            expectEquals(formatFileSizeDisplay(record), juce::String("1.0 KB"));
            record.fileSize = 36'200;
            expectEquals(formatFileSizeDisplay(record), juce::String("36.2 KB"));
            record.fileSize = 176'400;
            expectEquals(formatFileSizeDisplay(record), juce::String("176.4 KB"));
            record.fileSize = 999'999;
            expectEquals(formatFileSizeDisplay(record), juce::String("1000.0 KB"));
            record.fileSize = 1'000'000;
            expectEquals(formatFileSizeDisplay(record), juce::String("1.0 MB"));
            record.fileSize = 36'200'000;
            expectEquals(formatFileSizeDisplay(record), juce::String("36.2 MB"));
        }

        beginTest("Records sort by peak and by name");
        {
            std::vector<AudioAnalysisRecord> records(3);
            records[0].fileName = "b.wav";
            records[0].overallPeak = 0.9f;
            records[1].fileName = "a.wav";
            records[1].overallPeak = 0.1f;
            records[2].fileName = "c.wav";
            records[2].overallPeak = 0.5f;

            for (auto& record : records) {
                record.status = AudioAnalysisStatus::analyzed;
            }

            sortRecords(records, AudioAnalysisSortMode::peak, true);
            expectEquals(records[0].fileName, juce::String("a.wav"));
            expectEquals(records[2].fileName, juce::String("b.wav"));

            sortRecords(records, AudioAnalysisSortMode::name, false);
            expectEquals(records[0].fileName, juce::String("c.wav"));
            expectEquals(records[2].fileName, juce::String("a.wav"));
        }
    }
};

std::unique_ptr<juce::UnitTest> createFormatTests()
{
    return std::make_unique<FormatTests>();
}
}  // namespace audiobatch::tests
