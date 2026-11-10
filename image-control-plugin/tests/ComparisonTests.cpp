/// Tests persistent processor-owned A/B sounds and their single chronological local undo stream.
/// Host state remains authoritative, including headless automation, legacy migration, and reentrant saves.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "UserPresets.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
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

/// Creates a detached selection pointing at an absent file, proving host state never depends on library access.
UserPreset comparisonPreset(ImageControlProcessor& processor, const juce::String& name, const float gain)
{
    auto snapshot = processor.getSoundSnapshot();
    snapshot.at(OUTPUT_GAIN_ID) = gain;
    return {
        .file
        = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(juce::Uuid().toString() + ".xml"),
        .fingerprint = "detached-exact-revision",
        .id = juce::Uuid().toString(),
        .name = name,
        .snapshot = std::move(snapshot),
    };
}

/// Loads an explicit detached preset using one coherent restore generation and comparison slot.
bool loadComparisonPreset(ImageControlProcessor& processor, const UserPreset& preset)
{
    const auto presentation = processor.getPresetPresentation();
    return processor.loadUserPreset(preset, presentation.generation, presentation.comparisonSlot);
}

/// Counts non-parameter dirty notifications independently of parameter automation.
class ComparisonDirtyListener final : public juce::AudioProcessorListener
{
public:
    /// Parameter writes are tracked independently by APVTS listeners where needed.
    void audioProcessorParameterChanged(juce::AudioProcessor* /*processor*/, int /*index*/, float /*value*/) override {}

    /// Records only actual non-parameter dirty notifications from explicit metadata changes.
    void audioProcessorChanged(juce::AudioProcessor* /*processor*/, const ChangeDetails& details) override
    {
        if (details.nonParameterStateChanged) {
            ++changes;
            if (const auto action = std::exchange(callback, {})) {
                action();
            }
        }
    }

    /// One-shot host action invoked after committed metadata becomes dirty.
    std::function<void()> callback;
    /// Dirty notifications observed while registered with the processor.
    int changes {0};
};

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
TEST(Comparison, ProvenanceRoundTripsBothSlotsCleanAndModifiedWithoutLibraryFiles)
{
    for (const int selected : {0, 1}) {
        for (const bool modified : {false, true}) {
            SCOPED_TRACE(selected);
            SCOPED_TRACE(modified);
            ImageControlProcessor source;
            const auto first = comparisonPreset(source, "Missing A", -6.0F);
            ASSERT_FALSE(first.file.exists());
            ASSERT_TRUE(loadComparisonPreset(source, first));
            if (modified) {
                setComparisonGain(source, -9.0F);
            }
            ASSERT_TRUE(source.selectComparisonSlot(1));
            const auto second = comparisonPreset(source, "Missing B", -3.0F);
            ASSERT_TRUE(loadComparisonPreset(source, second));
            if (modified) {
                setComparisonGain(source, -12.0F);
            }
            if (selected == 0) {
                ASSERT_TRUE(source.selectComparisonSlot(0));
            }
            const auto state = comparisonState(source);
            ImageControlProcessor restored;
            restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            EXPECT_EQ(restored.getActiveComparisonSlot(), selected);
            for (const int slot : {selected, 1 - selected}) {
                if (slot != restored.getActiveComparisonSlot()) {
                    ASSERT_TRUE(restored.selectComparisonSlot(slot));
                }
                const auto presentation = restored.getPresetPresentation();
                const auto& expected = slot == 0 ? first : second;
                ASSERT_TRUE(presentation.provenance);
                EXPECT_EQ(presentation.provenance->id, expected.id);
                EXPECT_EQ(presentation.provenance->name, expected.name);
                EXPECT_TRUE(sameProcessingState(presentation.provenance->baseline, expected.snapshot));
                EXPECT_EQ(presentation.modified, modified);
                EXPECT_FALSE(presentation.selection);
                EXPECT_FALSE(restored.getSelectedUserPreset());
                EXPECT_NEAR(
                    comparisonGain(restored),
                    modified ? (slot == 0 ? -9.0F : -12.0F) : (slot == 0 ? -6.0F : -3.0F),
                    0.001F
                );
            }
            const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
            ASSERT_NE(xml, nullptr);
            const auto* metadata = xml->getChildByName("PresetProvenanceState");
            ASSERT_NE(metadata, nullptr);
            EXPECT_EQ(metadata->getStringAttribute("version"), "1");
            EXPECT_EQ(xml->getChildByName("ComparisonState")->getStringAttribute("version"), "1");
            EXPECT_FALSE(metadata->toString().contains(first.file.getFullPathName()));
            EXPECT_FALSE(metadata->toString().contains(first.fingerprint));
        }
    }
}

