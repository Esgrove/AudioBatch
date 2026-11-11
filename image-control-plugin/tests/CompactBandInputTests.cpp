/// Tests production pointer, wheel, key, and mixed input through compact and expanded band controls.
/// Parameter attachments preserve local history, Link, automation exclusion, and balanced host gestures.

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "tests/CompactBandTestSupport.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/DisplayDisclosure.h"
#include "ui/ImageControlLookAndFeel.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

#include <thread>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Returns native default text and plain values for all four compact controls.
std::pair<juce::String, double> compactDefaultExample(const ControlSlider& slider)
{
    if (slider.getName() == "Width") {
        return {"1.00", 1.0};
    }
    if (slider.getName() == "Pan") {
        return {"C", 0.0};
    }
    return {"0.0 dB", 0.0};
}
}  // namespace

/// Heading row changes retain uncommitted numeric text and linked Mid/Side gesture ownership through grow and shrink.
TEST(CompactBands, ResponsiveHeadingPreservesNumericDraftAndLinkedGestures)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setRangeText("Below 120 Hz");
    auto* name = bandHeadingLabel(strip, "Low");
    auto* range = bandHeadingLabel(strip, "Below 120 Hz");
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(name, nullptr);
    ASSERT_NE(range, nullptr);
    ASSERT_NE(mid, nullptr);
    const int threshold = bandHeadingInlineWidth(*name, *range, 0);
    strip.setSize(threshold, BAND_STRIP_MIN_HEIGHT + 80);
    strip.addToDesktop(0);
    strip.setVisible(true);
    const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
    const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    const auto* label = compactValueBox(*mid);
    ASSERT_NE(label, nullptr);
    const juce::Component::SafePointer<juce::TextEditor> editor(label->getCurrentTextEditor());
    ASSERT_NE(editor, nullptr);
    editor->setText("-3.27", false);
    editor->setHighlightedRegion({1, 4});
    for (const int width : {threshold, threshold + 80, threshold - 1, threshold}) {
        strip.setSize(width, strip.getHeight());
        for (const auto* text : {"Below 16.00 kHz", "Below 120 Hz"}) {
            strip.setRangeText(text);
            strip.setRangeOnSecondRow(juce::String(text) == "Below 16.00 kHz");
            EXPECT_EQ(compactValueBox(*mid), label);
            ASSERT_EQ(label->getCurrentTextEditor(), editor.getComponent());
            EXPECT_EQ(editor->getText(), "-3.27");
            EXPECT_EQ(editor->getHighlightedRegion(), (juce::Range<int> {1, 4}));
            EXPECT_TRUE(editor->hasKeyboardFocus(true));
            EXPECT_FALSE(history.canUndo());
            EXPECT_TRUE(midGestures.gestureBoundaries().empty());
            EXPECT_TRUE(sideGestures.gestureBoundaries().empty());
        }
    }
    mid->hideTextBox(true);
    EXPECT_NEAR(mid->getValue(), 0.0, 0.0001);
    mid->mouseDown(compactMouse(*mid));
    mid->mouseDrag(compactMouse(*mid, {32.0F, 12.0F}));
    ASSERT_TRUE(history.isEditing());
    const double edited = mid->getValue();
    for (const int width : {threshold, threshold + 80, threshold - 1, threshold}) {
        strip.setSize(width, strip.getHeight());
        for (const auto* text : {"Below 16.00 kHz", "Below 120 Hz"}) {
            strip.setRangeText(text);
            strip.setRangeOnSecondRow(juce::String(text) == "Below 16.00 kHz");
            EXPECT_TRUE(history.isEditing());
            EXPECT_DOUBLE_EQ(mid->getValue(), edited);
            EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true}));
            EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true}));
        }
    }
    mid->mouseDrag(compactMouse(*mid, {52.0F, 12.0F}));
    EXPECT_GT(mid->getValue(), edited);
    mid->endDrag();
    EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), 0.0F, 0.0001F);
    EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
    EXPECT_FALSE(history.canUndo());
}

