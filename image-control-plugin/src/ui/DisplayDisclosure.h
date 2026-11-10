/// Declares a compact disclosure for optional analysis displays.
/// Expanded displays reveal only a chevron on hover or keyboard focus.
/// Collapsed displays retain a labelled restore handle without a dedicated toolbar.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control
{
/// A keyboard-accessible display toggle with an independent hover presentation.
class DisplayDisclosure final : public juce::Button
{
public:
    explicit DisplayDisclosure(const juce::String& displayName, bool showCollapsedLabel = true);

    void setExpanded(bool expanded);
    void setRevealed(bool shouldReveal);

protected:
    void paintButton(juce::Graphics& graphics, bool highlighted, bool down) override;

private:
    /// Name shown in the collapsed restore handle and accessibility description.
    const juce::String displayName;
    /// Display sections retain a labelled restore handle, while compact controls use an icon-only action.
    const bool labelWhenCollapsed;
    /// Whether the expanded display currently contains the pointer.
    bool revealed {false};
};
}  // namespace esgrove::image_control
