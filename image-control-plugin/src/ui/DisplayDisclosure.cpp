/// Implements the contextual display chevron and compact labelled restore handle.
/// Presentation changes never remove the button from keyboard or accessibility traversal.

#include "ui/DisplayDisclosure.h"

#include "ui/ImageControlLookAndFeel.h"

namespace esgrove::image_control
{
namespace
{
/// Disclosure geometry matches compact header buttons and the display's quiet labels.
constexpr float CORNER_RADIUS {3.0F};
constexpr float ICON_STROKE {1.5F};
constexpr float LABEL_HEIGHT {11.0F};
}  // namespace
/// Creates a focusable toggle whose state describes display visibility.
DisplayDisclosure::DisplayDisclosure(const juce::String& name) : Button(name), displayName(name)
{
    setWantsKeyboardFocus(true);
    setHasFocusOutline(true);
    setToggleable(true);
    setExpanded(true);
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

/// Reveals an expanded display's chevron without changing its hit target or focusability.
void DisplayDisclosure::setRevealed(const bool shouldReveal)
{
    if (revealed != shouldReveal) {
        revealed = shouldReveal;
        repaint();
    }
}

/// Draws a restrained local action, or a persistent restore handle when the display is collapsed.
void DisplayDisclosure::paintButton(juce::Graphics& graphics, const bool highlighted, const bool down)
{
    const bool expanded = getToggleState();
    if (expanded && !revealed && !highlighted && !hasKeyboardFocus(true)) {
        return;
    }
    auto area = getLocalBounds().toFloat();
    graphics.setColour(juce::Colour(palette::GREY_DARK));
    graphics.fillRoundedRectangle(area, CORNER_RADIUS);
    if (highlighted || down) {
        graphics.setColour(juce::Colour(palette::GREY_MEDIUM_DARK));
        graphics.fillRoundedRectangle(area, CORNER_RADIUS);
    }
    const auto icon = area.removeFromLeft(24.0F).withSizeKeepingCentre(10.0F, 5.0F);
    juce::Path chevron;
    const float tip = expanded ? icon.getY() : icon.getBottom();
    const float edge = expanded ? icon.getBottom() : icon.getY();
    chevron.startNewSubPath(icon.getX(), edge);
    chevron.lineTo(icon.getCentreX(), tip);
    chevron.lineTo(icon.getRight(), edge);
    graphics.setColour(juce::Colour(highlighted ? palette::GREY_SUPER_LIGHT : palette::GREY_MIDDLE_LIGHT));
    graphics.strokePath(
        chevron, juce::PathStrokeType(ICON_STROKE, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
    );
    if (!expanded) {
        graphics.setFont(textFont(LABEL_HEIGHT));
        graphics.drawText(displayName, area, juce::Justification::centredLeft);
    }
}
}  // namespace esgrove::image_control
