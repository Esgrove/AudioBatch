/// Tests for the normalization functions.
/// Covers output path resolution, batch output conflicts, refusal to overwrite unrelated files, cancellation, the level
/// and sample rate of the written output, and the tags and art carried over to it.
/// Only in-place `.aif` sources are normalized successfully here,
/// because converting other formats moves the original to the system trash.

#include "analysis/AudioAnalysis.h"
#include "normalization/AudioNormalization.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioMetadata.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Allowed difference between the normalized peak and 0 dBFS, in dB.
constexpr double peakTolerance = 0.01;

/// Builds an analysis record for a file path without analyzing it.
AudioAnalysisRecord recordForPath(const juce::String& path)
{
    return AudioAnalysisRecord::fromFile(juce::File(path));
}
}  // namespace

/// Checks normalization paths, conflicts, and output.
class NormalizationTests final : public juce::UnitTest
{
public:
    NormalizationTests() : juce::UnitTest("Normalization", "AudioBatch") {}

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
        expectEquals(getNormalizationOutputFile(root.getChildFile("song.mp3")).getFileName(), juce::String("song.aif"));
        expectEquals(
            getNormalizationOutputFile(root.getChildFile("song.aiff")).getFileName(), juce::String("song.aif")
        );
        expectEquals(getNormalizationOutputFile(root.getChildFile("song.aif")).getFileName(), juce::String("song.aif"));

        beginTest("The format summary describes .aif sources as rewritten in place");
        {
            const auto summary = getFormatSupportSummary();
            expect(summary.contains("A .aif file is rewritten in place"), summary);
            expect(summary.contains("including other AIFF extensions, to a .aif file"), summary);
            expect(!summary.contains("when they are already AIFF"), summary);
        }
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

