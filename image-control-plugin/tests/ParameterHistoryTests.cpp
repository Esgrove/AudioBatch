/// Exercises explicit local deltas, nested gestures, chronological branching, and host-restore invalidation.

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"

#include <gtest/gtest.h>

#include <thread>

namespace esgrove::image_control::test
{
namespace
{
/// Writes a plain parameter value without recording a local edit, as incoming host automation would.
void automate(ImageControlProcessor& processor, const juce::String& parameterId, const float value)
{
    auto& parameter = *processor.getParameters().getParameter(parameterId);
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
}
}  // namespace

TEST(ParameterHistory, NestedTouchedDeltasLeaveUnrelatedAutomationAndEditorPreferencesAlone)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    EXPECT_EQ(processor.getParameters().undoManager, nullptr);
    history.beginEdit("Linked trims");
    history.touch(midGainParameterId(0));
    automate(processor, midGainParameterId(0), -6.0F);
    history.beginEdit("Nested");
    history.touch(sideGainParameterId(0));
    automate(processor, sideGainParameterId(0), -3.0F);
    history.endEdit();
    EXPECT_TRUE(history.isEditing());
    EXPECT_FALSE(history.canUndo());
    history.endEdit();
    automate(processor, OUTPUT_GAIN_ID, -12.0F);
    processor.getEditorState().setProperty(PERSISTENCE_PROPERTY, 1.5, nullptr);
    EXPECT_EQ(history.undoDescription(), "Linked trims");
    ASSERT_TRUE(history.undo());
    EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(midGainParameterId(0))), 0.0F, 0.001F);
    EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(sideGainParameterId(0))), 0.0F, 0.001F);
    EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(OUTPUT_GAIN_ID)), -12.0F, 0.001F);
    EXPECT_DOUBLE_EQ(static_cast<double>(processor.getEditorState().getProperty(PERSISTENCE_PROPERTY)), 1.5);
    ASSERT_TRUE(history.redo());
    EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(midGainParameterId(0))), -6.0F, 0.001F);
    EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(sideGainParameterId(0))), -3.0F, 0.001F);
}

TEST(ParameterHistory, AuditionChangesAreUndoableAndNoOpsPreserveRedo)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    history.beginEdit("Audition");
    for (const auto& parameterId : {juce::String(BYPASS_ID), juce::String(DELTA_ID), soloParameterId(1)}) {
        history.touch(parameterId);
        automate(processor, parameterId, 1.0F);
    }
    history.endEdit();
    ASSERT_TRUE(history.undo());
    EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(BYPASS_ID)), 0.0F, 0.001F);
    EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(DELTA_ID)), 0.0F, 0.001F);
    history.beginEdit("No-op");
    history.touch(OUTPUT_GAIN_ID);
    history.endEdit();
    EXPECT_TRUE(history.canRedo());
    history.beginEdit("New branch");
    history.touch(OUTPUT_GAIN_ID);
    automate(processor, OUTPUT_GAIN_ID, -3.0F);
    history.endEdit();
    EXPECT_FALSE(history.canRedo());
}

TEST(ParameterHistory, BoundedHistoryRetainsNewestCommandsWithoutObservingAutomation)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    automate(processor, OUTPUT_GAIN_ID, -9.0F);
    EXPECT_FALSE(history.canUndo());
    for (int edit = 0; edit < 140; ++edit) {
        history.beginEdit("Gain edit");
        history.touch(OUTPUT_GAIN_ID);
        automate(processor, OUTPUT_GAIN_ID, edit % 2 == 0 ? -6.0F : -3.0F);
        history.endEdit();
    }
    int undos = 0;
    while (history.undo()) {
        ++undos;
    }
    EXPECT_EQ(undos, 128);
    EXPECT_FALSE(history.canUndo());
    EXPECT_TRUE(history.canRedo());
}

TEST(ParameterHistory, WorkerHostRestoreInvalidatesOpenGestureWithoutDispatch)
{
    ImageControlProcessor source;
    juce::MemoryBlock state;
    source.getStateInformation(state);
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    history.beginEdit("Interrupted");
    history.touch(OUTPUT_GAIN_ID);
    automate(processor, OUTPUT_GAIN_ID, -6.0F);
    std::thread restore([&] { processor.setStateInformation(state.getData(), static_cast<int>(state.getSize())); });
    restore.join();
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.canRedo());
    EXPECT_FALSE(history.isEditing());
    history.endEdit();
    EXPECT_FALSE(history.undo());
    history.beginEdit("After restore");
    history.touch(OUTPUT_GAIN_ID);
    automate(processor, OUTPUT_GAIN_ID, -3.0F);
    history.endEdit();
    EXPECT_TRUE(history.undo());
}
}  // namespace esgrove::image_control::test
