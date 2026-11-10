/// Declares a compact disclosure for optional analysis displays.
/// Displays reveal a translucent centered triangle on hover.
/// Collapsed displays retain a labelled restore handle without a dedicated toolbar.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control
{
/// A keyboard-accessible display toggle with an independent hover presentation.
class DisplayDisclosure final : public juce::Button
{
public:
    explicit DisplayDisclosure(
        const juce::String& displayName,
        bool showCollapsedLabel = true,
        bool closesDownwards = false
    );

    [[nodiscard]] juce::Rectangle<float> getIndicatorBounds() const;
    [[nodiscard]] bool isIndicatorPointingDown() const;
    [[nodiscard]] bool isIndicatorVisible() const;
    void setExpanded(bool expanded);
    void setRevealed(bool shouldReveal);

protected:
    void focusGained(juce::Component::FocusChangeType cause) override;
    void focusLost(juce::Component::FocusChangeType cause) override;
    void paintButton(juce::Graphics& graphics, bool highlighted, bool down) override;

private:
    /// Name shown in the collapsed restore handle and accessibility description.
    const juce::String displayName;
    /// Display sections retain a labelled restore handle, while compact controls use an icon-only action.
    const bool labelWhenCollapsed;
    /// Correlation and controls collapse toward the bottom, while the spectrum closes upward.
    const bool collapseDownwards;
    /// Whether Tab traversal acquired focus rather than a pointer action.
    bool keyboardFocus {false};
    /// Whether the associated display currently contains the pointer.
    bool revealed {false};
};
}  // namespace esgrove::image_control
