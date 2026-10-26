/// Integration tests for analyzeFile on the committed fixtures.
/// Every fixture tone has the same level, so the tests check that each format,
/// channel count, sample rate, and bit depth is decoded and measured consistently.

#include "analysis/AudioAnalysis.h"
#include "metadata/AudioMetadata.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/StringFormat.h"

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
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_48000_24.wav",
        .channels = 2,
        .sampleRate = 48'000,
        .bitsPerSample = 24,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_mono_44100_16.wav",
        .channels = 1,
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_mono_48000_24.wav",
        .channels = 1,
        .sampleRate = 48'000,
        .bitsPerSample = 24,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_44100_32f.wav",
        .channels = 2,
        .sampleRate = 44'100,
        .bitsPerSample = 32,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_96000_16.wav",
        .channels = 2,
        .sampleRate = 96'000,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_mono_24000_16.wav",
        .channels = 1,
        .sampleRate = 24'000,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_44100_16.aif",
        .channels = 2,
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_mono_48000_24.aif",
        .channels = 1,
        .sampleRate = 48'000,
        .bitsPerSample = 24,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_44100_16.flac",
        .channels = 2,
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_48000_24.flac",
        .channels = 2,
        .sampleRate = 48'000,
        .bitsPerSample = 24,
        .lossy = false,
    },
    ToneFixture {
        .fileName = "sine_stereo_44100_320k.mp3",
        .channels = 2,
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = true,
    },
    ToneFixture {
        .fileName = "sine_joint_stereo_44100_192k.mp3",
        .channels = 2,
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = true,
    },
    ToneFixture {
        .fileName = "sine_mono_44100_128k.mp3",
        .channels = 1,
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = true,
    },
    ToneFixture {
        .fileName = "sine_stereo_48000.ogg",
        .channels = 2,
        .sampleRate = 48'000,
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
            const auto record = analyzeFile(fixtureFile("silence_stereo_44100_16.wav"));
            expect(!record.hasError(), record.errorMessage);
            expectEquals(record.overallPeak, 0.0f);
            expectEquals(record.integratedLufs, AudioAnalysisRecord::negativeInfinityLoudness);
            expect(record.shortTermLoudness != nullptr);
            if (record.shortTermLoudness) {
                expectEquals(static_cast<int>(record.shortTermLoudness->values.size()), 10);
                for (const auto value : record.shortTermLoudness->values) {
                    expectEquals(value, static_cast<float>(AudioAnalysisRecord::negativeInfinityLoudness));
                }
            }
        }

        testLoudnessHistory();

        beginTest("Hidden and AppleDouble files are skipped when collecting inputs");
        {
            const TemporaryDirectory directory;
            const auto visible = directory.copyFixture("sine_stereo_44100_16.wav", "visible.wav");
            juce::ignoreUnused(directory.copyFixture("sine_stereo_44100_16.wav", "._visible.wav"));
            juce::ignoreUnused(directory.copyFixture("sine_stereo_44100_16.wav", ".hidden.wav"));

            const auto files = collectInputFiles({visible.getParentDirectory()}, false);
            expectEquals(files.size(), 1);
            expectEquals(files[0].getFileName(), juce::String("visible.wav"));

            const auto explicitFiles = collectInputFiles({directory.getChildFile("._visible.wav")}, false);
            expect(explicitFiles.isEmpty());
        }

        beginTest("A recursive scan ends on a symlink that points back to its parent folder");
        {
            const TemporaryDirectory directory;
            const auto folder = directory.getChildFile("music");
            expect(folder.createDirectory());
            expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(folder.getChildFile("tone.wav")));

            // Creating a symlink needs extra privileges on Windows, so the check only runs where the link exists.
            if (folder.createSymbolicLink(folder.getChildFile("loop"), true)) {
                const auto files = collectInputFiles({folder}, true);
                expectEquals(files.size(), 1);
            }
        }

        beginTest("A cancelled folder scan stops without collecting files");
        {
            const auto files
                = collectInputFiles({fixtureFile("sine_stereo_44100_16.wav").getParentDirectory()}, true, [] {
                      return true;
                  });
            expect(files.isEmpty());
        }

        beginTest("Analysis reports increasing progress up to completion");
        {
            std::vector<float> reported;
            const auto record
                = analyzeFile(fixtureFile("sine_stereo_44100_16.wav"), {}, [&reported](const float value) {
                      reported.push_back(value);
                  });
            expect(!record.hasError(), record.errorMessage);
            expect(!reported.empty());
            expect(std::ranges::is_sorted(reported));
            expectEquals(reported.back(), 1.0f);
        }

        beginTest("Cancelled analysis fails without measuring");
        {
            const auto record = analyzeFile(fixtureFile("sine_stereo_44100_16.wav"), [] { return true; });
            expect(record.hasError());
            expect(record.errorMessage.contains("cancelled"), record.errorMessage);
            expect(record.shortTermLoudness == nullptr);
        }

        beginTest("Cancellation after partial decoding never publishes incomplete loudness history");
        {
            int checks = 0;
            const auto record
                = analyzeFile(fixtureFile("sine_stereo_44100_16.wav"), [&checks] { return ++checks > 3; });
            expect(record.hasError());
            expect(record.shortTermLoudness == nullptr);
        }

        beginTest("An MP3 bit depth is never read from its tags");
        {
            const TemporaryDirectory directory;
            const auto tagged = directory.copyFixture("sine_stereo_44100_320k.mp3", "tagged.mp3");
            AudioMetadata metadata;
            metadata.properties.push_back({.key = "TITLE", .values = {"Master 24-bit 96 kHz"}});
            metadata.properties.push_back({.key = "COMMENT", .values = {"24 bit"}});
            expect(writeMetadata(tagged, metadata));

            const auto record = analyzeFile(tagged);
            expect(!record.hasError(), record.errorMessage);
            expectEquals(record.bitsPerSample, 16);
        }

        beginTest("A file that is not audio fails analysis");
        {
            const auto record = analyzeFile(fixtureFile("broken.wav"));
            expect(record.hasError());
            expect(record.error == AnalysisError::unsupported);
            expect(record.errorMessage.isNotEmpty());
            expect(record.shortTermLoudness == nullptr);
        }

        beginTest("Failed analyses say whether the failure would repeat for the same bytes");
        {
            const auto missing = analyzeFile(fixtureFile("missing.wav"));
            expect(missing.error == AnalysisError::missingFile);
            const auto cancelled = analyzeFile(fixtureFile("sine_stereo_44100_16.wav"), [] { return true; });
            expect(cancelled.error == AnalysisError::cancelled);
            expect(!analyzeFile(fixtureFile("sine_stereo_44100_16.wav")).error.has_value());
            expect(isRepeatable(AnalysisError::unsupported));
            expect(!isRepeatable(AnalysisError::decodeFailed));
            expect(!isRepeatable(AnalysisError::cancelled));
        }
    }

