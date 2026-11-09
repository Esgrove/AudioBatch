/// Tests explicit local-control history through production slider, button, band, and spectrogram handlers.
/// Automation updates must never create commands or move linked controls, and teardown balances active gestures.

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/HistoryAttachments.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

#include <cmath>
#include <functional>
#include <thread>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Records host notifications for proving gesture balance without observing history from a parameter listener.
class HistoryGestureCounter final : public juce::AudioProcessorParameter::Listener
{
public:
    /// Ignores values because their before/after contract is checked against APVTS.
    void parameterValueChanged(int /*index*/, float /*value*/) override {}

    /// Records the ordered boundaries delivered by JUCE.
    void parameterGestureChanged(int /*index*/, const bool beginning) override
    {
        boundaries.push_back(beginning);
    }

    /// Ordered host gesture starts and ends.
    std::vector<bool> boundaries;
};

/// Restores a saved host state from the synchronous gesture-start callback exactly once.
class RestoreAtGestureStart final : public juce::AudioProcessorParameter::Listener
{
public:
    /// Retains a processor and immutable snapshot that outlive this temporary test listener.
    RestoreAtGestureStart(ImageControlProcessor& owner, const juce::MemoryBlock& saved) : processor(owner), state(saved)
    {}

    /// Leaves the processor and saved state owned by the test fixture.
    ~RestoreAtGestureStart() override = default;

    RestoreAtGestureStart(const RestoreAtGestureStart&) = delete;
    RestoreAtGestureStart(RestoreAtGestureStart&&) = delete;
    RestoreAtGestureStart& operator=(const RestoreAtGestureStart&) = delete;
    RestoreAtGestureStart& operator=(RestoreAtGestureStart&&) = delete;

    /// Parameter delivery is unrelated to the synchronous gesture-start interruption.
    void parameterValueChanged(int /*index*/, float /*value*/) override {}

    /// Cancels the local command before its first host-notified value can overwrite restored state.
    void parameterGestureChanged(int /*index*/, const bool beginning) override
    {
        if (beginning && !restored) {
            restored = true;
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        }
    }

private:
    /// Host-state owner, which outlives this temporary observer.
    ImageControlProcessor& processor;
    /// Saved host state, owned by the test and alive until listener removal.
    const juce::MemoryBlock& state;
    /// Prevents replaying the restore from subsequent test gestures.
    bool restored {false};
};

/// Creates a production-handler event with stable pointer-down coordinates.
juce::MouseEvent historyMouse(
    juce::Component& control,
    const juce::Point<float> position,
    const juce::Point<float> down,
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

/// Returns one positive notched wheel event, independent of platform wheel calibration.
juce::MouseWheelDetails historyNotch()
{
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = 60.0F / 256.0F;
    return wheel;
}

/// Finds a direct production control by its stable accessible title, or null if absent or of another type.
/// The parent owns the returned control, which is valid only while it remains a child and must be checked before use.
template<typename Control>
Control* historyControl(const juce::Component& parent, const juce::String& title)
{
    for (auto* child : parent.getChildren()) {
        if (child->getTitle() == title) {
            return dynamic_cast<Control*>(child);
        }
    }
    return nullptr;
}

/// Returns the current plain parameter value, not the display's cached copy.
float historyValue(ImageControlProcessor& processor, const juce::String& parameterId)
{
    return plainValue(*processor.getParameters().getParameter(parameterId));
}

/// Projects a known frequency into the real view's logarithmic plot.
float historyX(const SpatialSpectrogramView& view, const double frequency)
{
    const auto plot = view.getPlotBounds();
    return plot.getX() + static_cast<float>(frequencyToShare(frequency, FrequencyScale::logarithmic)) * plot.getWidth();
}

/// Delivers ParameterAttachment's queued host-thread synchronization with a bounded native message pump.
void dispatchHistoryChanges()
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

/// Drags a real slider through pointer handlers and leaves it open until the caller releases or destroys it.
void startHistoryDrag(ControlSlider& slider)
{
    const juce::Point<float> start {40.0F, 13.0F};
    slider.mouseDown(historyMouse(slider, start, start));
    slider.mouseDrag(historyMouse(slider, {90.0F, 13.0F}, start));
}
}  // namespace

