/// Tests compact BandStrip geometry and the production sliders' pointer, wheel, key, and native numeric input.
/// The same parameter attachments must preserve local history, Link, automation exclusion, and balanced host gestures.

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

#include <array>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Observes one parameter's host gestures and always detaches before its processor is destroyed.
class CompactGestureCounter final : private juce::AudioProcessorParameter::Listener
{
public:
    /// Observes a processor-owned parameter that outlives this counter.
    explicit CompactGestureCounter(juce::RangedAudioParameter& observed) : parameter(observed)
    {
        parameter.addListener(this);
    }

    /// Removes the observer even when an assertion returns from the test early.
    ~CompactGestureCounter() override
    {
        parameter.removeListener(this);
    }

    CompactGestureCounter(const CompactGestureCounter&) = delete;
    CompactGestureCounter(CompactGestureCounter&&) = delete;
    CompactGestureCounter& operator=(const CompactGestureCounter&) = delete;
    CompactGestureCounter& operator=(CompactGestureCounter&&) = delete;

    /// Returns host begin and end notifications in delivery order.
    [[nodiscard]] const std::vector<bool>& gestureBoundaries() const
    {
        return boundaries;
    }

private:
    /// Value expectations are checked independently against the processor's plain parameter values.
    void parameterValueChanged(int /*index*/, float /*value*/) override {}

    /// Records each host gesture boundary in delivery order.
    void parameterGestureChanged(int /*index*/, const bool beginning) override
    {
        boundaries.push_back(beginning);
    }

    /// Processor-owned parameter that remains valid until this observer detaches.
    juce::RangedAudioParameter& parameter;
    /// Ordered host begin and end notifications.
    mutable std::vector<bool> boundaries;
};

/// Finds a direct child by full accessible title, independent of child ordering.
/// The returned observer can be null and is valid only while the parent owns the component.
template<typename Control>
Control* compactControl(const juce::Component& parent, const juce::String& title)
{
    for (auto* child : parent.getChildren()) {
        if (child->getTitle() == title) {
            return dynamic_cast<Control*>(child);
        }
    }
    return nullptr;
}

/// Finds the slider's JUCE-owned numeric label, valid until its presentation or look and feel changes.
juce::Label* compactValueBox(const ControlSlider& slider)
{
    for (auto* child : slider.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child)) {
            return label;
        }
    }
    return nullptr;
}

/// Delivers native-editor Return messages and queued parameter synchronization with a bounded native pump.
void dispatchCompactChanges()
{
#if JUCE_MAC
    for (int pass = 0; pass < 8; ++pass) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, static_cast<Boolean>(true));
    }
#elif JUCE_WINDOWS
    MSG message {};
    for (int pass = 0; pass < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0; ++pass) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#endif
}

/// Creates production-handler pointer input with stable local pointer-down coordinates.
juce::MouseEvent compactMouse(
    juce::Component& control,
    const juce::Point<float> position = {12.0F, 12.0F},
    const juce::Point<float> down = {12.0F, 12.0F},
    const juce::ModifierKeys modifiers = juce::ModifierKeys::leftButtonModifier
)
{
    const auto now = juce::Time::getCurrentTime();
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        position,
        modifiers,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        &control,
        &control,
        now,
        down,
        now,
        1,
        position != down,
    };
}

/// Produces a single positive mouse-wheel notch regardless of platform wheel calibration.
juce::MouseWheelDetails compactNotch()
{
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = 60.0F / 256.0F;
    return wheel;
}

/// Opens the real JUCE text editor with Return and commits through its own Return-key handling.
bool enterCompactValue(ControlSlider& slider, const juce::String& text)
{
    if (!slider.keyPressed(juce::KeyPress(juce::KeyPress::returnKey))) {
        return false;
    }
    const auto* label = compactValueBox(slider);
    if (label == nullptr || label->getCurrentTextEditor() == nullptr) {
        return false;
    }
    auto* editor = label->getCurrentTextEditor();
    editor->setText(text, false);
    // Return posts a message even when the editor leaves the key available to its parent.
    editor->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    do {
        dispatchCompactChanges();
        if (!label->isBeingEdited()) {
            return true;
        }
    } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    slider.hideTextBox(true);
    return false;
}

