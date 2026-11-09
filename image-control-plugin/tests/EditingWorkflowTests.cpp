/// Exercises composition of the editor's local editing toolbar and keyboard shortcuts.
/// Assertions inspect production controls, parameter values, and geometry rather than pixels.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "tests/TestSignals.h"
#include "ui/BandStrip.h"
#include "ui/ProcessingToolbar.h"
#include "UserPresets.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <vector>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

namespace esgrove::image_control::test
{
namespace
{
/// Returns an editor-owned direct child, or null while that component type is absent.
/// Check the result before use. It remains valid only while the parent retains that child.
template<typename Child>
Child* editingChild(const juce::Component& parent)
{
    for (auto* child : parent.getChildren()) {
        if (auto* result = dynamic_cast<Child*>(child)) {
            return result;
        }
    }
    return nullptr;
}
/// Finds a control by stable ID through the toolbar's implementation, or null when absent.
/// The parent hierarchy owns the result. Check it before use and do not retain it after that control is removed.
juce::Component* editingControl(juce::Component& parent, const juce::String& identifier)
{
    std::vector<juce::Component*> pending {&parent};
    while (!pending.empty()) {
        auto* component = pending.back();
        pending.pop_back();
        if (component->getComponentID() == identifier) {
            return component;
        }
        for (auto* child : component->getChildren()) {
            pending.push_back(child);
        }
    }
    return nullptr;
}

/// Consumes asynchronous selector messages and disk results with a bounded native event pump.
void pumpEditing(ProcessingToolbar& toolbar, const std::function<bool()>& done)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    do {
#if JUCE_MAC
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, static_cast<Boolean>(true));
#elif JUCE_WINDOWS
        MSG message {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        juce::Thread::sleep(1);
#endif
        toolbar.refresh();
    } while (!done() && juce::Time::getMillisecondCounterHiRes() < deadline);
    EXPECT_TRUE(done());
}
}  // namespace

/// The toolbar occupies its own usable row even at the minimum width and with analysis hidden.
TEST(EditingWorkflow, ToolbarFitsMinimumWidthAndCompactLayout)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    ImageControlEditor editor(processor);
    editor.setSize(760, editor.getHeight());
    auto* toolbar = editingChild<ProcessingToolbar>(editor);
    ASSERT_NE(toolbar, nullptr);
    EXPECT_EQ(toolbar->getHeight(), PROCESSING_TOOLBAR_HEIGHT);
    EXPECT_TRUE(editor.getLocalBounds().contains(toolbar->getBounds()));
    for (const auto* child : toolbar->getChildren()) {
        if (child->isVisible()) {
            EXPECT_FALSE(child->getBounds().isEmpty());
            EXPECT_TRUE(toolbar->getLocalBounds().contains(child->getBounds()));
        }
    }
    auto* strip = editingChild<BandStrip>(editor);
    ASSERT_NE(strip, nullptr);
    EXPECT_GE(strip->getY(), toolbar->getBottom());
    EXPECT_GE(strip->getHeight(), BAND_STRIP_MIN_HEIGHT);
}

