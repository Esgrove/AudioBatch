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
    explicit PluginChainEditor(PluginChain& chain);
    ~PluginChainEditor() override;

    void resized() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

private:
    /// One row in the chain list, bound to a slot index.
    class SlotRow : public juce::Component
    {
    public:
        SlotRow(PluginChainEditor& ownerEditor, int slotIndex);

        void resized() override;
        void paint(juce::Graphics& graphics) override;

        void mouseDown(const juce::MouseEvent& event) override;
        void mouseDrag(const juce::MouseEvent& event) override;
        void mouseUp(const juce::MouseEvent& event) override;

        /// Updates the enabled state of the reorder buttons for the row's position in the chain.
        void updateMoveButtonStates(int numRows);

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

    void showAddPluginMenu();
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