/// Measures representative digit widths and endpoints without walking thousands of equivalent numeric strings.
void expectCompactBoundaryValuesFit(ControlSlider& slider)
{
    auto* label = compactValueBox(slider);
    ASSERT_NE(label, nullptr);
    const auto font = label->getLookAndFeel().getLabelFont(*label);
    std::array values {-60.0, -59.99, -59.95, -11.11, -10.0, -9.99, -0.01, 0.0, 9.99, 11.11, 12.0};
    if (slider.getName() == "Width") {
        values = {0.0, 0.01, 0.09, 0.10, 0.99, 1.0, 1.01, 1.11, 1.88, 1.99, 2.0};
    } else if (slider.getName() == "Pan") {
        values = {-100.0, -99.9, -88.0, -33.0, -25.0, 0.0, 1.0, 25.0, 33.0, 99.9, 100.0};
    }
    for (const double value : values) {
        slider.setValue(value, juce::dontSendNotification);
        const auto textWidth = juce::GlyphArrangement::getStringWidth(font, slider.compactValueText());
        EXPECT_LE(textWidth, static_cast<float>(slider.getWidth() - 4));
    }
    EXPECT_EQ(slider.getComponentAt(slider.getLocalBounds().getCentre()), &slider);
}

/// Returns exact-entry text and expected plain values for each compact control without duplicating its parser.
std::pair<juce::String, double> compactEntryExample(const ControlSlider& slider)
{
    if (slider.getName() == "Width") {
        return {"1.37", 1.37};
    }
    if (slider.getName() == "Pan") {
        return {"25L", -25.0};
    }
    return {"-3.27 dB", -3.27};
}

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

/// Reads the processor's current plain value rather than relying on a slider's cached representation.
float compactParameterValue(ImageControlProcessor& processor, const juce::String& parameterId)
{
    return plainValue(*processor.getParameters().getParameter(parameterId));
}
}  // namespace

/// Both compact heights fit four nonoverlapping fields, all band names, and representative worst-case glyph widths.
TEST(CompactBands, MinimumWidthGeometryAndBoundaryValuesRemainLegible)
{
    ImageControlProcessor processor;
    for (std::size_t band = 0; band < BAND_COUNT; ++band) {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), band, processor.getHistory());
        strip.setCompact(true);
        for (const auto size : {juce::Point<int> {180, 44}, {181, 44}, {240, 44}, {400, 44}, {180, 60}, {240, 60}}) {
            SCOPED_TRACE(band);
            SCOPED_TRACE(size.toString().toStdString());
            strip.setSize(size.x, size.y);
            const auto title = bandName(band);
            auto* mid = compactControl<ControlSlider>(strip, title + " Mid");
            ASSERT_NE(mid, nullptr);
            int previousRight = 0;
            for (const auto* name : {"Mid", "Side", "Width", "Pan"}) {
                auto* slider = compactControl<ControlSlider>(strip, title + " " + name);
                ASSERT_NE(slider, nullptr);
                EXPECT_TRUE(slider->isVisible());
                EXPECT_EQ(slider->getY(), mid->getY());
                EXPECT_EQ(slider->getHeight(), 24);
                EXPECT_GT(slider->getX(), previousRight);
                EXPECT_GE(slider->getWidth(), slider->getName() == "Pan" ? 30 : 40);
                previousRight = slider->getRight();
                expectCompactBoundaryValuesFit(*slider);
            }
            bool foundName = false;
            for (auto* child : strip.getChildren()) {
                if (const auto* label = dynamic_cast<juce::Label*>(child);
                    label != nullptr && label->getText() == title)
                {
                    foundName = true;
                    EXPECT_TRUE(label->isVisible());
                    EXPECT_LE(label->getBottom(), mid->getY());
                    EXPECT_GE(label->getWidth(), 150);
                    EXPECT_EQ(strip.getWidth() - label->getRight(), 26);
                }
                if (child->isVisible()) {
                    EXPECT_TRUE(strip.getLocalBounds().contains(child->getBounds()));
                }
            }
            EXPECT_TRUE(foundName);
        }
    }
}