/// Required history records local bindings while host updates stay silent and preserve independent undo commands.
TEST(HistoryControls, RequiredHistoryBindsSliderAndButtonWithoutRecordingHostUpdates)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    auto& parameter = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
    HistoryGestureCounter counter;
    parameter.addListener(&counter);
    {
        ControlSlider slider;
        juce::ToggleButton button;
        const HistorySliderAttachment sliderAttachment(processor.getParameters(), OUTPUT_GAIN_ID, slider, history);
        const HistoryButtonAttachment buttonAttachment(processor.getParameters(), MONO_ID, button, history);
        slider.setValue(-6.0, juce::sendNotificationSync);
        button.setToggleState(true, juce::sendNotificationSync);
        EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), -6.0F, 0.001F);
        EXPECT_NEAR(historyValue(processor, MONO_ID), 1.0F, 0.001F);
        EXPECT_EQ(counter.boundaries, (std::vector<bool> {true, false}));
        EXPECT_TRUE(history.canUndo());
        parameter.setValueNotifyingHost(parameter.convertTo0to1(-3.0F));
        EXPECT_NEAR(slider.getValue(), -3.0, 0.001);
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(historyValue(processor, MONO_ID), 0.0F, 0.001F);
        EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), -3.0F, 0.001F);
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), 0.0F, 0.001F);
        EXPECT_FALSE(history.canUndo());
    }
    parameter.removeListener(&counter);
}

/// Multiple pointer updates become one command and replay restores both its endpoints.
TEST(HistoryControls, SliderDragIsOneCommandWithBalancedHostGestures)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    auto& parameter = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
    HistoryGestureCounter counter;
    parameter.addListener(&counter);
    ControlSlider slider;
    slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slider.setSize(240, 26);
    {
        const HistorySliderAttachment attachment(processor.getParameters(), OUTPUT_GAIN_ID, slider, history);
        startHistoryDrag(slider);
        EXPECT_TRUE(history.isEditing());
        EXPECT_FALSE(history.canUndo());
        slider.mouseDrag(historyMouse(slider, {110.0F, 13.0F}, {40.0F, 13.0F}));
        const float edited = historyValue(processor, OUTPUT_GAIN_ID);
        EXPECT_GT(edited, 0.0F);
        slider.endDrag();
        EXPECT_FALSE(history.isEditing());
        EXPECT_EQ(counter.boundaries, (std::vector<bool> {true, false}));
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), 0.0F, 0.001F);
        EXPECT_FALSE(history.canUndo());
        EXPECT_TRUE(history.redo());
        EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), edited, 0.001F);
    }
    parameter.removeListener(&counter);
}

