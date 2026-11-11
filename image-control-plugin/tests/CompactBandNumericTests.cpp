/// Tests native numeric editor presentation, parsing, focus, and parameter history in compact and expanded controls.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "tests/CompactBandTestSupport.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/HistoryAttachments.h"
#include "ui/ImageControlLookAndFeel.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Clicks through the real window peer so hit testing, focus, label release, and slider listeners all participate.
bool clickCompactComponent(juce::Component& root, juce::Component& target)
{
    auto* peer = root.getPeer();
    if (peer == nullptr || !target.isShowing()) {
        return false;
    }
    const auto position = root.getLocalPoint(&target, target.getLocalBounds().getCentre()).toFloat();
    peer->grabFocus();
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 1000.0;
    while (!peer->contains(position.roundToInt(), true) && juce::Time::getMillisecondCounterHiRes() < deadline) {
        dispatchCompactChanges();
    }
    if (root.getComponentAt(position) != &target || !peer->contains(position.roundToInt(), true)) {
        return false;
    }
    const juce::ScopedValueSetter<juce::ModifierKeys> restoreModifiers(juce::ModifierKeys::currentModifiers, {});
    auto eventTime = juce::Time::currentTimeMillis();
    for (const juce::ModifierKeys modifiers :
         {juce::ModifierKeys {}, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), juce::ModifierKeys {}})
    {
        juce::ModifierKeys::currentModifiers = modifiers;
        peer->handleMouseEvent(
            juce::MouseInputSource::InputSourceType::mouse,
            position,
            modifiers,
            juce::MouseInputSource::defaultPressure,
            juce::MouseInputSource::defaultOrientation,
            eventTime
        );
        eventTime += 20;
    }
    return true;
}
}  // namespace

/// Pan keeps directional compact text and the unchanged parameter parser for numeric entry and malformed tokens.
TEST(CompactBands, PanDirectionsAndNumericEntryUseExistingParameterParser)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* pan = compactControl<ControlSlider>(strip, "Low Pan");
    ASSERT_NE(pan, nullptr);
    for (const auto& [text, value] :
         std::array {
             std::pair {"C", 0.0},
             std::pair {"25L", -25.0},
             std::pair {"33R", 33.0},
             std::pair {"100L", -100.0},
             std::pair {"100R", 100.0},
         })
    {
        SCOPED_TRACE(text);
        ASSERT_TRUE(enterCompactValue(*pan, text));
        EXPECT_NEAR(pan->getValue(), value, 0.0001);
        EXPECT_EQ(pan->compactValueText(), text);
    }
    ASSERT_TRUE(enterCompactValue(*pan, "-25 %"));
    EXPECT_EQ(pan->compactValueText(), "25L");
    for (const auto* malformed : {"25LR", "33RR", "25%L", "nan", "inf", "1e9999", "25 percent"}) {
        ASSERT_TRUE(enterCompactValue(*pan, malformed));
        EXPECT_NEAR(pan->getValue(), -25.0, 0.0001);
    }
    EXPECT_TRUE(pan->getTooltip().contains("percent pan"));
}

/// Return opens the existing JUCE editor, preserving hundredth-step entry, gain Off, and malformed-token rejection.
TEST(CompactBands, ReturnNumericEntryUsesFullParameterParserAndPreservesMalformedValues)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan"}) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        const bool gain = slider->getName() == "Mid" || slider->getName() == "Side";
        const auto [entry, expected] = compactEntryExample(*slider);
        ASSERT_TRUE(enterCompactValue(*slider, entry));
        EXPECT_NEAR(slider->getValue(), expected, 0.0001);
        const double accepted = slider->getValue();
        for (const auto* invalid : {"", "junk", "nan", "inf", "1e9999", "++2", "1.2junk", "1 dB dB", "1 2"}) {
            SCOPED_TRACE(invalid);
            ASSERT_TRUE(enterCompactValue(*slider, invalid));
            EXPECT_DOUBLE_EQ(slider->getValue(), accepted);
        }
        if (gain) {
            ASSERT_TRUE(enterCompactValue(*slider, "-inf"));
            EXPECT_DOUBLE_EQ(slider->getValue(), BAND_GAIN_MIN_DB);
        }
        EXPECT_FALSE(processor.getHistory().isEditing());
    }
}