TEST(Comparison, ProvenanceMetadataFallbackNeverDamagesValidComparisonOrSound)
{
    ImageControlProcessor source;
    const auto first = comparisonPreset(source, "First", -6.0F);
    ASSERT_TRUE(loadComparisonPreset(source, first));
    ASSERT_TRUE(source.selectComparisonSlot(1));
    const auto second = comparisonPreset(source, "Second", -3.0F);
    ASSERT_TRUE(loadComparisonPreset(source, second));
    const auto saved = comparisonState(source);
    const std::array<std::function<void(juce::XmlElement&, juce::XmlElement&)>, 23> corruptions {
        [](auto& root, auto& metadata) { root.removeChildElement(&metadata, true); },
        [](auto&, auto& metadata) { metadata.setAttribute("version", 2); },
        [](auto&, auto& metadata) { metadata.setAttribute("version", "1junk"); },
        [](auto& root, auto& metadata) { root.addChildElement(new juce::XmlElement(metadata)); },
        [](auto&, auto& metadata) { metadata.setAttribute("unexpected", true); },
        [](auto&, auto& metadata) { metadata.getFirstChildElement()->setAttribute("id", "2"); },
        [](auto&, auto& metadata) { metadata.getFirstChildElement()->setAttribute("id", "0junk"); },
        [](auto&, auto& metadata) { metadata.addChildElement(new juce::XmlElement(*metadata.getFirstChildElement())); },
        [](auto&, auto& metadata) { metadata.getFirstChildElement()->setAttribute("presetId", "not-a-uuid"); },
        [](auto&, auto& metadata) {
            metadata.getFirstChildElement()->setAttribute("presetId", juce::String::repeatedString("0", 32));
        },
        [](auto&, auto& metadata) {
            metadata.getFirstChildElement()->setAttribute("presetId", "ABCDEF0123456789ABCDEF0123456789");
        },
        [](auto&, auto& metadata) { metadata.getFirstChildElement()->setAttribute("name", "../unsafe"); },
        [](auto&, auto& metadata) { metadata.getFirstChildElement()->setAttribute("name", ""); },
        [](auto&, auto& metadata) { metadata.getFirstChildElement()->removeAttribute("name"); },
        [](auto&, auto& metadata) { metadata.getFirstChildElement()->deleteAllChildElements(); },
        [](auto&, auto& metadata) {
            metadata.getFirstChildElement()->getFirstChildElement()->setAttribute("version", 2);
        },
        [](auto&, auto& metadata) {
            metadata.getFirstChildElement()->getFirstChildElement()->getFirstChildElement()->setAttribute(
                "value", "NaN"
            );
        },
        [](auto&, auto& metadata) {
            metadata.getFirstChildElement()->getFirstChildElement()->getFirstChildElement()->setAttribute(
                "value", 1.0e20
            );
        },
        [](auto&, auto& metadata) { metadata.createNewChildElement("Unknown"); },
        [](auto&, auto& metadata) { metadata.getFirstChildElement()->createNewChildElement("Unknown"); },
        [](auto&, auto& metadata) {
            metadata.getFirstChildElement()->getChildByName("EffectiveBaseline")->deleteAllChildElements();
        },
        [](auto&, auto& metadata) {
            metadata.getFirstChildElement()->getChildByName("EffectiveBaseline")->setAttribute("unknown", true);
        },
        [](auto&, auto& metadata) {
            metadata.getFirstChildElement()
                ->getChildByName("EffectiveBaseline")
                ->getFirstChildElement()
                ->getFirstChildElement()
                ->setAttribute("value", "NaN");
        },
    };
    for (std::size_t index = 0; index < corruptions.size(); ++index) {
        SCOPED_TRACE(index);
        const auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        ASSERT_NE(xml, nullptr);
        auto* metadata = xml->getChildByName("PresetProvenanceState");
        ASSERT_NE(metadata, nullptr);
        corruptions.at(index)(*xml, *metadata);
        juce::MemoryBlock state;
        juce::AudioProcessor::copyXmlToBinary(*xml, state);
        ImageControlProcessor restored;
        ASSERT_TRUE(loadComparisonPreset(restored, first));
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        EXPECT_EQ(restored.getActiveComparisonSlot(), 1);
        EXPECT_NEAR(comparisonGain(restored), -3.0F, 0.001F);
        EXPECT_FALSE(restored.getPresetProvenance());
        EXPECT_FALSE(restored.getSelectedUserPreset());
        ASSERT_TRUE(restored.selectComparisonSlot(0));
        EXPECT_NEAR(comparisonGain(restored), -6.0F, 0.001F);
        EXPECT_FALSE(restored.getPresetProvenance());
        EXPECT_FALSE(restored.getSelectedUserPreset());
        const auto parameterTree = restored.getParameters().copyState();
        EXPECT_FALSE(parameterTree.getChildWithName("PresetProvenanceState").isValid());
        EXPECT_FALSE(parameterTree.getChildWithName("ComparisonState").isValid());
        EXPECT_FALSE(parameterTree.getChildWithName("EditorState").isValid());
    }
}

