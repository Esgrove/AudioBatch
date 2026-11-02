/// One plugin chain editor row, bound to a stable slot id.
/// Owns its controls and drag gesture while the editor owns row placement and rebuilds.

#pragma once

#include "gui/LiveChain.h"

#include <JuceHeader.h>

#include <functional>

namespace audiobatch
{
/// One row whose built position supplies its number and move buttons, but never identifies the slot it edits.
class PluginChainSlotRow : public juce::Component
{
public:
    PluginChainSlotRow(
        LiveChain& ownerChain,
        PluginSlotId rowSlotId,
        int rowChainPosition,
        std::function<int(int containerY)> dropIndexCallback,
        std::function<void(PluginChainSlotRow& row, int targetIndex)> draggedCallback,
        std::function<void(PluginChainSlotRow& row, int targetIndex)> droppedCallback
    );

    ~PluginChainSlotRow() override = default;

    PluginChainSlotRow(const PluginChainSlotRow&) = delete;
    PluginChainSlotRow& operator=(const PluginChainSlotRow&) = delete;
    PluginChainSlotRow(PluginChainSlotRow&&) = delete;
    PluginChainSlotRow& operator=(PluginChainSlotRow&&) = delete;

    /// Returns the stable identity of the slot this row edits.
    [[nodiscard]] PluginSlotId getSlotId() const noexcept
    {
        return slotId;
    }

    /// Returns the slot position when the row was built.
    [[nodiscard]] int getChainPosition() const noexcept
    {
        return chainPosition;
    }

    void updateLatency();
    void updateMoveButtonStates(int numRows);

    // juce::Component
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    void moveBy(int offset);

    /// The chain, which outlives the editor window.
    LiveChain& liveChain;
    /// Stable slot identity, independent of moves and removals.
    PluginSlotId slotId;

    /// Measured latency, or why it is unavailable.
    juce::Label latencyLabel;
    /// Position, manufacturer, name and format.
    juce::Label nameLabel;
    /// Reorder controls, labelled with arrows in the constructor.
    juce::TextButton downButton;
    /// Opens the processor editor.
    juce::TextButton editButton {"Edit"};
    /// Removes the slot.
    juce::TextButton removeButton {"X"};
    juce::TextButton upButton;
    /// Enables or disables the slot.
    juce::ToggleButton enabledToggle;

    /// Maps the row's vertical centre to a drop index.
    std::function<int(int containerY)> getDropIndex;
    /// Previews the drop position.
    std::function<void(PluginChainSlotRow& row, int targetIndex)> onDragged;
    /// Commits the reorder.
    std::function<void(PluginChainSlotRow& row, int targetIndex)> onDropped;

    /// True during a background drag gesture.
    bool dragging {false};
    /// Position when the row was built, used for presentation and move buttons.
    int chainPosition;
    /// Grab position inside the row, preventing jumps during dragging.
    int dragGrabOffsetY {0};

    JUCE_LEAK_DETECTOR(PluginChainSlotRow)
};
}  // namespace audiobatch
