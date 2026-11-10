/// Integration tests for analyzeFile on the committed fixtures.
/// Every fixture tone has the same level, so the tests check that each format,
/// channel count, sample rate, and bit depth is decoded and measured consistently.
/// The intersample fixture has a different sample peak, true peak, and loudness,
/// which shows that each measurement ends up in its own field.

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "metadata/AudioMetadata.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#if !JUCE_WINDOWS
#include <sys/stat.h>
#endif

namespace audiobatch::tests
{
namespace
{
/// Expected properties of one fixture tone, ordered to match the designated fixture initializers.
struct ToneFixture {
    /// Name of the committed fixture file.
    const char* fileName {};
    /// Expected channel count.
    int channels {};
    /// Expected sample rate in Hz.
    int sampleRate {};
    /// Expected source bit depth.
    /// MP3 and AAC files report the fixed default of 16 bits, since the formats store no source bit depth.
    int bitsPerSample {};
    /// True for lossy formats, which get a wider level tolerance.
    bool lossy {};
    /// Exact decoded length in samples, or nothing for one second at the sample rate.
    /// Lossy files decode to an exact length too, since their encoder delay and padding are removed.
    std::optional<juce::int64> lengthInSamples {std::nullopt};
};

/// The tone fixtures and what analysis should report for them.
constexpr std::array TONE_FIXTURES {
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
        .fileName = "sine_stereo_44100_192k_no_xing.mp3",
        .channels = 2,
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = true,
        .lengthInSamples = NO_XING_FIXTURE_LENGTH,
    },
#if JUCE_MAC
    // Only Core Audio decodes AAC, and it removes the priming and padding that the MP4 edit list records.
    ToneFixture {
        .fileName = "sine_stereo_44100_aac.m4a",
        .channels = 2,
        .sampleRate = 44'100,
        .bitsPerSample = 16,
        .lossy = true,
    },
#endif
};

/// Allowed level difference for lossless fixtures, in dB or LU.
constexpr double LOSSLESS_TOLERANCE {0.05};
/// Allowed level difference for lossy fixtures, in dB or LU.
constexpr double LOSSY_TOLERANCE {0.5};

}  // namespace

/// Checks the measured peaks, loudness, and file properties of every fixture.
class AnalysisTests final : public juce::UnitTest
{
public:
    AnalysisTests() : juce::UnitTest("Audio analysis", "AudioBatch") {}

    /// Runs the analysis checks.
    void runTest() override
    {
        for (const auto& fixture : TONE_FIXTURES) {
            checkToneFixture(fixture);
        }

        beginTest("Digital silence has no peak and no measurable loudness");
        {
            const auto record = analyzeFile(fixtureFile("silence_stereo_44100_16.wav"));
            expect(!record.hasError(), record.errorMessage);
            expectEquals(record.overallPeak, 0.0f);
            expectEquals(record.integratedLufs, AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS);
            expect(record.shortTermLoudness != nullptr);
            if (record.shortTermLoudness) {
                expectEquals(static_cast<int>(record.shortTermLoudness->values.size()), 10);
                for (const auto value : record.shortTermLoudness->values) {
                    expectEquals(value, static_cast<float>(AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS));
                }
            }
        }

        beginTest("Sample peak, true peak, and loudness are reported in their own fields");
        {
            const auto record = analyzeFile(fixtureFile("intersample_stereo_44100_24.wav"));
            expect(!record.hasError(), record.errorMessage);
            expectWithinAbsoluteError(peakDecibels(record), INTERSAMPLE_FIXTURE_PEAK_DECIBELS, LOSSLESS_TOLERANCE);
            expectWithinAbsoluteError(
                juce::Decibels::gainToDecibels(record.overallTruePeak),
                INTERSAMPLE_FIXTURE_TRUE_PEAK_DECIBELS,
                LOSSLESS_TOLERANCE
            );
            expectWithinAbsoluteError(record.integratedLufs, INTERSAMPLE_FIXTURE_LOUDNESS, LOSSLESS_TOLERANCE);
        }

        testLoudnessHistory();
        testGainAdjustedLoudness();
        testLimiterIntegratedLoudness();

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

        beginTest("Only a recursive scan collects files from subfolders, and only supported audio files");
        {
            const TemporaryDirectory directory;
            const auto top = directory.copyFixture("sine_stereo_44100_16.wav", "top.wav");
            const auto nested = directory.getChildFile("albums/nested.wav");
            const auto deep = directory.getChildFile("albums/disc 2/deep.flac");
            expect(deep.getParentDirectory().createDirectory());
            expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(nested));
            expect(fixtureFile("sine_stereo_44100_16.flac").copyFileTo(deep));
            expect(directory.getChildFile("albums/notes.txt").replaceWithText("not audio"));

            const auto root = top.getParentDirectory();
            const auto flatFiles = collectInputFiles({root}, false);
            expectEquals(flatFiles.size(), 1);
            expect(flatFiles.contains(top));

            const auto recursiveFiles = collectInputFiles({root}, true);
            expectEquals(recursiveFiles.size(), 3);
            expect(recursiveFiles.contains(top));
            expect(recursiveFiles.contains(nested));
            expect(recursiveFiles.contains(deep));
        }

