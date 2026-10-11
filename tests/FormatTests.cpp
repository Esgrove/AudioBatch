/// Tests for the display formatting helpers in AudioAnalysisService
/// and the record sorting used by the GUI table and the CLI output.

#include "AudioAnalysisService.h"
#include "TestSuites.h"

#include <vector>

namespace audiobatch::tests
{
/// Checks peak, true peak, loudness, sample rate, and bitrate formatting, and record sorting.
class FormatTests final : public juce::UnitTest
{
public:
    FormatTests() : juce::UnitTest("Display formatting", "AudioBatch") { }

    /// Runs the formatting and sorting checks.
    void runTest() override
    {
        beginTest("Peaks format as dBFS, with silence as -INF");
        {
            expectEquals(AudioAnalysisService::formatPeakCompact(1.0), juce::String("0.00"));
            expectEquals(AudioAnalysisService::formatPeakCompact(0.5), juce::String("-6.02"));
            expectEquals(AudioAnalysisService::formatPeakCompact(-0.5), juce::String("-6.02"));
            expectEquals(AudioAnalysisService::formatPeakCompact(0.0), juce::String("-INF"));
            expectEquals(AudioAnalysisService::formatPeakDisplay(0.5f), juce::String("-6.02 dBFS"));
            expectEquals(AudioAnalysisService::formatTruePeakDisplay(0.0), juce::String("-INF dBTP"));
        }

        beginTest("Very quiet peaks are floored at -100 dB");
        {
            expectEquals(AudioAnalysisService::formatPeakCompact(1.0e-9), juce::String("-100.00"));
        }

        beginTest("Loudness formats as LUFS, with the sentinel as -INF");
        {
            expectEquals(AudioAnalysisService::formatLoudnessCompact(-14.25), juce::String("-14.25"));
            expectEquals(
                AudioAnalysisService::formatLoudnessDisplay(AudioAnalysisRecord::negativeInfinityLoudness),
                juce::String("-INF LUFS")
            );
        }

        beginTest("Sample rates format in kHz");
        {
            AudioAnalysisRecord record;
            expectEquals(AudioAnalysisService::formatSampleRateDisplay(record), juce::String("-"));
            record.sampleRate = 48000;
            expectEquals(AudioAnalysisService::formatSampleRateDisplay(record), juce::String("48 kHz"));
            record.sampleRate = 44100;
            expectEquals(AudioAnalysisService::formatSampleRateDisplay(record), juce::String("44.1 kHz"));
        }

        beginTest("Bitrate is derived from file size and duration");
        {
            AudioAnalysisRecord record;
            expectEquals(AudioAnalysisService::formatBitrateDisplay(record), juce::String("-"));
            record.fileSize = 40000;
            record.durationSeconds = 1.0;
            expectEquals(AudioAnalysisService::formatBitrateDisplay(record), juce::String("320 kbps"));
        }

        beginTest("The type label prefers the extension, then the format name");
        {
            AudioAnalysisRecord record;
            record.file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("song.flac");
            expectEquals(AudioAnalysisService::formatTypeLabel(record), juce::String("FLAC"));

            record.file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("no_extension");
            record.formatName = "WAV file";
            expectEquals(AudioAnalysisService::formatTypeLabel(record), juce::String("WAV"));

            record.formatName = {};
            expectEquals(AudioAnalysisService::formatTypeLabel(record), juce::String("Unknown"));
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

            AudioAnalysisService::sortRecords(records, AudioAnalysisSortMode::peak, true);
            expectEquals(records[0].fileName, juce::String("a.wav"));
            expectEquals(records[2].fileName, juce::String("b.wav"));

            AudioAnalysisService::sortRecords(records, AudioAnalysisSortMode::name, false);
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