/// Pan edits and automation never invoke gain-only Link or add host-originated changes to local history.
TEST(CompactBands, PanHistoryAndAutomationRemainIndependentOfLinkedGains)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    auto* pan = compactControl<ControlSlider>(strip, "Low Pan");
    ASSERT_NE(pan, nullptr);
    auto& parameter = *processor.getParameters().getParameter(panParameterId(0));
    const CompactGestureCounter panGestures(parameter);
    const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
    const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
    EXPECT_TRUE(pan->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_EQ(panGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_TRUE(history.undo());
    EXPECT_FALSE(history.canUndo());
    parameter.setValueNotifyingHost(parameter.convertTo0to1(-25.0F));
    EXPECT_NEAR(pan->getValue(), -25.0, 0.0001);
    EXPECT_EQ(pan->compactValueText(), "25L");
    EXPECT_FALSE(history.canUndo());
    EXPECT_TRUE(history.canRedo());
    EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), 0.0F, 0.0001F);
    EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
    EXPECT_TRUE(midGestures.gestureBoundaries().empty());
    EXPECT_TRUE(sideGestures.gestureBoundaries().empty());
}

/// Resizing between short and full inline names preserves the native numeric editor and active parameter gesture.
TEST(CompactBands, WideResizePreservesNumericEditorAndActivePanGesture)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* pan = compactControl<ControlSlider>(strip, "Low Pan");
    ASSERT_NE(pan, nullptr);
    ASSERT_TRUE(pan->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    const auto* label = compactValueBox(*pan);
    ASSERT_NE(label, nullptr);
    const juce::Component::SafePointer<juce::TextEditor> editor(label->getCurrentTextEditor());
    ASSERT_NE(editor, nullptr);
    editor->setText("33R", false);
    strip.setSize(400, BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_EQ(compactValueBox(*pan), label);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    EXPECT_EQ(editor->getText(), "33R");
    EXPECT_EQ(pan->compactValueText(), "C PAN");
    pan->setCompactValueFullNames(false);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    EXPECT_EQ(pan->compactValueText(), "C");
    pan->setCompactValueFullNames(true);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    EXPECT_EQ(editor->getText(), "33R");
    EXPECT_EQ(pan->compactValueText(), "C PAN");
    EXPECT_FALSE(history.canUndo());
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    pan->hideTextBox(false);
    EXPECT_NEAR(pan->getValue(), 33.0, 0.0001);
    EXPECT_TRUE(history.undo());
    const CompactGestureCounter gestures(*processor.getParameters().getParameter(panParameterId(0)));
    pan->mouseDown(compactMouse(*pan));
    pan->mouseDrag(compactMouse(*pan, {32.0F, 12.0F}));
    EXPECT_TRUE(history.isEditing());
    strip.setSize(400, BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_TRUE(history.isEditing());
    EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true}));
    pan->endDrag();
    EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(pan->getValue(), 0.0, 0.0001);
}

/// Coarse and fine wheel and key actions reach the slider rather than an overlaid editable label.
TEST(CompactBands, WheelAndArrowsKeepFineStepsAndHostShortcutRouting)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan"}) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        auto* routed = dynamic_cast<ControlSlider*>(slider->getComponentAt(juce::Point<int> {12, 12}));
        ASSERT_EQ(routed, slider);
        const double initial = slider->getValue();
        routed->mouseWheelMove(compactMouse(*routed, {12.0F, 12.0F}, {12.0F, 12.0F}, {}), compactNotch());
        const double coarseWheel = slider->getValue() - initial;
        EXPECT_GT(coarseWheel, 0.0);
        slider->setValue(initial, juce::dontSendNotification);
        routed->mouseWheelMove(
            compactMouse(*routed, {12.0F, 12.0F}, {12.0F, 12.0F}, juce::ModifierKeys::shiftModifier), compactNotch()
        );
        const double fineWheel = slider->getValue() - initial;
        EXPECT_GT(fineWheel, 0.0);
        EXPECT_LE(fineWheel, coarseWheel + 0.00001);
        slider->setValue(initial, juce::dontSendNotification);
        EXPECT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        const double coarseKey = slider->getValue() - initial;
        slider->setValue(initial, juce::dontSendNotification);
        EXPECT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)));
        const double fineKey = slider->getValue() - initial;
        EXPECT_GT(fineKey, 0.0);
        EXPECT_LT(fineKey, coarseKey);
        for (const int modifier :
             {juce::ModifierKeys::commandModifier, juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::altModifier})
        {
            EXPECT_FALSE(slider->keyPressed(juce::KeyPress(juce::KeyPress::rightKey, modifier, 0)));
        }
        EXPECT_FALSE(processor.getHistory().isEditing());
    }
}