private:
    /// Uses short slices of the committed tone to cover exact EOF and the zero-filled startup window.
    void testLoudnessHistory()
    {
        beginTest("Short-term history reuses the three-second meter at ten block ends per second");
        const auto record = analyzeFile(fixtureFile("sine_stereo_44100_16.wav"));
        expect(record.shortTermLoudness != nullptr);
        if (record.shortTermLoudness) {
            const auto& history = *record.shortTermLoudness;
            expectEquals(static_cast<int>(history.values.size()), 10);
            expectWithinAbsoluteError(history.timeAt(0), 0.1, 0.000001);
            expectWithinAbsoluteError(history.timeAt(9), 1.0, 0.000001);
            expectWithinAbsoluteError(
                static_cast<double>(history.values.back()), fixtureLoudness - 10.0 * std::log10(3.0), 0.1
            );
            expect(history.values.front() < history.values.back());
            auto copied = record;
            expect(copied.shortTermLoudness == record.shortTermLoudness);
            copied.shortTermLoudness.reset();
            expect(record.shortTermLoudness != nullptr);
        }

        beginTest("A final partial block and a sub-block file end at their actual last sample");
        const TemporaryDirectory temporary;
        const auto reader = utils::createAudioReader(fixtureFile("sine_stereo_44100_16.wav"));
        expect(reader != nullptr);
        if (!reader) {
            return;
        }
        for (const int frames : {1000, 4500}) {
            const auto file = temporary.getChildFile(utils::format("slice-{}.wav", frames));
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
            auto writer = format.createWriterFor(
                stream,
                juce::AudioFormatWriterOptions()
                    .withSampleRate(reader->sampleRate)
                    .withNumChannels(2)
                    .withBitsPerSample(16)
            );
            expect(writer != nullptr);
            if (!writer) {
                continue;
            }
            expect(writer->writeFromAudioReader(*reader, 0, frames));
            writer.reset();
            const auto slice = analyzeFile(file);
            expect(!slice.hasError(), slice.errorMessage);
            expect(slice.shortTermLoudness != nullptr);
            if (slice.shortTermLoudness) {
                const auto& history = *slice.shortTermLoudness;
                expectEquals(static_cast<int>(history.values.size()), frames < 4410 ? 1 : 2);
                expectWithinAbsoluteError(
                    history.timeAt(history.values.size() - 1), static_cast<double>(frames) / 44'100.0, 0.0000001
                );
                expectEquals(slice.lengthInSamples, static_cast<std::int64_t>(frames));
            }
        }
    }

    /// Analyzes one fixture and compares the results with its expected properties.
    void checkToneFixture(const ToneFixture& fixture)
    {
        beginTest(fixture.fileName);

        const auto record = analyzeFile(fixtureFile(fixture.fileName));
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
        expect(record.shortTermLoudness != nullptr);
        if (record.shortTermLoudness && !record.shortTermLoudness->values.empty()) {
            const auto& history = *record.shortTermLoudness;
            const auto expectedCount = record.lengthInSamples / history.samplesPerPoint
                + (record.lengthInSamples % history.samplesPerPoint != 0 ? 1 : 0);
            expectEquals(static_cast<std::int64_t>(history.values.size()), expectedCount);
            expectWithinAbsoluteError(history.timeAt(history.values.size() - 1), record.durationSeconds, 0.0000001);
            expectWithinAbsoluteError(
                static_cast<double>(*std::ranges::max_element(history.values)), record.maxShortTermLufs, 0.00001
            );
        }
    }
};

std::unique_ptr<juce::UnitTest> createAnalysisTests()
{
    return std::make_unique<AnalysisTests>();
}
}  // namespace audiobatch::tests
