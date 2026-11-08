/// Tests ImageControlProcessor: the supported bus layouts, the reported latency,
/// neutral offline processing, delta mode and its bypass, the analyzer tap, the pan text, the solos,
/// the saved state with its editor settings, and the editor restoring its saved size.

#include "dsp/MultibandImager.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "tests/TestSignals.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>
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

TEST(Processor, BypassPassesAudioThroughWithTheSameLatency)
{
    for (const bool hostBypass : {false, true}) {
        SCOPED_TRACE(hostBypass);
        ImageControlProcessor processor;
        processor.setNonRealtime(true);
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            setParameter(processor, midGainParameterId(band), -6.0F);
            setParameter(processor, sideGainParameterId(band), BAND_GAIN_MIN_DB);
            setParameter(processor, panParameterId(band), 60.0F);
        }
        setParameter(processor, BYPASS_ID, hostBypass ? 0.0F : 1.0F);
        // Bypass passes every band through even while some are soloed.
        setParameter(processor, soloParameterId(1), 1.0F);
        // Bypass passes the input through even in delta mode.
        setParameter(processor, DELTA_ID, 1.0F);
        EXPECT_EQ(processor.getBypassParameter(), processor.getParameters().getParameter(BYPASS_ID));
        processor.prepareToPlay(TEST_SAMPLE_RATE, 512);
        const int latency = processor.getLatencySamples();

        const auto left = makeNoise(0.5F, 8192, 24);
        const auto right = makeNoise(0.5F, 8192, 25);
        auto buffer = makeBuffer({left, right});
        juce::MidiBuffer midi;
        for (int start = 0; start < buffer.getNumSamples(); start += 512) {
            juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, start, 512);
            if (hostBypass) {
                processor.processBlockBypassed(block, midi);
            } else {
                processor.processBlock(block, midi);
            }
        }
        for (int sample = latency; sample < buffer.getNumSamples(); ++sample) {
            ASSERT_EQ(buffer.getSample(0, sample), left[static_cast<std::size_t>(sample - latency)]);
            ASSERT_EQ(buffer.getSample(1, sample), right[static_cast<std::size_t>(sample - latency)]);
        }
        processor.releaseResources();
    }
}

TEST(Processor, DeltaIsSilentAtNeutralSettings)
{
    ImageControlProcessor processor;
    processor.setNonRealtime(true);
    setParameter(processor, DELTA_ID, 1.0F);
    processor.prepareToPlay(TEST_SAMPLE_RATE, 512);
    auto buffer = makeBuffer({makeNoise(0.5F, 16'384, 30), makeNoise(0.5F, 16'384, 31)});
    juce::MidiBuffer midi;
    for (int start = 0; start < buffer.getNumSamples(); start += 512) {
        juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, start, 512);
        processor.processBlock(block, midi);
    }
    // While Delta glides in, the bands sum back to the delayed input up to rounding,
    // and once it has settled, the output is exact silence.
    const int settled = static_cast<int>(GAIN_SMOOTHING_SECONDS * TEST_SAMPLE_RATE) + 2 * PARTITION_SIZE;
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample) {
        for (int channel = 0; channel < 2; ++channel) {
            if (sample < settled) {
                ASSERT_NEAR(buffer.getSample(channel, sample), 0.0F, 1.0e-6F) << sample;
            } else {
                ASSERT_EQ(buffer.getSample(channel, sample), 0.0F) << sample;
            }
        }
    }
    processor.releaseResources();
}

TEST(Processor, DeltaIsTheProcessedOutputMinusTheDelayedInput)
{
    const auto left = makeNoise(0.5F, 16'384, 32);
    const auto right = makeNoise(0.5F, 16'384, 33);
    const auto render = [&left, &right](const bool delta) {
        ImageControlProcessor processor;
        processor.setNonRealtime(true);
        setParameter(processor, midGainParameterId(1), -6.0F);
        setParameter(processor, sideGainParameterId(2), BAND_GAIN_MIN_DB);
        setParameter(processor, panParameterId(3), 40.0F);
        setParameter(processor, OUTPUT_GAIN_ID, 2.0F);
        setParameter(processor, DELTA_ID, delta ? 1.0F : 0.0F);
        processor.prepareToPlay(TEST_SAMPLE_RATE, 512);
        auto buffer = makeBuffer({left, right});
        juce::MidiBuffer midi;
        for (int start = 0; start < buffer.getNumSamples(); start += 512) {
            juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), 2, start, 512);
            processor.processBlock(block, midi);
        }
        processor.releaseResources();
        return buffer;
    };

    const auto processed = render(false);
    const auto difference = render(true);
    const int latency = 4096 + PARTITION_SIZE;
    double largest = 0.0;
    for (int sample = latency; sample < processed.getNumSamples(); ++sample) {
        const auto delayed = static_cast<std::size_t>(sample - latency);
        for (const auto& [channel, input] : {std::pair {0, &left}, std::pair {1, &right}}) {
            const float expected = processed.getSample(channel, sample) - (*input)[delayed];
            ASSERT_NEAR(difference.getSample(channel, sample), expected, 1.0e-5F) << sample;
            largest = std::max(largest, std::abs(static_cast<double>(expected)));
        }
    }
    // The settings change the audio, so the difference is not silence.
    EXPECT_GT(largest, 0.1);
}