/// Compact dragging remains relative and fine Shift movement is smaller, with double-click returning to default.
TEST(CompactBands, RelativeDragFineAdjustmentAndResetUseExistingSliderGestures)
{
    for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan"}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(true);
        strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        const double initial = slider->getValue();
        slider->mouseDown(compactMouse(*slider));
        EXPECT_DOUBLE_EQ(slider->getValue(), initial);
        slider->mouseDrag(compactMouse(*slider, {52.0F, 12.0F}));
        const double coarse = slider->getValue() - initial;
        slider->endDrag();
        EXPECT_GT(coarse, 0.0);
        EXPECT_TRUE(processor.getHistory().undo());
        const int fineModifiers = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier;
        slider->mouseDown(compactMouse(*slider, {12.0F, 12.0F}, {12.0F, 12.0F}, fineModifiers));
        slider->mouseDrag(compactMouse(*slider, {52.0F, 12.0F}, {12.0F, 12.0F}, fineModifiers));
        const double fine = slider->getValue() - initial;
        slider->endDrag();
        EXPECT_GT(fine, 0.0);
        EXPECT_LT(fine, coarse);
        slider->mouseDoubleClick(compactMouse(*slider));
        EXPECT_NEAR(slider->getValue(), initial, 0.0001);
        EXPECT_FALSE(processor.getHistory().isEditing());
    }
}

/// Every compact Mid input shares balanced Mid/Side gestures and exactly one undo command when Link is enabled.
TEST(CompactBands, LinkedInputPathsShareOneHistoryCommandAndBalancedHostGestures)
{
    for (int input = 0; input < 5; ++input) {
        SCOPED_TRACE(input);
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        auto& midParameter = *processor.getParameters().getParameter(midGainParameterId(0));
        auto& sideParameter = *processor.getParameters().getParameter(sideGainParameterId(0));
        setParameterInGesture(midParameter, -6.0F);
        setParameterInGesture(sideParameter, -6.0F);
        const CompactGestureCounter midGestures(midParameter);
        const CompactGestureCounter sideGestures(sideParameter);
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(true);
        strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
        strip.addToDesktop(0);
        strip.setVisible(true);
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        if (input == 0) {
            mid->mouseDown(compactMouse(*mid));
            mid->mouseDrag(compactMouse(*mid, {32.0F, 12.0F}));
            mid->endDrag();
        } else if (input == 1) {
            EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        } else if (input == 2) {
            mid->mouseWheelMove(compactMouse(*mid), compactNotch());
        } else if (input == 3) {
            ASSERT_TRUE(enterCompactValue(*mid, "-3.27 dB"));
        } else {
            mid->mouseDoubleClick(compactMouse(*mid));
        }
        const float edited = compactParameterValue(processor, midGainParameterId(0));
        EXPECT_GT(edited, -6.0F);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), edited, 0.0001F);
        EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_FALSE(history.isEditing());
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), -6.0F, 0.0001F);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), -6.0F, 0.0001F);
        EXPECT_FALSE(history.canUndo());
        EXPECT_TRUE(history.redo());
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), edited, 0.0001F);
    }
}

/// Host updates remain silent during a linked gesture, and untouched Side Off is never captured into local history.
TEST(CompactBands, AutomationExcludedAndLinkedSideOffPreserved)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(mid, nullptr);
    auto& midParameter = *processor.getParameters().getParameter(midGainParameterId(0));
    mid->mouseDown(compactMouse(*mid));
    std::thread automation([&midParameter] { midParameter.setValueNotifyingHost(midParameter.convertTo0to1(-9.0F)); });
    automation.join();
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    do {
        dispatchCompactChanges();
    } while ((mid->getValue() < -9.0001 || mid->getValue() > -8.9999)
             && juce::Time::getMillisecondCounterHiRes() < deadline);
    EXPECT_NEAR(mid->getValue(), -9.0, 0.0001);
    EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
    mid->endDrag();
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.isEditing());
    auto& sideParameter = *processor.getParameters().getParameter(sideGainParameterId(0));
    setParameterInGesture(sideParameter, BAND_GAIN_MIN_DB);
    EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_FLOAT_EQ(compactParameterValue(processor, sideGainParameterId(0)), BAND_GAIN_MIN_DB);
    setParameterInGesture(sideParameter, -12.0F);
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), -9.0F, 0.0001F);
    EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), -12.0F, 0.0001F);
}

