/// Implements centered hover triangles and full-width collapsed restore rows.
/// Pointer focus stays quiet while keyboard traversal retains a neutral focus cue.

#include "ui/DisplayDisclosure.h"

#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace esgrove::image_control
{
namespace
{
/// Filled translucent triangles remain legible without competing with the analysis displays.
constexpr float ICON_WIDTH {36.0F};
constexpr float ICON_HEIGHT {8.0F};
constexpr float ICON_ALPHA {0.18F};
constexpr float PRESSED_ALPHA {0.3F};
/// Narrow vertical proximity keeps the action contextual instead of revealing it throughout the section.
constexpr float ACTIVATION_HORIZONTAL_PADDING {18.0F};
constexpr float ACTIVATION_VERTICAL_PADDING {6.0F};
/// A finite eased fade has no delay or long exponential tail after the pointer leaves.
constexpr double FADE_IN_SECONDS {0.14};
constexpr double FADE_OUT_SECONDS {0.1};
constexpr float LABEL_HEIGHT {11.0F};
constexpr float LABEL_INSET {4.0F};
}  // namespace

/// Creates a focusable toggle whose state describes display visibility.
DisplayDisclosure::DisplayDisclosure(
    const juce::String& name,
    const bool showCollapsedLabel,
    const bool closesDownwards
) :
    Button(name),
    displayName(name),
    labelWhenCollapsed(showCollapsedLabel),
    collapseDownwards(closesDownwards)
{
    setWantsKeyboardFocus(true);
    setHasFocusOutline(false);
    setToggleable(true);
    setExpanded(true);
}

/// Restricts pointer activation to the immediate neighbourhood of the triangle, even on a full-width restore row.
juce::Rectangle<float> DisplayDisclosure::getActivationBounds() const
{
    return getIndicatorBounds().expanded(ACTIVATION_HORIZONTAL_PADDING, ACTIVATION_VERTICAL_PADDING);
}

/// Returns the painted triangle's geometry for layout alignment with the section border.
juce::Rectangle<float> DisplayDisclosure::getIndicatorBounds() const
{
    return getLocalBounds().toFloat().withSizeKeepingCentre(ICON_WIDTH, ICON_HEIGHT);
}

/// Points toward the direction in which this section closes, and reverses the arrow when it is collapsed.
bool DisplayDisclosure::isIndicatorPointingDown() const
{
    return getToggleState() == collapseDownwards;
}

/// Reports the hover-only glyph policy independently of keyboard focus styling.
bool DisplayDisclosure::isIndicatorVisible() const
{
    return revealed;
}

/// Eases the current fade progress into a restrained foreground alpha.
float DisplayDisclosure::getIndicatorOpacity() const
{
    const float eased = revealProgress * revealProgress * (3.0F - 2.0F * revealProgress);
    return eased * (isDown() ? PRESSED_ALPHA : ICON_ALPHA);
}

/// Advances from the editor's display refresh without an independent timer or delayed hide callback.
void DisplayDisclosure::advance(const double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0) {
        return;
    }
    const auto step = static_cast<float>(seconds / (revealed ? FADE_IN_SECONDS : FADE_OUT_SECONDS));
    const float next = std::clamp(revealProgress + (revealed ? step : -step), 0.0F, 1.0F);
    if (!juce::approximatelyEqual(next, revealProgress)) {
        revealProgress = next;
        repaint();
    }
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

/// Retargets the fade immediately when the pointer crosses the small local proximity region.
void DisplayDisclosure::setPointerPosition(const juce::Point<float> position)
{
    setRevealed(isEnabled() && isVisible() && getActivationBounds().contains(position));
}

/// Changes the fade target without changing the pointer hit target or keyboard focusability.
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
void DisplayDisclosure::paintButton(juce::Graphics& graphics, const bool /*highlighted*/, const bool /*down*/)
{
    const auto area = getLocalBounds().toFloat();
    if (!getToggleState() && labelWhenCollapsed) {
        graphics.setColour(juce::Colour(palette::GREY_MIDDLE_LIGHT));
        graphics.setFont(textFont(LABEL_HEIGHT));
        graphics.drawText(displayName, area.reduced(LABEL_INSET, 0.0F), juce::Justification::centredLeft);
    }
    if (keyboardFocus && hasKeyboardFocus(false)) {
        graphics.setColour(juce::Colour(palette::GREY_LIGHT).withAlpha(ICON_ALPHA));
        graphics.drawRoundedRectangle(area.reduced(1.0F), corner_radius::CONTROL, 1.0F);
    }
    const float opacity = getIndicatorOpacity();
    if (opacity <= 0.0F) {
        return;
    }
    const auto icon = getIndicatorBounds();
    const float tip = isIndicatorPointingDown() ? icon.getBottom() : icon.getY();
    const float edge = isIndicatorPointingDown() ? icon.getY() : icon.getBottom();
    juce::Path triangle;
    triangle.addTriangle(icon.getX(), edge, icon.getCentreX(), tip, icon.getRight(), edge);
    graphics.setColour(juce::Colour(palette::GREY_LIGHT).withAlpha(opacity));
    graphics.fillPath(triangle);
}
}  // namespace esgrove::image_control
