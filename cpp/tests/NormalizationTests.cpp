/// Tests for the normalization functions.
/// Covers output path resolution, batch output conflicts, refusal to overwrite unrelated files,
/// cancellation at every check, the level, sample rate, and bit depth of the written output,
/// the largest gain normalization applies,
/// and the tags and art carried over to the output, in place and when converting to AIFF.
/// A converted source is deleted instead of moved to the system trash, so the tests never touch the user's trash.
/// NormalizeCoordinatorTests.cpp covers the coordinator of a normalization run.

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "metadata/AudioMetadata.h"
#include "normalization/AudioNormalization.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/StringFormat.h"
#include <system_error>
#include <type_traits>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>
#include <variant>
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
        runGainLimitTests();
        runConversionTests();
        runLateCancellationTests();
    }

private:
    /// Checks both sides of the largest gain normalization applies, and loudness mode on digital silence.
    void runGainLimitTests()
    {
        beginTest("A gain of exactly 40 dB is applied and anything above it is refused");
        {
            auto record = analyzeFile(fixtureFile("sine_stereo_44100_16.aif"));
            // -46 dBFS, low enough that the true peak ceiling never limits the gain below.
            record.overallPeak = 0.005f;
            record.overallTruePeak = 0.005;
            auto options = testNormalizationOptions();
            options.mode = AudioNormalizationMode::loudness;
            options.targetLufs = -14.0;

            // Both values are whole numbers, so the gain is exactly the limit.
            record.integratedLufs = options.targetLufs - AudioNormalizationOptions::maximumGainDb;
            const auto atLimit = getNormalizationGain(record, options);
            expect(atLimit.has_value(), atLimit.error_or({}));
            expectWithinAbsoluteError(
                static_cast<double>(juce::Decibels::gainToDecibels(atLimit.value_or(LinearGain {0.0f}).value)),
                AudioNormalizationOptions::maximumGainDb,
                0.001
            );

            record.integratedLufs -= 0.01;
            const auto aboveLimit = getNormalizationGain(record, options);
            expect(!aboveLimit.has_value());
            expect(aboveLimit.error_or({}).contains("more than 40 dB"), aboveLimit.error_or({}));

            options.mode = AudioNormalizationMode::peak;
            options.targetPeakDbfs = 0.0;
            record.overallPeak = juce::Decibels::decibelsToGain(-39.99f);
            expect(getNormalizationGain(record, options).has_value());
            record.overallPeak = juce::Decibels::decibelsToGain(-40.01f);
            expect(!getNormalizationGain(record, options).has_value());
        }

        beginTest("Loudness mode refuses digital silence and a file without measurable loudness");
        {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture("silence_stereo_44100_16.wav");
            const auto record = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.mode = AudioNormalizationMode::loudness;

            const auto gain = getNormalizationGain(record, options);
            expect(!gain.has_value());
            expectEquals(gain.error_or({}), juce::String("File contains no signal that can be normalized"));

            const auto result = normalizeFile(record, options);
            expect(result.hasError());
            expectEquals(outcomeError(result.outcome), gain.error_or({}));
            expect(source.existsAsFile());
            expect(!getNormalizationOutputFile(source).exists());

            // A single click has a peak, but the gate leaves no loudness to measure.
            auto click = analyzeFile(fixtureFile("click_mono_44100_16.wav"));
            click.integratedLufs = AudioAnalysisRecord::negativeInfinityLoudness;
            const auto clickGain = getNormalizationGain(click, options);
            expectEquals(clickGain.error_or({}), juce::String("File has no measurable loudness to normalize"));
            options.mode = AudioNormalizationMode::peak;
            expect(getNormalizationGain(click, options).has_value());
        }
    }

    /// Converts sources of other formats to AIFF, checking the tags and art carried over and the output bit depth.
    void runConversionTests()
    {
        beginTest("Converting a FLAC or MP3 file to AIFF keeps its tags and cover art");
        for (const auto* fixtureName : {"tagged_stereo_44100_16.flac", "sine_stereo_44100_320k.mp3"}) {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture(fixtureName);
            AudioMetadata metadata;
            expect(readMetadata(source, metadata) || metadata.isEmpty());
            std::erase_if(metadata.properties, [](const MetadataProperty& property) {
                return property.key == "TITLE";
            });
            metadata.properties.push_back({.key = "TITLE", .values = {"Converted tone"}});
            metadata.properties.push_back({.key = "CUSTOM_FIELD", .values = {"custom value"}});
            const juce::String pictureText("cover art bytes");
            metadata.pictures.push_back({
                .data = juce::MemoryBlock(pictureText.toRawUTF8(), pictureText.getNumBytesAsUTF8()),
                .mimeType = "image/png",
                .description = "cover",
                .pictureType = "Front Cover",
            });
            expect(writeMetadata(source, metadata), fixtureName);

            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(!result.hasError(), outcomeError(result.outcome));
            const auto output = getNormalizationOutputFile(source);
            expect(output.existsAsFile(), fixtureName);
            expect(!source.exists(), "a converted source is disposed of");

            AudioMetadata written;
            expect(readMetadata(output, written), fixtureName);
            const auto valueOf = [&written](const juce::String& key) {
                const auto property = std::ranges::find(written.properties, key, &MetadataProperty::key);
                return property != written.properties.end() && !property->values.isEmpty() ? property->values[0]
                                                                                           : juce::String();
            };
            expectEquals(valueOf("TITLE"), juce::String("Converted tone"), fixtureName);
            expectEquals(valueOf("CUSTOM_FIELD"), juce::String("custom value"), fixtureName);
            expectEquals(static_cast<int>(written.pictures.size()), 1, fixtureName);
            if (!written.pictures.empty()) {
                expect(written.pictures.front().data == metadata.pictures.back().data, fixtureName);
                expectEquals(written.pictures.front().mimeType, juce::String("image/png"), fixtureName);
            }
        }

        beginTest(
            "Converted output keeps a supported source bit depth, writes lossy sources as 16-bit, and float as 24"
        );
        for (const auto& [fixtureName, expectedBits] :
             {
                 std::pair {"sine_stereo_44100_16.wav", 16},
                 std::pair {"sine_stereo_48000_24.flac", 24},
                 std::pair {"sine_stereo_44100_32f.wav", 24},
                 std::pair {"sine_stereo_44100_320k.mp3", 16},
                 std::pair {"sine_stereo_48000.ogg", 16},
             })
        {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture(fixtureName);
            const auto record = analyzeFile(source);
            const auto sourceReader = utils::createAudioReader(source);
            expect(sourceReader != nullptr, fixtureName);
            if (sourceReader != nullptr) {
                expectEquals(getNormalizationOutputBitDepth(record, *sourceReader), expectedBits, fixtureName);
            }

            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;
            const auto result = normalizeFile(record, options);
            expect(!result.hasError(), outcomeError(result.outcome));
            const auto outputReader = utils::createAudioReader(getNormalizationOutputFile(source));
            expect(outputReader != nullptr, fixtureName);
            if (outputReader != nullptr) {
                expectEquals(static_cast<int>(outputReader->bitsPerSample), expectedBits, fixtureName);
                expect(!outputReader->usesFloatingPointData, fixtureName);
                expectEquals(static_cast<int>(outputReader->sampleRate), record.sampleRate, fixtureName);
                expectEquals(outputReader->lengthInSamples, static_cast<juce::int64>(record.lengthInSamples));
            }
        }

        beginTest("A record without a bit depth uses 16 bits for MP3 and the reader's depth otherwise");
        {
            for (const auto& [fixtureName, expectedBits] :
                 {
                     std::pair {"sine_stereo_44100_320k.mp3", 16},
                     std::pair {"sine_stereo_48000_24.wav", 24},
                 })
            {
                auto record = analyzeFile(fixtureFile(fixtureName));
                record.bitsPerSample = 0;
                const auto reader = utils::createAudioReader(fixtureFile(fixtureName));
                expect(reader != nullptr, fixtureName);
                if (reader != nullptr) {
                    expectEquals(getNormalizationOutputBitDepth(record, *reader), expectedBits, fixtureName);
                }
            }
        }
    }

    /// Counts the cancellation checks of a complete in-place normalization,
    /// then cancels the same normalization at its first, a middle, and its last check.
    /// The last check comes after the output was written and validated, just before it would replace the source,
    /// so it covers the cleanup of a finished temporary output.
    void runLateCancellationTests()
    {
        beginTest("Replacing a source during rendering preserves newer source and approved output");
        for (const auto* fixture : {"sine_stereo_44100_16.wav", "sine_stereo_44100_16.aif"}) {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture(fixture);
            const auto output = getNormalizationOutputFile(source);
            const auto record = analyzeFile(source);
            if (source != output) {
                expect(output.replaceWithText("existing"));
            }
            auto options = testNormalizationOptions();
            options.overwriteExisting = true;
            int disposed = 0;
            options.disposeFile = [&disposed](const juce::File&) {
                ++disposed;
                return false;
            };
            bool replaced = false;
            const auto result = normalizeFile(record, options, {}, [&](const float fraction) {
                if (fraction >= 0.8f && !replaced) {
                    expect(source.replaceWithText("external"));
                    replaced = true;
                }
            });
            expect(replaced);
            expect(outcomeError(result.outcome).contains("source file changed"), outcomeError(result.outcome));
            expectEquals(disposed, 0);
            expectEquals(source.loadFileAsString(), juce::String("external"));
            if (source != output) {
                expectEquals(output.loadFileAsString(), juce::String("existing"));
            }
        }

        beginTest("Cancelling at any later check leaves the source unchanged and removes the temporary output");
        std::vector<int> allowedCheckCounts;
        {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "complete.aif");
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;
            const auto checks = std::make_shared<std::atomic<int>>(0);
            const auto result = normalizeFile(
                analyzeFile(source), options, cancelAfterChecks(std::numeric_limits<int>::max(), checks)
            );
            expect(!result.hasError(), outcomeError(result.outcome));
            expect(checks->load() > 1, "normalization must check for cancellation more than once");
            allowedCheckCounts = {0, checks->load() / 2, checks->load() - 1};
        }

        for (const auto allowedChecks : allowedCheckCounts) {
            const TemporaryDirectory directory;
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "late_cancel.aif");
            juce::MemoryBlock originalBytes;
            expect(source.loadFileAsData(originalBytes));
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;
            int disposedCount = 0;
            options.disposeFile = [&disposedCount](const juce::File& file) {
                ++disposedCount;
                return file.deleteFile();
            };

            const auto checks = std::make_shared<std::atomic<int>>(0);
            const auto result = normalizeFile(analyzeFile(source), options, cancelAfterChecks(allowedChecks, checks));
            const auto context = utils::format("cancelled after {} checks", allowedChecks);
            expect(result.hasError(), context);
            expect(outcomeError(result.outcome).containsIgnoreCase("cancel"), outcomeError(result.outcome));
            expectEquals(checks->load(), allowedChecks + 1, context);
            expectEquals(disposedCount, 0, context);
            juce::MemoryBlock remainingBytes;
            expect(source.loadFileAsData(remainingBytes));
            expect(remainingBytes == originalBytes, context);
            const auto leftovers = source.getParentDirectory().findChildFiles(
                juce::File::findFiles | juce::File::ignoreHiddenFiles, false
            );
            const auto hiddenLeftovers = source.getParentDirectory().findChildFiles(juce::File::findFiles, false);
            expectEquals(leftovers.size(), 1, context);
            expectEquals(hiddenLeftovers.size(), 1, context);
        }
    }

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

