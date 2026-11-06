/// Tests ImageControlProcessor: the supported bus layouts, the reported latency,
/// neutral offline processing, the analyzer tap, and the saved state with its editor settings.

#include "dsp/MultibandImager.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Sets a parameter of a processor to a plain value.
void setParameter(ImageControlProcessor& processor, const juce::String& parameterId, const float value)
{
    auto* parameter = processor.getParameters().getParameter(parameterId);
    ASSERT_NE(parameter, nullptr);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

/// Returns the plain value of a parameter of a processor.
float getParameter(ImageControlProcessor& processor, const juce::String& parameterId)
{
    const auto* parameter = processor.getParameters().getParameter(parameterId);
    return parameter->convertFrom0to1(parameter->getValue());
}
}  // namespace

TEST(Processor, AcceptsMatchingMonoOrStereoLayouts)
{
    const ImageControlProcessor processor;
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::stereo());
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    EXPECT_TRUE(processor.checkBusesLayoutSupported(layout));
    layout.inputBuses.getReference(0) = juce::AudioChannelSet::mono();
    EXPECT_FALSE(processor.checkBusesLayoutSupported(layout));
    layout.outputBuses.getReference(0) = juce::AudioChannelSet::mono();
    EXPECT_TRUE(processor.checkBusesLayoutSupported(layout));
}

TEST(Processor, ReportsLatencyAndPassesAudioThroughAtDefaults)
{
    ImageControlProcessor processor;
    processor.setNonRealtime(true);
    processor.prepareToPlay(TEST_SAMPLE_RATE, 512);
    const int latency = processor.getLatencySamples();
    EXPECT_EQ(latency, 4096 + PARTITION_SIZE);

    const auto left = makeNoise(0.5F, 8192, 20);
    const auto right = makeNoise(0.5F, 8192, 21);
    auto buffer = makeBuffer({left, right});
    juce::MidiBuffer midi;
    for (int start = 0; start < buffer.getNumSamples(); start += 512) {
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, start, 512);
        processor.processBlock(block, midi);
    }
    for (int sample = latency; sample < buffer.getNumSamples(); ++sample) {
        ASSERT_EQ(buffer.getSample(0, sample), left[static_cast<std::size_t>(sample - latency)]);
        ASSERT_EQ(buffer.getSample(1, sample), right[static_cast<std::size_t>(sample - latency)]);
    }
    processor.releaseResources();
}

TEST(Processor, OfflineCrossoverChangeAppliesWithoutWaiting)
{
    ImageControlProcessor processor;
    processor.setNonRealtime(true);
    processor.prepareToPlay(TEST_SAMPLE_RATE, 512);
    setParameter(processor, crossoverParameterId(1), 2000.0F);
    EXPECT_NEAR(processor.getCrossovers()[1], 2000.0F, 1.0F);

    juce::AudioBuffer<float> buffer(2, 512);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
    processor.releaseResources();
}

TEST(Processor, AnalyzerTapReceivesTheBlock)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(TEST_SAMPLE_RATE, 256);
    auto buffer = makeBuffer({makeNoise(0.5F, 256, 22), makeNoise(0.5F, 256, 23)});
    juce::MidiBuffer midi;
    processor.getEditorState().setProperty(ANALYZE_OUTPUT_PROPERTY, false, nullptr);
    const auto input = makeBuffer({std::vector<float>(channelOf(buffer, 0).begin(), channelOf(buffer, 0).end())});
    processor.processBlock(buffer, midi);

    std::vector<float> left(512, 0.0F);
    std::vector<float> right(512, 0.0F);
    EXPECT_EQ(processor.getAnalyzerFifo().pop(left, right), 256);
    EXPECT_EQ(left[100], input.getSample(0, 100));
    processor.releaseResources();
}

TEST(Processor, StateRoundTripsParametersAndEditorSettings)
{
    ImageControlProcessor source;
    setParameter(source, crossoverParameterId(0), 200.0F);
    setParameter(source, midGainParameterId(2), -3.0F);
    setParameter(source, sideGainParameterId(0), BAND_GAIN_MIN_DB);
    setParameter(source, OUTPUT_GAIN_ID, 1.5F);
    source.getEditorState().setProperty(linkPropertyName(1), false, nullptr);
    source.getEditorState().setProperty(PERSISTENCE_PROPERTY, 2.5, nullptr);
    juce::MemoryBlock state;
    source.getStateInformation(state);

    ImageControlProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_NEAR(getParameter(restored, crossoverParameterId(0)), 200.0F, 0.5F);
    EXPECT_NEAR(getParameter(restored, midGainParameterId(2)), -3.0F, 0.01F);
    EXPECT_NEAR(getParameter(restored, sideGainParameterId(0)), BAND_GAIN_MIN_DB, 0.01F);
    EXPECT_NEAR(getParameter(restored, OUTPUT_GAIN_ID), 1.5F, 0.01F);
    EXPECT_FALSE(static_cast<bool>(restored.getEditorState().getProperty(linkPropertyName(1))));
    EXPECT_TRUE(static_cast<bool>(restored.getEditorState().getProperty(linkPropertyName(0))));
    EXPECT_DOUBLE_EQ(static_cast<double>(restored.getEditorState().getProperty(PERSISTENCE_PROPERTY)), 2.5);
}

TEST(Processor, IgnoresForeignState)
{
    ImageControlProcessor processor;
    setParameter(processor, OUTPUT_GAIN_ID, -6.0F);
    const juce::String foreign = "<Other gain=\"1\"/>";
    juce::MemoryBlock state;
    juce::AudioProcessor::copyXmlToBinary(*juce::parseXML(foreign), state);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_NEAR(getParameter(processor, OUTPUT_GAIN_ID), -6.0F, 0.01F);
}
}  // namespace esgrove::image_control::test