TEST(Processor, DeltaGlidesInAndOutWithoutAJump)
{
    // A neutral sine turns into silence and back as Delta toggles, with no step larger than the sine's own.
    constexpr int blockSize = 256;
    const auto tone = makeSine(1000.0, 0.5F, static_cast<std::size_t>(blockSize) * 180);
    ImageControlProcessor processor;
    processor.setNonRealtime(true);
    processor.prepareToPlay(TEST_SAMPLE_RATE, blockSize);
    auto buffer = makeBuffer({tone, tone});
    juce::MidiBuffer midi;
    for (int block = 0; block * blockSize < buffer.getNumSamples(); ++block) {
        if (block == 60) {
            setParameter(processor, DELTA_ID, 1.0F);
        } else if (block == 120) {
            setParameter(processor, DELTA_ID, 0.0F);
        }
        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 2, block * blockSize, blockSize);
        processor.processBlock(view, midi);
    }
    const float sineStep = 0.5F * juce::MathConstants<float>::twoPi * 1000.0F / 48'000.0F;
    for (int sample = 1; sample < buffer.getNumSamples(); ++sample) {
        ASSERT_LT(std::abs(buffer.getSample(0, sample) - buffer.getSample(0, sample - 1)), sineStep * 1.1F) << sample;
    }
    // Between the toggles, once the glide is over, the output is silent.
    const int latency = 4096 + PARTITION_SIZE;
    for (int sample = 70 * blockSize + latency; sample < 120 * blockSize; ++sample) {
        ASSERT_EQ(buffer.getSample(0, sample), 0.0F) << sample;
    }
    processor.releaseResources();
}

TEST(Processor, OfflineCrossoverMovesApplyDeterministically)
{
    // A hard-left 700 Hz tone starts in the High Mid band, whose side is on,
    // and the mid crossover then moves above it in two steps within one partition,
    // putting it in the Low Mid band, whose side is off, so the output turns mono.
    constexpr int samples = 48'000;
    constexpr int blockSize = 64;
    constexpr int moveBlock = 200;
    const auto tone = makeSine(700.0, 0.5F, samples);

    const auto render = [&tone] {
        ImageControlProcessor processor;
        processor.setNonRealtime(true);
        setParameter(processor, sideGainParameterId(1), BAND_GAIN_MIN_DB);
        processor.prepareToPlay(TEST_SAMPLE_RATE, blockSize);
        auto buffer = makeBuffer({tone, std::vector<float>(tone.size(), 0.0F)});
        juce::MidiBuffer midi;
        for (int block = 0; block * blockSize < samples; ++block) {
            if (block == moveBlock) {
                setParameter(processor, crossoverParameterId(1), 1500.0F);
            } else if (block == moveBlock + 1) {
                setParameter(processor, crossoverParameterId(1), 2000.0F);
            }
            juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 2, block * blockSize, blockSize);
            processor.processBlock(view, midi);
        }
        processor.releaseResources();
        return buffer;
    };

    const auto first = render();
    const auto second = render();
    for (int sample = 0; sample < samples; ++sample) {
        ASSERT_EQ(first.getSample(0, sample), second.getSample(0, sample)) << "sample " << sample;
        ASSERT_EQ(first.getSample(1, sample), second.getSample(1, sample)) << "sample " << sample;
    }

    const int latency = 4096 + PARTITION_SIZE;
    const int beforeMove = moveBlock * blockSize;
    double sideBefore = 0.0;
    double sideAfter = 0.0;
    for (int sample = latency; sample < beforeMove; ++sample) {
        sideBefore = std::max(
            sideBefore, std::abs(static_cast<double>(first.getSample(0, sample) - first.getSample(1, sample)))
        );
    }
    for (int sample = samples - 8000; sample < samples; ++sample) {
        sideAfter = std::max(
            sideAfter, std::abs(static_cast<double>(first.getSample(0, sample) - first.getSample(1, sample)))
        );
    }
    EXPECT_GT(sideBefore, 0.4);
    EXPECT_LT(sideAfter, 1.0e-3);
}

