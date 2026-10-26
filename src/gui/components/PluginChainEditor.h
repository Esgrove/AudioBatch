/// GUI content for the chain editor window.
/// Declares PluginChainEditor, a component that shows the plugin chain
/// as a scrolling list of slot rows with enable, edit, reorder, and remove controls,
/// plus an add button below the rows for appending new plugins.

#pragma once

#include "gui/GuiLayout.h"
#include "gui/LiveChain.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <vector>

namespace audiobatch
{
/// One row in the chain list, bound to the id of its slot.
/// The rows are rebuilt after every structural change of the chain,
/// so the position a row was built at is only used for its number, its move buttons, and drag-to-reorder.
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

    void resized() override;

    void paint(juce::Graphics& graphics) override;

    void mouseDown(const juce::MouseEvent& event) override;

    void mouseDrag(const juce::MouseEvent& event) override;

    void mouseUp(const juce::MouseEvent& event) override;

    void updateMoveButtonStates(int numRows);
    void updateLatency();

    /// Returns the id of the slot this row is bound to.
    [[nodiscard]] PluginSlotId getSlotId() const noexcept
    {
        return slotId;
    }

    /// Returns the position of the slot in the chain when the row was built.
    [[nodiscard]] int getChainPosition() const noexcept
    {
        return chainPosition;
    }

private:
    void moveBy(int offset);

    LiveChain& liveChain;
    PluginSlotId slotId;
    int chainPosition;

    /// Maps the vertical centre of the row in its container to the slot index it would drop onto.
    std::function<int(int containerY)> getDropIndex;
    /// Called while the row is dragged, so the editor can preview the drop position.
    std::function<void(PluginChainSlotRow& row, int targetIndex)> onDragged;
    /// Called when the drag ends, so the editor can commit the reorder.
    std::function<void(PluginChainSlotRow& row, int targetIndex)> onDropped;

    juce::ToggleButton enabledToggle;
    juce::Label nameLabel;
    juce::Label latencyLabel;
    juce::TextButton editButton {"Edit"};
    /// Reorder buttons, labelled with Unicode arrows in the constructor.
    juce::TextButton upButton;
    juce::TextButton downButton;
    juce::TextButton removeButton {"X"};

    bool dragging {false};
    int dragGrabOffsetY {0};

    JUCE_LEAK_DETECTOR(PluginChainSlotRow)
};

/// Editor window content for the plugin chain.
/// Shows one row per slot with enable, edit, reorder, and remove controls,
/// plus an add button below the rows for appending new plugins.
/// Rows can be reordered by dragging their background or with the arrow buttons.
class PluginChainEditor : public juce::Component, public juce::ChangeListener, private juce::AsyncUpdater
{
public:
    explicit PluginChainEditor(LiveChain& chain);

    ~PluginChainEditor() override;

    PluginChainEditor(const PluginChainEditor&) = delete;
    PluginChainEditor& operator=(const PluginChainEditor&) = delete;
    PluginChainEditor(PluginChainEditor&&) = delete;
    PluginChainEditor& operator=(PluginChainEditor&&) = delete;

    void resized() override;

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

private:
    void handleAsyncUpdate() override;

    void rebuildRows();

    void layoutRows(const PluginChainSlotRow* draggedRow = nullptr, int gapIndex = -1);

    void rowDragged(const PluginChainSlotRow& row, int targetIndex);

    void rowDropped(const PluginChainSlotRow& row, int targetIndex);

    [[nodiscard]] int rowIndexForPosition(int containerY) const;

    void showAddPluginMenu();

    void updateHintLabel();
    void updateLatencyLabels();

    LiveChain& liveChain;

    /// Appends a new plugin to the chain.
    /// Lives inside the row container below the last row, so it scrolls with the rows.
    juce::TextButton addButton {"+"};
    juce::Label hintLabel;
    juce::Label totalLatencyLabel;
    juce::Label processingRateLabel;
    std::uint64_t displayedRevision {0};

    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<PluginChainSlotRow>> rows;

    /// Shows tooltips for the controls in this window.
    /// The main window's tooltip client cannot serve a separate dialog window.
    juce::TooltipWindow tooltipWindow {this, layout::tooltipDelayMilliseconds};

    JUCE_LEAK_DETECTOR(PluginChainEditor)
};
}  // namespace audiobatch