TEST(Comparison, InvalidComparisonAndLegacyRestoreDiscardProvenanceOnReusedInstance)
{
    ImageControlProcessor source;
    const auto preset = comparisonPreset(source, "Durable", -6.0F);
    ASSERT_TRUE(loadComparisonPreset(source, preset));
    const auto saved = comparisonState(source);
    for (const int corruption : {0, 1, 2, 3}) {
        const auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
        auto* comparison = xml->getChildByName("ComparisonState");
        if (corruption == 0) {
            xml->removeChildElement(comparison, true);
        } else if (corruption == 1) {
            comparison->setAttribute("version", 99);
        } else if (corruption == 2) {
            xml->addChildElement(new juce::XmlElement(*comparison));
        } else {
            xml->removeChildElement(xml->getChildByName("PresetProvenanceState"), true);
        }
        juce::MemoryBlock state;
        juce::AudioProcessor::copyXmlToBinary(*xml, state);
        ImageControlProcessor reused;
        ASSERT_TRUE(loadComparisonPreset(reused, preset));
        ASSERT_TRUE(reused.selectComparisonSlot(1));
        ASSERT_TRUE(loadComparisonPreset(reused, preset));
        reused.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        EXPECT_FALSE(reused.getPresetProvenance());
        EXPECT_FALSE(reused.getSelectedUserPreset());
        ASSERT_TRUE(reused.selectComparisonSlot(1));
        EXPECT_FALSE(reused.getPresetProvenance());
        EXPECT_FALSE(reused.getSelectedUserPreset());
        EXPECT_NEAR(comparisonGain(reused), corruption == 3 ? 0.0F : -6.0F, 0.001F);
    }
}

TEST(Comparison, MetadataOnlyChangesDirtyHostButRebindingAndRestoreDoNot)
{
    ImageControlProcessor processor;
    ComparisonDirtyListener listener;
    processor.addListener(&listener);
    auto preset = comparisonPreset(processor, "Same sound", 0.0F);
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    EXPECT_EQ(listener.changes, 1);
    EXPECT_FALSE(processor.getHistory().canUndo());
    const auto generation = processor.getHistory().generation();
    ASSERT_TRUE(processor.setSelectedUserPreset(preset, generation, 0));
    EXPECT_EQ(listener.changes, 1);
    preset.fingerprint = "another-revision";
    ASSERT_TRUE(processor.setSelectedUserPreset(preset, generation, 0));
    EXPECT_EQ(listener.changes, 1);
    preset.name = "Renamed";
    ASSERT_TRUE(processor.setSelectedUserPreset(preset, generation, 0));
    EXPECT_EQ(listener.changes, 2);
    const auto before = processor.getPresetProvenance();
    preset.name = "Externally renamed";
    preset.snapshot.at(OUTPUT_GAIN_ID) = -12.0F;
    ASSERT_TRUE(processor.reconnectSelectedUserPreset(preset, generation, 0));
    EXPECT_EQ(listener.changes, 2);
    EXPECT_TRUE(samePresetProvenance(processor.getPresetProvenance(), before));
    EXPECT_NEAR(comparisonGain(processor), 0.0F, 0.001F);
    ASSERT_TRUE(processor.reconnectSelectedUserPreset(std::nullopt, generation, 0));
    EXPECT_EQ(listener.changes, 2);
    auto wrong = preset;
    wrong.id = juce::Uuid().toString();
    EXPECT_FALSE(processor.reconnectSelectedUserPreset(wrong, generation, 0));
    const auto saved = comparisonState(processor);
    processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    EXPECT_EQ(listener.changes, 2);
    EXPECT_FALSE(processor.getSelectedUserPreset());
    EXPECT_TRUE(processor.getPresetProvenance());
    EXPECT_FALSE(processor.setSelectedUserPreset(preset, generation, 0));
    const auto current = processor.getHistory().generation();
    ASSERT_TRUE(processor.reconnectSelectedUserPreset(preset, current, 0));
    EXPECT_EQ(listener.changes, 2);
    ASSERT_TRUE(processor.setSelectedUserPreset(std::nullopt, current, 0));
    EXPECT_EQ(listener.changes, 3);
    ASSERT_TRUE(processor.setSelectedUserPreset(std::nullopt, current, 0));
    EXPECT_EQ(listener.changes, 3);
    processor.removeListener(&listener);
}

TEST(Comparison, RecallUndoAndSoundOnlyCopyPreserveIndependentPresetBaselines)
{
    ImageControlProcessor processor;
    const auto first = comparisonPreset(processor, "A preset", -6.0F);
    ASSERT_TRUE(loadComparisonPreset(processor, first));
    ASSERT_TRUE(processor.getHistory().undo());
    EXPECT_TRUE(processor.getPresetPresentation().modified);
    EXPECT_EQ(processor.getPresetProvenance().value_or(PresetProvenance {}).id, first.id);
    ASSERT_TRUE(processor.getHistory().redo());
    EXPECT_FALSE(processor.getPresetPresentation().modified);
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    const auto second = comparisonPreset(processor, "B preset", -3.0F);
    ASSERT_TRUE(loadComparisonPreset(processor, second));
    ASSERT_TRUE(processor.selectComparisonSlot(0));
    ASSERT_TRUE(processor.copyComparisonToOtherSlot());
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    EXPECT_EQ(processor.getPresetProvenance().value_or(PresetProvenance {}).id, second.id);
    EXPECT_TRUE(processor.getPresetPresentation().modified);
    ASSERT_TRUE(processor.getHistory().undo());
    ASSERT_TRUE(processor.getHistory().undo());
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    EXPECT_EQ(processor.getPresetProvenance().value_or(PresetProvenance {}).id, second.id);
    EXPECT_FALSE(processor.getPresetPresentation().modified);
}