/// Suffixes identify short compact values, while roomy labels identify the same unchanged numeric controls.
TEST(CompactBands, SuffixesAndRoomyLabelsIdentifyAllFourValues)
{
    ImageControlProcessor processor;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    const std::array examples {
        std::pair {"Low Mid", "+0.5M"},
        std::pair {"Low Side", "-1.2S"},
        std::pair {"Low Width", "1.00W"},
        std::pair {"Low Pan", "C"},
    };
    for (const auto& [title, expected] : examples) {
        auto* slider = compactControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        if (slider->getName() == "Mid") {
            slider->setValue(0.5, juce::dontSendNotification);
        } else if (slider->getName() == "Side") {
            slider->setValue(-1.2, juce::dontSendNotification);
        }
        EXPECT_EQ(slider->compactValueText(), expected);
    }
    strip.setSize(180, BAND_STRIP_COMPACT_LABELLED_HEIGHT);
    for (const auto* name : {"Mid", "Side", "Width", "Pan"}) {
        auto* slider = compactControl<ControlSlider>(strip, "Low " + juce::String(name));
        ASSERT_NE(slider, nullptr);
        bool foundLabel = false;
        for (auto* child : strip.getChildren()) {
            if (auto* label = dynamic_cast<juce::Label*>(child);
                label != nullptr && label->getText() == juce::String(name).toUpperCase())
            {
                foundLabel = true;
                EXPECT_TRUE(label->isVisible());
                EXPECT_EQ(label->getX(), slider->getX());
                EXPECT_EQ(label->getWidth(), slider->getWidth());
                EXPECT_LE(label->getBottom(), slider->getY());
                const auto font = label->getLookAndFeel().getLabelFont(*label);
                EXPECT_LE(
                    juce::GlyphArrangement::getStringWidth(font, label->getText()),
                    static_cast<float>(label->getWidth())
                );
            }
        }
        EXPECT_TRUE(foundLabel);
        EXPECT_FALSE(slider->compactValueText().endsWith(slider->getName().substring(0, 1)));
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Pan keeps directional compact text and the unchanged parameter parser for numeric entry and malformed tokens.
TEST(CompactBands, PanDirectionsAndNumericEntryUseExistingParameterParser)
{
    ImageControlProcessor processor;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
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

/// Pan edits and automation never invoke gain-only Link or add host-originated changes to local history.
TEST(CompactBands, PanHistoryAndAutomationRemainIndependentOfLinkedGains)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
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

/// Resizing between compact heights changes only labels and suffixes, not active editing or parameter gestures.
TEST(CompactBands, RoomyResizePreservesNumericEditorAndActivePanGesture)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
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
    strip.setSize(180, BAND_STRIP_COMPACT_LABELLED_HEIGHT);
    EXPECT_EQ(compactValueBox(*pan), label);
    EXPECT_EQ(label->getCurrentTextEditor(), editor.getComponent());
    EXPECT_EQ(editor->getText(), "33R");
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
    strip.setSize(180, BAND_STRIP_COMPACT_LABELLED_HEIGHT);
    EXPECT_TRUE(history.isEditing());
    EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true}));
    pan->endDrag();
    EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true, false}));
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(pan->getValue(), 0.0, 0.0001);
}

