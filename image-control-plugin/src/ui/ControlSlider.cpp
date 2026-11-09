/// Implements ControlSlider: the drag behaviour, the arrow key nudges, and the mouse wheel steps.

#include "ui/ControlSlider.h"

#include <algorithm>
#include <cmath>

namespace esgrove::image_control
{
namespace
{
/// Drag sensitivity while Shift is held, relative to a normal drag.
constexpr double FINE_DRAG_SENSITIVITY {0.15};
/// Smallest pointer movement in pixels that moves the slider while dragging finely.
constexpr int FINE_DRAG_THRESHOLD {1};
/// Number of nudges that cross the whole range with the arrow keys.
constexpr double NUDGES_PER_RANGE {100.0};
/// Share of a nudge used while Shift is held.
constexpr double FINE_NUDGE_SHARE {0.1};
/// Wheel movement of one notch of a mouse wheel on Windows, as JUCE reports it.
/// A notched wheel event of at least this much takes one step per notch.
constexpr float WHEEL_NOTCH_DELTA {60.0F / 256.0F};
/// Smooth wheel movement, such as from a trackpad, that takes one step.
constexpr float SMOOTH_WHEEL_DELTA_PER_STEP {0.05F};
}  // namespace

/// Turns a mouse wheel event into a signed number of whole steps, positive for scrolling up or away.
/// A notched wheel takes at least one step per event, and one per Windows notch,
/// since the movement of one notch differs between the platforms and the mice.
/// Smooth movement, such as from a trackpad, adds up in the remainder,
/// which keeps what is left over for the next event, so slow scrolling still moves.
int wheelSteps(const juce::MouseWheelDetails& wheel, float& remainder)
{
    float delta = std::abs(wheel.deltaX) > std::abs(wheel.deltaY) ? -wheel.deltaX : wheel.deltaY;
    if (wheel.isReversed) {
        delta = -delta;
    }
    if (!wheel.isSmooth) {
        remainder = 0.0F;
        if (juce::exactlyEqual(delta, 0.0F)) {
            return 0;
        }
        const int notches = std::max(1, static_cast<int>(std::lround(std::abs(delta) / WHEEL_NOTCH_DELTA)));
        return delta > 0.0F ? notches : -notches;
    }
    remainder += delta / SMOOTH_WHEEL_DELTA_PER_STEP;
    const float steps = std::trunc(remainder);
    remainder -= steps;
    return static_cast<int>(steps);
}

/// Creates a horizontal slider that drags relative to its grab point, with Shift switching to a slow drag.
ControlSlider::ControlSlider()
{
    setSliderStyle(juce::Slider::LinearHorizontal);
    setSliderSnapsToMousePosition(false);
    setVelocityModeParameters(FINE_DRAG_SENSITIVITY, FINE_DRAG_THRESHOLD, 0.0, true, juce::ModifierKeys::shiftModifier);
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
}

/// Closes a pointer gesture while its listeners and parameter attachments still exist.
ControlSlider::~ControlSlider()
{
    endDrag();
}

/// Ends a pointer drag through JUCE, which balances both slider listeners and user callbacks.
void ControlSlider::endDrag()
{
    if (dragStartEvent.has_value()) {
        juce::Slider::mouseUp(*dragStartEvent);
        dragStartEvent.reset();
    }
}

/// Ends the pointer gesture before JUCE opens the separate double-click reset gesture.
void ControlSlider::mouseDoubleClick(const juce::MouseEvent& event)
{
    endDrag();
    juce::Slider::mouseDoubleClick(event);
}

/// Starts a pointer gesture, retaining its event so owners can close it before detaching listeners.
void ControlSlider::mouseDown(const juce::MouseEvent& event)
{
    endDrag();
    if (!isEnabled() || !event.mods.isLeftButtonDown()) {
        return;
    }
    dragStartEvent.emplace(event);
    juce::Slider::mouseDown(event);
}

/// Ends JUCE's pointer gesture and forgets the retained event.
void ControlSlider::mouseUp(const juce::MouseEvent& event)
{
    juce::Slider::mouseUp(event);
    dragStartEvent.reset();
}

/// Sets the change of the value per mouse wheel step, such as 0.1 dB for a gain or one percent for a pan.
void ControlSlider::setWheelStep(const double step)
{
    wheelStep = step;
}

/// Nudges the value with the arrow keys: right and up increase it, left and down decrease it,
/// a hundredth of the range per press, or a tenth of that with Shift.
/// Each nudge is its own host gesture, which the slider's attachment passes on.
bool ControlSlider::keyPressed(const juce::KeyPress& key)
{
    if (!isEnabled() || dragStartEvent.has_value()) {
        return false;
    }
    const int keyCode = key.getKeyCode();
    double direction = 0.0;
    if (keyCode == juce::KeyPress::rightKey || keyCode == juce::KeyPress::upKey) {
        direction = 1.0;
    } else if (keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::downKey) {
        direction = -1.0;
    } else {
        return juce::Slider::keyPressed(key);
    }
    const juce::Range<double> range {getMinimum(), getMaximum()};
    const double nudge
        = range.getLength() / NUDGES_PER_RANGE * (key.getModifiers().isShiftDown() ? FINE_NUDGE_SHARE : 1.0);
    const ScopedDragNotification gesture(*this);
    setValue(std::clamp(getValue() + direction * nudge, range.getStart(), range.getEnd()), juce::sendNotificationSync);
    return true;
}

/// Moves the value by the wheel step per wheel step, such as 0.1 dB on a gain slider,
/// or a tenth of that with Shift, but never less than the slider's interval.
/// Each wheel event is its own host gesture, like a key nudge,
/// so Link moves Side with Mid as it does while Mid is dragged.
/// A disabled slider leaves the wheel to juce::Slider, which passes it on to its parent.
void ControlSlider::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (!isEnabled()) {
        juce::Slider::mouseWheelMove(event, wheel);
        return;
    }
    if (dragStartEvent.has_value()) {
        return;
    }
    const int steps = wheelSteps(wheel, wheelRemainder);
    if (steps == 0) {
        return;
    }
    const double step = std::max(wheelStep * (event.mods.isShiftDown() ? FINE_WHEEL_SHARE : 1.0), getInterval());
    const juce::Range<double> range {getMinimum(), getMaximum()};
    const ScopedDragNotification gesture(*this);
    setValue(
        std::clamp(getValue() + static_cast<double>(steps) * step, range.getStart(), range.getEnd()),
        juce::sendNotificationSync
    );
}
}  // namespace esgrove::image_control
