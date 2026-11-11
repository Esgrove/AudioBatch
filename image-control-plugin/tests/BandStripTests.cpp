/// Tests BandStrip's balanced linked gestures, independent Width and Recover attachments,
/// accessible parameter values, and minimum-height control layout.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "tests/ViewInteractionTestSupport.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include <initializer_list>

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace esgrove::image_control::test
{
TEST(BandStrip, LinkChangesDuringDragKeepTheOriginalGestureBalanced)
{
    ImageControlProcessor processor;
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(0));
    GestureCounter gestures;
    side.addListener(&gestures);
    {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* mid = dynamic_cast<ControlSlider*>(childWithTitle(strip, "Low Mid"));
        ASSERT_NE(mid, nullptr);
        const juce::Point<float> position {40.0F, 13.0F};
        mid->mouseDown(mouseEvent(*mid, position, position, juce::ModifierKeys::leftButtonModifier));
        processor.getEditorState().setProperty(linkPropertyName(0), false, nullptr);
        mid->setValue(3.0, juce::sendNotificationSync);
        EXPECT_NEAR(plainValue(side), 3.0F, 0.01F);
    }
    EXPECT_EQ(gestures.events, (std::vector<bool> {true, false}));
    side.removeListener(&gestures);
}

TEST(BandStrip, EnablingLinkDuringAnUnlinkedDragDoesNotOpenOrEndSideGesture)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(linkPropertyName(0), false, nullptr);
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(0));
    GestureCounter gestures;
    side.addListener(&gestures);
    {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* mid = dynamic_cast<ControlSlider*>(childWithTitle(strip, "Low Mid"));
        ASSERT_NE(mid, nullptr);
        const juce::Point<float> position {40.0F, 13.0F};
        mid->mouseDown(mouseEvent(*mid, position, position, juce::ModifierKeys::leftButtonModifier));
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        mid->setValue(3.0, juce::sendNotificationSync);
        EXPECT_NEAR(plainValue(side), 0.0F, 0.01F);
    }
    EXPECT_TRUE(gestures.events.empty());
    side.removeListener(&gestures);
}

TEST(BandStrip, WheelAndKeysMoveLinkedSideInBalancedGestures)
{
    ImageControlProcessor processor;
    setParameterInGesture(*processor.getParameters().getParameter(widthParameterId(1)), 0.4F);
    setParameterInGesture(*processor.getParameters().getParameter(recoverParameterId(1)), 75.0F);
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(1));
    GestureCounter gestures;
    side.addListener(&gestures);
    {
        const BandStrip strip(processor.getParameters(), processor.getEditorState(), 1, processor.getHistory());
        auto* mid = dynamic_cast<ControlSlider*>(childWithTitle(strip, "Low Mid Mid"));
        ASSERT_NE(mid, nullptr);
        const juce::Point<float> position {40.0F, 13.0F};
        mid->mouseWheelMove(mouseEvent(*mid, position, position, {}), notch(1.0F));
        EXPECT_NEAR(plainValue(side), 0.1F, 0.001F);
        EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        EXPECT_NEAR(plainValue(side), 0.82F, 0.001F);
        EXPECT_NEAR(parameterValue(processor, widthParameterId(1)), 0.4F, 0.001F);
        EXPECT_NEAR(parameterValue(processor, recoverParameterId(1)), 75.0F, 0.001F);
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false, true, false}));
    }
    side.removeListener(&gestures);
}

TEST(BandStrip, InterruptedWidthAndRecoverDragsCloseOnlyTheirOwnGestures)
{
    for (const bool recovering : {false, true}) {
        ImageControlProcessor processor;
        auto& parameter
            = *processor.getParameters().getParameter(recovering ? recoverParameterId(0) : widthParameterId(0));
        auto& side = *processor.getParameters().getParameter(sideGainParameterId(0));
        GestureCounter gestures;
        GestureCounter sideGestures;
        parameter.addListener(&gestures);
        side.addListener(&sideGestures);
        {
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
            strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
            auto* slider
                = dynamic_cast<ControlSlider*>(childWithTitle(strip, recovering ? "Low Recover" : "Low Width"));
            ASSERT_NE(slider, nullptr);
            const juce::Point<float> position {40.0F, 13.0F};
            slider->mouseDown(mouseEvent(*slider, position, position, juce::ModifierKeys::leftButtonModifier));
            slider->setValue(recovering ? 50.0 : 0.5, juce::sendNotificationSync);
            EXPECT_NEAR(plainValue(parameter), recovering ? 50.0F : 0.5F, 0.001F);
            EXPECT_NEAR(plainValue(side), 0.0F, 0.01F);
        }
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false}));
        EXPECT_TRUE(sideGestures.events.empty());
        parameter.removeListener(&gestures);
        side.removeListener(&sideGestures);
    }
}