#if !JUCE_WINDOWS
        beginTest("Aliased parents reserve the same absent output with deterministic in-place priority");
        {
            const TemporaryDirectory directory;
            const auto real = directory.getChildFile("real");
            const auto alias = directory.getChildFile("alias");
            expect(real.createDirectory());
            std::error_code error;
            std::filesystem::create_directory_symlink(
                real.getFullPathName().toStdString(), alias.getFullPathName().toStdString(), error
            );
            expect(!error, juce::String(error.message()));
            std::vector records {
                recordForPath(real.getChildFile("song.wav").getFullPathName()),
                recordForPath(alias.getChildFile("song.flac").getFullPathName()),
            };
            auto conflicts = findBatchOutputConflicts(records);
            expectEquals(conflicts.size(), 1);
            expect(conflicts.contains(real.getChildFile("song.wav").getFullPathName()));
            std::ranges::reverse(records);
            expect(findBatchOutputConflicts(records) == conflicts);
            records.push_back(recordForPath(real.getChildFile("song.aif").getFullPathName()));
            conflicts = findBatchOutputConflicts(records);
            expectEquals(conflicts.size(), 2);
            expect(!conflicts.contains(real.getChildFile("song.aif").getFullPathName()));
            std::ranges::reverse(records);
            expect(findBatchOutputConflicts(records) == conflicts);
            expect(!real.getChildFile("song.aif").exists());
        }