/// Hiding a native numeric editor commits valid contents once and preserves the old value for malformed contents.
TEST(CompactBands, ModeSwitchFinishesNumericEntrySafely)
{
    for (const bool initiallyCompact : {false, true}) {
        for (const bool valid : {false, true}) {
            ImageControlProcessor processor;
            ImageControlLookAndFeel lookAndFeel;
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
            strip.setLookAndFeel(&lookAndFeel);
            strip.setCompact(initiallyCompact);
            strip.setSize(240, initiallyCompact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
            strip.addToDesktop(0);
            strip.setVisible(true);
            auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
            ASSERT_NE(mid, nullptr);
            ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            auto* label = compactValueBox(*mid);
            ASSERT_NE(label, nullptr);
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            label->getCurrentTextEditor()->setText(valid ? "-3.27" : "broken", false);
            strip.setCompact(!initiallyCompact);
            EXPECT_NEAR(mid->getValue(), valid ? -3.27 : 0.0, 0.0001);
            EXPECT_FALSE(processor.getHistory().isEditing());
            auto* restoredLabel = compactValueBox(*mid);
            ASSERT_NE(restoredLabel, nullptr);
            EXPECT_FALSE(restoredLabel->isBeingEdited());
            EXPECT_EQ(processor.getHistory().canUndo(), valid);
        }
    }
}

/// Teardown discards uncommitted text while attachments still exist and releases editor focus before slider teardown.
TEST(CompactBands, TeardownWithFocusedNumericEditorDoesNotCommitOrLeakGestures)
{
    for (const bool compact : {false, true}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
        const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
        {
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
            strip.setLookAndFeel(&lookAndFeel);
            strip.setCompact(compact);
            strip.setSize(240, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
            strip.addToDesktop(0);
            strip.setVisible(true);
            auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
            ASSERT_NE(mid, nullptr);
            ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            const auto* label = compactValueBox(*mid);
            ASSERT_NE(label, nullptr);
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            label->getCurrentTextEditor()->setText("-3.27 dB", false);
        }
        EXPECT_NEAR(compactParameterValue(processor, midGainParameterId(0)), 0.0F, 0.0001F);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), 0.0F, 0.0001F);
        EXPECT_FALSE(history.isEditing());
        EXPECT_FALSE(history.canUndo());
        EXPECT_TRUE(midGestures.gestureBoundaries().empty());
        EXPECT_TRUE(sideGestures.gestureBoundaries().empty());
    }
}

