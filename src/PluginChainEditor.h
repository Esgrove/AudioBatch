/// GUI content for the chain editor window.
/// Declares PluginChainEditor, a component that shows the plugin chain as a scrolling list of slot rows
/// with enable, edit, reorder, and remove controls,
/// plus buttons for adding plugins and opening the plugin scanner.

#pragma once

#include "PluginChain.h"

#include <JuceHeader.h>

#include <memory>
#include <vector>

/// Editor window content for the plugin chain.
/// Shows one row per slot with enable, edit, reorder, and remove controls,
/// plus buttons for adding plugins and opening the plugin scanner.
/// Rows can be reordered by dragging their background or with the arrow buttons.
class PluginChainEditor : public juce::Component, public juce::ChangeListener, private juce::AsyncUpdater
{
public:
    /// Builds the initial rows for the given chain
    /// and registers as a change listener so the rows follow external chain changes.
    explicit PluginChainEditor(PluginChain& chain);

    /// Unregisters from the chain's change notifications.
    ~PluginChainEditor() override;

    /// Lays out the top bar buttons, the hint label, and the scrolling row list.
    void resized() override;

    /// Schedules a deferred row rebuild when the chain changes.
    /// The rebuild is asynchronous because the change may originate from a button on a row
    /// that a synchronous rebuild would delete while its onClick is still executing.
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

private:
    /// One row in the chain list, bound to a slot index.
    class SlotRow : public juce::Component
    {
    public:
        /// Builds the row for the given slot and wires its controls to the owning chain.
        SlotRow(PluginChainEditor& ownerEditor, int slotIndex);

        /// Lays out the toggle, name label, and action buttons within the row.
        void resized() override;

        /// Fills the row background, using a lighter shade while the row is being dragged.
        void paint(juce::Graphics& graphics) override;

        /// Starts a drag-reorder gesture and brings the row to the front.
        void mouseDown(const juce::MouseEvent& event) override;

        /// Moves the row with the pointer and asks the editor to preview the drop position.
        void mouseDrag(const juce::MouseEvent& event) override;

        /// Ends the drag gesture and asks the editor to commit the reorder.
        void mouseUp(const juce::MouseEvent& event) override;

        /// Updates the enabled state of the reorder buttons for the row's position in the chain.
        void updateMoveButtonStates(int numRows);

        /// Returns the chain slot index this row is bound to.
        [[nodiscard]] int getSlotIndex() const noexcept
        {
            return index;
        }

    private:
        PluginChainEditor& editor;
        int index;

        juce::ToggleButton enabledToggle;
        juce::Label nameLabel;
        juce::TextButton editButton {"Edit"};
        juce::TextButton upButton {"^"};
        juce::TextButton downButton {"v"};
        juce::TextButton removeButton {"X"};

        bool dragging = false;
        int dragGrabOffsetY = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotRow)
    };

    /// Performs the row rebuild deferred by changeListenerCallback().
    void handleAsyncUpdate() override;

    /// Recreates the slot rows from the current chain state.
    void rebuildRows();

    /// Positions all rows in the container.
    /// While a row is being dragged it is excluded and a gap is left at the given visual index.
    void layoutRows(const SlotRow* draggedRow = nullptr, int gapIndex = -1);

    /// Called by a row while it is being dragged to preview the drop position.
    void rowDragged(SlotRow& row, int targetIndex);

    /// Called by a row when a drag ends to commit the reorder.
    void rowDropped(SlotRow& row, int targetIndex);

    /// Returns the visual row index for the given Y position in the row container.
    [[nodiscard]] int rowIndexForPosition(int containerY) const;

    /// Pops up the known-plugins menu and appends the chosen plugin to the chain.
    void showAddPluginMenu();

    /// Shows a hint label when the chain is empty,
    /// suggesting a plugin scan if no plugins are known yet.
    void updateHintLabel();

    PluginChain& pluginChain;

    juce::TextButton addButton {"Add Plugin"};
    juce::TextButton scanButton {"Scan..."};
    juce::Label hintLabel;

    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<SlotRow>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginChainEditor)
};
