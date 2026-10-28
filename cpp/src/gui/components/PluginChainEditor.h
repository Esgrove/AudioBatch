/// GUI content for the chain editor window.
/// Declares PluginChainEditor, a component that shows the plugin chain
/// as a scrolling list of slot rows with enable, edit, reorder, and remove controls,
/// plus an add button and a Clear Chain button below the rows,
/// and a preset row above them for saving, updating, loading, and deleting chain presets.

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

    /// The chain the row edits, which outlives the editor window.
    LiveChain& liveChain;
    /// The slot the row acts on, even after other slots moved or were removed.
    PluginSlotId slotId;
    /// Position of the slot in the chain when the row was built, shown in its name.
    int chainPosition;

    /// Maps the vertical centre of the row in its container to the slot index it would drop onto.
    std::function<int(int containerY)> getDropIndex;
    /// Called while the row is dragged, so the editor can preview the drop position.
    std::function<void(PluginChainSlotRow& row, int targetIndex)> onDragged;
    /// Called when the drag ends, so the editor can commit the reorder.
    std::function<void(PluginChainSlotRow& row, int targetIndex)> onDropped;

    /// Enables or disables the slot.
    juce::ToggleButton enabledToggle;
    /// The position, manufacturer, name, and format of the plugin.
    juce::Label nameLabel;
    /// The measured latency of the slot, or why it is not available.
    juce::Label latencyLabel;
    /// Opens the editor of the plugin.
    juce::TextButton editButton {"Edit"};
    /// Reorder buttons, labelled with Unicode arrows in the constructor.
    juce::TextButton upButton;
    juce::TextButton downButton;
    /// Removes the slot from the chain.
    juce::TextButton removeButton {"X"};

    /// True while the row is dragged by its background.
    bool dragging {false};
    /// Height within the row at which the drag grabbed it, so the row follows the mouse without jumping.
    int dragGrabOffsetY {0};

    JUCE_LEAK_DETECTOR(PluginChainSlotRow)
};

/// Editor window content for the plugin chain.
/// Shows one row per slot with enable, edit, reorder, and remove controls,
/// plus an add button below the rows for appending new plugins and a button that clears the chain.
/// A preset row above them saves the chain as a preset, updates the loaded preset once the chain was modified,
/// loads a saved preset, and deletes one.
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

    void loadSelectedPreset();
    void deleteSelectedPreset();
    void refreshPresets();
    void updateHintLabel();
    void updateLatencyLabels();

    /// The chain the editor shows and edits, which outlives the editor window.
    LiveChain& liveChain;

    /// The preset row above the slot rows.
    /// Choosing a preset in the list replaces the chain with it.
    juce::Label presetLabel {{}, "Preset"};
    juce::ComboBox presetBox;
    /// Saves the modified chain over the loaded preset without asking.
    juce::TextButton updatePresetButton {"Update"};
    juce::TextButton savePresetButton {"Save As..."};
    juce::TextButton deletePresetButton {"Delete"};
    /// The presets in the order of the list, whose item ids are their index plus one.
    std::vector<ChainPresetInfo> listedPresets;

    /// Appends a new plugin to the chain.
    /// Lives inside the row container below the last row, so it scrolls with the rows.
    juce::TextButton addButton {"+"};
    /// Removes every slot from the chain, on the line of the add button at the right edge.
    juce::TextButton clearChainButton {"Clear Chain"};
    /// Explains how to add a plugin, or how to scan for plugins, while the chain is empty.
    juce::Label hintLabel;
    /// The summed latency of the enabled slots.
    juce::Label totalLatencyLabel;
    /// The rate the latencies were measured at, and whether a preview device is open.
    juce::Label processingRateLabel;
    /// Revision of the chain the rows were built for.
    /// A change message without a structural change only refreshes the latency labels.
    std::uint64_t displayedRevision {0};

    /// Scrolls the rows.
    juce::Viewport viewport;
    /// The rows and the add button inside the viewport.
    juce::Component rowContainer;
    /// One row per slot in chain order.
    std::vector<std::unique_ptr<PluginChainSlotRow>> rows;

    /// Shows tooltips for the controls in this window.
    /// The main window's tooltip client cannot serve a separate dialog window.
    juce::TooltipWindow tooltipWindow {this, layout::tooltipDelayMilliseconds};

    JUCE_LEAK_DETECTOR(PluginChainEditor)
};
}  // namespace audiobatch