/// Real expanded and compact fields keep their value font, text origin, and fixed suffix outside selection.
TEST(CompactBands, NumericEditorKeepsFieldPresentationAndNoneditableSuffixes)
{
    for (const bool compact : {false, true}) {
        ImageControlProcessor processor;
        ImageControlLookAndFeel lookAndFeel;
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setLookAndFeel(&lookAndFeel);
        strip.setCompact(compact);
        strip.setSize(240, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
        strip.addToDesktop(0);
        strip.setVisible(true);
        for (const auto* title : {"Low Mid", "Low Side", "Low Width", "Low Pan", "Low Recover"}) {
            auto* slider = compactControl<ControlSlider>(strip, title);
            ASSERT_NE(slider, nullptr);
            if (!slider->isVisible()) {
                continue;
            }
            auto* label = compactValueBox(*slider);
            ASSERT_NE(label, nullptr);
            if (slider->getName() == "Recover") {
                setParameterInGesture(*processor.getParameters().getParameter(recoverParameterId(0)), 25.0F);
                EXPECT_EQ(slider->numericValueSuffix(), "%");
            }
            const auto bounds = label->getBounds();
            const auto font = label->getLookAndFeel().getLabelFont(*label);
            const auto number = slider->numericValueText();
            const auto suffix = slider->numericValueSuffix();
            ASSERT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            auto* editor = label->getCurrentTextEditor();
            ASSERT_NE(editor, nullptr);
            EXPECT_EQ(editor->getText(), number);
            EXPECT_EQ(editor->getHighlightedText(), number);
            EXPECT_EQ(editor->getFont(), font);
            EXPECT_EQ(editor->getJustificationType(), juce::Justification::centredLeft);
            EXPECT_EQ(editor->getBorder(), juce::BorderSize<int>());
            EXPECT_EQ(editor->getLeftIndent(), 0);
            EXPECT_EQ(editor->getTopIndent(), 0);
            EXPECT_TRUE(editor->findColour(juce::TextEditor::backgroundColourId).isTransparent());
            EXPECT_TRUE(editor->findColour(juce::TextEditor::focusedOutlineColourId).isTransparent());
            EXPECT_EQ(
                editor->findColour(juce::TextEditor::textColourId),
                slider->findColour(juce::Slider::textBoxTextColourId)
            );
            EXPECT_EQ(label->getBounds(), bounds);
            const auto area = label->getBorderSize().subtractedFrom(label->getLocalBounds());
            const int suffixWidth = static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(font, suffix)));
            const int numberWidth
                = std::max(1, static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(font, number))));
            EXPECT_EQ(editor->getX(), area.getX() + (area.getWidth() - numberWidth - suffixWidth) / 2);
            EXPECT_LE(editor->getRight() + suffixWidth, area.getRight());
            const auto editorBounds = editor->getBounds();
            editor->selectAll();
            EXPECT_EQ(editor->getHighlightedText(), number);
            slider->hideTextBox(true);
            ASSERT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            EXPECT_EQ(label->getCurrentTextEditor()->getBounds(), editorBounds);
            slider->hideTextBox(true);
        }
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
}

/// Production construction and a compact/full round trip keep Recover's click-opened editor numeric and nudgeable.
TEST(CompactBands, EditorRecoveryLabelClickKeepsPercentFixedAndArrowGesturesIndependent)
{
    for (const bool initiallyCompact : {false, true}) {
        SCOPED_TRACE(initiallyCompact);
        ImageControlProcessor processor;
        auto& state = processor.getEditorState();
        state.setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
        state.setProperty(COMPACT_BAND_CONTROLS_PROPERTY, initiallyCompact, nullptr);
        auto& parameter = *processor.getParameters().getParameter(recoverParameterId(0));
        setParameterInGesture(parameter, 25.0F);
        const CompactGestureCounter recoveryGestures(parameter);
        const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
        const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
        ImageControlEditor editor(processor);
        editor.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        editor.setAlwaysOnTop(true);
        editor.setVisible(true);
        editor.setTopLeftPosition(80, 100);
        editor.toFront(true);
        dispatchCompactChanges();
        if (initiallyCompact) {
            juce::Button* disclosure = nullptr;
            for (auto* child : editor.getChildren()) {
                if (child->getComponentID() == "band-controls-disclosure") {
                    disclosure = dynamic_cast<juce::Button*>(child);
                }
            }
            ASSERT_NE(disclosure, nullptr);
            disclosure->onClick();
        }
        BandStrip* strip = nullptr;
        for (auto* child : editor.getChildren()) {
            if (auto* candidate = dynamic_cast<BandStrip*>(child);
                candidate != nullptr && compactControl<ControlSlider>(*candidate, "Low Recover") != nullptr)
            {
                strip = candidate;
            }
        }
        ASSERT_NE(strip, nullptr);
        auto* slider = compactControl<ControlSlider>(*strip, "Low Recover");
        ASSERT_NE(slider, nullptr);
        auto* label = compactValueBox(*slider);
        ASSERT_NE(label, nullptr);
        ASSERT_TRUE(slider->isShowing());
        ASSERT_TRUE(label->isEditableOnSingleClick());
        ASSERT_TRUE(clickCompactComponent(editor, *label));
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), "25");
        EXPECT_EQ(label->getCurrentTextEditor()->getHighlightedText(), "25");
        EXPECT_EQ(slider->numericValueSuffix(), "%");
        auto* peer = editor.getPeer();
        ASSERT_NE(peer, nullptr);
        EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::upKey)));
        EXPECT_NEAR(slider->getValue(), 27.0, 0.0001);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), "27");
        EXPECT_TRUE(
            peer->handleKeyPress(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0))
        );
        EXPECT_DOUBLE_EQ(slider->getValue(), 26.0);
        EXPECT_FLOAT_EQ(plainValue(parameter), 26.0F);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), "26");
        EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::returnKey)));
        EXPECT_TRUE(waitForCompactEditorClose(*label));
        EXPECT_EQ(recoveryGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_TRUE(midGestures.gestureBoundaries().empty());
        EXPECT_TRUE(sideGestures.gestureBoundaries().empty());
        EXPECT_FALSE(processor.getHistory().isEditing());
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(slider->getValue(), 27.0, 0.0001);
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(slider->getValue(), 25.0, 0.0001);
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
}

