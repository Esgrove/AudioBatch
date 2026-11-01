/// Tests fixed-interval streaming measurements and exact normalization PCM conversion.
/// Checks chunk-boundary invariance against file analysis and compares integer AIFF output
/// and observed samples with JUCE's float writer over quantization boundaries.

#include "analysis/AudioAnalysis.h"
#include "analysis/AudioAnalysisTypes.h"
#include "analysis/AudioMeasurement.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/FileUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace audiobatch::tests
{
/// Exercises arbitrary chunk sizes, complete histories, PCM endpoints, and invalid input.
class MeasurementTests final : public juce::UnitTest
{
public:
    /// Registers the lightweight stream and PCM regression suite.
    MeasurementTests() : juce::UnitTest("Streaming_measurement", "AudioBatch") {}

    /// Runs chunking, PCM roundtrip, and initialization failure checks.
    void runTest() override
    {
        runChunkTests();
        runPcmTests();

        beginTest("Rendered identity survives a rename but rejects measurements for replaced bytes");
        {
            const TemporaryDirectory directory;
            const auto file = directory.copyFixture("sine_stereo_44100_16.aif");
            const auto reference = analyzeFile(file);
            const utils::FileIdentity identity(file);
            const auto committed = directory.getChildFile("committed.aif");
            expect(file.moveFileTo(committed));
            const auto completed = completeRenderedAnalysis(reference, committed, 16, identity);
            expectAnalysisMeasurementsEqual(*this, completed, reference);
            expect(completed.matchesFileOnDisk());
            expect(committed.replaceWithText("external output replacement"));
            const auto replaced = completeRenderedAnalysis(reference, committed, 16, identity);
            expect(replaced.hasError());
            expect(replaced.errorMessage.containsIgnoreCase("changed"));
        }

        beginTest("Invalid streams and incomplete or invalid chunks fail explicitly");
        AudioMeasurement invalid(0, 44'100.0, 10);
        expect(!invalid.checkInitialization());
        expect(!invalid.finish());
        AudioMeasurement incomplete(1, 44'100.0, 10);
        expect(!incomplete.finish());
        juce::AudioBuffer<float> samples(1, 10);
        samples.clear();
        expect(!incomplete.push(samples, 1, 10));
        expect(!incomplete.push(samples, 0, 10));
        expect(!incomplete.finish());

        AudioMeasurement complete(1, 44'100.0, 10);
        expect(complete.push(samples, 0, 10).has_value());
        expect(complete.finish().has_value());
        expect(!complete.finish());
        expect(!complete.push(samples, 0, 0));

        beginTest("Invalid PCM buffers and non-finite samples fail explicitly");
        std::array<float, 1> input {std::numeric_limits<float>::infinity()};
        std::array<int, 1> pcm {};
        std::array<float, 1> observed {};
        expect(!utils::quantizeNormalizedSamples(input, pcm, observed, 1.0f, 16));
        input[0] = 0.5f;
        expect(!utils::quantizeNormalizedSamples(input, pcm, observed, 1.0f, 12));
        expect(!utils::quantizeNormalizedSamples(input, std::span<int>(), observed, 1.0f, 16));
    }

private:
    /// Feeds fixtures and overloads in chunks unrelated to the meter's fixed update interval.
    void runChunkTests()
    {
        for (const auto* fixtureName :
             {
                 "sine_stereo_44100_16.wav",
                 "sine_mono_48000_24.aif",
                 "intersample_stereo_44100_24.wav",
                 "click_mono_44100_16.wav",
                 "silence_stereo_44100_16.wav",
             })
        {
            beginTest(juce::String("Chunk boundaries preserve every measurement: ") + fixtureName);
            const auto file = fixtureFile(fixtureName);
            const auto reference = analyzeFile(file);
            const auto reader = utils::createAudioReader(file);
            expect(reference.isReady(), reference.errorMessage);
            expect(reader != nullptr);
            if (reader == nullptr) {
                continue;
            }
            juce::AudioBuffer<float> samples(
                static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples)
            );
            expect(reader->read(&samples, 0, samples.getNumSamples(), 0, true, true));
            for (const auto chunk : {1, 1024, 32'768, samples.getNumSamples()}) {
                AudioMeasurement meter(samples.getNumChannels(), reader->sampleRate, reader->lengthInSamples);
                for (int offset = 0; offset < samples.getNumSamples(); offset += chunk) {
                    const auto pushed = meter.push(samples, offset, std::min(chunk, samples.getNumSamples() - offset));
                    expect(pushed.has_value(), pushed ? juce::String() : pushed.error());
                    if (!pushed) {
                        break;
                    }
                }
                const auto measured = meter.finish();
                expect(measured.has_value(), measured ? juce::String() : measured.error());
                if (measured) {
                    expectAnalysisMeasurementsEqual(*this, *measured, reference);
                }
            }
        }

        beginTest("Overs spanning a chunk boundary retain interval-local flags and a final partial point");
        constexpr int interval {4410};
        juce::AudioBuffer<float> samples(2, interval * 2 + 17);
        samples.clear();
        samples.setSample(0, interval - 1, -1.25f);
        samples.setSample(1, interval, 1.5f);
        AudioMeasurement whole(2, 44'100.0, samples.getNumSamples());
        AudioMeasurement split(2, 44'100.0, samples.getNumSamples());
        expect(whole.push(samples, 0, samples.getNumSamples()).has_value());
        expect(split.push(samples, 0, interval - 1).has_value());
        expect(split.push(samples, interval - 1, samples.getNumSamples() - interval + 1).has_value());
        const auto first = whole.finish();
        const auto second = split.finish();
        expect(first.has_value() && second.has_value());
        if (first && second) {
            expectAnalysisMeasurementsEqual(*this, *first, *second);
            expectEquals(static_cast<int>(second->shortTermLoudness->values.size()), 3);
            expect((second->overloads->flagsAt(0, 0) & OverloadHistory::sampleOver) != 0);
            expect((second->overloads->flagsAt(1, 1) & OverloadHistory::sampleOver) != 0);
        }
    }

    /// Roundtrips both writer paths and checks that observed PCM equals the decoded float bit patterns.
    void runPcmTests()
    {
        juce::AiffAudioFormat format;
        for (const auto bits : format.getPossibleBitDepths()) {
            beginTest(juce::String("PCM alignment preserves signed boundaries at bit depth ") + juce::String(bits));
            const auto quantum = static_cast<int>(1U << static_cast<unsigned int>(32 - bits));
            constexpr auto minimum = std::numeric_limits<int>::min();
            constexpr auto maximum = std::numeric_limits<int>::max();
            constexpr auto scale = 1.0f / static_cast<float>(maximum);
            const std::array samples {
                minimum,
                minimum + quantum - 1,
                -quantum - 1,
                -quantum,
                -1,
                0,
                quantum - 1,
                maximum,
            };
            const std::array aligned {minimum, minimum, -2 * quantum, -quantum, -quantum, 0, 0, maximum - quantum + 1};
            for (std::size_t index = 0; index < samples.size(); ++index) {
                expectEquals(
                    std::bit_cast<std::uint32_t>(utils::pcmSampleToFloat(samples[index], bits)),
                    std::bit_cast<std::uint32_t>(static_cast<float>(aligned[index]) * scale)
                );
            }

            const auto step = std::ldexp(1.0f, 1 - bits);
            const std::array values {
                -1.25f,
                -1.0f,
                std::nextafter(-1.0f, 0.0f),
                -0.5f,
                -step * 1.5f,
                -step,
                -step * 0.5f,
                -std::numeric_limits<float>::denorm_min(),
                -0.0f,
                0.0f,
                std::numeric_limits<float>::denorm_min(),
                step * 0.5f,
                step,
                step * 1.5f,
                0.5f,
                std::nextafter(1.0f, 0.0f),
                1.0f,
                1.25f,
                std::nextafter(step * 0.5f, 0.0f),
                std::nextafter(step * 0.5f, 1.0f),
                std::nextafter(-step * 0.5f, -1.0f),
            };
            for (const auto channels : {1, 2}) {
                for (const auto gain : {1.0f, 0.123f, 2.0f}) {
                    runPcmCase(format, bits, channels, gain, values);
                }
            }
        }
    }

    /// Compares one writer configuration and its observed samples with the independent JUCE writer and reader.
    void runPcmCase(
        juce::AiffAudioFormat& format,
        const int bits,
        const int channels,
        const float gain,
        const std::span<const float> values
    )
    {
        beginTest("Normalized PCM and observed samples match the JUCE float writer");
        const auto frames = static_cast<int>(values.size());
        juce::AudioBuffer<float> input(channels, frames);
        juce::AudioBuffer<float> observed(channels, frames);
        juce::AudioBuffer<int> pcm(channels, frames);
        for (int channel = 0; channel < channels; ++channel) {
            for (int frame = 0; frame < frames; ++frame) {
                input.setSample(channel, frame, values[static_cast<std::size_t>(frames - frame - 1)]);
            }
        }
        std::vector<const int*> pointers(static_cast<std::size_t>(channels) + 1, nullptr);
        for (int channel = 0; channel < channels; ++channel) {
            const auto count = values.size();
            const auto quantized = utils::quantizeNormalizedSamples(
                std::span<const float>(input.getReadPointer(channel), count),
                std::span<int>(pcm.getWritePointer(channel), count),
                std::span<float>(observed.getWritePointer(channel), count),
                gain,
                bits
            );
            expect(quantized.has_value(), quantized ? juce::String() : quantized.error());
            pointers[static_cast<std::size_t>(channel)] = pcm.getReadPointer(channel);
        }
        const auto options
            = juce::AudioFormatWriterOptions().withSampleRate(44'100.0).withNumChannels(channels).withBitsPerSample(
                bits
            );
        juce::MemoryBlock floatBytes;
        juce::MemoryBlock pcmBytes;
        std::unique_ptr<juce::OutputStream> floatStream = std::make_unique<juce::MemoryOutputStream>(floatBytes, false);
        std::unique_ptr<juce::OutputStream> pcmStream = std::make_unique<juce::MemoryOutputStream>(pcmBytes, false);
        auto floatWriter = format.createWriterFor(floatStream, options);
        auto pcmWriter = format.createWriterFor(pcmStream, options);
        expect(floatWriter != nullptr && pcmWriter != nullptr);
        if (floatWriter == nullptr || pcmWriter == nullptr) {
            return;
        }
        input.applyGain(gain);
        expect(floatWriter->writeFromAudioSampleBuffer(input, 0, frames));
        expect(pcmWriter->write(pointers.data(), frames));
        floatWriter.reset();
        pcmWriter.reset();
        expect(floatBytes == pcmBytes, "The two paths must write identical AIFF bytes");
        const std::unique_ptr<juce::AudioFormatReader> reader(
            format.createReaderFor(new juce::MemoryInputStream(pcmBytes, false), true)
        );
        expect(reader != nullptr);
        if (reader == nullptr) {
            return;
        }
        juce::AudioBuffer<float> decoded(channels, frames);
        expect(reader->read(&decoded, 0, frames, 0, true, true));
        for (int channel = 0; channel < channels; ++channel) {
            for (int frame = 0; frame < frames; ++frame) {
                expect(
                    std::bit_cast<std::uint32_t>(decoded.getSample(channel, frame))
                        == std::bit_cast<std::uint32_t>(observed.getSample(channel, frame)),
                    "Observed PCM must equal the actual decoder result"
                );
            }
        }
    }
};

/// Creates the stream and PCM regression suite.
std::unique_ptr<juce::UnitTest> createMeasurementTests()
{
    return std::make_unique<MeasurementTests>();
}
}  // namespace audiobatch::tests