/// A mode round trip preserves all existing controls, identities, full units, tooltips, and expanded geometry.
TEST(CompactBands, FullLayoutRestoresWithoutReplacingControlsOrAttachments)
{
    ImageControlProcessor processor;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setRangeText("20 Hz - 120 Hz");
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    std::vector<std::pair<juce::Component::SafePointer<juce::Component>, juce::Rectangle<int>>> bounds;
    for (auto* child : strip.getChildren()) {
        bounds.emplace_back(child, child->getBounds());
    }
    auto* mid = compactControl<ControlSlider>(strip, "Low Mid");
    auto* side = compactControl<ControlSlider>(strip, "Low Side");
    auto* width = compactControl<ControlSlider>(strip, "Low Width");
    ASSERT_NE(mid, nullptr);
    ASSERT_NE(side, nullptr);
    ASSERT_NE(width, nullptr);
    const auto tooltip = mid->getTooltip();
    const auto componentId = mid->getComponentID();
    strip.setCompact(true);
    strip.setSize(180, BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_EQ(mid, compactControl<ControlSlider>(strip, "Low Mid"));
    EXPECT_EQ(mid->getComponentID(), componentId);
    EXPECT_EQ(mid->compactValueText(), "+0.0M");
    EXPECT_EQ(side->compactValueText(), "+0.0S");
    EXPECT_EQ(width->compactValueText(), "1.00W");
    EXPECT_TRUE(mid->getTooltip().contains("Return"));
    EXPECT_TRUE(mid->getTooltip().contains("dB"));
    EXPECT_TRUE(width->getTooltip().contains("multiplier"));
    mid->setValue(static_cast<double>(BAND_GAIN_MIN_DB), juce::dontSendNotification);
    EXPECT_EQ(mid->compactValueText(), "-infM");
    EXPECT_EQ(mid->getTextFromValue(mid->getValue()), "-inf dB");
    for (const auto* title : {"Low Recover", "Low Solo", "Low Link"}) {
        auto* control = compactControl<juce::Component>(strip, title);
        ASSERT_NE(control, nullptr);
        EXPECT_FALSE(control->isVisible());
    }
    strip.setCompact(false);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    for (const auto& [child, originalBounds] : bounds) {
        ASSERT_NE(child, nullptr);
        EXPECT_TRUE(child->isVisible());
        EXPECT_EQ(child->getBounds(), originalBounds);
    }
    EXPECT_EQ(mid->getTooltip(), tooltip);
    EXPECT_EQ(mid->getTextBoxWidth(), 78);
    EXPECT_EQ(mid->getTextBoxHeight(), 22);
    EXPECT_EQ(mid->getTextBoxPosition(), juce::Slider::TextBoxRight);
    EXPECT_EQ(mid->getTextFromValue(mid->getValue()), "-inf dB");
    EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_GT(compactParameterValue(processor, midGainParameterId(0)), BAND_GAIN_MIN_DB);
}

/// Coarse and fine wheel and key actions reach the slider rather than an overlaid editable label.
TEST(CompactBands, WheelAndArrowsKeepFineStepsAndHostShortcutRouting)
{
    ImageControlProcessor processor;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
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
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
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

/// Return opens the existing JUCE editor, preserving hundredth-step entry, gain Off, and malformed-token rejection.
TEST(CompactBands, ReturnNumericEntryUsesFullParameterParserAndPreservesMalformedValues)
{
    ImageControlProcessor processor;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
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
            ASSERT_TRUE(enterCompactValue(*slider, "-inf dB"));
            EXPECT_DOUBLE_EQ(slider->getValue(), BAND_GAIN_MIN_DB);
        }
        EXPECT_FALSE(processor.getHistory().isEditing());
    }
}

/// Every compact Mid input shares balanced Mid/Side gestures and exactly one undo command when Link is enabled.
TEST(CompactBands, LinkedInputPathsShareOneHistoryCommandAndBalancedHostGestures)
{
    for (int input = 0; input < 5; ++input) {
        SCOPED_TRACE(input);
        ImageControlProcessor processor;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        auto& midParameter = *processor.getParameters().getParameter(midGainParameterId(0));
        auto& sideParameter = *processor.getParameters().getParameter(sideGainParameterId(0));
        setParameterInGesture(midParameter, -6.0F);
        setParameterInGesture(sideParameter, -6.0F);
        const CompactGestureCounter midGestures(midParameter);
        const CompactGestureCounter sideGestures(sideParameter);
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
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
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
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
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
        const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
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

/// Hiding a native numeric editor commits valid contents once and preserves the old value for malformed contents.
TEST(CompactBands, ModeSwitchFinishesNumericEntrySafely)
{
    for (const bool initiallyCompact : {false, true}) {
        for (const bool valid : {false, true}) {
            ImageControlProcessor processor;
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
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
            label->getCurrentTextEditor()->setText(valid ? "-3.27 dB" : "broken", false);
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
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        const CompactGestureCounter midGestures(*processor.getParameters().getParameter(midGainParameterId(0)));
        const CompactGestureCounter sideGestures(*processor.getParameters().getParameter(sideGainParameterId(0)));
        {
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
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

/// Hidden controls hand focus to Mid, while retained Mid/Side/Width/Pan keep focus across both presentations.
TEST(CompactBands, FocusMovesFromHiddenControlsAndRetainedValuesKeepFocus)
{
    ImageControlProcessor processor;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
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
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
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
}  // namespace esgrove::image_control::test