/// Long valid drafts scroll within the numeric region so their trailing precision remains reachable.
TEST(CompactBands, NumericEditorKeepsLongDraftCaretVisibleWithoutCoveringUnit)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(mid, nullptr);
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    auto* label = compactValueBox(*mid);
    ASSERT_NE(label, nullptr);
    auto* editor = label->getCurrentTextEditor();
    ASSERT_NE(editor, nullptr);
    editor->selectAll();
    editor->insertTextAtCaret("-12.34567890123456789");
    dispatchCompactChanges();
    const auto caret = editor->getCaretRectangleForCharIndex(editor->getCaretPosition());
    EXPECT_GE(caret.getX(), 0);
    EXPECT_LE(caret.getX(), editor->getWidth());
    EXPECT_LT(editor->getWidth(), editor->getTextWidth());
    EXPECT_LT(editor->getRight(), label->getWidth());
    mid->hideTextBox(true);
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Unitless frequency edits use the displayed scale while explicit units retain the strict host parser contract.
TEST(CompactBands, FrequencyEditorKeepsHertzAndKilohertzFixedWithoutChangingHostParser)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    ControlSlider slider;
    slider.setLookAndFeel(&lookAndFeel);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 100, 24);
    const HistorySliderAttachment attachment(
        processor.getParameters(), crossoverParameterId(1), slider, processor.getHistory()
    );
    slider.setSize(240, 24);
    slider.addToDesktop(0);
    slider.setVisible(true);
    auto& parameter = *processor.getParameters().getParameter(crossoverParameterId(1));
    for (const auto& [value, number, suffix, entry, expected] :
         std::array {
             std::tuple {800.0F, "800", " Hz", "850", 850.0F},
             std::tuple {1500.0F, "1.50", " kHz", "1.75", 1750.0F},
             std::tuple {1500.0F, "1.50", " kHz", "900 Hz", 900.0F},
         })
    {
        setParameterInGesture(parameter, value);
        ASSERT_TRUE(slider.keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        auto* label = compactValueBox(slider);
        ASSERT_NE(label, nullptr);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), number);
        EXPECT_EQ(slider.numericValueSuffix(), suffix);
        slider.hideTextBox(true);
        ASSERT_TRUE(enterCompactValue(slider, entry));
        EXPECT_NEAR(slider.getValue(), expected, 0.01);
    }
    EXPECT_NEAR(slider.getValueFromText("1.75"), MIN_FREQUENCY, 0.01);
    EXPECT_NEAR(slider.getValueFromText("1.75 kHz"), 1750.0, 0.01);
    const double accepted = slider.getValue();
    for (const auto* malformed : {"1 kHz kHz", "1 Hz Hz", "1.5junk", "", "nan", "inf"}) {
        ASSERT_TRUE(enterCompactValue(slider, malformed));
        EXPECT_DOUBLE_EQ(slider.getValue(), accepted);
    }
}

