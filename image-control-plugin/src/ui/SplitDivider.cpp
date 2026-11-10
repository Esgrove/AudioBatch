/// Implements SplitDivider: the hover grip, the resize cursor, and the drag reports.

#include "ui/SplitDivider.h"

#include "ui/ImageControlLookAndFeel.h"

#include <functional>
#include <utility>

namespace esgrove::image_control
{
namespace
{
/// Width of the grip, in pixels.
constexpr float GRIP_WIDTH {48.0F};
/// Height of the grip, in pixels.
constexpr float GRIP_HEIGHT {4.0F};
/// The ordinary keyboard step is easy to see, while Shift retains single-pixel control.
constexpr int KEYBOARD_STEP {8};
/// Idle grip opacity keeps the resize affordance visible without competing with the plot.
constexpr float IDLE_GRIP_ALPHA {0.35F};
}  // namespace

/// Names the resize affordance and makes it reachable without a pointer.
SplitDivider::SplitDivider()
{
    setComponentID("displayBandDivider");
    setName("Spectrogram and band controls divider");
    setTitle(getName());
    setDescription("Drag vertically or use Up and Down arrows to resize. Hold Shift for single-pixel steps.");
    setWantsKeyboardFocus(true);
    setHasFocusOutline(true);
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

/// Releases a captured pointer drag before its layout is hidden or replaced.
void SplitDivider::cancelDrag()
{
    dragging = false;
    repaint();
}

/// Cancels captured movement as soon as this divider is hidden.
void SplitDivider::visibilityChanged()
{
    if (!isVisible()) {
        cancelDrag();
    }
}

/// Sets the callback that receives vertical movement since the previous drag report, in pixels, positive downwards.
void SplitDivider::setDragCallback(std::function<void(int)> callback)
{
    onDrag = std::move(callback);
}

/// Shows a subtle idle grip, brightening it during hover, dragging, or keyboard focus.
void SplitDivider::paint(juce::Graphics& graphics)
{
    const auto grip = getLocalBounds().toFloat().withSizeKeepingCentre(GRIP_WIDTH, GRIP_HEIGHT);
    const bool focused = hasKeyboardFocus(false);
    graphics.setColour(
        juce::Colour(dragging || focused ? palette::GREY_MIDDLE_LIGHT : palette::GREY_MIDDLE)
            .withAlpha(hovered || dragging || focused ? 1.0F : IDLE_GRIP_ALPHA)
    );
    graphics.fillRoundedRectangle(grip, GRIP_HEIGHT * 0.5F);
}

/// Routes plain arrow keys through the same editor clamp as pointer movement, leaving host shortcuts untouched.
bool SplitDivider::keyPressed(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    if (!isEnabled() || dragging || modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown()) {
        return false;
    }
    const int keyCode = key.getKeyCode();
    if (keyCode != juce::KeyPress::upKey && keyCode != juce::KeyPress::downKey) {
        return false;
    }
    if (onDrag) {
        const int step = modifiers.isShiftDown() ? 1 : KEYBOARD_STEP;
        onDrag(keyCode == juce::KeyPress::upKey ? -step : step);
    }
    return true;
}

/// Brightens the grip while it is the keyboard resize target.
void SplitDivider::focusGained(juce::Component::FocusChangeType /*cause*/)
{
    repaint();
}

/// Restores the ordinary idle or hovered grip after focus leaves.
void SplitDivider::focusLost(juce::Component::FocusChangeType /*cause*/)
{
    repaint();
}

/// Shows the grip.
void SplitDivider::mouseEnter(const juce::MouseEvent& /*event*/)
{
    hovered = true;
    repaint();
}

/// Returns the grip to its subdued idle appearance.
void SplitDivider::mouseExit(const juce::MouseEvent& /*event*/)
{
    hovered = false;
    repaint();
}

/// Starts a drag.
void SplitDivider::mouseDown(const juce::MouseEvent& event)
{
    if (!isEnabled() || event.mods.withOnlyMouseButtons().getRawFlags() != juce::ModifierKeys::leftButtonModifier) {
        return;
    }
    dragging = true;
    lastScreenY = event.getScreenY();
    repaint();
}

/// Reports the movement since the previous report.
/// The divider moves with the layout, so the movement is measured on the screen.
void SplitDivider::mouseDrag(const juce::MouseEvent& event)
{
    if (!isEnabled() || !dragging
        || event.mods.withOnlyMouseButtons().getRawFlags() != juce::ModifierKeys::leftButtonModifier)
    {
        return;
    }
    const int screenY = event.getScreenY();
    const int delta = screenY - lastScreenY;
    lastScreenY = screenY;
    if (delta != 0 && onDrag) {
        onDrag(delta);
    }
}

/// Ends the drag.
void SplitDivider::mouseUp(const juce::MouseEvent& /*event*/)
{
    cancelDrag();
}
}  // namespace esgrove::image_control