TEST(Comparison, ReentrantSaveSeesPresetProvenanceBeforeFirstParameterNotification)
{
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Atomic recall", -12.0F);
    preset.snapshot.at(midGainParameterId(0)) = -6.0F;
    ComparisonCallback callback;
    juce::MemoryBlock saved;
    callback.callback = [&] {
        EXPECT_EQ(processor.getPresetProvenance().value_or(PresetProvenance {}).id, preset.id);
        EXPECT_EQ(processor.getSelectedUserPreset().value_or(UserPreset {}).id, preset.id);
        saved = comparisonState(processor);
    };
    processor.getParameters().addParameterListener(midGainParameterId(0), &callback);
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    processor.getParameters().removeParameterListener(midGainParameterId(0), &callback);
    ASSERT_GT(saved.getSize(), 0U);
    ImageControlProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    ASSERT_TRUE(restored.getPresetProvenance());
    EXPECT_EQ(restored.getPresetProvenance().value_or(PresetProvenance {}).id, preset.id);
    EXPECT_TRUE(
        sameProcessingState(restored.getPresetProvenance().value_or(PresetProvenance {}).baseline, preset.snapshot)
    );
    EXPECT_FALSE(restored.getSelectedUserPreset());
}

TEST(Comparison, ReentrantRestoreWinsPresetRecallAndRejectsStaleCompletion)
{
    ImageControlProcessor target;
    const auto targetPreset = comparisonPreset(target, "Restored target", -3.0F);
    ASSERT_TRUE(loadComparisonPreset(target, targetPreset));
    const auto state = comparisonState(target);
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Interrupted", -12.0F);
    preset.snapshot.at(midGainParameterId(0)) = -9.0F;
    const auto before = processor.getPresetPresentation();
    ComparisonCallback callback;
    callback.callback = [&] { processor.setStateInformation(state.getData(), static_cast<int>(state.getSize())); };
    processor.getParameters().addParameterListener(midGainParameterId(0), &callback);
    EXPECT_FALSE(processor.loadUserPreset(preset, before.generation, before.comparisonSlot));
    processor.getParameters().removeParameterListener(midGainParameterId(0), &callback);
    EXPECT_NEAR(comparisonGain(processor), -3.0F, 0.001F);
    EXPECT_EQ(processor.getPresetProvenance().value_or(PresetProvenance {}).id, targetPreset.id);
    EXPECT_FALSE(processor.getSelectedUserPreset());
    EXPECT_FALSE(processor.getHistory().canUndo());
    EXPECT_FALSE(processor.loadUserPreset(preset, before.generation, before.comparisonSlot));
    EXPECT_FALSE(processor.setSelectedUserPreset(preset, before.generation, before.comparisonSlot));
    EXPECT_FALSE(processor.reconnectSelectedUserPreset(preset, before.generation, before.comparisonSlot));
    const auto current = processor.getPresetPresentation();
    EXPECT_FALSE(processor.loadUserPreset(preset, current.generation, 1));
    EXPECT_FALSE(processor.setSelectedUserPreset(preset, current.generation, 1));
    EXPECT_FALSE(processor.reconnectSelectedUserPreset(preset, current.generation, 1));
}
TEST(Comparison, InvalidPresetCommandsLeaveSoundProvenanceHistoryAndDirtyStateUntouched)
{
    ImageControlProcessor processor;
    const auto preset = comparisonPreset(processor, "Valid", -6.0F);
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    const auto before = processor.getPresetPresentation();
    const auto description = processor.getHistory().undoDescription();
    ComparisonDirtyListener dirty;
    processor.addListener(&dirty);
    auto invalid = preset;
    invalid.id = "invalid";
    EXPECT_FALSE(processor.loadUserPreset(invalid, before.generation, 0));
    EXPECT_FALSE(processor.setSelectedUserPreset(invalid, before.generation, 0));
    EXPECT_FALSE(processor.reconnectSelectedUserPreset(invalid, before.generation, 0));
    invalid = preset;
    invalid.snapshot.erase(OUTPUT_GAIN_ID);
    EXPECT_FALSE(processor.loadUserPreset(invalid, before.generation, 0));
    EXPECT_FALSE(processor.setSelectedUserPreset(invalid, before.generation, 0));
    EXPECT_FALSE(processor.reconnectSelectedUserPreset(invalid, before.generation, 0));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), before.sound));
    EXPECT_TRUE(samePresetProvenance(processor.getPresetProvenance(), before.provenance));
    EXPECT_EQ(processor.getSelectedUserPreset().value_or(UserPreset {}).fingerprint, preset.fingerprint);
    EXPECT_EQ(processor.getHistory().undoDescription(), description);
    EXPECT_EQ(dirty.changes, 0);
    processor.removeListener(&dirty);
}

