/// GUI content for the chain editor window.
/// Declares PluginChainEditor, a component that shows the plugin chain
/// as a scrolling list of slot rows with enable, edit, reorder, and remove controls,
/// plus an add button and a Clear Chain button below the rows,
/// and a preset row above them for saving, updating, loading, and deleting chain presets.

#pragma once

#include "gui/components/PluginChainSlotRow.h"
#include "gui/GuiLayout.h"
#include "gui/LiveChain.h"

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <vector>

namespace audiobatch
{
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

    // juce::Component
    void resized() override;

    // juce::ChangeListener
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

private:
    [[nodiscard]] int rowIndexForPosition(int containerY) const;

    void deleteSelectedPreset();
    void layoutRows(const PluginChainSlotRow* draggedRow = nullptr, int gapIndex = -1);
    void loadSelectedPreset();
    void rebuildRows();
    void refreshPresets();

    // Row interaction and label refresh.
    void rowDragged(const PluginChainSlotRow& row, int targetIndex);
    void rowDropped(const PluginChainSlotRow& row, int targetIndex);
    void showAddPluginMenu();
    void updateHintLabel();
    void updateLatencyLabels();

    // juce::AsyncUpdater
    void handleAsyncUpdate() override;

    /// The chain the editor shows and edits, which outlives the editor window.
    LiveChain& liveChain;

    /// Choosing a preset replaces the chain with it.
    juce::ComboBox presetBox;
    /// The rows and add button. Outlives the viewport that observes it without ownership.
    juce::Component rowContainer;

    /// Explains how to add or scan plugins while empty.
    juce::Label hintLabel;
    juce::Label presetLabel {{}, "Preset"};
    /// The rate at which the latencies were measured.
    juce::Label processingRateLabel;
    /// Summed latency of enabled slots.
    juce::Label totalLatencyLabel;

    /// Appends a plugin and scrolls with the rows.
    juce::TextButton addButton {"+"};
    juce::TextButton clearChainButton {"Clear Chain"};
    juce::TextButton deletePresetButton {"Delete"};
    juce::TextButton savePresetButton {"Save As..."};
    /// Saves over the loaded preset without asking.
    juce::TextButton updatePresetButton {"Update"};

    /// Serves tooltips independently of the main window.
    juce::TooltipWindow tooltipWindow {this, layout::tooltipDelayMilliseconds};
    /// Scrolls the borrowed row container.
    juce::Viewport viewport;

    /// Presets in list order, with item ids equal to index plus one.
    std::vector<ChainPresetInfo> listedPresets;
    /// Owned rows are destroyed before their parent container.
    std::vector<std::unique_ptr<PluginChainSlotRow>> rows;

    /// Revision used to distinguish structural changes from latency-only updates.
    std::uint64_t displayedRevision {0};

    JUCE_LEAK_DETECTOR(PluginChainEditor)
};
}  // namespace audiobatch
