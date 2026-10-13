/// GUI content for the chain editor window.
/// Declares PluginChainEditor, a component that shows the plugin chain
/// as a scrolling list of slot rows with enable, edit, reorder, and remove controls,
/// plus an add button below the rows for appending new plugins.

#pragma once

#include "PluginChain.h"

#include <JuceHeader.h>

#include <memory>
#include <vector>

/// Editor window content for the plugin chain.
/// Shows one row per slot with enable, edit, reorder, and remove controls,
/// plus an add button below the rows for appending new plugins.
/// Rows can be reordered by dragging their background or with the arrow buttons.
class PluginChainEditor : public juce::Component, public juce::ChangeListener, private juce::AsyncUpdater
{
public:
    explicit PluginChainEditor(PluginChain& chain);

    ~PluginChainEditor() override;

    PluginChainEditor(const PluginChainEditor&) = delete;
    PluginChainEditor& operator=(const PluginChainEditor&) = delete;
    PluginChainEditor(PluginChainEditor&&) = delete;
    PluginChainEditor& operator=(PluginChainEditor&&) = delete;

    void resized() override;

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

private:
    /// One row in the chain list, bound to a slot index.
    class SlotRow : public juce::Component
    {
    public:
        SlotRow(PluginChainEditor& ownerEditor, int slotIndex);

        ~SlotRow() override = default;

        SlotRow(const SlotRow&) = delete;
        SlotRow& operator=(const SlotRow&) = delete;
        SlotRow(SlotRow&&) = delete;
        SlotRow& operator=(SlotRow&&) = delete;

        void resized() override;

        void paint(juce::Graphics& graphics) override;

        void mouseDown(const juce::MouseEvent& event) override;

        void mouseDrag(const juce::MouseEvent& event) override;

        void mouseUp(const juce::MouseEvent& event) override;

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
        /// Reorder buttons, labelled with Unicode arrows in the constructor.
        juce::TextButton upButton;
        juce::TextButton downButton;
        juce::TextButton removeButton {"X"};

        bool dragging = false;
        int dragGrabOffsetY = 0;

        JUCE_LEAK_DETECTOR(SlotRow)
    };

    void handleAsyncUpdate() override;

    void rebuildRows();

    void layoutRows(const SlotRow* draggedRow = nullptr, int gapIndex = -1);

    void rowDragged(SlotRow& row, int targetIndex);

    void rowDropped(SlotRow& row, int targetIndex);

    [[nodiscard]] int rowIndexForPosition(int containerY) const;

    void showAddPluginMenu();

    void updateHintLabel();

    PluginChain& pluginChain;

    /// Appends a new plugin to the chain.
    /// Lives inside the row container below the last row, so it scrolls with the rows.
    juce::TextButton addButton {"+"};
    juce::Label hintLabel;

    juce::Viewport viewport;
    juce::Component rowContainer;
    std::vector<std::unique_ptr<SlotRow>> rows;

    /// Hover time before a tooltip appears in this window, in milliseconds.
    static constexpr int tooltipDelayMilliseconds {700};

    /// Shows tooltips for the controls in this window.
    /// The main window's tooltip client cannot serve a separate dialog window.
    juce::TooltipWindow tooltipWindow {this, tooltipDelayMilliseconds};

    JUCE_LEAK_DETECTOR(PluginChainEditor)
};
