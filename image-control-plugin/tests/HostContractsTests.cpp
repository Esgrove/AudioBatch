/// Tests host reset and finite FIR tails, neutral crossover transitions, and automatable Mono output.
/// These processor-level contracts complement the isolated convolver and steady-state imaging tests.

#include "analysis/StereoSampleFifo.h"
#include "dsp/CrossoverDesign.h"
#include "dsp/MultibandImager.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Writes a plain parameter value using the same conversion the host uses.
void setPlain(ImageControlProcessor& processor, const juce::String& parameterId, const float value)
{
    auto& parameter = *processor.getParameters().getParameter(parameterId);
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
}

/// Processes arbitrary blocks, including a short final block, without allocating in the processor callback.
void processBuffer(
    ImageControlProcessor& processor,
    juce::AudioBuffer<float>& buffer,
    const int blockSize,
    const bool hostBypassed = false
)
{
    juce::MidiBuffer midi;
    for (int start = 0; start < buffer.getNumSamples(); start += blockSize) {
        const int count = std::min(blockSize, buffer.getNumSamples() - start);
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start, count);
        if (hostBypassed) {
            processor.processBlockBypassed(block, midi);
        } else {
            processor.processBlock(block, midi);
        }
    }
}
}  // namespace

TEST(HostContracts, ResetDropsEveryPendingSampleWithoutChangingParameters)
{
    for (const bool processed : {false, true}) {
        ImageControlProcessor processor;
        processor.setNonRealtime(true);
        processor.prepareToPlay(TEST_SAMPLE_RATE, 37);
        if (processed) {
            setPlain(processor, sideGainParameterId(1), -9.0F);
            setPlain(processor, soloParameterId(1), 1.0F);
        }
        auto input = makeBuffer({makeNoise(0.7F, 9001, 81), makeNoise(0.7F, 9001, 82)});
        processBuffer(processor, input, 37);
        processor.reset();
        juce::AudioBuffer<float> silence(2, 16'384);
        silence.clear();
        processBuffer(processor, silence, 511);
        for (int channel = 0; channel < 2; ++channel) {
            for (const float value : channelOf(silence, channel)) {
                ASSERT_EQ(value, 0.0F);
            }
        }
        EXPECT_FLOAT_EQ(
            processor.getParameters().getParameter(soloParameterId(1))->getValue(), processed ? 1.0F : 0.0F
        );
        processor.releaseResources();
    }
}

TEST(HostContracts, TailCoversAnEndpointImpulseAtEverySupportedRate)
{
    for (const double rate : {44'100.0, 48'000.0, 96'000.0, 192'000.0}) {
        SCOPED_TRACE(rate);
        ImageControlProcessor processor;
        processor.setNonRealtime(true);
        setPlain(processor, soloParameterId(0), 1.0F);
        processor.prepareToPlay(rate, 257);
        const int halfLength = kernelHalfLength(rate);
        const int latency = processor.getLatencySamples();
        const int tail = static_cast<int>(std::ceil(processor.getTailLengthSeconds() * rate));
        EXPECT_GE(tail, halfLength);
        const int endpoint = halfLength * 2;
        juce::AudioBuffer<float> buffer(2, endpoint + latency + tail + 1024);
        buffer.clear();
        buffer.setSample(0, endpoint - 1, 1.0F);
        buffer.setSample(1, endpoint - 1, 1.0F);
        processBuffer(processor, buffer, 257);
        const auto kernel = designLowpassKernel(static_cast<double>(DEFAULT_CROSSOVERS[0]), rate, halfLength);
        for (std::size_t tap = 0; tap < kernel.size(); ++tap) {
            const int sample = endpoint - 1 + PARTITION_SIZE + static_cast<int>(tap);
            ASSERT_NEAR(buffer.getSample(0, sample), kernel[tap], 2.0e-6F) << tap;
        }
        EXPECT_GT(rms(channelOf(buffer, 0).subspan(static_cast<std::size_t>(endpoint + latency), 256)), 1.0e-5);
        for (int sample = endpoint + latency + tail; sample < buffer.getNumSamples(); ++sample) {
            ASSERT_NEAR(buffer.getSample(0, sample), 0.0F, 1.0e-7F);
        }
        processor.releaseResources();
    }
}

TEST(HostContracts, MovingCrossoversPreservesExactNeutralBypassAndDelta)
{
    for (const int mode : {0, 1, 2}) {
        ImageControlProcessor processor;
        processor.setNonRealtime(true);
        if (mode == 1) {
            setPlain(processor, BYPASS_ID, 1.0F);
            setPlain(processor, midGainParameterId(0), -6.0F);
        } else if (mode == 2) {
            setPlain(processor, DELTA_ID, 1.0F);
        }
        processor.prepareToPlay(TEST_SAMPLE_RATE, 37);
        const auto left = makeNoise(0.7F, 20'000, 83);
        const auto right = makeNoise(0.7F, 20'000, 84);
        auto buffer = makeBuffer({left, right});
        juce::MidiBuffer midi;
        for (int start = 0; start < buffer.getNumSamples(); start += 37) {
            if (start == 9990) {
                setPlain(processor, crossoverParameterId(1), 1800.0F);
            }
            juce::AudioBuffer<float> block(
                buffer.getArrayOfWritePointers(), 2, start, std::min(37, buffer.getNumSamples() - start)
            );
            processor.processBlock(block, midi);
        }
        for (int sample = 6000; sample < buffer.getNumSamples(); ++sample) {
            const auto source = static_cast<std::size_t>(sample - processor.getLatencySamples());
            ASSERT_EQ(buffer.getSample(0, sample), mode == 2 ? 0.0F : left[source]) << sample;
            ASSERT_EQ(buffer.getSample(1, sample), mode == 2 ? 0.0F : right[source]) << sample;
        }
        processor.releaseResources();
    }
}

TEST(Mono, FoldsTheFinalOutputIncludingBypassOfflineAndSavedState)
{
    for (const bool offline : {false, true}) {
        for (const int bypassMode : {0, 1, 2}) {
            ImageControlProcessor processor;
            EXPECT_FLOAT_EQ(processor.getParameters().getParameter(MONO_ID)->getValue(), 0.0F);
            processor.setNonRealtime(offline);
            setPlain(processor, MONO_ID, 1.0F);
            setPlain(processor, BYPASS_ID, bypassMode == 1 ? 1.0F : 0.0F);
            if (bypassMode != 0) {
                setPlain(processor, OUTPUT_GAIN_ID, 6.0F);
                setPlain(processor, DELTA_ID, 1.0F);
            }
            processor.prepareToPlay(TEST_SAMPLE_RATE, 64);
            const auto left = makeNoise(0.5F, 16'384, 85);
            const auto right = makeNoise(0.5F, 16'384, 86);
            auto buffer = makeBuffer({left, right});
            processBuffer(processor, buffer, 64, bypassMode == 2);
            for (int sample = 6000; sample < buffer.getNumSamples(); ++sample) {
                const auto source = static_cast<std::size_t>(sample - processor.getLatencySamples());
                const float mono = (left[source] + right[source]) * 0.5F;
                ASSERT_EQ(buffer.getSample(0, sample), mono);
                ASSERT_EQ(buffer.getSample(1, sample), mono);
            }
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            ImageControlProcessor restored;
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            EXPECT_FLOAT_EQ(restored.getParameters().getParameter(MONO_ID)->getValue(), 1.0F);
            restored.setNonRealtime(offline);
            restored.prepareToPlay(TEST_SAMPLE_RATE, 37);
            auto restoredBuffer = makeBuffer({left, right});
            processBuffer(restored, restoredBuffer, 37, bypassMode == 2);
            for (int sample = 6000; sample < buffer.getNumSamples(); ++sample) {
                ASSERT_EQ(restoredBuffer.getSample(0, sample), buffer.getSample(0, sample));
                ASSERT_EQ(restoredBuffer.getSample(1, sample), buffer.getSample(1, sample));
            }
            restored.releaseResources();
            processor.releaseResources();
        }
    }
}

TEST(Mono, ResetInitializesTheFoldFromTheParameter)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(TEST_SAMPLE_RATE, 64);
    for (const bool enabled : {true, false}) {
        setPlain(processor, MONO_ID, enabled ? 1.0F : 0.0F);
        processor.reset();
        const auto input = makeNoise(0.3F, 8192, 103);
        const std::vector<float> silence(input.size(), 0.0F);
        auto buffer = makeBuffer({input, silence});
        processBuffer(processor, buffer, 64);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
            const float source = sample < processor.getLatencySamples()
                ? 0.0F
                : input[static_cast<std::size_t>(sample - processor.getLatencySamples())];
            ASSERT_EQ(buffer.getSample(0, sample), enabled ? source * 0.5F : source);
            ASSERT_EQ(buffer.getSample(1, sample), enabled ? source * 0.5F : 0.0F);
        }
    }
    processor.releaseResources();
}

TEST(Mono, HostParameterMetadataAndOlderStateDefaultsAreStable)
{
    ImageControlProcessor processor;
    auto* parameter = processor.getParameters().getParameter(MONO_ID);
    ASSERT_NE(parameter, nullptr);
    EXPECT_EQ(parameter->getName(100), "Mono");
    EXPECT_EQ(parameter->getVersionHint(), 5);
    EXPECT_EQ(static_cast<juce::AudioProcessor&>(processor).getParameters().getLast(), parameter);
    EXPECT_TRUE(parameter->isAutomatable());
    EXPECT_TRUE(parameter->isBoolean());
    EXPECT_TRUE(parameter->isDiscrete());
    EXPECT_EQ(parameter->getNumSteps(), 2);
    EXPECT_FLOAT_EQ(parameter->getDefaultValue(), 0.0F);
    EXPECT_EQ(parameter->getText(0.0F, 100), "Off");
    EXPECT_EQ(parameter->getText(1.0F, 100), "On");

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    auto* monoState = xml->getChildByAttribute("id", MONO_ID);
    ASSERT_NE(monoState, nullptr);
    xml->removeChildElement(monoState, true);
    juce::AudioProcessor::copyXmlToBinary(*xml, state);
    setPlain(processor, MONO_ID, 1.0F);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_FLOAT_EQ(parameter->getValue(), 0.0F);
}

TEST(Mono, FoldsAfterAllBandProcessingSolosOutputGainAndDelta)
{
    for (const bool offline : {false, true}) {
        for (const bool delta : {false, true}) {
            ImageControlProcessor stereoProcessor;
            ImageControlProcessor monoProcessor;
            setPlain(monoProcessor, MONO_ID, 1.0F);
            for (auto* processor : {&stereoProcessor, &monoProcessor}) {
                processor->setNonRealtime(offline);
                setPlain(*processor, midGainParameterId(1), -6.0F);
                setPlain(*processor, sideGainParameterId(1), 3.0F);
                setPlain(*processor, panParameterId(1), -70.0F);
                setPlain(*processor, soloParameterId(1), 1.0F);
                setPlain(*processor, midGainParameterId(3), 4.0F);
                setPlain(*processor, panParameterId(3), 40.0F);
                setPlain(*processor, soloParameterId(3), 1.0F);
                setPlain(*processor, OUTPUT_GAIN_ID, 3.0F);
                setPlain(*processor, DELTA_ID, delta ? 1.0F : 0.0F);
                processor->prepareToPlay(TEST_SAMPLE_RATE, 37);
            }
            const auto left = makeNoise(0.3F, 16'384, 101);
            const auto right = makeNoise(0.3F, 16'384, 102);
            auto stereo = makeBuffer({left, right});
            auto mono = makeBuffer({left, right});
            processBuffer(stereoProcessor, stereo, 37);
            processBuffer(monoProcessor, mono, 37);
            EXPECT_GT(rms(channelOf(stereo, 0).subspan(6000)), 0.01);
            for (int sample = 0; sample < mono.getNumSamples(); ++sample) {
                const float expected = (stereo.getSample(0, sample) + stereo.getSample(1, sample)) * 0.5F;
                ASSERT_EQ(mono.getSample(0, sample), expected);
                ASSERT_EQ(mono.getSample(1, sample), expected);
            }
            stereoProcessor.releaseResources();
            monoProcessor.releaseResources();
        }
    }
}

TEST(Mono, MonoInputIsUnaffectedAndStereoMetersStayIndependent)
{
    ImageControlProcessor monoProcessor;
    const auto monoLayout = juce::AudioChannelSet::mono();
    ASSERT_TRUE(monoProcessor.setBusesLayout({{monoLayout}, {monoLayout}}));
    setPlain(monoProcessor, MONO_ID, 1.0F);
    monoProcessor.prepareToPlay(TEST_SAMPLE_RATE, 37);
    const auto input = makeNoise(0.5F, 16'384, 97);
    auto monoBuffer = makeBuffer({input});
    processBuffer(monoProcessor, monoBuffer, 37);
    for (int sample = monoProcessor.getLatencySamples(); sample < monoBuffer.getNumSamples(); ++sample) {
        ASSERT_EQ(
            monoBuffer.getSample(0, sample), input[static_cast<std::size_t>(sample - monoProcessor.getLatencySamples())]
        );
    }
    monoProcessor.releaseResources();

    ImageControlProcessor stereoProcessor;
    setPlain(stereoProcessor, MONO_ID, 1.0F);
    stereoProcessor.setCorrelationEnabled(true);
    stereoProcessor.prepareToPlay(TEST_SAMPLE_RATE, 256);
    std::vector<float> inverted(input.size());
    std::ranges::transform(input, inverted.begin(), [](const float value) { return -value; });
    auto stereo = makeBuffer({input, inverted});
    processBuffer(stereoProcessor, stereo, 256);
    for (const float value : stereoProcessor.getBandCorrelations()) {
        EXPECT_NEAR(value, -1.0F, 1.0e-6F);
    }
    EXPECT_EQ(stereo.getSample(0, 12'000), 0.0F);
    EXPECT_EQ(stereo.getSample(1, 12'000), 0.0F);
    stereoProcessor.releaseResources();
}

TEST(Mono, DeltaFoldsAfterDifferenceAndDisablingRestoresStereo)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(TEST_SAMPLE_RATE, 64);
    setPlain(processor, OUTPUT_GAIN_ID, 6.0F);
    setPlain(processor, DELTA_ID, 1.0F);
    setPlain(processor, MONO_ID, 1.0F);
    const auto left = makeNoise(0.3F, 16'384, 89);
    const auto right = makeNoise(0.3F, 16'384, 90);
    auto buffer = makeBuffer({left, right});
    processBuffer(processor, buffer, 64);
    const float differenceGain = outputGainFromDecibels(6.0F) - 1.0F;
    for (int sample = 6000; sample < buffer.getNumSamples(); ++sample) {
        const auto source = static_cast<std::size_t>(sample - processor.getLatencySamples());
        const float expected = (left[source] + right[source]) * 0.5F * differenceGain;
        ASSERT_NEAR(buffer.getSample(0, sample), expected, 2.0e-6F);
        ASSERT_EQ(buffer.getSample(0, sample), buffer.getSample(1, sample));
    }
    setPlain(processor, MONO_ID, 0.0F);
    setPlain(processor, BYPASS_ID, 1.0F);
    buffer = makeBuffer({left, right});
    processBuffer(processor, buffer, 64);
    for (int sample = 6000; sample < buffer.getNumSamples(); ++sample) {
        const auto source = static_cast<std::size_t>(sample - processor.getLatencySamples());
        ASSERT_EQ(buffer.getSample(0, sample), left[source]);
        ASSERT_EQ(buffer.getSample(1, sample), right[source]);
    }
    processor.releaseResources();
}

TEST(Mono, TogglingUsesASampleRateAwareRampAndPostAnalysisReceivesTheFinalOutput)
{
    for (const double rate : {48'000.0, 96'000.0}) {
        for (const bool tapOutput : {false, true}) {
            ImageControlProcessor processor;
            processor.getAnalyzerFifo().setTap(tapOutput);
            processor.prepareToPlay(rate, 64);
            const int rampSamples = static_cast<int>(rate * GAIN_SMOOTHING_SECONDS);
            juce::AudioBuffer<float> warm(2, processor.getLatencySamples() + rampSamples);
            for (int sample = 0; sample < warm.getNumSamples(); ++sample) {
                warm.setSample(0, sample, 0.5F);
                warm.setSample(1, sample, -0.5F);
            }
            processBuffer(processor, warm, 64);
            std::vector<float> left(ANALYZER_FIFO_FRAMES);
            std::vector<float> right(ANALYZER_FIFO_FRAMES);
            processor.getAnalyzerFifo().pop(left, right);
            for (const bool enabled : {true, false}) {
                setPlain(processor, MONO_ID, enabled ? 1.0F : 0.0F);
                juce::AudioBuffer<float> block(2, rampSamples + 64);
                for (int sample = 0; sample < block.getNumSamples(); ++sample) {
                    block.setSample(0, sample, 0.5F);
                    block.setSample(1, sample, -0.5F);
                }
                processBuffer(processor, block, 64);
                EXPECT_NEAR(block.getSample(0, 0), enabled ? 0.5F : 0.0F, 0.001F);
                EXPECT_EQ(block.getSample(0, rampSamples), enabled ? 0.0F : 0.5F);
                for (int sample = 1; sample < block.getNumSamples(); ++sample) {
                    EXPECT_LE(std::abs(block.getSample(0, sample) - block.getSample(0, sample - 1)), 0.001F);
                }
                const int frames = processor.getAnalyzerFifo().pop(left, right);
                ASSERT_EQ(frames, block.getNumSamples());
                for (int sample = 0; sample < frames; ++sample) {
                    const auto index = static_cast<std::size_t>(sample);
                    ASSERT_EQ(left[index], tapOutput ? block.getSample(0, sample) : 0.5F);
                    ASSERT_EQ(right[index], tapOutput ? block.getSample(1, sample) : -0.5F);
                }
            }
            processor.releaseResources();
        }
    }
}

TEST(HostContracts, NeutralAudioRemainsExactAcrossRatesAndIrregularBlocks)
{
    for (const double rate : {44'100.0, 48'000.0, 88'200.0, 96'000.0, 192'000.0}) {
        for (const int blockSize : {1, 37, 511, 4097}) {
            ImageControlProcessor processor;
            processor.setNonRealtime(true);
            processor.prepareToPlay(rate, blockSize);
            const int latency = processor.getLatencySamples();
            const auto count = static_cast<std::size_t>(latency) + 1025;
            const auto left = makeNoise(0.5F, count, 87);
            const auto right = makeNoise(0.5F, count, 88);
            auto buffer = makeBuffer({left, right});
            processBuffer(processor, buffer, blockSize);
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
                const auto source = static_cast<std::size_t>(std::max(0, sample - latency));
                ASSERT_EQ(buffer.getSample(0, sample), sample < latency ? 0.0F : left[source]);
                ASSERT_EQ(buffer.getSample(1, sample), sample < latency ? 0.0F : right[source]);
            }
            processor.releaseResources();
        }
    }
}

TEST(HostContracts, EmptyBlocksAndRepeatedLayoutPreparationAreSafe)
{
    ImageControlProcessor processor;
    processor.reset();
    for (const int channels : {1, 2, 1, 2}) {
        const auto layout = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        ASSERT_TRUE(processor.setBusesLayout({{layout}, {layout}}));
        processor.prepareToPlay(TEST_SAMPLE_RATE, 1);
        juce::AudioBuffer<float> empty(channels, 0);
        juce::MidiBuffer midi;
        processor.processBlock(empty, midi);
        processor.reset();
        processor.releaseResources();
    }
}
}  // namespace esgrove::image_control::test
