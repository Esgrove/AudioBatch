/// Tests portable sound snapshots independently of host state, including strict XML rejection and scope boundaries.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <thread>
#include <utility>

namespace esgrove::image_control::test
{
namespace
{
/// Captures public state APIs once from the first synchronous parameter notification during host restore.
class RestoreSnapshotListener final : public juce::AudioProcessorValueTreeState::Listener
{
public:
    /// Consumes the request before running it, so reentrant host callbacks cannot repeat the observation.
    void parameterChanged(const juce::String& /*parameterId*/, float /*value*/) override
    {
        if (const auto action = std::exchange(callback, {})) {
            ++calls;
            action();
        }
    }

    /// One-shot observation owned by the enclosing test and run on the restoring thread.
    std::function<void()> callback;
    /// Number of callbacks that actually captured a target.
    int calls {0};
};

/// Establishes edited live values for missing-default and invalid-token restore checks.
void prepareRestoreSnapshotProcessor(ImageControlProcessor& processor)
{
    for (const auto& [parameterId, value] :
         ProcessingSnapshot {
             {midGainParameterId(3), -4.2F},
             {widthParameterId(3), 1.9F},
         })
    {
        auto& parameter = *processor.getParameters().getParameter(parameterId);
        parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
    }
}

/// Verifies every saved host parameter against the real finite parameter boundary's atomic readback.
void expectAcceptedRestoreSnapshot(const juce::XmlElement& xml, const ProcessingSnapshot& expected)
{
    for (const auto& [parameterId, value] : expected) {
        SCOPED_TRACE(parameterId.toStdString());
        const auto* child = xml.getChildByAttribute("id", parameterId);
        ASSERT_NE(child, nullptr);
        EXPECT_TRUE(juce::exactlyEqual(static_cast<float>(child->getDoubleAttribute("value")), value));
    }
}
}  // namespace

/// Detached targets must already match accepted sound and strict comparison XML during synchronous restore callbacks.
TEST(ProcessingState, ReentrantRestoreSnapshotsCanonicalizeEveryParameterBeforePublication)
{
    ImageControlProcessor source;
    auto& output = *source.getParameters().getParameter(OUTPUT_GAIN_ID);
    output.setValueNotifyingHost(output.convertTo0to1(-6.0F));
    const auto inactiveSound = source.getSoundSnapshot();
    ASSERT_TRUE(source.selectComparisonSlot(1));
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    const ProcessingSnapshot inputs {
        {crossoverParameterId(0), 137.23F},
        {crossoverParameterId(1), 1234.5677F},
        {crossoverParameterId(2), 40'000.0F},
        {midGainParameterId(0), 500.0F},
        {sideGainParameterId(1), -500.0F},
        {panParameterId(0), -37.5F},
        {panParameterId(1), 1000.0F},
        {widthParameterId(0), 0.735F},
        {widthParameterId(1), 5.0F},
        {recoverParameterId(0), 37.5F},
        {recoverParameterId(1), -20.0F},
        {recoverParameterId(2), 300.0F},
        {soloParameterId(0), 0.49F},
        {soloParameterId(1), 0.51F},
        {OUTPUT_GAIN_ID, 500.0F},
        {BYPASS_ID, 0.7F},
        {DELTA_ID, 0.3F},
        {MONO_ID, 0.5F},
    };
    for (const auto& [parameterId, value] : inputs) {
        auto* child = xml->getChildByAttribute("id", parameterId);
        ASSERT_NE(child, nullptr);
        child->setAttribute("value", static_cast<double>(value));
    }
    xml->getChildByAttribute("id", midGainParameterId(3))->setAttribute("value", "nan");
    xml->removeChildElement(xml->getChildByAttribute("id", widthParameterId(3)), true);
    juce::AudioProcessor::copyXmlToBinary(*xml, state);

    // An independent live parameter path supplies exact accepted readback, including skew and step round trips.
    ImageControlProcessor accepted;
    prepareRestoreSnapshotProcessor(accepted);
    for (const auto& [parameterId, value] : inputs) {
        auto& parameter = *accepted.getParameters().getParameter(parameterId);
        parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
    }
    auto& defaultWidth = *accepted.getParameters().getParameter(widthParameterId(3));
    defaultWidth.setValueNotifyingHost(defaultWidth.getDefaultValue());
    const auto expected = accepted.getSoundSnapshot();
    const auto expectedParameters = captureProcessingState(accepted.getParameters(), false);
    EXPECT_FLOAT_EQ(expected.at(recoverParameterId(0)), 38.0F);
    EXPECT_FLOAT_EQ(expected.at(MONO_ID), 1.0F);
    EXPECT_FLOAT_EQ(expected.at(widthParameterId(3)), 1.0F);

    for (const bool nested : {false, true}) {
        for (const bool fromWorker : {false, true}) {
            SCOPED_TRACE(nested);
            SCOPED_TRACE(fromWorker);
            ImageControlProcessor processor;
            prepareRestoreSnapshotProcessor(processor);
            juce::MemoryBlock outerState;
            processor.getStateInformation(outerState);
            const auto outerXml
                = juce::AudioProcessor::getXmlFromBinary(outerState.getData(), static_cast<int>(outerState.getSize()));
            ASSERT_NE(outerXml, nullptr);
            // The first parameter callback must still see a complete canonical target for later unwritten controls.
            const auto trigger = outerXml->getFirstChildElement()->getStringAttribute("id");
            auto& triggerParameter = *processor.getParameters().getParameter(trigger);
            const auto* triggerTarget = (nested ? outerXml : xml)->getChildByAttribute("id", trigger);
            ASSERT_NE(triggerTarget, nullptr);
            const auto target = static_cast<float>(triggerTarget->getDoubleAttribute("value"));
            triggerParameter.setValueNotifyingHost(triggerParameter.convertTo0to1(target) >= 0.5F ? 0.0F : 1.0F);
            const auto first = nested ? outerState : state;
            std::optional<PresetPresentation> presentation;
            ProcessingSnapshot sound;
            juce::MemoryBlock reentrant;
            RestoreSnapshotListener listener;
            listener.callback = [&] {
                if (nested) {
                    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                }
                sound = processor.getSoundSnapshot();
                presentation = processor.getPresetPresentation();
                processor.getStateInformation(reentrant);
            };
            processor.getParameters().addParameterListener(trigger, &listener);
            if (fromWorker) {
                // No message dispatch while joining: restore and its reentrant readers must remain worker-safe.
                std::thread worker([&] {
                    processor.setStateInformation(first.getData(), static_cast<int>(first.getSize()));
                });
                worker.join();
            } else {
                processor.setStateInformation(first.getData(), static_cast<int>(first.getSize()));
            }
            processor.getParameters().removeParameterListener(trigger, &listener);
            ASSERT_EQ(listener.calls, 1);
            ASSERT_TRUE(presentation.has_value());
            EXPECT_TRUE(sameProcessingState(sound, expected));
            const auto capturedPresentation = presentation.value_or(PresetPresentation {});
            EXPECT_TRUE(sameProcessingState(capturedPresentation.sound, expected));
            EXPECT_EQ(capturedPresentation.comparisonSlot, 1);
            EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), expected));
            EXPECT_TRUE(
                sameProcessingState(captureProcessingState(processor.getParameters(), false), expectedParameters)
            );
            const auto saved
                = juce::AudioProcessor::getXmlFromBinary(reentrant.getData(), static_cast<int>(reentrant.getSize()));
            ASSERT_NE(saved, nullptr);
            expectAcceptedRestoreSnapshot(*saved, expectedParameters);
            const auto comparison = comparisonStateFromXml(*saved, processor.getParameters());
            ASSERT_TRUE(comparison.has_value());
            const auto capturedComparison = comparison.value_or(ComparisonState {});
            EXPECT_EQ(capturedComparison.active, 1);
            EXPECT_TRUE(sameProcessingState(capturedComparison.slots[1], expected));
            EXPECT_TRUE(sameProcessingState(capturedComparison.slots[0], inactiveSound));
            ImageControlProcessor roundTrip;
            roundTrip.setStateInformation(reentrant.getData(), static_cast<int>(reentrant.getSize()));
            EXPECT_EQ(roundTrip.getActiveComparisonSlot(), 1);
            EXPECT_TRUE(sameProcessingState(roundTrip.getSoundSnapshot(), expected));
            const auto recalledInactive = processingStateAfterRecall(inactiveSound, roundTrip.getParameters());
            ASSERT_TRUE(roundTrip.selectComparisonSlot(0));
            EXPECT_TRUE(sameProcessingState(roundTrip.getSoundSnapshot(), recalledInactive));
        }
    }
}

