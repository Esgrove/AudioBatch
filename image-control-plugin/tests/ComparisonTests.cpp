/// Tests persistent processor-owned A/B sounds and their single chronological local undo stream.
/// Host state remains authoritative, including headless automation, legacy migration, and reentrant saves.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <latch>
#include <memory>
#include <thread>
#include <utility>

namespace esgrove::image_control::test
{
namespace
{
/// Applies unrecorded host-style output automation to a headless processor.
void setComparisonGain(ImageControlProcessor& processor, const float value)
{
    auto& parameter = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
}

/// Returns the current output control in plain units.
float comparisonGain(ImageControlProcessor& processor)
{
    return processor.getParameters().getRawParameterValue(OUTPUT_GAIN_ID)->load();
}

/// Makes binary host state without opening an editor or traversing its bound settings tree.
juce::MemoryBlock comparisonState(ImageControlProcessor& processor)
{
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    return state;
}

/// Runs one host request synchronously from a real parameter notification.
class ComparisonCallback final : public juce::AudioProcessorValueTreeState::Listener
{
public:
    /// Consumes the action before invoking it so nested save-triggered notifications cannot repeat it.
    void parameterChanged(const juce::String& /*parameterId*/, float /*value*/) override
    {
        if (auto const action = std::exchange(callback, {})) {
            action();
        }
    }

    /// One-shot host request owned by the enclosing test.
    std::function<void()> callback;
};
}  // namespace

TEST(Comparison, SwitchingCapturesHeadlessAutomationAndLeavesAuditionControlsAlone)
{
    ImageControlProcessor processor;
    setComparisonGain(processor, -6.0F);
    for (const auto& parameterId : {juce::String(BYPASS_ID), juce::String(DELTA_ID), soloParameterId(0)}) {
        processor.getParameters().getParameter(parameterId)->setValueNotifyingHost(1.0F);
    }
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    EXPECT_NEAR(comparisonGain(processor), 0.0F, 0.001F);
    setComparisonGain(processor, -3.0F);
    ASSERT_TRUE(processor.selectComparisonSlot(0));
    EXPECT_NEAR(comparisonGain(processor), -6.0F, 0.001F);
    for (const auto& parameterId : {juce::String(BYPASS_ID), juce::String(DELTA_ID), soloParameterId(0)}) {
        EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(parameterId)), 1.0F, 0.001F);
    }
    ASSERT_TRUE(processor.getHistory().undo());
    EXPECT_EQ(processor.getActiveComparisonSlot(), 1);
    EXPECT_NEAR(comparisonGain(processor), -3.0F, 0.001F);
    ASSERT_TRUE(processor.getHistory().redo());
    EXPECT_EQ(processor.getActiveComparisonSlot(), 0);
    EXPECT_NEAR(comparisonGain(processor), -6.0F, 0.001F);
    EXPECT_FALSE(processor.selectComparisonSlot(-1));
    EXPECT_FALSE(processor.selectComparisonSlot(2));
    EXPECT_FALSE(processor.selectComparisonSlot(0));
}

TEST(Comparison, CopySwitchAndPresetRecallShareChronologicalDeltaHistory)
{
    ImageControlProcessor processor;
    setComparisonGain(processor, -6.0F);
    ASSERT_TRUE(processor.copyComparisonToOtherSlot());
    EXPECT_NEAR(comparisonGain(processor), -6.0F, 0.001F);
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    auto preset = processor.getSoundSnapshot();
    preset.at(OUTPUT_GAIN_ID) = -12.0F;
    preset.at(MONO_ID) = 1.0F;
    ASSERT_TRUE(processor.loadSoundSnapshot(preset, "Recall test preset"));
    EXPECT_EQ(processor.getHistory().undoDescription(), "Recall test preset");
    // Unrelated automation must not be rewritten by preset undo, slot undo, or metadata-only copy undo.
    auto& pan = *processor.getParameters().getParameter(panParameterId(3));
    pan.setValueNotifyingHost(pan.convertTo0to1(25.0F));
    ASSERT_TRUE(processor.getHistory().undo());
    EXPECT_NEAR(comparisonGain(processor), -6.0F, 0.001F);
    ASSERT_TRUE(processor.getHistory().undo());
    EXPECT_EQ(processor.getActiveComparisonSlot(), 0);
    ASSERT_TRUE(processor.getHistory().undo());
    EXPECT_NEAR(plainValue(pan), 25.0F, 0.001F);
    ASSERT_TRUE(processor.getHistory().redo());
    EXPECT_NEAR(plainValue(pan), 25.0F, 0.001F);
    ASSERT_TRUE(processor.getHistory().redo());
    ASSERT_TRUE(processor.getHistory().redo());
    EXPECT_NEAR(comparisonGain(processor), -12.0F, 0.001F);
    EXPECT_NEAR(plainValue(pan), 25.0F, 0.001F);
}

