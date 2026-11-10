/// Implements quiet contextual chevrons and full-width collapsed restore rows.
/// Glyphs remain small while keyboard and pointer targets preserve display visibility access.

#include "ui/DisplayDisclosure.h"

#include "ui/ImageControlLookAndFeel.h"

namespace esgrove::image_control
{
namespace
{
/// Small neutral glyphs fit the ordinary display margins without becoming prominent controls.
constexpr float ICON_WIDTH {7.0F};
constexpr float ICON_HEIGHT {3.0F};
constexpr float ICON_STROKE {1.0F};
constexpr float ICON_TARGET_WIDTH {20.0F};
constexpr float LABEL_HEIGHT {11.0F};

/// Draws a font-independent directional chevron inside a stable pointer target.
void paintChevron(const juce::Graphics& graphics, const juce::Rectangle<float> target, const bool expanded)
{
    const auto icon = target.withSizeKeepingCentre(ICON_WIDTH, ICON_HEIGHT);
    const float tip = expanded ? icon.getY() : icon.getBottom();
    const float edge = expanded ? icon.getBottom() : icon.getY();
    juce::Path chevron;
    chevron.startNewSubPath(icon.getX(), edge);
    chevron.lineTo(icon.getCentreX(), tip);
    chevron.lineTo(icon.getRight(), edge);
    graphics.strokePath(
        chevron, juce::PathStrokeType(ICON_STROKE, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
    );
}
}  // namespace

/// Creates a focusable toggle whose state describes display visibility.
DisplayDisclosure::DisplayDisclosure(const juce::String& name, const bool showCollapsedLabel) :
    Button(name),
    displayName(name),
    labelWhenCollapsed(showCollapsedLabel)
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

/// Uses only a neutral foreground change on hover, without painting a broad background highlight.
void DisplayDisclosure::paintButton(juce::Graphics& graphics, const bool highlighted, const bool down)
{
    const bool expanded = getToggleState();
    if (expanded && !revealed && !highlighted && !hasKeyboardFocus(true)) {
        return;
    }
    auto area = getLocalBounds().toFloat();
    graphics.setColour(juce::Colour(highlighted || down ? palette::GREY_LIGHT : palette::GREY_MIDDLE_LIGHT));
    if (!expanded && labelWhenCollapsed) {
        paintChevron(graphics, area.removeFromLeft(ICON_TARGET_WIDTH), false);
        paintChevron(graphics, area.removeFromRight(ICON_TARGET_WIDTH), false);
        graphics.setFont(textFont(LABEL_HEIGHT));
        graphics.drawText(displayName, area, juce::Justification::centredLeft);
        return;
    }
    paintChevron(graphics, area, expanded);
}
}  // namespace esgrove::image_control