/// Legacy restores seed both comparison slots from legal accepted sound, not unclamped host XML.
TEST(ProcessingState, LegacyRestoreSeedsRoundTrippableCanonicalComparisonSlots)
{
    ImageControlProcessor source;
    const auto xml = source.getParameters().copyState().createXml();
    ASSERT_NE(xml, nullptr);
    xml->getChildByAttribute("id", OUTPUT_GAIN_ID)->setAttribute("value", 500.0);
    xml->getChildByAttribute("id", MONO_ID)->setAttribute("value", 0.5);
    xml->getChildByAttribute("id", recoverParameterId(0))->setAttribute("value", 37.5);
    juce::MemoryBlock state;
    juce::AudioProcessor::copyXmlToBinary(*xml, state);
    ImageControlProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    const auto expected = restored.getSoundSnapshot();
    EXPECT_FLOAT_EQ(expected.at(OUTPUT_GAIN_ID), OUTPUT_GAIN_MAX_DB);
    EXPECT_FLOAT_EQ(expected.at(MONO_ID), 1.0F);
    EXPECT_FLOAT_EQ(expected.at(recoverParameterId(0)), 38.0F);
    restored.getStateInformation(state);
    const auto saved = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(saved, nullptr);
    const auto comparison = comparisonStateFromXml(*saved, restored.getParameters());
    ASSERT_TRUE(comparison.has_value());
    for (const auto& slot : comparison.value_or(ComparisonState {}).slots) {
        EXPECT_TRUE(sameProcessingState(slot, expected));
    }
    ImageControlProcessor roundTrip;
    roundTrip.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_TRUE(roundTrip.selectComparisonSlot(1));
    EXPECT_TRUE(sameProcessingState(roundTrip.getSoundSnapshot(), expected));
}

