#pragma once

#include <JuceHeader.h>

#include <cmath>

/// Slider that steps by exactly one interval per mouse-wheel notch, regardless of wheel velocity.
class IntervalStepSlider : public juce::Slider
{
public:
    using juce::Slider::Slider;

    /// Steps the value by one interval in the wheel direction, honouring reversed scrolling.
    /// This replaces the default JUCE behaviour where fast wheel movement can jump several intervals.
    /// Falls back to the stock handling when the slider is disabled or the vertical delta is zero.
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (!isEnabled() || std::abs(wheel.deltaY) <= 0.0f) {
            juce::Slider::mouseWheelMove(event, wheel);
            return;
        }

        const auto interval = getInterval() > 0.0 ? getInterval() : 1.0;
        const auto direction = wheel.isReversed ? -1.0 : 1.0;
        const auto step = (wheel.deltaY > 0.0f ? 1.0 : -1.0) * direction * interval;
        setValue(getValue() + step, juce::sendNotificationSync);
    }
};
