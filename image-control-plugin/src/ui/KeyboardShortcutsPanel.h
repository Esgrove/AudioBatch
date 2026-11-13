/// Declares KeyboardShortcutsPanel, a read-only native reference for the editor's local controls.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>

namespace esgrove::image_control
{
/// Owns grouped action and key labels without editor state, sound parameters, or external content.
/// Settings owns this component as a nested reference, without a separate native window.
class KeyboardShortcutsPanel final : public juce::Component
{
public:
    KeyboardShortcutsPanel();

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    /// Number of shortcut category headings.
    static constexpr std::size_t GROUP_COUNT {4};
    /// Total number of documented keyboard and pointer shortcuts.
    static constexpr std::size_t SHORTCUT_COUNT {18};

    /// Content outlives the non-owning viewport, which permits smaller host-constrained windows.
    juce::Component content;
    /// Scrollable viewport containing the shortcut reference rows.
    juce::Viewport viewport;

    /// Subtitle explaining where shortcuts apply.
    juce::Label descriptionLabel;
    /// Panel heading label.
    juce::Label titleLabel;
    /// Category heading labels.
    std::array<juce::Label, GROUP_COUNT> groupLabels;
    /// Action description labels for each shortcut row.
    std::array<juce::Label, SHORTCUT_COUNT> actionLabels;
    /// Key or gesture labels for each shortcut row.
    std::array<juce::Label, SHORTCUT_COUNT> keyLabels;
};
}  // namespace esgrove::image_control