TEST(Comparison, SameUuidMayBeSelectedIndependentlyWithDifferentSlotBaselines)
{
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Shared identity", -6.0F);
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    preset.name = "Shared renamed";
    preset.snapshot.at(OUTPUT_GAIN_ID) = -3.0F;
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    const auto state = comparisonState(processor);
    ImageControlProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    ASSERT_TRUE(restored.getPresetProvenance());
    EXPECT_EQ(restored.getPresetProvenance().value_or(PresetProvenance {}).id, preset.id);
    EXPECT_EQ(restored.getPresetProvenance().value_or(PresetProvenance {}).name, "Shared renamed");
    EXPECT_FALSE(restored.getPresetPresentation().modified);
    ASSERT_TRUE(restored.selectComparisonSlot(0));
    ASSERT_TRUE(restored.getPresetProvenance());
    EXPECT_EQ(restored.getPresetProvenance().value_or(PresetProvenance {}).id, preset.id);
    EXPECT_EQ(restored.getPresetProvenance().value_or(PresetProvenance {}).name, "Shared identity");
    EXPECT_FALSE(restored.getPresetPresentation().modified);
    EXPECT_NEAR(
        restored.getPresetProvenance().value_or(PresetProvenance {}).baseline.at(OUTPUT_GAIN_ID), -6.0F, 0.001F
    );
}

TEST(Comparison, MetadataDirtyCallbackMaySaveOrRestoreReentrantlyWithoutStaleRepublication)
{
    ImageControlProcessor target;
    const auto restoredPreset = comparisonPreset(target, "Latest", -3.0F);
    ASSERT_TRUE(loadComparisonPreset(target, restoredPreset));
    const auto restoredState = comparisonState(target);
    for (const bool load : {false, true}) {
        ImageControlProcessor processor;
        const auto preset = comparisonPreset(processor, "Metadata only", 0.0F);
        const auto context = processor.getPresetPresentation();
        ComparisonDirtyListener dirty;
        juce::MemoryBlock saved;
        dirty.callback = [&] {
            saved = comparisonState(processor);
            processor.setStateInformation(restoredState.getData(), static_cast<int>(restoredState.getSize()));
        };
        processor.addListener(&dirty);
        if (load) {
            EXPECT_FALSE(processor.loadUserPreset(preset, context.generation, context.comparisonSlot));
        } else {
            EXPECT_FALSE(processor.setSelectedUserPreset(preset, context.generation, context.comparisonSlot));
        }
        processor.removeListener(&dirty);
        EXPECT_EQ(dirty.changes, 1);
        EXPECT_EQ(processor.getPresetProvenance().value_or(PresetProvenance {}).id, restoredPreset.id);
        EXPECT_FALSE(processor.getSelectedUserPreset());
        EXPECT_NEAR(comparisonGain(processor), -3.0F, 0.001F);
        EXPECT_FALSE(processor.getHistory().canUndo());
        ASSERT_GT(saved.getSize(), 0U);
        ImageControlProcessor reentrantSave;
        reentrantSave.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        ASSERT_TRUE(reentrantSave.getPresetProvenance());
        EXPECT_EQ(reentrantSave.getPresetProvenance().value_or(PresetProvenance {}).id, preset.id);
        EXPECT_FALSE(reentrantSave.getPresetPresentation().modified);
    }
}
TEST(Comparison, OptionalSingleSlotProvenanceDoesNotInventAnInactiveSelection)
{
    for (const int selected : {0, 1}) {
        ImageControlProcessor source;
        if (selected == 1) {
            ASSERT_TRUE(source.selectComparisonSlot(1));
        }
        const auto preset = comparisonPreset(source, "Only selected slot", -6.0F);
        ASSERT_TRUE(loadComparisonPreset(source, preset));
        const auto state = comparisonState(source);
        ImageControlProcessor restored;
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        ASSERT_TRUE(restored.getPresetProvenance());
        EXPECT_EQ(restored.getPresetProvenance().value_or(PresetProvenance {}).id, preset.id);
        ASSERT_TRUE(restored.selectComparisonSlot(1 - selected));
        EXPECT_FALSE(restored.getPresetProvenance());
        EXPECT_FALSE(restored.getSelectedUserPreset());
        EXPECT_FALSE(restored.getPresetPresentation().modified);
    }
}