TEST(BandStrip, WidthAndRecoverExposeIndependentAccessibleValuesAndTextAttachments)
{
    ImageControlProcessor processor;
    setParameterInGesture(*processor.getParameters().getParameter(midGainParameterId(0)), -12.0F);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    strip.addToDesktop(0);
    for (const bool recovering : {false, true}) {
        auto* slider = dynamic_cast<ControlSlider*>(childWithTitle(strip, recovering ? "Low Recover" : "Low Width"));
        ASSERT_NE(slider, nullptr);
        auto* handler = slider->getAccessibilityHandler();
        ASSERT_NE(handler, nullptr);
        EXPECT_EQ(handler->getRole(), juce::AccessibilityRole::slider);
        EXPECT_EQ(handler->getTitle(), recovering ? "Low Recover" : "Low Width");
        auto* value = handler->getValueInterface();
        ASSERT_NE(value, nullptr);
        EXPECT_FALSE(value->isReadOnly());
        EXPECT_EQ(value->getCurrentValueAsString(), recovering ? "0%" : "1.00");
        EXPECT_NEAR(value->getCurrentValue(), recovering ? 0.0 : 1.0, 0.001);
        juce::Label* valueBox = nullptr;
        for (auto* child : slider->getChildren()) {
            if (auto* label = dynamic_cast<juce::Label*>(child)) {
                valueBox = label;
                break;
            }
        }
        ASSERT_NE(valueBox, nullptr);
        EXPECT_EQ(valueBox->getText(), recovering ? "0%" : "1.00");
        // Deliver text through the production value-box callback and its parameter attachment.
        valueBox->setText(recovering ? "37.5 %" : "150 %", juce::sendNotificationSync);
        EXPECT_NEAR(
            parameterValue(processor, recovering ? recoverParameterId(0) : widthParameterId(0)),
            recovering ? 38.0F : 1.5F,
            0.001F
        );
        EXPECT_NEAR(value->getCurrentValue(), recovering ? 38.0 : 1.5, 0.001);
        EXPECT_EQ(value->getCurrentValueAsString(), recovering ? "38%" : "1.50");
        EXPECT_EQ(valueBox->getText(), recovering ? "38%" : "1.50");
        if (recovering) {
            value->setValueAsString("37.4 %");
            EXPECT_DOUBLE_EQ(value->getCurrentValue(), 37.0);
            EXPECT_FLOAT_EQ(parameterValue(processor, recoverParameterId(0)), 37.0F);
            EXPECT_EQ(value->getCurrentValueAsString(), "37%");
            EXPECT_EQ(valueBox->getText(), "37%");
            value->setValue(37.5);
            EXPECT_DOUBLE_EQ(value->getCurrentValue(), 38.0);
            EXPECT_EQ(value->getCurrentValueAsString(), "38%");
            value->setValueAsString("0.1 %");
            EXPECT_DOUBLE_EQ(value->getCurrentValue(), 0.0);
            EXPECT_FLOAT_EQ(parameterValue(processor, recoverParameterId(0)), 0.0F);
            EXPECT_EQ(value->getCurrentValueAsString(), "0%");
        }
        value->setValueAsString(recovering ? "Off" : "2");
        EXPECT_NEAR(
            parameterValue(processor, recovering ? recoverParameterId(0) : widthParameterId(0)),
            recovering ? 0.0F : WIDTH_MAX,
            0.001F
        );
        EXPECT_NEAR(parameterValue(processor, midGainParameterId(0)), -12.0F, 0.01F);
        EXPECT_NEAR(parameterValue(processor, sideGainParameterId(0)), 0.0F, 0.01F);
    }
}