/// Key nudges, wheel steps, value-box text, accessibility, and ungrouped notifications all close their own edits.
TEST(HistoryControls, SliderDiscreteInputPathsAreCompleteCommands)
{
    for (int input = 0; input < 5; ++input) {
        SCOPED_TRACE(input);
        ImageControlProcessor processor;
        auto& history = processor.getHistory();
        ControlSlider slider;
        slider.setSize(240, 26);
        slider.addToDesktop(0);
        const HistorySliderAttachment attachment(processor.getParameters(), recoverParameterId(0), slider, history);
        if (input == 0) {
            EXPECT_TRUE(slider.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        } else if (input == 1) {
            slider.mouseWheelMove(historyMouse(slider, {40.0F, 13.0F}, {40.0F, 13.0F}, {}), historyNotch());
        } else if (input == 2) {
            juce::Label* valueBox = nullptr;
            for (auto* child : slider.getChildren()) {
                if (auto* label = dynamic_cast<juce::Label*>(child)) {
                    valueBox = label;
                    break;
                }
            }
            ASSERT_NE(valueBox, nullptr);
            valueBox->setText("37.5 %", juce::sendNotificationSync);
        } else if (input == 3) {
            auto* handler = slider.getAccessibilityHandler();
            ASSERT_NE(handler, nullptr);
            auto* value = handler->getValueInterface();
            ASSERT_NE(value, nullptr);
            value->setValueAsString("25 %");
        } else {
            slider.setValue(12.5, juce::sendNotificationSync);
        }
        const float edited = historyValue(processor, recoverParameterId(0));
        EXPECT_GT(edited, 0.0F);
        EXPECT_FALSE(history.isEditing());
        EXPECT_TRUE(history.canUndo());
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(historyValue(processor, recoverParameterId(0)), 0.0F, 0.001F);
        EXPECT_FALSE(history.canUndo());
        EXPECT_TRUE(history.redo());
        EXPECT_NEAR(historyValue(processor, recoverParameterId(0)), edited, 0.001F);
    }
}

/// JUCE-compatible reset and custom parameter range conversion survive replacing its built-in attachment.
TEST(HistoryControls, SliderResetAndRangeMatchParameter)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    auto& parameter = *processor.getParameters().getParameter(crossoverParameterId(0));
    setParameterInGesture(parameter, 400.0F);
    ControlSlider slider;
    slider.setSize(240, 26);
    const HistorySliderAttachment attachment(processor.getParameters(), crossoverParameterId(0), slider, history);
    for (const double share : {0.0, 0.1, 0.5, 0.9, 1.0}) {
        EXPECT_NEAR(
            slider.proportionOfLengthToValue(share), parameter.convertFrom0to1(static_cast<float>(share)), 0.01
        );
    }
    slider.mouseDoubleClick(historyMouse(slider, {40.0F, 13.0F}, {40.0F, 13.0F}));
    EXPECT_NEAR(
        historyValue(processor, crossoverParameterId(0)), parameter.convertFrom0to1(parameter.getDefaultValue()), 0.01F
    );
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(historyValue(processor, crossoverParameterId(0)), 400.0F, 0.01F);
}

/// Host writes update controls but do not call local callbacks, create commands, or erase the redo branch.
TEST(HistoryControls, AutomationIsExcludedForSlidersAndButtons)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    auto& gain = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
    auto& mono = *processor.getParameters().getParameter(MONO_ID);
    ControlSlider slider;
    juce::ToggleButton button;
    const HistorySliderAttachment sliderAttachment(processor.getParameters(), OUTPUT_GAIN_ID, slider, history);
    const HistoryButtonAttachment buttonAttachment(processor.getParameters(), MONO_ID, button, history);
    int sliderCallbacks = 0;
    int buttonCallbacks = 0;
    slider.onValueChange = [&sliderCallbacks] { ++sliderCallbacks; };
    button.onClick = [&buttonCallbacks] { ++buttonCallbacks; };
    std::thread automation([&gain, &mono] {
        gain.setValueNotifyingHost(gain.convertTo0to1(-6.0F));
        mono.setValueNotifyingHost(1.0F);
    });
    automation.join();
    dispatchHistoryChanges();
    EXPECT_NEAR(slider.getValue(), -6.0, 0.001);
    EXPECT_TRUE(button.getToggleState());
    EXPECT_EQ(sliderCallbacks, 0);
    EXPECT_EQ(buttonCallbacks, 0);
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.isEditing());
    slider.setValue(-3.0, juce::sendNotificationSync);
    EXPECT_TRUE(history.undo());
    mono.setValueNotifyingHost(0.0F);
    EXPECT_TRUE(history.canRedo());
    EXPECT_TRUE(history.redo());
    EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), -3.0F, 0.001F);
    EXPECT_FALSE(button.getToggleState());
}