TEST(Comparison, ExplicitBaselineUpdateAloneDirtiesHostWithoutChangingSoundOrHistory)
{
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Baseline", -6.0F);
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    const auto description = processor.getHistory().undoDescription();
    setComparisonGain(processor, -3.0F);
    EXPECT_TRUE(processor.getPresetPresentation().modified);
    const auto context = processor.getPresetPresentation();
    preset.snapshot = context.sound;
    preset.fingerprint = "updated-sound-revision";
    ComparisonDirtyListener dirty;
    processor.addListener(&dirty);
    ASSERT_TRUE(processor.setSelectedUserPreset(preset, context.generation, context.comparisonSlot));
    EXPECT_EQ(dirty.changes, 1);
    EXPECT_FALSE(processor.getPresetPresentation().modified);
    EXPECT_NEAR(comparisonGain(processor), -3.0F, 0.001F);
    EXPECT_EQ(processor.getHistory().undoDescription(), description);
    processor.removeListener(&dirty);
}
TEST(Comparison, ValidFileRecallIsCleanWithoutRewritingSourceFloatsAndRepresentableAutomationIsModified)
{
    ImageControlProcessor processor;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("imagecontrol-provenance-" + juce::Uuid().toString());
    const UserPresets library(processor.getParameters(), directory);
    for (const float gain : {-6.0F, -3.0F, -7.7F, 2.3F}) {
        SCOPED_TRACE(gain);
        auto sound = processor.getSoundSnapshot();
        sound.at(OUTPUT_GAIN_ID) = gain;
        sound.at(midGainParameterId(0)) = -3.0F;
        sound.at(panParameterId(1)) = 17.3F;
        sound.at(widthParameterId(2)) = 0.73F;
        sound.at(recoverParameterId(3)) = 23.7F;
        const auto saved = library.saveAs("Source " + juce::String(gain), sound);
        ASSERT_TRUE(saved.has_value());
        const auto loaded = library.load(saved.value());
        ASSERT_TRUE(loaded.has_value());
        ASSERT_TRUE(loadComparisonPreset(processor, loaded.value()));
        auto presentation = processor.getPresetPresentation();
        EXPECT_FALSE(presentation.modified);
        EXPECT_TRUE(sameProcessingState(presentation.provenance.value_or(PresetProvenance {}).baseline, sound));
        EXPECT_TRUE(sameProcessingState(presentation.selection.value_or(UserPreset {}).snapshot, sound));
        EXPECT_FLOAT_EQ(presentation.sound.at(recoverParameterId(3)), 24.0F);
        EXPECT_FLOAT_EQ(
            presentation.provenance.value_or(PresetProvenance {}).baseline.at(recoverParameterId(3)), 23.7F
        );
        const auto state = comparisonState(processor);
        ImageControlProcessor restored;
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        EXPECT_FALSE(restored.getPresetPresentation().modified);
        EXPECT_FLOAT_EQ(restored.getSoundSnapshot().at(recoverParameterId(3)), 24.0F);
        ASSERT_TRUE(restored.selectComparisonSlot(1));
        ASSERT_TRUE(restored.selectComparisonSlot(0));
        EXPECT_FALSE(restored.getPresetPresentation().modified);
        EXPECT_FLOAT_EQ(restored.getSoundSnapshot().at(recoverParameterId(3)), 24.0F);
        auto& recovery = *processor.getParameters().getParameter(recoverParameterId(3));
        recovery.setValueNotifyingHost(23.6F / 200.0F);
        EXPECT_FLOAT_EQ(processor.getSoundSnapshot().at(recoverParameterId(3)), 24.0F);
        EXPECT_FALSE(processor.getPresetPresentation().modified);
        recovery.setValueNotifyingHost(24.5F / 200.0F);
        EXPECT_FLOAT_EQ(processor.getSoundSnapshot().at(recoverParameterId(3)), 25.0F);
        EXPECT_TRUE(processor.getPresetPresentation().modified);
        ASSERT_TRUE(loadComparisonPreset(processor, loaded.value()));
        EXPECT_FALSE(processor.getPresetPresentation().modified);
        setComparisonGain(processor, gain + 0.1F);
        presentation = processor.getPresetPresentation();
        EXPECT_TRUE(presentation.modified);
        EXPECT_FALSE(juce::exactlyEqual(presentation.sound.at(OUTPUT_GAIN_ID), sound.at(OUTPUT_GAIN_ID)));
        ASSERT_TRUE(loadComparisonPreset(processor, loaded.value()));
        EXPECT_FALSE(processor.getPresetPresentation().modified);
    }
    EXPECT_TRUE(directory.deleteRecursively());
}

