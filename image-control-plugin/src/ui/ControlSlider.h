/// Declares ControlSlider, the slider of the ImageControl editor.
/// It drags relative to where it is grabbed instead of jumping to the pointer,
/// drags finely while Shift is held, resets on double-click, nudges with the arrow keys once clicked,
/// and moves in fixed steps with the mouse wheel.
/// Also declares wheelSteps, which turns mouse wheel movement into whole steps for every wheel control of the editor.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control
{
/// Default change of a slider per mouse wheel step, which is 0.1 dB on the gain sliders.
constexpr double DEFAULT_WHEEL_STEP {0.1};
/// Share of a mouse wheel step used while Shift is held.
constexpr double FINE_WHEEL_SHARE {0.1};

[[nodiscard]] int wheelSteps(const juce::MouseWheelDetails& wheel, float& remainder);

/// A horizontal slider with relative dragging, fine adjustment, keyboard nudging, and fixed mouse wheel steps.
class ControlSlider final : public juce::Slider
{
public:
    ControlSlider();

    void setWheelStep(double step);

    bool keyPressed(const juce::KeyPress& key) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
    /// Change of the value per mouse wheel step.
    double wheelStep {DEFAULT_WHEEL_STEP};
    /// Smooth wheel movement not yet turned into a whole step, carried to the next wheel event.
    float wheelRemainder {0.0F};
};
}  // namespace esgrove::image_control
