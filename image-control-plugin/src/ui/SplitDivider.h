/// Declares SplitDivider, the draggable divider between the spectrogram and the band strips.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace esgrove::image_control
{
/// A horizontal divider with an understated idle grip and keyboard resizing.
/// Dragging or arrow keys report vertical movement so the editor can clamp and resize the sections on either side.
class SplitDivider final : public juce::Component
{
public:
    SplitDivider();

    void cancelDrag();
    void setDragCallback(std::function<void(int)> callback);

    bool keyPressed(const juce::KeyPress& key) override;
    void focusGained(juce::Component::FocusChangeType cause) override;
    void focusLost(juce::Component::FocusChangeType cause) override;
    void paint(juce::Graphics& graphics) override;
    void visibilityChanged() override;
    void mouseEnter(const juce::MouseEvent& event) override;
    void mouseExit(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    /// Whether the pointer is over the divider.
    bool hovered {false};
    /// Whether a drag is in progress.
    bool dragging {false};
    /// Screen y position of the pointer at the previous drag report.
    int lastScreenY {0};
    /// Receives the drag movement.
    std::function<void(int)> onDrag;
};
}  // namespace esgrove::image_control
