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
}  // namespace

/// Sets the vertical resize cursor.
SplitDivider::SplitDivider()
{
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
}

/// Sets the callback that receives vertical movement since the previous drag report, in pixels, positive downwards.
void SplitDivider::setDragCallback(std::function<void(int)> callback)
{
    onDrag = std::move(callback);
}

/// Draws the centred grip while the pointer is over the divider or dragging it.
void SplitDivider::paint(juce::Graphics& graphics)
{
    if (!hovered && !dragging) {
        return;
    }
    const auto grip = getLocalBounds().toFloat().withSizeKeepingCentre(GRIP_WIDTH, GRIP_HEIGHT);
    graphics.setColour(juce::Colour(dragging ? palette::GREY_MIDDLE_LIGHT : palette::GREY_MIDDLE));
    graphics.fillRoundedRectangle(grip, GRIP_HEIGHT * 0.5F);
}

/// Shows the grip.
void SplitDivider::mouseEnter(const juce::MouseEvent& /*event*/)
{
    hovered = true;
    repaint();
}

/// Hides the grip.
void SplitDivider::mouseExit(const juce::MouseEvent& /*event*/)
{
    hovered = false;
    repaint();
}

/// Starts a drag.
void SplitDivider::mouseDown(const juce::MouseEvent& event)
{
    dragging = true;
    lastScreenY = event.getScreenY();
    repaint();
}

/// Reports the movement since the previous report.
/// The divider moves with the layout, so the movement is measured on the screen.
void SplitDivider::mouseDrag(const juce::MouseEvent& event)
{
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
    dragging = false;
    repaint();
}
}  // namespace esgrove::image_control