/// Mode switches finish drags once, ignore stale mouse movement, and preserve the linked edit as one command.
TEST(CompactBands, ModeSwitchClosesActiveGesturesInBothDirections)
{
    for (const bool initiallyCompact : {false, true}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
        const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(initiallyCompact);
        strip.setSize(240, initiallyCompact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        mid->mouseDown(compactMouse(*mid));
        mid->mouseDrag(compactMouse(*mid, {32.0F, 12.0F}));
        EXPECT_TRUE(history.isEditing());
        const double edited = mid->getValue();
        strip.setCompact(!initiallyCompact);
        EXPECT_FALSE(history.isEditing());
        mid->mouseDrag(compactMouse(*mid, {52.0F, 12.0F}));
        mid->mouseUp(compactMouse(*mid, {52.0F, 12.0F}));
        EXPECT_DOUBLE_EQ(mid->getValue(), edited);
        EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), 0.0F, 0.0001F);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
        EXPECT_FALSE(history.canUndo());
    }
}

/// Hidden controls hand focus to Mid, while retained Mid/Side/Width/Pan keep focus across both presentations.
TEST(CompactBands, FocusMovesFromHiddenControlsAndRetainedValuesKeepFocus)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    strip.toFront(true);
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(mid, nullptr);
    for (const auto* title : {"Low Recover", "Low Pan", "Low Solo", "Low Link", "Low Mid", "Low Side", "Low Width"}) {
        strip.setCompact(false);
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* control = compactControl<juce::Component>(strip, title);
        ASSERT_NE(control, nullptr);
        control->grabKeyboardFocus();
        ASSERT_TRUE(control->hasKeyboardFocus(false));
        strip.setCompact(true);
        strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
        if (control->isVisible()) {
            EXPECT_TRUE(control->hasKeyboardFocus(false));
        } else {
            EXPECT_TRUE(mid->hasKeyboardFocus(false));
        }
        auto* retained = juce::Component::getCurrentlyFocusedComponent();
        strip.setCompact(false);
        EXPECT_EQ(juce::Component::getCurrentlyFocusedComponent(), retained);
    }
}

/// Screen readers retain full band names, editable slider roles, and unit-bearing values in compact mode.
TEST(CompactBands, AccessibilityKeepsFullNamesUnitsAndEditing)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan"}) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        auto* handler = slider->getAccessibilityHandler();
        ASSERT_NE(handler, nullptr);
        EXPECT_EQ(handler->getRole(), juce::AccessibilityRole::slider);
        EXPECT_EQ(handler->getTitle(), title);
        auto* value = handler->getValueInterface();
        ASSERT_NE(value, nullptr);
        EXPECT_FALSE(value->isReadOnly());
        const auto [initialText, initialValue] = compactDefaultExample(*slider);
        const auto [entry, expected] = compactEntryExample(*slider);
        EXPECT_EQ(value->getCurrentValueAsString(), initialText);
        value->setValueAsString(entry);
        EXPECT_NEAR(slider->getValue(), expected, 0.0001);
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(slider->getValue(), initialValue, 0.0001);
    }
}

/// Display disclosures paint local keyboard focus instead of gaining a section-sized managed outline.
TEST(CompactBands, DisclosureFocusOptOutLeavesOrdinaryButtonOutlinesEnabled)
{
    ImageControlLookAndFeel lookAndFeel;
    juce::Component root;
    DisplayDisclosure disclosure("Controls", false);
    juce::TextButton ordinary("Action");
    root.setLookAndFeel(&lookAndFeel);
    root.addAndMakeVisible(disclosure);
    root.addAndMakeVisible(ordinary);
    root.setSize(240, 80);
    disclosure.setBounds(0, 0, 240, 28);
    disclosure.setHasFocusOutline(false);
    ordinary.setBounds(0, 40, 100, 28);
    ordinary.setHasFocusOutline(false);
    root.addToDesktop(0);
    root.setVisible(true);
    root.toFront(true);
    disclosure.grabKeyboardFocus();
    EXPECT_TRUE(disclosure.hasKeyboardFocus(false));
    EXPECT_FALSE(disclosure.hasFocusOutline());
    EXPECT_EQ(lookAndFeel.createFocusOutlineForComponent(disclosure), nullptr);
    ordinary.grabKeyboardFocus();
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    do {
        dispatchCompactChanges();
    } while (!ordinary.hasFocusOutline() && juce::Time::getMillisecondCounterHiRes() < deadline);
    EXPECT_TRUE(ordinary.hasKeyboardFocus(false));
    EXPECT_TRUE(ordinary.hasFocusOutline());
    EXPECT_NE(lookAndFeel.createFocusOutlineForComponent(ordinary), nullptr);
    root.setLookAndFeel(nullptr);
}
}  // namespace esgrove::image_control::test