/// Clicks and accessibility toggles use one local command per action and host synchronization creates none.
TEST(HistoryControls, ToggleClickAndAccessibilityAreUndoable)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    juce::ToggleButton button("Mono");
    button.setClickingTogglesState(true);
    button.addToDesktop(0);
    const HistoryButtonAttachment attachment(processor.getParameters(), MONO_ID, button, history);
    button.setToggleState(true, juce::sendNotificationSync);
    EXPECT_NEAR(historyValue(processor, MONO_ID), 1.0F, 0.001F);
    EXPECT_TRUE(history.undo());
    EXPECT_FALSE(button.getToggleState());
    EXPECT_FALSE(history.canUndo());
    auto* handler = button.getAccessibilityHandler();
    ASSERT_NE(handler, nullptr);
    EXPECT_TRUE(handler->getActions().invoke(juce::AccessibilityActionType::toggle));
    EXPECT_TRUE(button.getToggleState());
    EXPECT_TRUE(history.undo());
    EXPECT_FALSE(button.getToggleState());
}

/// Mid's listener ends before BandStrip's linked Side callback, but both values must remain one undo command.
TEST(HistoryControls, LinkedBandSliderInputsShareOneCommand)
{
    for (int input = 0; input < 4; ++input) {
        SCOPED_TRACE(input);
        ImageControlProcessor processor;
        auto& history = processor.getHistory();
        processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        strip.addToDesktop(0);
        auto* mid = historyControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        if (input == 0) {
            startHistoryDrag(*mid);
            mid->endDrag();
        } else if (input == 1) {
            EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        } else if (input == 2) {
            mid->mouseWheelMove(historyMouse(*mid, {40.0F, 13.0F}, {40.0F, 13.0F}, {}), historyNotch());
        } else {
            auto* handler = mid->getAccessibilityHandler();
            ASSERT_NE(handler, nullptr);
            ASSERT_NE(handler->getValueInterface(), nullptr);
            handler->getValueInterface()->setValueAsString("-6 dB");
        }
        const float edited = historyValue(processor, midGainParameterId(0));
        EXPECT_GT(std::abs(edited), 0.001F);
        EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), edited, 0.001F);
        EXPECT_FALSE(history.isEditing());
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(historyValue(processor, midGainParameterId(0)), 0.0F, 0.001F);
        EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), 0.0F, 0.001F);
        EXPECT_FALSE(history.canUndo());
        EXPECT_TRUE(history.redo());
        EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), edited, 0.001F);
    }
}

/// Automating Mid during a local linked gesture must not turn that host update into a local Side write.
TEST(HistoryControls, LinkedAutomationDoesNotWriteSide)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
    strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
    auto* mid = historyControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(mid, nullptr);
    mid->mouseDown(historyMouse(*mid, {40.0F, 13.0F}, {40.0F, 13.0F}));
    auto& parameter = *processor.getParameters().getParameter(midGainParameterId(0));
    parameter.setValueNotifyingHost(parameter.convertTo0to1(-9.0F));
    EXPECT_NEAR(mid->getValue(), -9.0, 0.001);
    EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), 0.0F, 0.001F);
    mid->endDrag();
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.isEditing());
}

/// Closing an editor or band during a drag preserves the completed local edit without leaking a history frame.
TEST(HistoryControls, TeardownClosesLinkedSliderAndSpectrogramGestures)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    {
        BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
        strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
        auto* mid = historyControl<ControlSlider>(strip, "Low Mid");
        ASSERT_NE(mid, nullptr);
        startHistoryDrag(*mid);
        EXPECT_TRUE(history.isEditing());
    }
    EXPECT_FALSE(history.isEditing());
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(historyValue(processor, midGainParameterId(0)), 0.0F, 0.001F);
    EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), 0.0F, 0.001F);
    {
        SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), history);
        view.setSize(1000, 400);
        const juce::Point<float> start {historyX(view, 60.0), view.getPlotBounds().getCentreY()};
        const auto modifiers = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier;
        view.mouseDown(historyMouse(view, start, start, modifiers));
        view.mouseDrag(historyMouse(view, start.translated(0.0F, -20.0F), start, modifiers));
        EXPECT_TRUE(history.isEditing());
    }
    EXPECT_FALSE(history.isEditing());
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(historyValue(processor, midGainParameterId(0)), 0.0F, 0.001F);
    EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), 0.0F, 0.001F);
}