/// Untouched rounded readouts retain precision, and legacy recovery Off input displays the numeric zero percentage.
TEST(CompactBands, NumericEditorPreservesUntouchedPrecisionAndRecoveryOff)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    for (const auto* title : {"Low Mid", "Low Pan", "Low Recover"}) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        auto parameterId = midGainParameterId(0);
        if (slider->getName() == "Pan") {
            parameterId = panParameterId(0);
        } else if (slider->getName() == "Recover") {
            parameterId = recoverParameterId(0);
        }
        auto& parameter = *processor.getParameters().getParameter(parameterId);
        setParameterInGesture(parameter, slider->getName() == "Pan" ? -25.4F : 3.27F);
        const double original = slider->getValue();
        EXPECT_TRUE(enterCompactValue(*slider, slider->numericValueText()));
        EXPECT_DOUBLE_EQ(slider->getValue(), original);
        EXPECT_FALSE(processor.getHistory().canUndo());
        if (slider->getName() == "Recover") {
            EXPECT_TRUE(enterCompactValue(*slider, "Off"));
            EXPECT_NEAR(slider->getValue(), 0.0, 0.0001);
            EXPECT_EQ(slider->numericValueText(), "0");
            EXPECT_EQ(slider->numericValueSuffix(), "%");
            EXPECT_TRUE(processor.getHistory().undo());
            EXPECT_DOUBLE_EQ(slider->getValue(), original);
            EXPECT_DOUBLE_EQ(original, 3.0);
            EXPECT_TRUE(enterCompactValue(*slider, "37.5"));
            EXPECT_DOUBLE_EQ(slider->getValue(), 38.0);
            EXPECT_FLOAT_EQ(plainValue(parameter), 38.0F);
            EXPECT_EQ(slider->numericValueText(), "38");
            EXPECT_TRUE(processor.getHistory().undo());
            EXPECT_DOUBLE_EQ(slider->getValue(), original);
            EXPECT_TRUE(processor.getHistory().redo());
            EXPECT_DOUBLE_EQ(slider->getValue(), 38.0);
            for (const auto* invalid : {"nan", "inf", "37%%", "37 trailing"}) {
                EXPECT_TRUE(enterCompactValue(*slider, invalid));
                EXPECT_DOUBLE_EQ(slider->getValue(), 38.0);
                EXPECT_FLOAT_EQ(plainValue(parameter), 38.0F);
            }
            EXPECT_TRUE(enterCompactValue(*slider, "0.1"));
            EXPECT_DOUBLE_EQ(slider->getValue(), 0.0);
            EXPECT_EQ(slider->numericValueText(), "0");
            EXPECT_TRUE(processor.getHistory().undo());
            EXPECT_DOUBLE_EQ(slider->getValue(), 38.0);
            EXPECT_TRUE(processor.getHistory().undo());
            EXPECT_DOUBLE_EQ(slider->getValue(), original);
            EXPECT_FALSE(processor.getHistory().canUndo());
        }
    }
}

/// Vertical arrows in the real numeric editor retain coarse/fine conventions, balanced Link gestures, and undo.
TEST(CompactBands, NumericEditorArrowsReuseSliderNudgesAndLinkedHistory)
{
    for (const bool compact : {false, true}) {
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
        strip.setCompact(compact);
        strip.setSize(240, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
        strip.addToDesktop(0);
        strip.setVisible(true);
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        auto* label = compactValueBox(*mid);
        ASSERT_NE(label, nullptr);
        for (const int modifier :
             {juce::ModifierKeys::commandModifier, juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::altModifier})
        {
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            EXPECT_FALSE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::upKey, modifier, 0)));
            EXPECT_NEAR(mid->getValue(), -6.0, 0.0001);
        }
        ASSERT_TRUE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
        const double coarse = mid->getValue();
        EXPECT_NEAR(coarse, -5.28, 0.0001);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(label->getCurrentTextEditor()->getText(), mid->numericValueText());
        ASSERT_TRUE(label->getCurrentTextEditor()->keyPressed(
            juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0)
        ));
        EXPECT_NEAR(mid->getValue(), coarse - 0.07, 0.0001);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), mid->getValue(), 0.0001);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        const double finalValue = mid->getValue();
        EXPECT_TRUE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        EXPECT_TRUE(waitForCompactEditorClose(*label));
        EXPECT_DOUBLE_EQ(mid->getValue(), finalValue);
        EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(mid->getValue(), coarse, 0.0001);
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(mid->getValue(), -6.0, 0.0001);
        EXPECT_NEAR(compactParameterValue(processor, sideGainParameterId(0)), -6.0F, 0.0001F);
        EXPECT_FALSE(history.canUndo());
    }
}

