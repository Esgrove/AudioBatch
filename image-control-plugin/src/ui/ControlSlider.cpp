/// Implements ControlSlider: the drag behaviour and the arrow key nudges.

#include "ui/ControlSlider.h"

#include <algorithm>

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
}  // namespace

/// Creates a horizontal slider that drags relative to its grab point, with Shift switching to a slow drag.
ControlSlider::ControlSlider()
{
    setSliderStyle(juce::Slider::LinearHorizontal);
    setSliderSnapsToMousePosition(false);
    setVelocityModeParameters(FINE_DRAG_SENSITIVITY, FINE_DRAG_THRESHOLD, 0.0, true, juce::ModifierKeys::shiftModifier);
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
}

/// Nudges the value with the arrow keys: right and up increase it, left and down decrease it,
/// a hundredth of the range per press, or a tenth of that with Shift.
/// Each nudge is its own host gesture, which the slider's attachment passes on.
bool ControlSlider::keyPressed(const juce::KeyPress& key)
{
    const int keyCode = key.getKeyCode();
    double direction = 0.0;
    if (keyCode == juce::KeyPress::rightKey || keyCode == juce::KeyPress::upKey) {
        direction = 1.0;
    } else if (keyCode == juce::KeyPress::leftKey || keyCode == juce::KeyPress::downKey) {
        direction = -1.0;
    } else {
        return juce::Slider::keyPressed(key);
    }
    const auto range = getRange();
    const double nudge
        = range.getLength() / NUDGES_PER_RANGE * (key.getModifiers().isShiftDown() ? FINE_NUDGE_SHARE : 1.0);
    const ScopedDragNotification gesture(*this);
    setValue(std::clamp(getValue() + direction * nudge, range.getStart(), range.getEnd()), juce::sendNotificationSync);
    return true;
}
}  // namespace esgrove::image_control