/// Moving a middle band records its two crossover IDs in one command and leaves the third untouched.
TEST(HistoryControls, SpectrogramBandDragGroupsBothCrossovers)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    const auto before = processor.getCrossovers();
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), history);
    view.setSize(1000, 400);
    const juce::Point<float> start {historyX(view, 250.0), view.getPlotBounds().getCentreY()};
    view.mouseDown(historyMouse(view, start, start));
    view.mouseDrag(historyMouse(view, start.translated(25.0F, 0.0F), start));
    view.mouseUp(historyMouse(view, start.translated(25.0F, 0.0F), start, {}));
    const auto after = processor.getCrossovers();
    EXPECT_GT(after.at(0), before.at(0));
    EXPECT_GT(after.at(1), before.at(1));
    EXPECT_FLOAT_EQ(after.at(2), before.at(2));
    EXPECT_TRUE(history.undo());
    for (std::size_t index = 0; index < CROSSOVER_COUNT; ++index) {
        EXPECT_NEAR(processor.getCrossovers().at(index), before.at(index), 0.01F);
    }
    EXPECT_FALSE(history.canUndo());
    EXPECT_TRUE(history.redo());
    for (std::size_t index = 0; index < CROSSOVER_COUNT; ++index) {
        EXPECT_NEAR(processor.getCrossovers().at(index), after.at(index), 0.01F);
    }
}

/// Spectrogram wheel gain edits and crossover resets are discrete commands, not one entry per changed parameter.
TEST(HistoryControls, SpectrogramLinkedWheelAndCrossoverResetAreUndoable)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), history);
    view.setSize(1000, 400);
    const juce::Point<float> band {historyX(view, 60.0), view.getPlotBounds().getCentreY()};
    view.mouseWheelMove(historyMouse(view, band, band, juce::ModifierKeys::ctrlModifier), historyNotch());
    EXPECT_NEAR(historyValue(processor, midGainParameterId(0)), 0.1F, 0.001F);
    EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), 0.1F, 0.001F);
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(historyValue(processor, midGainParameterId(0)), 0.0F, 0.001F);
    EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), 0.0F, 0.001F);
    auto& crossover = *processor.getParameters().getParameter(crossoverParameterId(0));
    setParameterInGesture(crossover, 300.0F);
    const juce::Point<float> line {historyX(view, 300.0), view.getPlotBounds().getCentreY()};
    view.mouseDoubleClick(historyMouse(view, line, line));
    EXPECT_NEAR(
        historyValue(processor, crossoverParameterId(0)), crossover.convertFrom0to1(crossover.getDefaultValue()), 0.01F
    );
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(historyValue(processor, crossoverParameterId(0)), 300.0F, 0.01F);
}

/// Width, Recover, Pan, and Solo use the same local history, while Link remains an editor-only preference.
TEST(HistoryControls, BandIndependentControlsAndSoloShareHistoryButLinkDoesNot)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    BandStrip const strip(processor.getParameters(), processor.getEditorState(), 0, history);
    for (const auto* title : {"Low Width", "Low Recover", "Low Pan"}) {
        auto* slider = historyControl<ControlSlider>(strip, title);
        ASSERT_NE(slider, nullptr);
        const double before = slider->getValue();
        EXPECT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(slider->getValue(), before, 0.001);
        EXPECT_FALSE(history.canUndo());
    }
    auto* solo = historyControl<juce::Button>(strip, "Low Solo");
    ASSERT_NE(solo, nullptr);
    solo->setToggleState(true, juce::sendNotificationSync);
    EXPECT_TRUE(history.undo());
    EXPECT_FALSE(solo->getToggleState());
    auto* link = historyControl<juce::Button>(strip, "Low Link");
    ASSERT_NE(link, nullptr);
    link->setToggleState(true, juce::sendNotificationSync);
    EXPECT_FALSE(history.canUndo());
}

