/// Declares TooltipOverlay, the tooltips of the ImageControl editor.
/// It draws the tooltip of the control under the pointer as a bubble inside the editor,
/// fading and sliding it in after the pointer rests and fading it out when the pointer moves on,
/// instead of JUCE's tooltip window, which pops up and vanishes without animation in a separate desktop window.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control
{
/// A tooltip bubble that the editor advances on every display refresh.
/// It shows the tooltip of the component under the pointer, or of its nearest parent with one,
/// for components inside the root it belongs to, including the settings call-out.
class TooltipOverlay final : public juce::Component
{
public:
    explicit TooltipOverlay(juce::Component& rootComponent);

    void advance(double seconds);
    void paint(juce::Graphics& graphics) override;

private:
    [[nodiscard]] juce::String tooltipUnderPointer() const;

    void placeNear(juce::Point<int> pointer);

    /// The editor the tooltips belong to, which owns this overlay.
    juce::Component& root;
    /// Text of the bubble being shown or fading out.
    juce::String text;
    /// The laid out text of the bubble.
    juce::TextLayout layout;
    /// Pointer position at the previous refresh, in screen coordinates.
    juce::Point<float> lastPointer;

    /// Time the pointer has rested over the current tooltip text, in seconds.
    double restSeconds {0.0};
    /// Current opacity of the bubble, from 0 to 1.
    float opacity {0.0F};
    /// Whether the bubble is fading in or shown, rather than fading out or hidden.
    bool showing {false};
    /// Whether a mouse button went down since the pointer last moved, which hides tooltips until it moves again.
    bool suppressed {false};
};
}  // namespace esgrove::image_control