TEST(Comparison, BothSelectedSlotsPersistAndLiveTopLevelParametersRemainAuthoritative)
{
    for (const int selected : {0, 1}) {
        ImageControlProcessor source;
        setComparisonGain(source, -6.0F);
        ASSERT_TRUE(source.selectComparisonSlot(1));
        setComparisonGain(source, -3.0F);
        if (selected == 0) {
            ASSERT_TRUE(source.selectComparisonSlot(0));
        }
        auto state = comparisonState(source);
        const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        ASSERT_NE(xml, nullptr);
        // Simulate a host changing a top-level parameter without patching the selected saved A/B copy.
        xml->getChildByAttribute("id", OUTPUT_GAIN_ID)->setAttribute("value", -9.0);
        juce::AudioProcessor::copyXmlToBinary(*xml, state);
        ImageControlProcessor restored;
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        EXPECT_EQ(restored.getActiveComparisonSlot(), selected);
        EXPECT_NEAR(comparisonGain(restored), -9.0F, 0.001F);
        EXPECT_FALSE(restored.getHistory().canUndo());
        ASSERT_TRUE(restored.selectComparisonSlot(1 - selected));
        EXPECT_NEAR(comparisonGain(restored), selected == 0 ? -3.0F : -6.0F, 0.001F);
        ASSERT_TRUE(restored.selectComparisonSlot(selected));
        EXPECT_NEAR(comparisonGain(restored), -9.0F, 0.001F);
    }
}

TEST(Comparison, SaveCapturesActiveHeadlessAutomationWithoutAnEditor)
{
    ImageControlProcessor source;
    ASSERT_TRUE(source.selectComparisonSlot(1));
    setComparisonGain(source, -9.0F);
    const auto state = comparisonState(source);
    ImageControlProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_EQ(restored.getActiveComparisonSlot(), 1);
    EXPECT_NEAR(comparisonGain(restored), -9.0F, 0.001F);
    ASSERT_TRUE(restored.selectComparisonSlot(0));
    EXPECT_NEAR(comparisonGain(restored), 0.0F, 0.001F);
    ASSERT_TRUE(restored.selectComparisonSlot(1));
    EXPECT_NEAR(comparisonGain(restored), -9.0F, 0.001F);
}

TEST(Comparison, MissingMalformedAndFutureMetadataSafelySeedBothSlotsFromRestoredSound)
{
    ImageControlProcessor source;
    setComparisonGain(source, -9.0F);
    const auto saved = comparisonState(source);
    for (const int format : {0, 1, 2}) {
        const auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        ASSERT_NE(xml, nullptr);
        auto* comparison = xml->getChildByName("ComparisonState");
        ASSERT_NE(comparison, nullptr);
        if (format == 0) {
            xml->removeChildElement(comparison, true);
        } else if (format == 1) {
            comparison->setAttribute("version", 99);
        } else {
            comparison->getFirstChildElement()->deleteAllChildElements();
        }
        juce::MemoryBlock state;
        juce::AudioProcessor::copyXmlToBinary(*xml, state);
        ImageControlProcessor restored;
        setComparisonGain(restored, 6.0F);
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        EXPECT_EQ(restored.getActiveComparisonSlot(), 0);
        ASSERT_TRUE(restored.selectComparisonSlot(1));
        EXPECT_NEAR(comparisonGain(restored), -9.0F, 0.001F);
    }
}

TEST(Comparison, LegacyMissingParametersUseDefaultsOnAnAlreadyEditedInstance)
{
    ImageControlProcessor source;
    auto state = comparisonState(source);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_NE(xml, nullptr);
    xml->removeChildElement(xml->getChildByName("ComparisonState"), true);
    xml->removeChildElement(xml->getChildByAttribute("id", MONO_ID), true);
    xml->removeChildElement(xml->getChildByAttribute("id", widthParameterId(0)), true);
    juce::AudioProcessor::copyXmlToBinary(*xml, state);
    ImageControlProcessor restored;
    restored.getParameters().getParameter(MONO_ID)->setValueNotifyingHost(1.0F);
    restored.getParameters().getParameter(widthParameterId(0))->setValueNotifyingHost(0.0F);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_NEAR(restored.getSoundSnapshot().at(MONO_ID), 0.0F, 0.001F);
    EXPECT_NEAR(restored.getSoundSnapshot().at(widthParameterId(0)), 1.0F, 0.001F);
    ASSERT_TRUE(restored.selectComparisonSlot(1));
    EXPECT_NEAR(restored.getSoundSnapshot().at(MONO_ID), 0.0F, 0.001F);
    EXPECT_NEAR(restored.getSoundSnapshot().at(widthParameterId(0)), 1.0F, 0.001F);
}