/// An unchanged edit does not erase redo, and disabled controls cannot open local commands.
TEST(HistoryControls, NoOpAndDisabledGesturesKeepRedo)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    ControlSlider slider;
    const HistorySliderAttachment attachment(processor.getParameters(), OUTPUT_GAIN_ID, slider, history);
    slider.setValue(-6.0, juce::sendNotificationSync);
    EXPECT_TRUE(history.undo());
    slider.mouseDoubleClick(historyMouse(slider, {40.0F, 13.0F}, {40.0F, 13.0F}));
    EXPECT_FALSE(history.isEditing());
    EXPECT_TRUE(history.canRedo());
    slider.setEnabled(false);
    EXPECT_FALSE(slider.keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    slider.mouseDown(historyMouse(slider, {40.0F, 13.0F}, {40.0F, 13.0F}));
    slider.endDrag();
    EXPECT_FALSE(history.isEditing());
    EXPECT_TRUE(history.canRedo());
}

/// Side Off remains untouched during linked Mid edits and therefore is not replayed over later automation.
TEST(HistoryControls, LinkedSideOffIsNotCaptured)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    auto& side = *processor.getParameters().getParameter(sideGainParameterId(0));
    setParameterInGesture(side, BAND_GAIN_MIN_DB);
    processor.getEditorState().setProperty(linkPropertyName(0), true, nullptr);
    BandStrip const strip(processor.getParameters(), processor.getEditorState(), 0, history);
    auto* mid = historyControl<ControlSlider>(strip, "Low Mid");
    ASSERT_NE(mid, nullptr);
    EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), BAND_GAIN_MIN_DB, 0.001F);
    setParameterInGesture(side, -12.0F);
    EXPECT_TRUE(history.undo());
    EXPECT_NEAR(historyValue(processor, midGainParameterId(0)), 0.0F, 0.001F);
    EXPECT_NEAR(historyValue(processor, sideGainParameterId(0)), -12.0F, 0.001F);
}

/// Direct crossover drags and unlinked Side drags capture only their own affected parameter.
TEST(HistoryControls, SpectrogramSingleParameterDragsStayScoped)
{
    for (const bool crossoverDrag : {false, true}) {
        SCOPED_TRACE(crossoverDrag);
        ImageControlProcessor processor;
        auto& history = processor.getHistory();
        SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), history);
        view.setSize(1000, 400);
        const auto parameterId = crossoverDrag ? crossoverParameterId(0) : sideGainParameterId(0);
        const float before = historyValue(processor, parameterId);
        const juce::Point<float> start {
            historyX(view, crossoverDrag ? static_cast<double>(DEFAULT_CROSSOVERS.at(0)) : 60.0),
            view.getPlotBounds().getCentreY()
        };
        const auto end = crossoverDrag ? start.translated(20.0F, 0.0F) : start.translated(0.0F, -20.0F);
        const juce::ModifierKeys leftButton {juce::ModifierKeys::leftButtonModifier};
        const auto modifiers = crossoverDrag ? leftButton : leftButton.withFlags(juce::ModifierKeys::altModifier);
        view.mouseDown(historyMouse(view, start, start, modifiers));
        view.mouseDrag(historyMouse(view, end, start, modifiers));
        view.mouseUp(historyMouse(view, end, start, {}));
        EXPECT_GT(historyValue(processor, parameterId), before);
        auto& unrelated = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
        setParameterInGesture(unrelated, -9.0F);
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(historyValue(processor, parameterId), before, 0.01F);
        EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), -9.0F, 0.001F);
        EXPECT_FALSE(history.canUndo());
    }
}

