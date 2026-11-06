/// Implements TooltipOverlay: finding the tooltip under the pointer, the rest delay,
/// the time-based fade and slide, the placement inside the editor, and the bubble painting.

#include "ui/TooltipOverlay.h"

#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <cmath>

namespace esgrove::image_control
{
namespace
{
/// Time the pointer rests over a control before its tooltip appears, in seconds.
constexpr double REST_SECONDS {0.55};
/// Time the bubble takes to fade in, in seconds.
constexpr double FADE_IN_SECONDS {0.12};
/// Time the bubble takes to fade out, in seconds.
constexpr double FADE_OUT_SECONDS {0.08};
/// Pointer movement that counts as moving rather than resting, in pixels.
constexpr float MOVE_THRESHOLD {2.0F};
/// Distance the bubble slides up while it fades in, in pixels.
constexpr float SLIDE_DISTANCE {5.0F};
/// Widest the text of a bubble wraps at, in pixels.
constexpr float MAX_TEXT_WIDTH {300.0F};
/// Font height of the tooltip text.
constexpr float FONT_HEIGHT {13.0F};
/// Space between the text and the bubble edge, in pixels.
constexpr int PADDING {8};
/// Room around the bubble for its shadow and slide, in pixels.
constexpr int MARGIN {10};
/// Offset of the bubble from the pointer, in pixels.
constexpr juce::Point<int> POINTER_OFFSET {14, 20};
/// Corner radius of the bubble, in pixels.
constexpr float CORNER_RADIUS {6.0F};
/// Opacity of the bubble background.
constexpr float BACKGROUND_ALPHA {0.97F};
/// Radius of the bubble shadow, in pixels.
constexpr int SHADOW_RADIUS {10};
/// Opacity of the bubble shadow.
constexpr float SHADOW_ALPHA {0.55F};
}  // namespace

/// Creates a hidden overlay for the tooltips of a root component, which never takes mouse clicks.
TooltipOverlay::TooltipOverlay(juce::Component& rootComponent) : root(rootComponent)
{
    setInterceptsMouseClicks(false, false);
    setVisible(false);
}

/// Advances the tooltip by the time since the last display refresh.
/// A tooltip appears once the pointer has rested over its control for REST_SECONDS,
/// and fades out when the pointer moves to a control with another tooltip or none, or a button goes down.
void TooltipOverlay::advance(const double seconds)
{
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto pointer = source.getScreenPosition();
    const bool moved = pointer.getDistanceFrom(lastPointer) > MOVE_THRESHOLD;
    lastPointer = pointer;
    if (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown()) {
        suppressed = true;
    } else if (moved) {
        suppressed = false;
    }

    const auto candidate = suppressed ? juce::String() : tooltipUnderPointer();
    if (candidate.isEmpty() || candidate != text) {
        showing = false;
    }
    if (!showing) {
        restSeconds = moved || candidate.isEmpty() ? 0.0 : restSeconds + seconds;
        if (opacity <= 0.0F && restSeconds >= REST_SECONDS && candidate.isNotEmpty()) {
            text = candidate;
            showing = true;
            placeNear(root.getLocalPoint(nullptr, pointer.roundToInt()));
            toFront(false);
        }
    }

    const auto step = static_cast<float>(seconds / (showing ? FADE_IN_SECONDS : FADE_OUT_SECONDS));
    const float previous = opacity;
    opacity = std::clamp(opacity + (showing ? step : -step), 0.0F, 1.0F);
    setVisible(opacity > 0.0F);
    if (!juce::approximatelyEqual(previous, opacity)) {
        setAlpha(opacity);
        repaint();
    }
}

/// Paints the bubble with its shadow, sliding up into place as it fades in.
void TooltipOverlay::paint(juce::Graphics& graphics)
{
    const float slide = (1.0F - opacity) * SLIDE_DISTANCE;
    const auto bubble = getLocalBounds().reduced(MARGIN).toFloat().translated(0.0F, slide);

    juce::Path outline;
    outline.addRoundedRectangle(bubble, CORNER_RADIUS);
    juce::DropShadow(juce::Colours::black.withAlpha(SHADOW_ALPHA), SHADOW_RADIUS, {0, 2})
        .drawForPath(graphics, outline);
    graphics.setColour(juce::Colour(palette::GREY_MEDIUM_DARK).withAlpha(BACKGROUND_ALPHA));
    graphics.fillPath(outline);
    graphics.setColour(juce::Colour(palette::GREY_MEDIUM));
    graphics.strokePath(outline, juce::PathStrokeType(1.0F));
    layout.draw(graphics, bubble.reduced(static_cast<float>(PADDING)));
}

/// Returns the tooltip of the component under the pointer, or of its nearest parent with one,
/// when that component is inside the root. Returns an empty string otherwise.
juce::String TooltipOverlay::tooltipUnderPointer() const
{
    auto* component = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    if (component == nullptr || (component != &root && !root.isParentOf(component))) {
        return {};
    }
    for (; component != nullptr && component != &root; component = component->getParentComponent()) {
        if (auto* client = dynamic_cast<juce::TooltipClient*>(component)) {
            if (auto tooltip = client->getTooltip(); tooltip.isNotEmpty()) {
                return tooltip;
            }
        }
    }
    return {};
}

/// Lays out the text and places the bubble below and to the right of the pointer, kept inside the root.
void TooltipOverlay::placeNear(const juce::Point<int> pointer)
{
    juce::AttributedString attributed;
    attributed.append(text, textFont(FONT_HEIGHT), juce::Colour(palette::GREY_SUPER_LIGHT));
    attributed.setWordWrap(juce::AttributedString::byWord);
    layout.createLayout(attributed, MAX_TEXT_WIDTH);

    const int width = juce::roundToInt(std::ceil(layout.getWidth())) + (PADDING + MARGIN) * 2;
    const int height = juce::roundToInt(std::ceil(layout.getHeight())) + (PADDING + MARGIN) * 2;
    auto bounds
        = juce::Rectangle<int>(width, height).withPosition(pointer + POINTER_OFFSET - juce::Point {MARGIN, MARGIN});
    bounds = bounds.constrainedWithin(root.getLocalBounds());
    if (bounds.contains(pointer)) {
        // Below the pointer would cover it, so the bubble goes above it.
        bounds.setY(std::max(0, pointer.y - height - POINTER_OFFSET.y / 2));
    }
    setBounds(bounds);
}
}  // namespace esgrove::image_control