TEST(Comparison, AtomicRenamePreservesBaselineAndPublishesActualRevisionBeforeDirtyNotification)
{
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Before rename", -6.0F);
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    setComparisonGain(processor, -3.0F);
    const auto before = processor.getPresetPresentation();
    preset.name = "After rename";
    preset.fingerprint = "renamed-actual-revision";
    preset.snapshot.at(OUTPUT_GAIN_ID) = -12.0F;
    ComparisonDirtyListener dirty;
    dirty.callback = [&] {
        const auto during = processor.getPresetPresentation();
        EXPECT_EQ(during.provenance.value_or(PresetProvenance {}).name, preset.name);
        EXPECT_EQ(during.selection.value_or(UserPreset {}).fingerprint, preset.fingerprint);
        EXPECT_TRUE(sameProcessingState(during.selection.value_or(UserPreset {}).snapshot, preset.snapshot));
        EXPECT_TRUE(sameProcessingState(
            during.provenance.value_or(PresetProvenance {}).baseline,
            before.provenance.value_or(PresetProvenance {}).baseline
        ));
        EXPECT_TRUE(during.modified);
    };
    processor.addListener(&dirty);
    ASSERT_TRUE(processor.renameSelectedUserPreset(preset, before.generation, before.comparisonSlot));
    EXPECT_EQ(dirty.changes, 1);
    EXPECT_NEAR(comparisonGain(processor), -3.0F, 0.001F);
    ASSERT_TRUE(processor.renameSelectedUserPreset(preset, before.generation, before.comparisonSlot));
    EXPECT_EQ(dirty.changes, 1);
    auto wrong = preset;
    wrong.id = juce::Uuid().toString();
    EXPECT_FALSE(processor.renameSelectedUserPreset(wrong, before.generation, before.comparisonSlot));
    EXPECT_FALSE(processor.renameSelectedUserPreset(preset, before.generation, 1));
    const auto state = comparisonState(processor);
    processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_FALSE(processor.renameSelectedUserPreset(preset, before.generation, before.comparisonSlot));
    EXPECT_EQ(dirty.changes, 1);
    processor.removeListener(&dirty);
}
TEST(Comparison, SmallestPublishedRepresentableCrossoverAutomationMarksPresetModifiedExactly)
{
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Exact crossover", -6.0F);
    preset.snapshot.at(crossoverParameterId(0)) = 137.23F;
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    const auto clean = processor.getPresetPresentation();
    ASSERT_FALSE(clean.modified);
    auto& parameter = *processor.getParameters().getParameter(crossoverParameterId(0));
    auto normalized = parameter.getValue();
    auto changed = clean.sound;
    for (int step = 0; step < 32 && sameProcessingState(changed, clean.sound); ++step) {
        normalized = std::nextafter(normalized, std::numeric_limits<float>::infinity());
        parameter.setValueNotifyingHost(normalized);
        changed = processor.getSoundSnapshot();
    }
    ASSERT_FALSE(sameProcessingState(changed, clean.sound));
    EXPECT_TRUE(processor.getPresetPresentation().modified);
    EXPECT_TRUE(
        sameProcessingState(processor.getPresetProvenance().value_or(PresetProvenance {}).baseline, preset.snapshot)
    );
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    EXPECT_FALSE(processor.getPresetPresentation().modified);
}
TEST(Comparison, LegacyProvenanceWithoutEffectiveBaselineLoadsAndRetainsSourceSnapshot)
{
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Legacy source", -6.0F);
    preset.snapshot.at(crossoverParameterId(0)) = 40.179798F;
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    auto state = comparisonState(processor);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    auto* slot = xml->getChildByName("PresetProvenanceState")->getFirstChildElement();
    slot->removeChildElement(slot->getChildByName("EffectiveBaseline"), true);
    juce::AudioProcessor::copyXmlToBinary(*xml, state);
    ImageControlProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    EXPECT_FALSE(restored.getPresetPresentation().modified);
    EXPECT_TRUE(
        sameProcessingState(restored.getPresetProvenance().value_or(PresetProvenance {}).baseline, preset.snapshot)
    );
    ASSERT_TRUE(restored.selectComparisonSlot(1));
    ASSERT_TRUE(restored.selectComparisonSlot(0));
    EXPECT_FALSE(restored.getPresetPresentation().modified);
}

TEST(Comparison, ArbitrarySkewedCrossoversRemainCleanOrModifiedAcrossRepeatedComparisonAndHostRecall)
{
    for (const float frequency : {40.179798F, 137.23F, 21.123457F, 333.33334F, 1234.5677F}) {
        for (const bool automate : {false, true}) {
            SCOPED_TRACE(frequency);
            SCOPED_TRACE(automate);
            auto processor = std::make_unique<ImageControlProcessor>();
            auto preset = comparisonPreset(*processor, "Repeated skewed recall", -6.0F);
            preset.snapshot.at(crossoverParameterId(0)) = frequency;
            ASSERT_TRUE(loadComparisonPreset(*processor, preset));
            ASSERT_FALSE(processor->getPresetPresentation().modified);
            if (automate) {
                auto& parameter = *processor->getParameters().getParameter(crossoverParameterId(0));
                auto normalized = parameter.getValue();
                const auto clean = processor->getSoundSnapshot();
                for (int step = 0; step < 32 && sameProcessingState(clean, processor->getSoundSnapshot()); ++step) {
                    normalized = std::nextafter(normalized, std::numeric_limits<float>::infinity());
                    parameter.setValueNotifyingHost(normalized);
                }
                ASSERT_FALSE(sameProcessingState(clean, processor->getSoundSnapshot()));
                ASSERT_TRUE(processor->getPresetPresentation().modified);
            }
            for (int repetition = 0; repetition < 5; ++repetition) {
                SCOPED_TRACE(repetition);
                ASSERT_TRUE(processor->selectComparisonSlot(1));
                ASSERT_TRUE(processor->selectComparisonSlot(0));
                EXPECT_EQ(processor->getPresetPresentation().modified, automate);
                const auto state = comparisonState(*processor);
                auto restored = std::make_unique<ImageControlProcessor>();
                restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                processor = std::move(restored);
                EXPECT_EQ(processor->getPresetPresentation().modified, automate);
                EXPECT_TRUE(sameProcessingState(
                    processor->getPresetProvenance().value_or(PresetProvenance {}).baseline, preset.snapshot
                ));
            }
        }
    }
}
TEST(Comparison, SkewedPresetRecallUndoRedoKeepsCleanReferenceAcrossRepeatedReplay)
{
    for (const float frequency : {40.179798F, 137.23F, 21.123457F, 333.33334F, 1234.5677F}) {
        SCOPED_TRACE(frequency);
        ImageControlProcessor processor;
        auto preset = comparisonPreset(processor, "History skewed recall", -6.0F);
        preset.snapshot.at(crossoverParameterId(0)) = frequency;
        ASSERT_TRUE(loadComparisonPreset(processor, preset));
        ASSERT_FALSE(processor.getPresetPresentation().modified);
        for (int repetition = 0; repetition < 5; ++repetition) {
            SCOPED_TRACE(repetition);
            ASSERT_TRUE(processor.getHistory().undo());
            EXPECT_TRUE(processor.getPresetPresentation().modified);
            ASSERT_TRUE(processor.getHistory().redo());
            EXPECT_FALSE(processor.getPresetPresentation().modified);
            EXPECT_TRUE(sameProcessingState(
                processor.getPresetProvenance().value_or(PresetProvenance {}).baseline, preset.snapshot
            ));
        }
    }
}

