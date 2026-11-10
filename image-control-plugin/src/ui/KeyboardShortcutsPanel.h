/// Declares KeyboardShortcutsPanel, a read-only native reference for the editor's local controls.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>

namespace esgrove::image_control
{
/// Owns grouped action and key labels without editor state, sound parameters, or external content.
/// The editor owns this component and its surrounding window, including the window's close behavior.
class KeyboardShortcutsPanel final : public juce::Component
{
public:
    KeyboardShortcutsPanel();

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    static constexpr std::size_t GROUP_COUNT {4};
    static constexpr std::size_t SHORTCUT_COUNT {18};

    /// Content outlives the non-owning viewport, which permits smaller host-constrained windows.
    juce::Component content;
    juce::Viewport viewport;

    juce::Label descriptionLabel;
    juce::Label titleLabel;
    std::array<juce::Label, GROUP_COUNT> groupLabels;
    std::array<juce::Label, SHORTCUT_COUNT> actionLabels;
    std::array<juce::Label, SHORTCUT_COUNT> keyLabels;
};
}  // namespace esgrove::image_control