TEST(BandStrip, WidthAndRecoverStayEnabledWithMidOffAndFollowOnlyTheirOwnAutomation)
{
    ImageControlProcessor processor;
    const BandStrip strip(processor.getParameters(), processor.getEditorState(), 2, processor.getHistory());
    auto* width = dynamic_cast<ControlSlider*>(childWithTitle(strip, "High Mid Width"));
    auto* recover = dynamic_cast<ControlSlider*>(childWithTitle(strip, "High Mid Recover"));
    ASSERT_NE(width, nullptr);
    ASSERT_NE(recover, nullptr);
    auto& mid = *processor.getParameters().getParameter(midGainParameterId(2));
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(2));
    setParameterInGesture(mid, BAND_GAIN_MIN_DB);
    EXPECT_TRUE(width->isEnabled());
    EXPECT_TRUE(recover->isEnabled());
    EXPECT_TRUE(width->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_NEAR(width->getValue(), 1.02, 0.001);
    setParameterInGesture(*processor.getParameters().getParameter(widthParameterId(2)), 0.4F);
    setParameterInGesture(*processor.getParameters().getParameter(recoverParameterId(2)), 75.0F);
    setParameterInGesture(mid, -12.0F);
    setParameterInGesture(side, 6.0F);
    EXPECT_NEAR(width->getValue(), 0.4, 0.001);
    EXPECT_NEAR(recover->getValue(), 75.0, 0.001);
    auto& recovery = *processor.getParameters().getParameter(recoverParameterId(2));
    recovery.setValueNotifyingHost(37.5F / 200.0F);
    EXPECT_FLOAT_EQ(plainValue(recovery), 38.0F);
    EXPECT_DOUBLE_EQ(recover->getValue(), 38.0);
    EXPECT_EQ(recover->getTextFromValue(recover->getValue()), "38%");
    EXPECT_NEAR(width->getValue(), 0.4, 0.001);
    recovery.setValueNotifyingHost(0.1F / 200.0F);
    EXPECT_FLOAT_EQ(plainValue(recovery), 0.0F);
    EXPECT_DOUBLE_EQ(recover->getValue(), 0.0);
    EXPECT_EQ(recover->getTextFromValue(recover->getValue()), "0%");
    EXPECT_NEAR(plainValue(mid), -12.0F, 0.001F);
    EXPECT_NEAR(plainValue(side), 6.0F, 0.001F);
}

TEST(BandStrip, WidthAndRecoverWheelKeysAndResetUseBalancedIndependentGestures)
{
    for (const bool recovering : {false, true}) {
        ImageControlProcessor processor;
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* slider = dynamic_cast<ControlSlider*>(childWithTitle(strip, recovering ? "Low Recover" : "Low Width"));
        ASSERT_NE(slider, nullptr);
        auto& parameter
            = *processor.getParameters().getParameter(recovering ? recoverParameterId(0) : widthParameterId(0));
        GestureCounter gestures;
        parameter.addListener(&gestures);
        const juce::Point<float> position {40.0F, 13.0F};
        slider->mouseWheelMove(mouseEvent(*slider, position, position, {}), notch(1.0F));
        EXPECT_NEAR(plainValue(parameter), recovering ? 1.0F : 1.01F, 0.001F);
        EXPECT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        EXPECT_NEAR(plainValue(parameter), recovering ? 3.0F : 1.03F, 0.001F);
        const auto event = mouseEvent(*slider, position, position, juce::ModifierKeys::leftButtonModifier);
        slider->mouseDown(event);
        slider->mouseDoubleClick(event);
        slider->mouseUp(event);
        EXPECT_NEAR(plainValue(parameter), recovering ? 0.0F : 1.0F, 0.001F);
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false, true, false, true, false, true, false}));
        parameter.removeListener(&gestures);
    }
}

TEST(BandStrip, FiveRowsFitAtMinimumHeightWithRecoveryBelowWidth)
{
    ImageControlProcessor processor;
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, processor.getHistory());
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    int previousBottom = 0;
    for (const char* title : {"Low Mid", "Low Side", "Low Width", "Low Recover", "Low Pan"}) {
        auto* slider = dynamic_cast<ControlSlider*>(childWithTitle(strip, title));
        ASSERT_NE(slider, nullptr);
        EXPECT_GE(slider->getY(), previousBottom);
        EXPECT_EQ(slider->getHeight(), 26);
        EXPECT_TRUE(strip.getLocalBounds().contains(slider->getBounds()));
        previousBottom = slider->getBottom();
    }
}
}  // namespace esgrove::image_control::test