/// A host may restore state directly from beginChangeGesture, before a discrete input writes its value.
TEST(HistoryControls, ReentrantRestoreAtGestureStartRejectsDiscreteWrites)
{
    for (const bool toggle : {false, true}) {
        SCOPED_TRACE(toggle);
        ImageControlProcessor processor;
        auto& history = processor.getHistory();
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        auto& parameter = *processor.getParameters().getParameter(toggle ? MONO_ID : OUTPUT_GAIN_ID);
        RestoreAtGestureStart restore(processor, state);
        parameter.addListener(&restore);
        if (toggle) {
            juce::ToggleButton button;
            const HistoryButtonAttachment attachment(processor.getParameters(), MONO_ID, button, history);
            button.setToggleState(true, juce::sendNotificationSync);
            EXPECT_FALSE(button.getToggleState());
            EXPECT_NEAR(historyValue(processor, MONO_ID), 0.0F, 0.001F);
        } else {
            ControlSlider slider;
            const HistorySliderAttachment attachment(processor.getParameters(), OUTPUT_GAIN_ID, slider, history);
            slider.setValue(-6.0, juce::sendNotificationSync);
            EXPECT_NEAR(slider.getValue(), 0.0, 0.001);
            EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), 0.0F, 0.001F);
        }
        parameter.removeListener(&restore);
        EXPECT_FALSE(history.isEditing());
        EXPECT_FALSE(history.canUndo());
    }
}

/// Destroying a detached control after host automation cancels JUCE's pending delivery before its callback runs.
TEST(HistoryControls, AttachmentTeardownCancelsQueuedAutomation)
{
    ImageControlProcessor processor;
    auto& history = processor.getHistory();
    auto& parameter = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
    {
        ControlSlider slider;
        const HistorySliderAttachment attachment(processor.getParameters(), OUTPUT_GAIN_ID, slider, history);
        std::thread automation([&parameter] { parameter.setValueNotifyingHost(parameter.convertTo0to1(-9.0F)); });
        automation.join();
    }
    dispatchHistoryChanges();
    EXPECT_FALSE(history.isEditing());
    EXPECT_FALSE(history.canUndo());
    EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), -9.0F, 0.001F);
}

/// State restore invalidates active UI frames. Their remaining movement and end callbacks cannot create new history.
TEST(HistoryControls, HostRestoreCancelsStaleSliderAndSpectrogramFrames)
{
    for (const bool spectrogram : {false, true}) {
        SCOPED_TRACE(spectrogram);
        ImageControlProcessor processor;
        auto& history = processor.getHistory();
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        if (spectrogram) {
            SpatialSpectrogramView view(processor.getParameters(), processor.getEditorState(), history);
            view.setSize(1000, 400);
            const juce::Point<float> start {historyX(view, 60.0), view.getPlotBounds().getCentreY()};
            const auto modifiers = juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier;
            view.mouseDown(historyMouse(view, start, start, modifiers));
            view.mouseDrag(historyMouse(view, start.translated(0.0F, -20.0F), start, modifiers));
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            view.mouseDrag(historyMouse(view, start.translated(0.0F, -40.0F), start, modifiers));
            view.endDrag();
            EXPECT_NEAR(historyValue(processor, midGainParameterId(0)), 0.0F, 0.001F);
        } else {
            BandStrip strip(processor.getParameters(), processor.getEditorState(), 0, history);
            strip.setSize(240, BAND_STRIP_MIN_HEIGHT);
            auto* mid = historyControl<ControlSlider>(strip, "Low Mid");
            ASSERT_NE(mid, nullptr);
            startHistoryDrag(*mid);
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            mid->mouseDrag(historyMouse(*mid, {120.0F, 13.0F}, {40.0F, 13.0F}));
            mid->endDrag();
            EXPECT_NEAR(historyValue(processor, midGainParameterId(0)), 0.0F, 0.001F);
        }
        EXPECT_FALSE(history.canUndo());
        EXPECT_FALSE(history.isEditing());
        ControlSlider fresh;
        const HistorySliderAttachment attachment(processor.getParameters(), OUTPUT_GAIN_ID, fresh, history);
        fresh.setValue(-3.0, juce::sendNotificationSync);
        EXPECT_TRUE(history.undo());
        EXPECT_NEAR(historyValue(processor, OUTPUT_GAIN_ID), 0.0F, 0.001F);
    }
}
}  // namespace esgrove::image_control::test