            const auto conflicts = findBatchOutputConflicts(records);
            expectEquals(conflicts.size(), 2);
            expect(conflicts.contains(root.getChildFile("song.mp3").getFullPathName()));
            expect(conflicts.contains(root.getChildFile("song.flac").getFullPathName()));
        }

        beginTest(
            "Without an in-place source, the first path wins regardless of "
            "input order"
        );
        {
            const std::vector records {
                recordForPath(root.getChildFile("song.mp3").getFullPathName()),
                recordForPath(root.getChildFile("song.flac").getFullPathName()),
            };

            const auto conflicts = findBatchOutputConflicts(records);
            expectEquals(conflicts.size(), 1);
            expect(conflicts.contains(root.getChildFile("song.mp3").getFullPathName()));
        }

        if (!juce::File::areFileNamesCaseSensitive()) {
            beginTest(
                "Outputs that differ only in case conflict where file names "
                "ignore case"
            );
            {
                const std::vector records {
                    recordForPath(root.getChildFile("song.mp3").getFullPathName()),
                    recordForPath(root.getChildFile("SONG.flac").getFullPathName()),
                };

                const auto conflicts = findBatchOutputConflicts(records);
                expectEquals(conflicts.size(), 1);
            }
        }
    }

    /// Normalizes generated files and checks the results on disk.
    void runFileTests()
    {
        const TemporaryDirectory directory;

        beginTest("No-op normalization logs the action before unpadded time and filename");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "log_noop.aif");
            const auto record = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(record);
            const ScopedLogCapture capture;
            const auto result = normalizeFile(record, options);
            expect(!result.hasError(), result.errorMessage);
            const auto messages = capture.getMessages();
            expect(std::ranges::any_of(messages, [&](const auto& message) {
                return message.contains("[INFO]: Normalized: ")
                    && message.endsWith(utils::format(" s, {} (already at target)", source.getFileName().quoted()));
            }));
        }

        beginTest("An unrelated existing output file is not overwritten");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "collision.wav");
            const auto existingOutput = directory.getChildFile("collision.aif");
            expect(existingOutput.replaceWithText("unrelated"));

            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(result.hasError());
            expect(!result.committed);
            expect(result.errorMessage.contains("already exists"), result.errorMessage);
            expect(source.existsAsFile());
            expectEquals(existingOutput.loadFileAsString(), juce::String("unrelated"));
        }

        beginTest(
            "A folder at the output path fails before rendering, even with "
            "force overwrite"
        );
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.wav", "folder_output.wav");
            const auto folder = directory.getChildFile("folder_output.aif");
            expect(folder.createDirectory());

            auto options = testNormalizationOptions();
            options.overwriteExisting = true;
            int cancellationChecks = 0;
            const auto result = normalizeFile(analyzeFile(source), options, [&] {
                ++cancellationChecks;
                return false;
            });

            expect(result.hasError());
            expect(result.errorMessage.contains("not a file"), result.errorMessage);
            expectEquals(cancellationChecks, 0);
            expect(folder.isDirectory());
            expect(source.existsAsFile());
        }

        beginTest("A cancelled run leaves the source untouched");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled.aif");
            const auto sizeBefore = source.getSize();
            const auto record = analyzeFile(source);

            const auto options = testNormalizationOptions();
            const auto result = normalizeFile(record, options, [] { return true; });
            expect(result.hasError());
            expectEquals(source.getSize(), sizeBefore);
            expectEquals(analyzeFile(source).overallPeak, record.overallPeak);
        }

        checkInPlaceNormalization(directory, "sine_mono_24000_16.aif", 1, 24'000, 16);
        checkInPlaceNormalization(directory, "sine_mono_48000_24.aif", 1, 48'000, 24);
        checkInPlaceNormalization(directory, "sine_stereo_44100_16.aif", 2, 44'100, 16);

        beginTest("Tags and embedded art survive in-place normalization");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "tagged.aif");
            AudioMetadata metadata;
            metadata.properties.push_back({.key = "TITLE", .values = {"Tagged tone"}});
            metadata.properties.push_back({.key = "ARTIST", .values = {"AudioBatch"}});
            const juce::String pictureText("not really a picture");
            metadata.pictures.push_back({
                .data = juce::MemoryBlock(pictureText.toRawUTF8(), pictureText.getNumBytesAsUTF8()),
                .mimeType = "image/jpeg",
                .description = "cover",
                .pictureType = "Front Cover",
            });
            expect(writeMetadata(source, metadata));

            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(!result.hasError(), result.errorMessage);

            AudioMetadata written;
            expect(readMetadata(source, written));
            const auto title
                = std::ranges::find_if(written.properties, [](const auto& entry) { return entry.key == "TITLE"; });
            expect(title != written.properties.end() && title->values[0] == "Tagged tone");
            expectEquals(static_cast<int>(written.pictures.size()), 1);
            expect(!written.pictures.empty() && written.pictures.front().data == metadata.pictures.front().data);
        }

        if (!juce::File::areFileNamesCaseSensitive()) {
            beginTest(
                "A source whose extension differs only in case is normalized "
                "in place"
            );
            {
                const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "Upper.AIF");
                const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());

                expect(!result.hasError(), result.errorMessage);
                expect(result.analysisRecord.file == source);
                expect(source.existsAsFile());
                expectWithinAbsoluteError(peakDecibels(result.analysisRecord), 0.0, peakTolerance);
            }
        }

        beginTest(
            "A file that changed since analysis is analyzed again before "
            "computing the gain"
        );
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "changed.aif");
            auto staleRecord = analyzeFile(source);
            // Pretend the file was a quieter one when it was analyzed.
            // Trusting this peak would apply 12 dB of gain and clip the output.
            staleRecord.overallPeak = 0.125f;
            staleRecord.modifiedTimeMs -= 1000;

            const auto result = normalizeFile(staleRecord, testNormalizationOptions());
            expect(!result.hasError(), result.errorMessage);
            expectWithinAbsoluteError(result.analysisRecord.integratedLufs, fixtureLoudness - fixturePeakDecibels, 0.1);
        }

        beginTest("Cancellation stops changed-source analysis before an in-place no-op");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled_refresh.aif");
            auto staleRecord = analyzeFile(source);
            juce::MemoryBlock originalBytes;
            expect(source.loadFileAsData(originalBytes));
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(staleRecord);
            staleRecord.modifiedTimeMs -= 1000;

            const auto result = normalizeFile(staleRecord, options, [] { return true; });
            expect(result.hasError());
            expect(!result.committed);
            expect(result.errorMessage.containsIgnoreCase("cancel"), result.errorMessage);
            juce::MemoryBlock remainingBytes;
            expect(source.loadFileAsData(remainingBytes));
            expect(remainingBytes == originalBytes);
        }

        beginTest(
            "A successful in-place no-op returns refreshed analysis without "
            "committing output"
        );
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "refreshed_no_op.aif");
            auto staleRecord = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(staleRecord);
            staleRecord.modifiedTimeMs -= 1000;
            staleRecord.overallPeak = 0.125f;

            const auto result = normalizeFile(staleRecord, options);
            expect(result.succeeded, result.errorMessage);
            expect(!result.committed);
            expect(result.analysisRecord.matchesFileOnDisk());
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), options.targetPeakDbfs, peakTolerance);
        }

        beginTest("Cancellation stops re-analysis of an unchanged in-place no-op");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled_no_op.aif");
            const auto record = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(record);

            const auto result = normalizeFile(record, options, [] { return true; });
            expect(result.hasError());
            expect(!result.committed);
            expect(result.errorMessage.containsIgnoreCase("cancel"), result.errorMessage);
        }

        beginTest(
            "Loudness mode reaches the target loudness unless the true peak "
            "ceiling is lower"
        );
        {
            const auto record = analyzeFile(fixtureFile("sine_stereo_44100_16.wav"));
            auto options = testNormalizationOptions();
            options.mode = AudioNormalizationMode::loudness;

            options.targetLufs = -14.0;
            auto gain = getNormalizationGain(record, options);
            expect(gain.has_value(), gain.error_or({}));
            expectWithinAbsoluteError(
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(0.0f))),
                -14.0 - record.integratedLufs,
                0.01
            );

            // Reaching 0 LUFS would put the true peak at about 0 dBTP,
            // above the ceiling, so the gain stops at the ceiling.
            options.targetLufs = 0.0;
            gain = getNormalizationGain(record, options);
            expect(gain.has_value(), gain.error_or({}));
            expectWithinAbsoluteError(
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(0.0f))),
                AudioNormalizationOptions::defaultTruePeakCeilingDbtp
                    - juce::Decibels::gainToDecibels(record.overallTruePeak),
                0.01
            );
        }

        beginTest("In-place loudness normalization reaches the target");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "loudness.aif");
            auto options = testNormalizationOptions();
            options.mode = AudioNormalizationMode::loudness;
            options.targetLufs = -14.0;

            const auto result = normalizeFile(analyzeFile(source), options);
            expect(!result.hasError(), result.errorMessage);
            expectWithinAbsoluteError(result.analysisRecord.integratedLufs, -14.0, 0.1);
        }

        beginTest("Peak mode reaches a target below full scale");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "peak.aif");
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;

            const auto result = normalizeFile(analyzeFile(source), options);
            expect(!result.hasError(), result.errorMessage);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), -1.0, peakTolerance);
        }

        beginTest("A silent file cannot be normalized");
        {
            const auto source = directory.copyFixture("silence_stereo_44100_16.wav");
            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(result.hasError());
            expect(source.existsAsFile());
        }

        beginTest("A nearly silent file needs more gain than normalization applies");
        {
            auto record = analyzeFile(fixtureFile("sine_stereo_44100_16.aif"));
            // -60 dBFS, so reaching full scale would need about 60 dB of gain.
            record.overallPeak = 0.001f;
            record.overallTruePeak = 0.001;
            record.integratedLufs = -80.0;
            auto options = testNormalizationOptions();

            const auto peakGain = getNormalizationGain(record, options);
            expect(!peakGain.has_value());
            expect(peakGain.error_or({}).contains("dB of gain"), peakGain.error_or({}));

            options.mode = AudioNormalizationMode::loudness;
            const auto loudnessGain = getNormalizationGain(record, options);
            expect(!loudnessGain.has_value());
            expect(loudnessGain.error_or({}).contains("dB of gain"), loudnessGain.error_or({}));

            // 30 dB of gain stays within the limit.
            options.mode = AudioNormalizationMode::peak;
            options.targetPeakDbfs = -30.0;
            const auto gain = getNormalizationGain(record, options);
            expect(gain.has_value(), gain.error_or({}));
            expectWithinAbsoluteError(
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(0.0f))), 30.0, 0.01
            );
        }

        beginTest("Converting an MP3 file keeps the start of the audio in place");
        {
            const auto source = directory.copyFixture("click_mono_44100_320k.mp3", "click.mp3");
            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());

            expect(!result.hasError(), result.errorMessage);
            expectEquals(result.analysisRecord.file.getFileName(), juce::String("click.aif"));
            expectEquals(result.analysisRecord.lengthInSamples, clickFixtureLength);
            expectEquals(peakSamplePosition(result.analysisRecord.file), clickFixturePosition);
        }

        beginTest("An .aif source normalized in place disposes of a copy of the original first");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "backup.aif");
            const auto sourceSize = source.getSize();
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;
            juce::StringArray disposedNames;
            juce::Array<juce::int64> disposedSizes;
            options.disposeFile = [&](const juce::File& file) {
                disposedNames.add(file.getFileName());
                disposedSizes.add(file.getSize());
                return file.deleteFile();
            };

            const auto result = normalizeFile(analyzeFile(source), options);
            expect(!result.hasError(), result.errorMessage);
            expectEquals(disposedNames.joinIntoString(","), juce::String("backup.aif"));
            expectEquals(disposedSizes[0], sourceSize);
            expectWithinAbsoluteError(peakDecibels(result.analysisRecord), -1.0, peakTolerance);
        }

        beginTest("An in-place no-op disposes of nothing");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "no_backup.aif");
            const auto record = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(record);
            int disposedCount = 0;
            options.disposeFile = [&disposedCount](const juce::File& file) {
                ++disposedCount;
                return file.deleteFile();
            };

            const auto result = normalizeFile(record, options);
            expect(!result.hasError(), result.errorMessage);
            expectEquals(disposedCount, 0);
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
        const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
        expect(!result.hasError(), result.errorMessage);
        expectEquals(result.analysisRecord.fullPath, source.getFullPathName());
        expectEquals(result.analysisRecord.channels, channels);
        expectEquals(result.analysisRecord.sampleRate, sampleRate);
        expectEquals(result.analysisRecord.bitsPerSample, bitsPerSample);
        expectWithinAbsoluteError(peakDecibels(result.analysisRecord), 0.0, peakTolerance);
    }
};

std::unique_ptr<juce::UnitTest> createNormalizationTests()
{
    return std::make_unique<NormalizationTests>();
}
}  // namespace audiobatch::tests
