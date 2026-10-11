/// Tests for AudioNormalizationService.
/// Covers output path resolution, batch output conflicts, refusal to overwrite unrelated files,
/// cancellation, and the level and sample rate of the written output.
/// Only in-place `.aif` sources are normalized successfully here,
/// because converting other formats moves the original to the system trash.

#include "AudioAnalysisService.h"
#include "AudioNormalizationService.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils.h"

#include <cmath>
#include <vector>

namespace audiobatch::tests
{
/// Allowed difference between the normalized peak and 0 dBFS, in dB.
constexpr double peakTolerance = 0.01;

/// Builds an analysis record for a file path without analyzing it.
static AudioAnalysisRecord recordForPath(const juce::String& path)
{
    return AudioAnalysisRecord::fromFile(juce::File(path));
}

/// Checks normalization paths, conflicts, and output.
class NormalizationTests final : public juce::UnitTest
{
public:
    NormalizationTests() : juce::UnitTest("Normalization", "AudioBatch") { }

    /// Runs the normalization checks.
    void runTest() override
    {
        runOutputPathTests();
        runConflictTests();
        runFileTests();
    }

private:
    /// Checks that every source maps to a sibling `.aif` file.
    void runOutputPathTests()
    {
        beginTest("Output is always a sibling .aif file");

        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory);
        expectEquals(
            AudioNormalizationService::getOutputFile(root.getChildFile("song.mp3")).getFileName(),
            juce::String("song.aif")
        );
        expectEquals(
            AudioNormalizationService::getOutputFile(root.getChildFile("song.aiff")).getFileName(),
            juce::String("song.aif")
        );
        expectEquals(
            AudioNormalizationService::getOutputFile(root.getChildFile("song.aif")).getFileName(),
            juce::String("song.aif")
        );
    }

    /// Checks which records lose when several sources map to the same output.
    void runConflictTests()
    {
        const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory);

        beginTest("An in-place .aif source wins its output path");
        {
            const std::vector records {
                recordForPath(root.getChildFile("song.mp3").getFullPathName()),
                recordForPath(root.getChildFile("song.aif").getFullPathName()),
                recordForPath(root.getChildFile("song.flac").getFullPathName()),
                recordForPath(root.getChildFile("other.wav").getFullPathName()),
            };

            const auto conflicts = AudioNormalizationService::findBatchOutputConflicts(records);
            expectEquals(conflicts.size(), 2);
            expect(conflicts.contains(root.getChildFile("song.mp3").getFullPathName()));
            expect(conflicts.contains(root.getChildFile("song.flac").getFullPathName()));
        }

        beginTest("Without an in-place source, the first path wins regardless of input order");
        {
            const std::vector records {
                recordForPath(root.getChildFile("song.mp3").getFullPathName()),
                recordForPath(root.getChildFile("song.flac").getFullPathName()),
            };

            const auto conflicts = AudioNormalizationService::findBatchOutputConflicts(records);
            expectEquals(conflicts.size(), 1);
            expect(conflicts.contains(root.getChildFile("song.mp3").getFullPathName()));
        }
    }

    /// Normalizes generated files and checks the results on disk.
    void runFileTests()
    {
        const TemporaryDirectory directory;

        beginTest("An unrelated existing output file is not overwritten");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "collision.wav");
            const auto existingOutput = directory.getChildFile("collision.aif");
            expect(existingOutput.replaceWithText("unrelated"));

            const auto result = AudioNormalizationService::normalizeFile(AudioAnalysisService::analyzeFile(source), {});
            expect(result.hasError());
            expect(result.errorMessage.contains("already exists"), result.errorMessage);
            expect(source.existsAsFile());
            expectEquals(existingOutput.loadFileAsString(), juce::String("unrelated"));
        }

        beginTest("A cancelled run leaves the source untouched");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled.aif");
            const auto sizeBefore = source.getSize();
            const auto record = AudioAnalysisService::analyzeFile(source);

            const AudioNormalizationOptions options;
            const auto result = AudioNormalizationService::normalizeFile(record, options, [] { return true; });
            expect(result.hasError());
            expectEquals(source.getSize(), sizeBefore);
            expectEquals(AudioAnalysisService::analyzeFile(source).overallPeak, record.overallPeak);
        }

        checkInPlaceNormalization(directory, "sine_mono_24000_16.aif", 1, 24000, 16);
        checkInPlaceNormalization(directory, "sine_mono_48000_24.aif", 1, 48000, 24);
        checkInPlaceNormalization(directory, "sine_stereo_44100_16.aif", 2, 44100, 16);

        beginTest("A file that changed since analysis is analyzed again before computing the gain");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "changed.aif");
            auto staleRecord = AudioAnalysisService::analyzeFile(source);
            // Pretend the file was a quieter one when it was analyzed.
            // Trusting this peak would apply 12 dB of gain and clip the output.
            staleRecord.overallPeak = 0.125f;
            staleRecord.modifiedTimeMs -= 1000;

            const auto result = AudioNormalizationService::normalizeFile(staleRecord, {});
            expect(!result.hasError(), result.errorMessage);
            expectWithinAbsoluteError(
                result.analysisRecord.integratedLufs, fixtureStereoLoudness - fixturePeakDecibels, 0.1
            );
        }

        beginTest("A silent file cannot be normalized");
        {
            const auto source = directory.copyFixture("silence_stereo_44100_16.wav");
            const auto result = AudioNormalizationService::normalizeFile(AudioAnalysisService::analyzeFile(source), {});
            expect(result.hasError());
            expect(source.existsAsFile());
        }
    }

    /// Normalizes a copy of an `.aif` fixture in place,
    /// and checks that it reaches 0 dBFS and keeps its channel count, sample rate, and bit depth.
    void checkInPlaceNormalization(
        const TemporaryDirectory& directory,
        const juce::String& fixtureName,
        const int channels,
        const int sampleRate,
        const int bitsPerSample
    )
    {
        beginTest(utils::format("In-place normalization of {}", fixtureName));

        const auto source = directory.copyFixture(fixtureName);
        const auto result = AudioNormalizationService::normalizeFile(AudioAnalysisService::analyzeFile(source), {});
        expect(!result.hasError(), result.errorMessage);
        expectEquals(result.analysisRecord.fullPath, source.getFullPathName());
        expectEquals(result.analysisRecord.channels, channels);
        expectEquals(result.analysisRecord.sampleRate, sampleRate);
        expectEquals(result.analysisRecord.bitsPerSample, bitsPerSample);
        expectWithinAbsoluteError(
            static_cast<double>(juce::Decibels::gainToDecibels(std::abs(result.analysisRecord.overallPeak))),
            0.0,
            peakTolerance
        );
    }
};

std::unique_ptr<juce::UnitTest> createNormalizationTests()
{
    return std::make_unique<NormalizationTests>();
}
}  // namespace audiobatch::tests