        beginTest("Overlapping inputs collect each file once, sorted by natural path order");
        {
            const TemporaryDirectory directory;
            const auto track10 = directory.copyFixture("sine_stereo_44100_16.wav", "track 10.wav");
            const auto track2 = directory.copyFixture("sine_stereo_44100_16.wav", "track 2.wav");

            const auto files = collectInputFiles({track10, track2.getParentDirectory(), track10, track2}, true);
            expectEquals(files.size(), 2);
            expect(files[0] == track2, files[0].getFullPathName());
            expect(files[1] == track10, files[1].getFullPathName());
        }

        beginTest("A recursive scan follows a symlink into another folder");
        {
            const TemporaryDirectory directory;
            const auto library = directory.getChildFile("library");
            const auto elsewhere = directory.getChildFile("elsewhere");
            expect(library.createDirectory());
            expect(elsewhere.createDirectory());
            expect(fixtureFile("sine_stereo_44100_16.wav").copyFileTo(elsewhere.getChildFile("linked.wav")));

            // Creating a symlink needs extra privileges on Windows, so the check only runs where the link exists.
            if (elsewhere.createSymbolicLink(library.getChildFile("link"), true)) {
                const auto files = collectInputFiles({library}, true);
                expectEquals(files.size(), 1);
                expect(!files.isEmpty() && files[0].getFileName() == "linked.wav");
                expect(collectInputFiles({library}, false).isEmpty());
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

        beginTest("Cancellation at the last check of an analysis fails it without any measurement");
        {
            const auto file = fixtureFile("sine_stereo_44100_16.wav");
            const auto completeChecks = std::make_shared<std::atomic<int>>(0);
            const auto complete = analyzeFile(file, cancelAfterChecks(std::numeric_limits<int>::max(), completeChecks));
            expect(!complete.hasError(), complete.errorMessage);
            expect(completeChecks->load() > 1, "the analysis must check for cancellation more than once");

            for (const auto allowedChecks : {completeChecks->load() / 2, completeChecks->load() - 1}) {
                const auto checks = std::make_shared<std::atomic<int>>(0);
                const auto record = analyzeFile(file, cancelAfterChecks(allowedChecks, checks));
                expect(record.error == AnalysisError::cancelled, utils::format("after {} checks", allowedChecks));
                expect(record.shortTermLoudness == nullptr);
                expectEquals(checks->load(), allowedChecks + 1);
            }
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
            expect(!isRepeatable(AnalysisError::unreadable));
            expect(!isRepeatable(AnalysisError::decodeFailed));
            expect(!isRepeatable(AnalysisError::cancelled));
        }

#if !JUCE_WINDOWS
        beginTest("A file that cannot be opened fails as unreadable instead of unsupported");
        {
            const TemporaryDirectory directory;
            const auto unreadableFile = directory.copyFixture("sine_stereo_44100_16.wav");
            const auto& path = unreadableFile.getFullPathName();
            expectEquals(::chmod(path.toRawUTF8(), 0), 0);
            const bool permissionsApply = unreadableFile.createInputStream() == nullptr;
            const auto record = analyzeFile(unreadableFile);
            expectEquals(::chmod(path.toRawUTF8(), S_IRUSR | S_IWUSR), 0);
            // Running as root ignores the permissions, so there is nothing to check then.
            if (permissionsApply) {
                expect(record.error == AnalysisError::unreadable);
                expectEquals(record.errorMessage, juce::String("File could not be opened for reading"));
            }
        }
#endif
    }

private:
    /// Checks the preset-specific integrated presentation fit against independent scipy predictions.
    void testLimiterIntegratedLoudness()
    {
        beginTest("L2 integrated estimate matches four independently fitted +2 dB track cases");
        // fixtures/limiter_model.py content_aware.fitted_predictions, gain index 2.
        for (const auto& values :
             std::array {
                 std::array {-10.8243, -8.9767, -9.635531818443239},
                 std::array {-11.1205, -9.1929, -9.70544841850931},
                 std::array {-8.6760, -6.7819, -7.359654457001012},
                 std::array {-13.3286, -11.3533, -11.753035984135359},
             })
        {
            const auto estimate = estimateLimiterIntegratedLoudness(values[0], Decibels {2.0f}, values[1]);
            expect(estimate.has_value());
            if (estimate) {
                expectWithinAbsoluteError(*estimate, values[2], 1.0e-12);
            }
        }

        beginTest("L2 estimate includes both gain endpoints and extrapolates source loudness only");
        expectWithinAbsoluteError(
            estimateLimiterIntegratedLoudness(-10.8243, Decibels {0.0f}, -10.8243).value_or(0.0), -11.1243, 1.0e-12
        );
        expectWithinAbsoluteError(
            estimateLimiterIntegratedLoudness(-10.8243, Decibels {6.0f}, -6.2739).value_or(0.0),
            -8.324502573333149,
            1.0e-12
        );
        // Negative measured loss is clamped before the fractional power, without inventing attenuation.
        expectWithinAbsoluteError(
            estimateLimiterIntegratedLoudness(-25.0, Decibels {2.0f}, -22.0).value_or(0.0), -22.3, 1.0e-12
        );
        expect(estimateLimiterIntegratedLoudness(-5.0, Decibels {2.0f}, -4.0).has_value());

        beginTest("Invalid, silent, negative and excessive gains use unchanged hard-clip fallback");
        for (const auto gain :
             {-1.0f, 6.001f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        {
            const auto estimate = estimateLimiterIntegratedLoudness(-10.0, Decibels {gain}, -8.1);
            expect(!estimate);
            expectEquals(estimate.value_or(-8.1), -8.1);
        }
        for (const auto invalid :
             {
                 AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS,
                 -1001.0,
                 std::numeric_limits<double>::infinity(),
                 -std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::quiet_NaN(),
             })
        {
            expect(!estimateLimiterIntegratedLoudness(invalid, Decibels {2.0f}, -8.1));
            expect(!estimateLimiterIntegratedLoudness(-10.0, Decibels {2.0f}, invalid));
        }
        const auto silence = AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS;
        expectEquals(estimateLimiterIntegratedLoudness(silence, Decibels {2.0f}, silence).value_or(silence), silence);
    }

    /// Compares the model with independently written clipped PCM, including absolute-gate changes and silence.
    void testGainAdjustedLoudness()
    {
        beginTest("Gain plus hard clipping remeasures PCM instead of adding gain to stored loudness");
        const TemporaryDirectory temporary;
        const auto source = temporary.copyFixture("sine_stereo_44100_16.wav", "source.wav");
        juce::MemoryBlock sourceBytes;
        expect(source.loadFileAsData(sourceBytes));
        const auto sourceRecord = analyzeFile(source);
        const auto reader = utils::createAudioReader(source);
        expect(reader != nullptr);
        if (!reader) {
            return;
        }
        juce::AudioBuffer<float> samples(2, static_cast<int>(reader->lengthInSamples));
        expect(reader->read(&samples, 0, samples.getNumSamples(), 0, true, true));
        for (const float gain : {-80.0f, -3.0f, 12.0f}) {
            juce::AudioBuffer<float> clipped(2, samples.getNumSamples());
            const auto linear = std::pow(10.0, static_cast<double>(gain) / 20.0);
            for (int channel = 0; channel < 2; ++channel) {
                for (int frame = 0; frame < clipped.getNumSamples(); ++frame) {
                    clipped.setSample(
                        channel,
                        frame,
                        static_cast<float>(
                            std::clamp(static_cast<double>(samples.getSample(channel, frame)) * linear, -1.0, 1.0)
                        )
                    );
                }
            }
            const auto rendered = temporary.getChildFile(utils::format("clipped-{}.wav", gain));
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream = rendered.createOutputStream();
            auto writer = format.createWriterFor(
                stream,
                juce::AudioFormatWriterOptions()
                    .withSampleRate(reader->sampleRate)
                    .withNumChannels(2)
                    .withBitsPerSample(24)
            );
            expect(writer != nullptr);
            if (!writer) {
                continue;
            }
            expect(writer->writeFromAudioSampleBuffer(clipped, 0, clipped.getNumSamples()));
            writer.reset();
            const auto expected = analyzeFile(rendered);
            const auto actual = measureGainAdjustedLoudness(source, Decibels {gain});
            expect(actual.has_value());
            if (actual) {
                expectWithinAbsoluteError(actual->integratedLufs, expected.integratedLufs, 0.001);
                expectWithinAbsoluteError(actual->maxShortTermLufs, expected.maxShortTermLufs, 0.001);
                if (gain > 0.0f) {
                    expect(actual->integratedLufs < sourceRecord.integratedLufs + static_cast<double>(gain) - 1.0);
                }
            }
        }
        const auto gatedSource = temporary.getChildFile("clipped--80.wav");
        expectEquals(analyzeFile(gatedSource).integratedLufs, AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS);
        const auto raisedAboveGate = measureGainAdjustedLoudness(gatedSource, Decibels {80.0f});
        expect(raisedAboveGate.has_value());
        if (raisedAboveGate) {
            expect(raisedAboveGate->integratedLufs > -20.0);
        }
        juce::MemoryBlock unchangedBytes;
        expect(source.loadFileAsData(unchangedBytes));
        expect(unchangedBytes == sourceBytes);
        expect(!measureGainAdjustedLoudness(source, Decibels {3.0f}, [] { return true; }));
        int cancellationChecks = 0;
        expect(!measureGainAdjustedLoudness(source, Decibels {3.0f}, [&] { return ++cancellationChecks > 3; }));
        expect(cancellationChecks > 3);
        expect(!measureGainAdjustedLoudness(source, Decibels {std::numeric_limits<float>::infinity()}));
        expect(!measureGainAdjustedLoudness(fixtureFile("broken.wav"), Decibels {3.0f}));
        const auto silence = measureGainAdjustedLoudness(fixtureFile("silence_stereo_44100_16.wav"), Decibels {20.0f});
        expect(silence.has_value());
        if (silence) {
            expectEquals(silence->integratedLufs, AudioAnalysisRecord::NEGATIVE_INFINITY_LOUDNESS);
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

        expectEquals(record.bitsPerSample, fixture.bitsPerSample);

        const auto tolerance = fixture.lossy ? LOSSY_TOLERANCE : LOSSLESS_TOLERANCE;
        expectWithinAbsoluteError(peakDecibels(record), FIXTURE_PEAK_DECIBELS, tolerance);
        expectWithinAbsoluteError(
            juce::Decibels::gainToDecibels(record.overallTruePeak), FIXTURE_PEAK_DECIBELS, tolerance
        );
        expectWithinAbsoluteError(record.integratedLufs, FIXTURE_LOUDNESS, tolerance);
        const auto expectedLength = fixture.lengthInSamples.value_or(juce::int64 {fixture.sampleRate});
        expectEquals(record.lengthInSamples, static_cast<std::int64_t>(expectedLength), "decoded length in samples");
        expectWithinAbsoluteError(
            record.durationSeconds, static_cast<double>(expectedLength) / fixture.sampleRate, 0.000001
        );
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
                static_cast<double>(history.values.back()), FIXTURE_LOUDNESS - 10.0 * std::log10(3.0), 0.1
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
};

std::unique_ptr<juce::UnitTest> createAnalysisTests()
{
    return std::make_unique<AnalysisTests>();
}
}  // namespace audiobatch::tests
