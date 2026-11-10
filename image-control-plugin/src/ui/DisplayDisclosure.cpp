/// Implements centered hover triangles and full-width collapsed restore rows.
/// Pointer focus stays quiet while keyboard traversal retains a neutral focus cue.

#include "ui/DisplayDisclosure.h"

#include "ui/ImageControlLookAndFeel.h"

namespace esgrove::image_control
{
namespace
{
/// Filled translucent triangles remain legible without competing with the analysis displays.
constexpr float ICON_WIDTH {16.0F};
constexpr float ICON_HEIGHT {8.0F};
constexpr float ICON_ALPHA {0.55F};
constexpr float PRESSED_ALPHA {0.8F};
constexpr float LABEL_HEIGHT {11.0F};
constexpr float LABEL_INSET {4.0F};
constexpr float FOCUS_CORNER_RADIUS {3.0F};
}  // namespace

/// Creates a focusable toggle whose state describes display visibility.
DisplayDisclosure::DisplayDisclosure(const juce::String& name, const bool showCollapsedLabel) :
    Button(name),
    displayName(name),
    labelWhenCollapsed(showCollapsedLabel)
{
    setWantsKeyboardFocus(true);
    setHasFocusOutline(false);
    setToggleable(true);
    setExpanded(true);
}

/// Reports the hover-only glyph policy independently of keyboard focus styling.
bool DisplayDisclosure::isIndicatorVisible() const
{
    return revealed || isMouseOver() || isDown();
}

/// Updates the accessible action and visible collapsed label together.
void DisplayDisclosure::setExpanded(const bool expanded)
{
    const auto action = (expanded ? "Hide " : "Show ") + displayName;
    if (getToggleState() == expanded && getTitle() == action) {
        return;
    }
    setToggleState(expanded, juce::dontSendNotification);
    setTitle(action);
    setDescription(action);
    setTooltip(action);
    repaint();
}

/// Reveals a display's triangle without changing its hit target or focusability.
void DisplayDisclosure::setRevealed(const bool shouldReveal)
{
    if (revealed != shouldReveal) {
        revealed = shouldReveal;
        repaint();
    }
}

/// Shows a neutral outline only when keyboard traversal acquired the focus.
void DisplayDisclosure::focusGained(const juce::Component::FocusChangeType cause)
{
    keyboardFocus = cause == juce::Component::focusChangedByTabKey;
    juce::Button::focusGained(cause);
    repaint();
}

/// Clears the traversal cue when focus leaves the section action.
void DisplayDisclosure::focusLost(const juce::Component::FocusChangeType cause)
{
    keyboardFocus = false;
    juce::Button::focusLost(cause);
    repaint();
}

/// Draws a centered hover triangle without painting a selected section background.
void DisplayDisclosure::paintButton(juce::Graphics& graphics, const bool highlighted, const bool down)
{
    const auto area = getLocalBounds().toFloat();
    if (!getToggleState() && labelWhenCollapsed) {
        graphics.setColour(juce::Colour(palette::GREY_MIDDLE_LIGHT));
        graphics.setFont(textFont(LABEL_HEIGHT));
        graphics.drawText(displayName, area.reduced(LABEL_INSET, 0.0F), juce::Justification::centredLeft);
    }
    if (keyboardFocus && hasKeyboardFocus(false)) {
        graphics.setColour(juce::Colour(palette::GREY_LIGHT).withAlpha(ICON_ALPHA));
        graphics.drawRoundedRectangle(area.reduced(1.0F), FOCUS_CORNER_RADIUS, 1.0F);
    }
    if (!isIndicatorVisible() && !highlighted && !down) {
        return;
    }
    const auto icon = area.withSizeKeepingCentre(ICON_WIDTH, ICON_HEIGHT);
    const float tip = getToggleState() ? icon.getY() : icon.getBottom();
    const float edge = getToggleState() ? icon.getBottom() : icon.getY();
    juce::Path triangle;
    triangle.addTriangle(icon.getX(), edge, icon.getCentreX(), tip, icon.getRight(), edge);
    graphics.setColour(juce::Colour(palette::GREY_LIGHT).withAlpha(down ? PRESSED_ALPHA : ICON_ALPHA));
    graphics.fillPath(triangle);
}
}  // namespace esgrove::image_control