/// Wheel input delivered through the native peer keeps numeric focus, slider steps, Link, and one undo per notch.
TEST(CompactBands, FocusedNumericEditorWheelUsesSliderStepsAndLinkedHistory)
{
    for (const bool compact : {false, true}) {
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
        strip.setCompact(compact);
        strip.setSize(240, compact ? BAND_STRIP_COMPACT_HEIGHT : BAND_STRIP_MIN_HEIGHT);
        strip.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        strip.setAlwaysOnTop(true);
        strip.setTopLeftPosition(80, 100);
        strip.setVisible(true);
        strip.toFront(true);
        auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        auto* label = compactValueBox(*mid);
        auto* peer = strip.getPeer();
        ASSERT_NE(label, nullptr);
        ASSERT_NE(peer, nullptr);
        const juce::ScopedValueSetter<juce::ModifierKeys> restoreModifiers(juce::ModifierKeys::currentModifiers, {});
        for (const bool fine : {false, true}) {
            auto* editor = label->getCurrentTextEditor();
            ASSERT_NE(editor, nullptr);
            EXPECT_TRUE(editor->hasKeyboardFocus(true));
            const auto position = strip.getLocalPoint(editor, editor->getLocalBounds().getCentre()).toFloat();
            const auto modifiers = juce::ModifierKeys(fine ? juce::ModifierKeys::shiftModifier : 0);
            juce::ModifierKeys::currentModifiers = modifiers;
            peer->handleMouseEvent(
                juce::MouseInputSource::InputSourceType::mouse,
                position,
                modifiers,
                juce::MouseInputSource::defaultPressure,
                juce::MouseInputSource::defaultOrientation,
                juce::Time::currentTimeMillis()
            );
            auto wheel = compactNotch();
            if (fine) {
                wheel.deltaY = -wheel.deltaY;
            }
            peer->handleMouseWheel(
                juce::MouseInputSource::InputSourceType::mouse, position, juce::Time::currentTimeMillis(), wheel
            );
            EXPECT_NEAR(mid->getValue(), fine ? -5.91 : -5.9, 0.0001);
            EXPECT_NEAR(sideParameter.convertFrom0to1(sideParameter.getValue()), mid->getValue(), 0.0001);
            ASSERT_NE(label->getCurrentTextEditor(), nullptr);
            EXPECT_TRUE(label->getCurrentTextEditor()->hasKeyboardFocus(true));
            EXPECT_EQ(label->getCurrentTextEditor()->getText(), mid->numericValueText());
        }
        mid->hideTextBox(true);
        EXPECT_EQ(midGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_EQ(sideGestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false}));
        EXPECT_FALSE(history.isEditing());
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(mid->getValue(), -5.9, 0.0001);
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(mid->getValue(), -6.0, 0.0001);
        EXPECT_FALSE(history.canUndo());
    }
}

