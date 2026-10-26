/// Tests for the display formatting helpers in AudioAnalysisFormatting
/// and the record sorting used by the GUI table and the CLI output.

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisFormatting.h"
#include "TestSuites.h"

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
        beginTest("Peaks format as dBFS, with silence as -INF");
        {
            expectEquals(formatPeakCompact(1.0), juce::String("0.00"));
            expectEquals(formatPeakCompact(0.5), juce::String("-6.02"));
            expectEquals(formatPeakCompact(-0.5), juce::String("-6.02"));
            expectEquals(formatPeakCompact(0.0), juce::String("-INF"));
            expectEquals(formatPeakDisplay(0.5f), juce::String("-6.02 dBFS"));
            expectEquals(formatTruePeakDisplay(0.0), juce::String("-INF dBTP"));
        }

        beginTest("Very quiet peaks are floored at -100 dB");
        {
            expectEquals(formatPeakCompact(1.0e-9), juce::String("-100.00"));
        }

        beginTest("Loudness formats as LUFS, with the sentinel as -INF");
        {
            expectEquals(formatLoudnessCompact(-14.25), juce::String("-14.25"));
            expectEquals(
                formatLoudnessDisplay(AudioAnalysisRecord::negativeInfinityLoudness), juce::String("-INF LUFS")
            );
        }

        beginTest("Sample rates format in kHz");
        {
            AudioAnalysisRecord record;
            expectEquals(formatSampleRateDisplay(record), juce::String("-"));
            record.sampleRate = 48'000;
            expectEquals(formatSampleRateDisplay(record), juce::String("48 kHz"));
            record.sampleRate = 44'100;
            expectEquals(formatSampleRateDisplay(record), juce::String("44.1 kHz"));
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
            expectEquals(formatBitrateDisplay(record), juce::String("-"));
            record.fileSize = 40'000;
            record.durationSeconds = 1.0;
            expectEquals(formatBitrateDisplay(record), juce::String("320 kbps"));
        }

        beginTest("Per-file gain shows its sign, and no gain shows nothing");
        {
            AudioAnalysisRecord record;
            record.customGainDb = 3.0f;
            expectEquals(formatCustomGainDisplay(record), juce::String());
            record.hasCustomGain = true;
            expectEquals(formatCustomGainDisplay(record), juce::String("+3.0 dB"));
            record.customGainDb = 0.0f;
            expectEquals(formatCustomGainDisplay(record), juce::String("+0.0 dB"));
            record.customGainDb = -1.5f;
            expectEquals(formatCustomGainDisplay(record), juce::String("-1.5 dB"));
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
