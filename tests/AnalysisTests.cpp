/// Integration tests for AudioAnalysisService::analyzeFile on the committed fixtures.
/// Every fixture tone has the same level, so the tests check that each format, channel count,
/// sample rate, and bit depth is decoded and measured consistently.

#include "AudioAnalysisService.h"
#include "MetadataService.h"
#include "TestHelpers.h"
#include "TestSuites.h"

#include <array>

namespace audiobatch::tests
{
/// Expected properties of one fixture tone.
struct ToneFixture {
    const char* fileName;
    int channels;
    int sampleRate;
    /// Expected source bit depth, or 0 where it is not checked.
    /// MP3 files report the fixed default of 16 bits, since the format stores no source bit depth.
    int bitsPerSample;
    /// True for lossy formats, which get a wider level tolerance.
    bool lossy;
};

/// The tone fixtures and what analysis should report for them.
constexpr std::array toneFixtures {
    ToneFixture {
        .fileName = "sine_stereo_44100_16.wav",
        .channels = 2,
        .sampleRate = 44100,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_48000_24.wav",
        .channels = 2,
        .sampleRate = 48000,
        .bitsPerSample = 24,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_mono_44100_16.wav",
        .channels = 1,
        .sampleRate = 44100,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_mono_48000_24.wav",
        .channels = 1,
        .sampleRate = 48000,
        .bitsPerSample = 24,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_44100_32f.wav",
        .channels = 2,
        .sampleRate = 44100,
        .bitsPerSample = 32,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_96000_16.wav",
        .channels = 2,
        .sampleRate = 96000,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_mono_24000_16.wav",
        .channels = 1,
        .sampleRate = 24000,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_44100_16.aif",
        .channels = 2,
        .sampleRate = 44100,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_mono_48000_24.aif",
        .channels = 1,
        .sampleRate = 48000,
        .bitsPerSample = 24,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_44100_16.flac",
        .channels = 2,
        .sampleRate = 44100,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_48000_24.flac",
        .channels = 2,
        .sampleRate = 48000,
        .bitsPerSample = 24,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_44100_320k.mp3",
        .channels = 2,
        .sampleRate = 44100,
        .bitsPerSample = 16,
        .lossy = true,
    },
    ToneFixture {
        .fileName = "sine_joint_stereo_44100_192k.mp3",
        .channels = 2,
        .sampleRate = 44100,
        .bitsPerSample = 16,
        .lossy = true,
    },
    ToneFixture {
        .fileName = "sine_mono_44100_128k.mp3",
        .channels = 1,
        .sampleRate = 44100,
        .bitsPerSample = 16,
        .lossy = true,
    },
    ToneFixture {
        .fileName = "sine_stereo_48000.ogg",
        .channels = 2,
        .sampleRate = 48000,
        .bitsPerSample = 0,
        .lossy = true,
    },
};

/// Allowed level difference for lossless fixtures, in dB or LU.
constexpr double losslessTolerance = 0.05;
/// Allowed level difference for lossy fixtures, in dB or LU.
constexpr double lossyTolerance = 0.5;

/// Checks the measured peaks, loudness, and file properties of every fixture.
class AnalysisTests final : public juce::UnitTest
{
public:
    AnalysisTests() : juce::UnitTest("Audio analysis", "AudioBatch") {}

    /// Runs the analysis checks.
    void runTest() override
    {
        for (const auto& fixture : toneFixtures) {
            checkToneFixture(fixture);
        }

        beginTest("Digital silence has no peak and no measurable loudness");
        {
            const auto record = AudioAnalysisService::analyzeFile(fixtureFile("silence_stereo_44100_16.wav"));
            expect(!record.hasError(), record.errorMessage);
            expectEquals(record.overallPeak, 0.0f);
            expectEquals(record.integratedLufs, AudioAnalysisRecord::negativeInfinityLoudness);
        }

        beginTest("Hidden and AppleDouble files are skipped when collecting inputs");
        {
            const TemporaryDirectory directory;
            const auto visible = directory.copyFixture("sine_stereo_44100_16.wav", "visible.wav");
            juce::ignoreUnused(directory.copyFixture("sine_stereo_44100_16.wav", "._visible.wav"));
            juce::ignoreUnused(directory.copyFixture("sine_stereo_44100_16.wav", ".hidden.wav"));

            const auto files = AudioAnalysisService::collectInputFiles({visible.getParentDirectory()}, false);
            expectEquals(files.size(), 1);
            expectEquals(files[0].getFileName(), juce::String("visible.wav"));

            const auto explicitFiles
                = AudioAnalysisService::collectInputFiles({directory.getChildFile("._visible.wav")}, false);
            expect(explicitFiles.isEmpty());
        }

        beginTest("A cancelled folder scan stops without collecting files");
        {
            const auto files = AudioAnalysisService::collectInputFiles(
                {fixtureFile("sine_stereo_44100_16.wav").getParentDirectory()}, true, [] { return true; }
            );
            expect(files.isEmpty());
        }

        beginTest("Cancelled analysis fails without measuring");
        {
            const auto record
                = AudioAnalysisService::analyzeFile(fixtureFile("sine_stereo_44100_16.wav"), [] { return true; });
            expect(record.hasError());
            expect(record.errorMessage.contains("cancelled"), record.errorMessage);
        }

        beginTest("An MP3 bit depth is never read from its tags");
        {
            const TemporaryDirectory directory;
            const auto tagged = directory.copyFixture("sine_stereo_44100_320k.mp3", "tagged.mp3");
            MetadataService::Metadata metadata;
            metadata.properties.push_back({.key = "TITLE", .values = {"Master 24-bit 96 kHz"}});
            metadata.properties.push_back({.key = "COMMENT", .values = {"24 bit"}});
            expect(MetadataService::writeMetadata(tagged, metadata));

            const auto record = AudioAnalysisService::analyzeFile(tagged);
            expect(!record.hasError(), record.errorMessage);
            expectEquals(record.bitsPerSample, 16);
        }

        beginTest("A file that is not audio fails analysis");
        {
            const auto record = AudioAnalysisService::analyzeFile(fixtureFile("broken.wav"));
            expect(record.hasError());
            expect(record.errorMessage.isNotEmpty());
        }
    }

private:
    /// Analyzes one fixture and compares the results with its expected properties.
    void checkToneFixture(const ToneFixture& fixture)
    {
        beginTest(fixture.fileName);

        const auto record = AudioAnalysisService::analyzeFile(fixtureFile(fixture.fileName));
        expect(!record.hasError(), record.errorMessage);
        expectEquals(record.channels, fixture.channels);
        expectEquals(record.sampleRate, fixture.sampleRate);

        if (fixture.bitsPerSample > 0) {
            expectEquals(record.bitsPerSample, fixture.bitsPerSample);
        }

        const auto tolerance = fixture.lossy ? lossyTolerance : losslessTolerance;
        expectWithinAbsoluteError(peakDecibels(record), fixturePeakDecibels, tolerance);
        expectWithinAbsoluteError(
            juce::Decibels::gainToDecibels(record.overallTruePeak), fixturePeakDecibels, tolerance
        );
        expectWithinAbsoluteError(record.integratedLufs, fixtureLoudness, tolerance);
        expectWithinAbsoluteError(record.durationSeconds, 1.0, 0.1);
    }
};

std::unique_ptr<juce::UnitTest> createAnalysisTests()
{
    return std::make_unique<AnalysisTests>();
}
}  // namespace audiobatch::tests