/// Smooth substeps and clamped wheel events retain the same native editor and uncommitted draft.
TEST(CompactBands, NumericEditorWheelPreservesDraftForSubstepsAndClamps)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& parameter = *processor.getParameters().getParameter(recoverParameterId(0));
    const CompactGestureCounter gestures(parameter);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setLookAndFeel(&lookAndFeel);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* recovery = compactControl<ControlSlider>(strip, "Low Recover");
    ASSERT_NE(recovery, nullptr);
    ASSERT_TRUE(recovery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    auto* label = compactValueBox(*recovery);
    ASSERT_NE(label, nullptr);
    auto* editor = label->getCurrentTextEditor();
    ASSERT_NE(editor, nullptr);
    editor->setText("123", false);
    auto wheel = compactNotch();
    wheel.isSmooth = true;
    wheel.deltaY = 0.02F;
    for (int step = 0; step < 2; ++step) {
        editor->mouseWheelMove(compactMouse(*editor, {}, {}, {}), wheel);
        EXPECT_EQ(label->getCurrentTextEditor(), editor);
        EXPECT_EQ(editor->getText(), "123");
        EXPECT_DOUBLE_EQ(recovery->getValue(), 0.0);
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
    editor->mouseWheelMove(compactMouse(*editor, {}, {}, juce::ModifierKeys::shiftModifier), wheel);
    EXPECT_DOUBLE_EQ(recovery->getValue(), 1.0);
    ASSERT_NE(label->getCurrentTextEditor(), nullptr);
    EXPECT_EQ(label->getCurrentTextEditor()->getText(), "1");
    EXPECT_EQ(recovery->numericValueSuffix(), "%");
    recovery->hideTextBox(true);
    EXPECT_TRUE(processor.getHistory().undo());
    ASSERT_TRUE(recovery->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    editor = label->getCurrentTextEditor();
    ASSERT_NE(editor, nullptr);
    editor->setText("123", false);
    wheel = compactNotch();
    wheel.deltaY = -wheel.deltaY;
    editor->mouseWheelMove(compactMouse(*editor, {}, {}, {}), wheel);
    EXPECT_EQ(label->getCurrentTextEditor(), editor);
    EXPECT_EQ(editor->getText(), "123");
    EXPECT_TRUE(processor.getHistory().canRedo());
    EXPECT_FALSE(processor.getHistory().canUndo());
    EXPECT_FALSE(processor.getHistory().isEditing());
    recovery->hideTextBox(true);
    EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false, true, false}));
}

/// Escape discards a draft, focus loss commits it, and automation closes stale text without becoming a local edit.
TEST(CompactBands, NumericEditorCancelFocusLossAndAutomationPreserveNativeLifecycle)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    auto& history = processor.getHistory();
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setLookAndFeel(&lookAndFeel);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    strip.setVisible(true);
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    auto* side = compactControl<ControlSlider>(strip, "Low Side");
    ASSERT_NE(mid, nullptr);
    ASSERT_NE(side, nullptr);
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    auto* label = compactValueBox(*mid);
    ASSERT_NE(label, nullptr);
    ASSERT_NE(label->getCurrentTextEditor(), nullptr);
    label->getCurrentTextEditor()->setText("-3.27", false);
    EXPECT_TRUE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    EXPECT_TRUE(waitForCompactEditorClose(*label));
    EXPECT_NEAR(mid->getValue(), 0.0, 0.0001);
    EXPECT_FALSE(history.canUndo());
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    ASSERT_NE(label->getCurrentTextEditor(), nullptr);
    label->getCurrentTextEditor()->setText("-3.27", false);
    side->grabKeyboardFocus();
    EXPECT_TRUE(waitForCompactEditorClose(*label));
    EXPECT_NEAR(mid->getValue(), -3.27, 0.0001);
    EXPECT_TRUE(history.undo());
    ASSERT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    ASSERT_NE(label->getCurrentTextEditor(), nullptr);
    label->getCurrentTextEditor()->setText("-4.5", false);
    auto& parameter = *processor.getParameters().getParameter(midGainParameterId(0));
    parameter.setValueNotifyingHost(parameter.convertTo0to1(-9.0F));
    EXPECT_TRUE(waitForCompactEditorClose(*label));
    EXPECT_NEAR(mid->getValue(), -9.0, 0.0001);
    EXPECT_FALSE(history.canUndo());
    EXPECT_TRUE(history.canRedo());
}
}  // namespace esgrove::image_control::test