TEST(Processor, RestoringAnOlderStateKeepsMissingEditorSettingsAtTheirDefaults)
{
    ImageControlProcessor source;
    source.getEditorState().setProperty(linkPropertyName(0), false, nullptr);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    auto* editorSettings = xml->getChildByName("EditorState");
    ASSERT_NE(editorSettings, nullptr);
    // A state saved before these settings existed has none of them.
    for (const char* property :
         {
             ANALYZE_OUTPUT_PROPERTY,
             PERSISTENCE_PROPERTY,
             PAN_MIDPOINT_PROPERTY,
             PAN_EDGE_PROPERTY,
             SHOW_FPS_PROPERTY,
             BYPASS_RENDERING_PROPERTY,
             HIDE_SPECTROGRAM_PROPERTY,
         })
    {
        editorSettings->removeAttribute(property);
    }
    // A state from before the pan curve has the linear focus setting instead.
    editorSettings->setAttribute(LEGACY_FOCUS_PROPERTY, 24.0);
    juce::MemoryBlock olderState;
    juce::AudioProcessor::copyXmlToBinary(*xml, olderState);

    ImageControlProcessor restored;
    restored.setStateInformation(olderState.getData(), static_cast<int>(olderState.getSize()));
    const auto& settings = restored.getEditorState();
    EXPECT_FALSE(static_cast<bool>(settings.getProperty(linkPropertyName(0))));
    EXPECT_TRUE(static_cast<bool>(settings.getProperty(ANALYZE_OUTPUT_PROPERTY)));
    EXPECT_DOUBLE_EQ(static_cast<double>(settings.getProperty(PERSISTENCE_PROPERTY)), DEFAULT_PERSISTENCE_SECONDS);
    EXPECT_DOUBLE_EQ(static_cast<double>(settings.getProperty(PAN_MIDPOINT_PROPERTY)), DEFAULT_PAN_MIDPOINT_DB);
    EXPECT_DOUBLE_EQ(static_cast<double>(settings.getProperty(PAN_EDGE_PROPERTY)), DEFAULT_PAN_EDGE_DB);
    EXPECT_FALSE(settings.hasProperty(LEGACY_FOCUS_PROPERTY));
    EXPECT_FALSE(static_cast<bool>(settings.getProperty(SHOW_FPS_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(settings.getProperty(BYPASS_RENDERING_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(settings.getProperty(HIDE_SPECTROGRAM_PROPERTY)));
}

TEST(Processor, RestoringAStateWithoutPansCentresThem)
{
    ImageControlProcessor source;
    setParameter(source, midGainParameterId(0), -3.0F);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    // A state saved before the pan parameters existed has none of them.
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        auto* pan = xml->getChildByAttribute("id", panParameterId(band));
        ASSERT_NE(pan, nullptr);
        xml->removeChildElement(pan, true);
    }
    juce::MemoryBlock olderState;
    juce::AudioProcessor::copyXmlToBinary(*xml, olderState);

    ImageControlProcessor restored;
    restored.setStateInformation(olderState.getData(), static_cast<int>(olderState.getSize()));
    EXPECT_NEAR(getParameter(restored, midGainParameterId(0)), -3.0F, 0.01F);
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_FLOAT_EQ(getParameter(restored, panParameterId(band)), 0.0F);
    }
}

TEST(Processor, RestoringAStateWithoutSolosLeavesThemOff)
{
    ImageControlProcessor source;
    setParameter(source, soloParameterId(2), 1.0F);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    // A state saved before the solo parameters existed has none of them.
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        auto* solo = xml->getChildByAttribute("id", soloParameterId(band));
        ASSERT_NE(solo, nullptr);
        xml->removeChildElement(solo, true);
    }
    juce::MemoryBlock olderState;
    juce::AudioProcessor::copyXmlToBinary(*xml, olderState);

    ImageControlProcessor restored;
    setParameter(restored, soloParameterId(2), 1.0F);
    restored.setStateInformation(olderState.getData(), static_cast<int>(olderState.getSize()));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_FLOAT_EQ(getParameter(restored, soloParameterId(band)), 0.0F);
    }
}