TEST(Comparison, SkewedHistoryReplayPreservesGenuineModificationAndUntouchedReferenceIds)
{
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Partial replay", -6.0F);
    preset.snapshot.at(crossoverParameterId(0)) = 137.23F;
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    auto& crossover = *processor.getParameters().getParameter(crossoverParameterId(0));
    const auto clean = processor.getPresetPresentation();
    processor.getHistory().beginEdit("Representable crossover edit");
    processor.getHistory().touch(crossoverParameterId(0));
    auto normalized = crossover.getValue();
    for (int step = 0; step < 32 && sameProcessingState(clean.sound, processor.getSoundSnapshot()); ++step) {
        normalized = std::nextafter(normalized, std::numeric_limits<float>::infinity());
        crossover.setValueNotifyingHost(normalized);
    }
    processor.getHistory().endEdit();
    ASSERT_TRUE(processor.getPresetPresentation().modified);
    const auto outputReference = clean.provenance.value_or(PresetProvenance {})
                                     .effectiveBaseline.value_or(ProcessingSnapshot {})
                                     .at(OUTPUT_GAIN_ID);
    for (int repetition = 0; repetition < 5; ++repetition) {
        SCOPED_TRACE(repetition);
        ASSERT_TRUE(processor.getHistory().undo());
        EXPECT_FALSE(processor.getPresetPresentation().modified);
        ASSERT_TRUE(processor.getHistory().redo());
        EXPECT_TRUE(processor.getPresetPresentation().modified);
        const auto provenance = processor.getPresetProvenance().value_or(PresetProvenance {});
        EXPECT_TRUE(
            juce::exactlyEqual(
                provenance.effectiveBaseline.value_or(ProcessingSnapshot {}).at(OUTPUT_GAIN_ID), outputReference
            )
        );
        EXPECT_TRUE(sameProcessingState(provenance.baseline, preset.snapshot));
    }
}

TEST(Comparison, PresetReplayProvenanceIsPublishedBeforeReentrantSaveAndRestoreWins)
{
    ImageControlProcessor processor;
    auto preset = comparisonPreset(processor, "Reentrant replay", 0.0F);
    preset.snapshot.at(crossoverParameterId(0)) = 40.179798F;
    ASSERT_TRUE(loadComparisonPreset(processor, preset));
    ASSERT_TRUE(processor.getHistory().undo());
    ComparisonCallback callback;
    bool sawClean = false;
    callback.callback = [&] {
        const auto state = comparisonState(processor);
        ImageControlProcessor saved;
        saved.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        sawClean = !saved.getPresetPresentation().modified;
    };
    processor.getParameters().addParameterListener(crossoverParameterId(0), &callback);
    ASSERT_TRUE(processor.getHistory().redo());
    EXPECT_TRUE(sawClean);
    ASSERT_TRUE(processor.getHistory().undo());
    ImageControlProcessor target;
    const auto targetPreset = comparisonPreset(target, "Restore wins replay", -3.0F);
    ASSERT_TRUE(loadComparisonPreset(target, targetPreset));
    const auto state = comparisonState(target);
    callback.callback = [&] { processor.setStateInformation(state.getData(), static_cast<int>(state.getSize())); };
    EXPECT_FALSE(processor.getHistory().redo());
    processor.getParameters().removeParameterListener(crossoverParameterId(0), &callback);
    EXPECT_EQ(processor.getPresetProvenance().value_or(PresetProvenance {}).id, targetPreset.id);
    EXPECT_FALSE(processor.getPresetPresentation().modified);
    EXPECT_FALSE(processor.getHistory().canUndo());
}
}  // namespace esgrove::image_control::test
