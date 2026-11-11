/// Shares component lookup and parameter readback helpers for compact band and editor workflow tests.

#pragma once

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/ProcessingToolbar.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
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

/// First whole-editor width fitting the compact 328-pixel preset group and a usable 190-pixel output slider.
inline constexpr int INLINE_PRESET_EDITOR_WIDTH {1106};

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

/// Finds a strip-owned heading by its displayed text, valid until the strip is destroyed.
inline juce::Label* bandHeadingLabel(const BandStrip& strip, const juce::String& text)
{
    for (auto* child : strip.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child); label != nullptr && label->getText() == text) {
            return label;
        }
    }
    return nullptr;
}

/// Measures uncompressed heading text and the actual look-and-feel border at its fixed display font.
inline int bandHeadingTextWidth(juce::Label& label)
{
    const auto font = label.getLookAndFeel().getLabelFont(label);
    const int glyphWidth = static_cast<int>(std::ceil(juce::GlyphArrangement::getStringWidth(font, label.getText())));
    return glyphWidth + label.getLookAndFeel().getLabelBorderSize(label).getLeftAndRight();
}

/// Includes both margins, Solo, Link, their gap, and two readable inline text gaps in the fit boundary.
inline int bandHeadingInlineWidth(juce::Label& name, juce::Label& range, const int disclosureInset)
{
    return 20 + 20 + 60 + 6 + 8 * 2 + disclosureInset + bandHeadingTextWidth(name) + bandHeadingTextWidth(range);
}

/// Finds the slider's JUCE-owned numeric label, valid until its presentation or look and feel changes.
inline juce::Label* compactValueBox(const ControlSlider& slider)
{
    for (auto* child : slider.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child)) {
            return label;
        }
    }
    return nullptr;
}

/// Reads the processor's current plain value rather than relying on a slider's cached representation.
inline float compactParameterValue(ImageControlProcessor& processor, const juce::String& parameterId)
{
    return plainValue(*processor.getParameters().getParameter(parameterId));
}

/// Delivers native-editor Return messages and queued parameter synchronization with a bounded native pump.
inline void dispatchCompactChanges()
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
inline juce::MouseEvent compactMouse(
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
inline juce::MouseWheelDetails compactNotch()
{
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = 60.0F / 256.0F;
    return wheel;
}

/// Waits for native editor callbacks without assuming one platform event pump delivers asynchronous messages.
inline bool waitForCompactEditorClose(const juce::Label& label)
{
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 500.0;
    do {
        dispatchCompactChanges();
        if (!label.isBeingEdited()) {
            return true;
        }
    } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    return false;
}

/// Opens the real JUCE text editor with Return and commits through its own Return-key handling.
inline bool enterCompactValue(ControlSlider& slider, const juce::String& text)
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
    if (waitForCompactEditorClose(*label)) {
        return true;
    }
    slider.hideTextBox(true);
    return false;
}

/// Returns exact-entry text and expected plain values for each compact control without duplicating its parser.
inline std::pair<juce::String, double> compactEntryExample(const ControlSlider& slider)
{
    if (slider.getName() == "Width") {
        return {"1.37", 1.37};
    }
    if (slider.getName() == "Pan") {
        return {"25L", -25.0};
    }
    return {"-3.27", -3.27};
}

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
inline juce::Component* editingControl(juce::Component& parent, const juce::String& identifier)
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
inline void pumpEditing(ProcessingToolbar& toolbar, const std::function<bool()>& done)
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
}  // namespace esgrove::image_control::test