TEST(Comparison, ConcurrentSavesAndRestoresKeepSelectedSnapshotPairedWithTopLevelSound)
{
    ImageControlProcessor first;
    setComparisonGain(first, -6.0F);
    ImageControlProcessor second;
    ASSERT_TRUE(second.selectComparisonSlot(1));
    setComparisonGain(second, -3.0F);
    const auto firstState = comparisonState(first);
    const auto secondState = comparisonState(second);
    ImageControlProcessor processor;
    std::latch start(2);
    std::jthread restores([&] {
        start.arrive_and_wait();
        for (int iteration = 0; iteration < 100; ++iteration) {
            const auto& state = iteration % 2 == 0 ? firstState : secondState;
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
    });
    start.arrive_and_wait();
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto state = comparisonState(processor);
        const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
        ASSERT_NE(xml, nullptr);
        const auto* comparison = xml->getChildByName("ComparisonState");
        ASSERT_NE(comparison, nullptr);
        const auto* slot = comparison->getChildByAttribute("id", comparison->getStringAttribute("active"));
        ASSERT_NE(slot, nullptr);
        const auto* sound = slot->getChildByName("SoundState");
        ASSERT_NE(sound, nullptr);
        const auto* topLevel = xml->getChildByAttribute("id", OUTPUT_GAIN_ID);
        const auto* stored = sound->getChildByAttribute("id", OUTPUT_GAIN_ID);
        ASSERT_NE(topLevel, nullptr);
        ASSERT_NE(stored, nullptr);
        EXPECT_DOUBLE_EQ(topLevel->getDoubleAttribute("value"), stored->getDoubleAttribute("value"));
    }
    restores.join();
}

TEST(Comparison, ReentrantHostRestoreAbortsRemainingLocalAndUndoWrites)
{
    ImageControlProcessor target;
    setComparisonGain(target, -3.0F);
    const auto state = comparisonState(target);
    for (const bool replay : {false, true}) {
        ImageControlProcessor processor;
        auto preset = processor.getSoundSnapshot();
        preset.at("band1_mid") = -12.0F;
        preset.at(OUTPUT_GAIN_ID) = -12.0F;
        if (replay) {
            ASSERT_TRUE(processor.loadSoundSnapshot(preset, "Before restore"));
        }
        ComparisonCallback listener;
        listener.callback = [&] { processor.setStateInformation(state.getData(), static_cast<int>(state.getSize())); };
        processor.getParameters().addParameterListener("band1_mid", &listener);
        if (replay) {
            EXPECT_FALSE(processor.getHistory().undo());
        } else {
            EXPECT_FALSE(processor.loadSoundSnapshot(preset, "Interrupted recall"));
        }
        processor.getParameters().removeParameterListener("band1_mid", &listener);
        EXPECT_NEAR(comparisonGain(processor), -3.0F, 0.001F);
        EXPECT_FALSE(processor.getHistory().canUndo());
        EXPECT_FALSE(processor.getHistory().canRedo());
    }
}

TEST(Comparison, ReentrantRestoreSaveRetainsConsistentSelectedSlotAndCompleteTargetSound)
{
    ImageControlProcessor source;
    setComparisonGain(source, -6.0F);
    ASSERT_TRUE(source.selectComparisonSlot(1));
    setComparisonGain(source, -3.0F);
    const auto state = comparisonState(source);
    ImageControlProcessor restored;
    ComparisonCallback listener;
    juce::MemoryBlock reentrant;
    listener.callback = [&] { reentrant = comparisonState(restored); };
    restored.getParameters().addParameterListener(OUTPUT_GAIN_ID, &listener);
    std::thread worker([&] { restored.setStateInformation(state.getData(), static_cast<int>(state.getSize())); });
    worker.join();
    restored.getParameters().removeParameterListener(OUTPUT_GAIN_ID, &listener);
    ASSERT_GT(reentrant.getSize(), 0U);
    ImageControlProcessor roundTrip;
    roundTrip.setStateInformation(reentrant.getData(), static_cast<int>(reentrant.getSize()));
    EXPECT_EQ(roundTrip.getActiveComparisonSlot(), 1);
    EXPECT_NEAR(comparisonGain(roundTrip), -3.0F, 0.001F);
    ASSERT_TRUE(roundTrip.selectComparisonSlot(0));
    EXPECT_NEAR(comparisonGain(roundTrip), -6.0F, 0.001F);
}
}  // namespace esgrove::image_control::test