/// Global toggle shortcuts participate in the same history as slider and bulk editing commands.
TEST(EditingWorkflow, KeyboardToggleUndoAndRedoUseLocalHistory)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    ImageControlEditor editor(processor);
    auto* bypass = processor.getParameters().getParameter(BYPASS_ID);
    ASSERT_NE(bypass, nullptr);
    const auto original = bypass->getValue();
    EXPECT_TRUE(editor.keyPressed(juce::KeyPress('b', juce::ModifierKeys::commandModifier, 0)));
    EXPECT_FALSE(juce::exactlyEqual(bypass->getValue(), original));
    EXPECT_TRUE(processor.getHistory().canUndo());
    EXPECT_TRUE(editor.keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)));
    EXPECT_FLOAT_EQ(bypass->getValue(), original);
    EXPECT_TRUE(editor.keyPressed(
        juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
    ));
    EXPECT_FALSE(juce::exactlyEqual(bypass->getValue(), original));
}
/// A recalled sound reaches the same production DSP path with the editor closed and different host block sizes.
TEST(EditingWorkflow, SnapshotRecallMatchesRealtimeAndOfflineHeadlessRendering)
{
    ImageControlProcessor realtime;
    ImageControlProcessor offline;
    auto sound = realtime.getSoundSnapshot();
    sound.at(widthParameterId(0)) = 0.5F;
    sound.at(recoverParameterId(0)) = 40.0F;
    sound.at(midGainParameterId(1)) = -3.0F;
    sound.at(panParameterId(2)) = 25.0F;
    sound.at(OUTPUT_GAIN_ID) = -6.0F;
    ASSERT_TRUE(realtime.loadSoundSnapshot(sound, "Recall sound"));
    ASSERT_TRUE(offline.loadSoundSnapshot(sound, "Recall sound"));
    realtime.setNonRealtime(false);
    offline.setNonRealtime(true);
    realtime.prepareToPlay(TEST_SAMPLE_RATE, 37);
    offline.prepareToPlay(TEST_SAMPLE_RATE, 511);
    const auto left = makeNoise(0.3F, 16'384, 114);
    const auto right = makeNoise(0.3F, 16'384, 115);
    auto first = makeBuffer({left, right});
    auto second = makeBuffer({left, right});
    const auto render = [](ImageControlProcessor& processor, juce::AudioBuffer<float>& buffer, const int size) {
        juce::MidiBuffer midi;
        for (int offset = 0; offset < buffer.getNumSamples(); offset += size) {
            juce::AudioBuffer<float> block(
                buffer.getArrayOfWritePointers(), 2, offset, std::min(size, buffer.getNumSamples() - offset)
            );
            processor.processBlock(block, midi);
        }
    };
    render(realtime, first, 37);
    render(offline, second, 511);
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < first.getNumSamples(); ++sample) {
            ASSERT_EQ(first.getSample(channel, sample), second.getSample(channel, sample));
        }
    }
    realtime.releaseResources();
    offline.releaseResources();
}

/// Asynchronous preset selection reaches production recall, shows automation dirtiness, and keeps provenance on reopen.
TEST(EditingWorkflow, PresetSelectorLoadsSoundAndRetainsBaselineAcrossToolbarReopen)
{
    ImageControlProcessor processor;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("ImageControl-Toolbar-" + juce::Uuid().toString());
    UserPresets const library(processor.getParameters(), directory);
    auto sound = processor.getSoundSnapshot();
    sound.at(OUTPUT_GAIN_ID) = -9.0F;
    const auto saved = library.saveAs("Narrow test", sound);
    ASSERT_TRUE(saved.has_value());
    {
        ProcessingToolbar toolbar(processor, directory);
        toolbar.setSize(744, PROCESSING_TOOLBAR_HEIGHT);
        auto* selector = dynamic_cast<juce::ComboBox*>(editingControl(toolbar, "preset-selector"));
        ASSERT_NE(selector, nullptr);
        pumpEditing(toolbar, [&] { return selector->getNumItems() == 1 && selector->isEnabled(); });
        selector->setSelectedId(1, juce::sendNotificationSync);
        pumpEditing(toolbar, [&] { return processor.getSelectedUserPreset().has_value(); });
        EXPECT_NEAR(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), -9.0F, 0.001F);
        EXPECT_TRUE(processor.getHistory().canUndo());
        auto* output = processor.getParameters().getParameter(OUTPUT_GAIN_ID);
        output->setValueNotifyingHost(output->convertTo0to1(-6.0F));
        toolbar.refresh();
        EXPECT_TRUE(selector->getText().endsWith(" *"));
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), 0.0F, 0.001F);
    }
    {
        ProcessingToolbar reopened(processor, directory);
        auto* selector = dynamic_cast<juce::ComboBox*>(editingControl(reopened, "preset-selector"));
        ASSERT_NE(selector, nullptr);
        EXPECT_EQ(selector->getText(), "Narrow test *");
    }
    EXPECT_TRUE(directory.deleteRecursively());
}
}  // namespace esgrove::image_control::test