TEST(ProcessingState, SoundScopeExcludesAuditionAndIncludesMonoAndOutput)
{
    ImageControlProcessor processor;
    const auto sound = captureProcessingState(processor.getParameters());
    const auto all = captureProcessingState(processor.getParameters(), false);
    EXPECT_EQ(sound.size(), 25U);
    EXPECT_EQ(all.size(), 31U);
    EXPECT_TRUE(sound.contains(MONO_ID));
    EXPECT_TRUE(sound.contains(OUTPUT_GAIN_ID));
    EXPECT_FALSE(sound.contains(BYPASS_ID));
    EXPECT_FALSE(sound.contains(DELTA_ID));
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        EXPECT_FALSE(sound.contains(soloParameterId(band)));
        EXPECT_TRUE(all.contains(soloParameterId(band)));
    }
}

TEST(ProcessingState, XmlRoundTripsEveryPlainValueExactly)
{
    ImageControlProcessor processor;
    const auto sound = captureProcessingState(processor.getParameters());
    const auto xml = processingStateToXml(sound);
    ASSERT_TRUE(xml->hasTagName("SoundState"));
    EXPECT_EQ(xml->getStringAttribute("plugin"), "com.esgrove.imagecontrol");
    const auto parsed = processingStateFromXml(*xml, processor.getParameters());
    ASSERT_TRUE(parsed.has_value());
    if (parsed.has_value()) {
        EXPECT_TRUE(sameProcessingState(sound, *parsed));
    }
}

TEST(ProcessingState, RejectsValuesBeforeRoundingAndFractionalBooleanStates)
{
    ImageControlProcessor processor;
    const auto sound = captureProcessingState(processor.getParameters());
    const auto xml = processingStateToXml(sound);
    auto* output = xml->getChildByAttribute("id", OUTPUT_GAIN_ID);
    ASSERT_NE(output, nullptr);
    output->setAttribute("value", "12.0000000001");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    output->setAttribute("value", "-24.0000000001");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    output->setAttribute("value", "0");
    auto* mono = xml->getChildByAttribute("id", MONO_ID);
    ASSERT_NE(mono, nullptr);
    mono->setAttribute("value", "0.5");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
}

TEST(ProcessingState, RejectsIncompleteUnknownDuplicateNonFiniteAndOutOfRangeData)
{
    ImageControlProcessor processor;
    const auto sound = captureProcessingState(processor.getParameters());
    for (const auto* invalid : {"nan", "inf", "-inf", "1garbage", "100000", "-100000", ""}) {
        const auto xml = processingStateToXml(sound);
        xml->getFirstChildElement()->setAttribute("value", invalid);
        EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    }
    auto xml = processingStateToXml(sound);
    xml->getFirstChildElement()->setAttribute("id", "unknown");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    xml = processingStateToXml(sound);
    xml->addChildElement(new juce::XmlElement(*xml->getFirstChildElement()));
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    xml = processingStateToXml(sound);
    xml->removeChildElement(xml->getFirstChildElement(), true);
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    xml = processingStateToXml(sound);
    xml->setAttribute("version", 2);
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    xml = processingStateToXml(sound);
    xml->setAttribute("plugin", "other.plugin");
    EXPECT_FALSE(processingStateFromXml(*xml, processor.getParameters()));
    auto invalid = sound;
    invalid.at(OUTPUT_GAIN_ID) = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(processor.loadSoundSnapshot(invalid, "Invalid"));
}
}  // namespace esgrove::image_control::test