#endif

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
            expect(!result.hasError(), outcomeError(result.outcome));
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
            expect(std::holds_alternative<OperationFailed>(result.outcome));
            expect(outcomeError(result.outcome).contains("already exists"), outcomeError(result.outcome));
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
            expect(outcomeError(result.outcome).contains("not a file"), outcomeError(result.outcome));
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
            expect(!result.hasError(), outcomeError(result.outcome));

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

                expect(!result.hasError(), outcomeError(result.outcome));
                expect(outcomeRecord(result.outcome).file == source);
                expect(source.existsAsFile());
                expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), 0.0, peakTolerance);
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
            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(
                outcomeRecord(result.outcome).integratedLufs, fixtureLoudness - fixturePeakDecibels, 0.1
            );
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
            expect(std::holds_alternative<OperationFailed>(result.outcome));
            expect(outcomeError(result.outcome).containsIgnoreCase("cancel"), outcomeError(result.outcome));
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
            const auto modifiedBefore = source.getLastModificationTime();
            const auto sizeBefore = source.getSize();

            const auto result = normalizeFile(staleRecord, options);
            expect(!result.hasError(), outcomeError(result.outcome));
            expect(source.getLastModificationTime() == modifiedBefore);
            expectEquals(source.getSize(), sizeBefore);
            expect(outcomeRecord(result.outcome).matchesFileOnDisk());
            expectWithinAbsoluteError(
                peakDecibels(outcomeRecord(result.outcome)), options.targetPeakDbfs, peakTolerance
            );
        }

        beginTest("Cancellation stops re-analysis of an unchanged in-place no-op");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "cancelled_no_op.aif");
            const auto record = analyzeFile(source);
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = peakDecibels(record);

            const auto result = normalizeFile(record, options, [] { return true; });
            expect(result.hasError());
            expect(std::holds_alternative<OperationFailed>(result.outcome));
            expect(outcomeError(result.outcome).containsIgnoreCase("cancel"), outcomeError(result.outcome));
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
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(LinearGain {0.0f}).value)),
                -14.0 - record.integratedLufs,
                0.01
            );

            // Reaching 0 LUFS would put the true peak at about 0 dBTP,
            // above the ceiling, so the gain stops at the ceiling.
            options.targetLufs = 0.0;
            gain = getNormalizationGain(record, options);
            expect(gain.has_value(), gain.error_or({}));
            expectWithinAbsoluteError(
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(LinearGain {0.0f}).value)),
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
            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(outcomeRecord(result.outcome).integratedLufs, -14.0, 0.1);
        }

        beginTest("Peak mode reaches a target below full scale");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "peak.aif");
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = -1.0;

            const auto result = normalizeFile(analyzeFile(source), options);
            expect(!result.hasError(), outcomeError(result.outcome));
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), -1.0, peakTolerance);
        }

        beginTest("A silent file cannot be normalized");
        {
            const auto source = directory.copyFixture("silence_stereo_44100_16.wav");
            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());
            expect(result.hasError());
            expect(source.existsAsFile());
        }

        beginTest("A gain in decibels converts to its linear factor and keeps the JUCE floor");
        {
            static_assert(!std::is_convertible_v<float, Decibels>);
            static_assert(!std::is_convertible_v<float, LinearGain>);
            expectWithinAbsoluteError(Decibels {}.toLinearGain().value, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError(Decibels {-6.0206f}.toLinearGain().value, 0.5f, 1.0e-4f);
            expectWithinAbsoluteError(Decibels {6.0206f}.toLinearGain().value, 2.0f, 1.0e-4f);
            expectEquals(Decibels {-120.0f}.toLinearGain().value, 0.0f);
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
                static_cast<double>(juce::Decibels::gainToDecibels(gain.value_or(LinearGain {0.0f}).value)), 30.0, 0.01
            );
        }

        beginTest("Converting an MP3 file keeps the start of the audio in place");
        {
            const auto source = directory.copyFixture("click_mono_44100_320k.mp3", "click.mp3");
            const auto result = normalizeFile(analyzeFile(source), testNormalizationOptions());

            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(outcomeRecord(result.outcome).file.getFileName(), juce::String("click.aif"));
            expectEquals(outcomeRecord(result.outcome).lengthInSamples, clickFixtureLength);
            expectEquals(peakSamplePosition(outcomeRecord(result.outcome).file), clickFixturePosition);
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
            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(disposedNames.joinIntoString(","), juce::String("backup.aif"));
            expectEquals(disposedSizes[0], sourceSize);
            expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), -1.0, peakTolerance);
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
            expect(!result.hasError(), outcomeError(result.outcome));
            expectEquals(disposedCount, 0);
        }

        beginTest("An .aif normalized to 0 dBFS is left as it is when normalized to 0 dBFS again");
        {
            const auto source = directory.copyFixture("sine_stereo_44100_16.aif", "full_scale.aif");
            auto options = testNormalizationOptions();
            options.targetPeakDbfs = 0.0;
            const auto first = normalizeFile(analyzeFile(source), options);
            expect(!first.hasError(), outcomeError(first.outcome));
            const auto normalizedSize = source.getSize();

            int disposedCount = 0;
            options.disposeFile = [&disposedCount](const juce::File& file) {
                ++disposedCount;
                return file.deleteFile();
            };
            const auto second = normalizeFile(analyzeFile(source), options);
            expect(!second.hasError(), outcomeError(second.outcome));
            expectEquals(disposedCount, 0);
            expectEquals(source.getSize(), normalizedSize);
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
        expect(!result.hasError(), outcomeError(result.outcome));
        expectEquals(outcomeRecord(result.outcome).fullPath, source.getFullPathName());
        expectEquals(outcomeRecord(result.outcome).channels, channels);
        expectEquals(outcomeRecord(result.outcome).sampleRate, sampleRate);
        expectEquals(outcomeRecord(result.outcome).bitsPerSample, bitsPerSample);
        expectWithinAbsoluteError(peakDecibels(outcomeRecord(result.outcome)), 0.0, peakTolerance);
    }
};

std::unique_ptr<juce::UnitTest> createNormalizationTests()
{
    return std::make_unique<NormalizationTests>();
}
}  // namespace audiobatch::tests