TEST(Processor, SolosAreNamedPerBandAndCombine)
{
    ImageControlProcessor processor;
    EXPECT_EQ(processor.getParameters().getParameter(soloParameterId(0))->getName(64), "Low: Solo");
    EXPECT_EQ(processor.getParameters().getParameter(soloParameterId(3))->getName(64), "High: Solo");

    EXPECT_EQ(soloGains({false, false, false, false}), (BandGains {1.0F, 1.0F, 1.0F, 1.0F}));
    EXPECT_EQ(soloGains({false, true, false, false}), (BandGains {0.0F, 1.0F, 0.0F, 0.0F}));
    EXPECT_EQ(soloGains({true, false, true, false}), (BandGains {1.0F, 0.0F, 1.0F, 0.0F}));
    // Soloing every band is the same as soloing none.
    EXPECT_EQ(soloGains({true, true, true, true}), (BandGains {1.0F, 1.0F, 1.0F, 1.0F}));
}

TEST(Processor, PansShowAndParseAsCentreOrSide)
{
    EXPECT_EQ(formatPan(0.0F), "C");
    EXPECT_EQ(formatPan(-35.0F), "35L");
    EXPECT_EQ(formatPan(100.0F), "100R");
    EXPECT_FLOAT_EQ(panPercentFromText("C"), 0.0F);
    EXPECT_FLOAT_EQ(panPercentFromText("35L"), -35.0F);
    EXPECT_FLOAT_EQ(panPercentFromText(" 100 r "), 100.0F);
    EXPECT_FLOAT_EQ(panPercentFromText("-20"), -20.0F);
    EXPECT_FLOAT_EQ(panPercentFromText("150R"), PAN_MAX_PERCENT);

    ImageControlProcessor processor;
    auto* parameter = processor.getParameters().getParameter(panParameterId(2));
    ASSERT_NE(parameter, nullptr);
    EXPECT_EQ(parameter->getCurrentValueAsText(), "C");
    parameter->setValueNotifyingHost(parameter->getValueForText("40L"));
    EXPECT_EQ(parameter->getCurrentValueAsText(), "40L");
}

TEST(Processor, PanCentreIsExactlyZero)
{
    EXPECT_EQ(panFromPercent(0.0F), 0.0F);
    EXPECT_EQ(panFromPercent(0.4F), 0.0F);
    EXPECT_FLOAT_EQ(panFromPercent(50.0F), 0.5F);
    EXPECT_FLOAT_EQ(panFromPercent(-PAN_MAX_PERCENT), -1.0F);
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
    setParameter(source, panParameterId(1), -35.0F);
    setParameter(source, soloParameterId(3), 1.0F);
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
    EXPECT_NEAR(getParameter(restored, panParameterId(1)), -35.0F, 0.01F);
    EXPECT_FLOAT_EQ(getParameter(restored, soloParameterId(3)), 1.0F);
    EXPECT_FLOAT_EQ(getParameter(restored, soloParameterId(0)), 0.0F);
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

TEST(Processor, EditorRestoresItsSavedSizeWhenReopened)
{
    ImageControlProcessor processor;
    auto& editorState = processor.getEditorState();
    editorState.setProperty(EDITOR_WIDTH_PROPERTY, 1500, nullptr);
    editorState.setProperty(EDITOR_HEIGHT_PROPERTY, 1100, nullptr);
    editorState.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 300, nullptr);

    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditorAndMakeActive());
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->getWidth(), 1500);
    EXPECT_EQ(editor->getHeight(), 1100);
    EXPECT_EQ(static_cast<int>(editorState.getProperty(EDITOR_WIDTH_PROPERTY)), 1500);
    EXPECT_EQ(static_cast<int>(editorState.getProperty(EDITOR_HEIGHT_PROPERTY)), 1100);
    EXPECT_EQ(static_cast<int>(editorState.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 300);
    editor->setSize(1700, 1200);
    // Hosts tell the processor before they delete its editor.
    processor.editorBeingDeleted(editor.get());
    editor.reset();

    editor.reset(processor.createEditorAndMakeActive());
    ASSERT_NE(editor, nullptr);
    EXPECT_EQ(editor->getWidth(), 1700);
    EXPECT_EQ(editor->getHeight(), 1200);
    EXPECT_EQ(static_cast<int>(editorState.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 300);
    processor.editorBeingDeleted(editor.get());
}
}  // namespace esgrove::image_control::test
