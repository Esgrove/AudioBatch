/// Declares SplitDivider, the draggable divider between the spectrogram and the band strips.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace esgrove::image_control
{
/// A horizontal divider that stays invisible until the pointer is over it or it is being dragged.
/// Dragging reports the vertical movement, so the editor can resize the sections on either side.
class SplitDivider final : public juce::Component
{
public:
    SplitDivider();

    void setDragCallback(std::function<void(int)> callback);

    void paint(juce::Graphics& graphics) override;
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
