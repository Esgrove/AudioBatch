/// Implementation of PluginChainEditor. PluginChainSlotRow.cpp owns individual row controls and gestures.
/// Covers building the rows for the slots of the owning LiveChain,
/// laying out the preset row, the hint label, and the scrolling row list with its add and clear buttons,
/// loading and deleting the presets chosen in the preset list,
/// drag-to-reorder gestures with a live drop-position preview,
/// and deferred row rebuilds when the chain changes.

#include "gui/components/PluginChainEditor.h"

#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "gui/LiveChain.h"
#include "plugins/PluginChainPreparation.h"
#include "utils/GuiUtils.h"
#include "utils/StringFormat.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace audiobatch
{
/// Builds the initial rows for the given chain
/// and registers as a change listener so the rows follow external chain changes.
PluginChainEditor::PluginChainEditor(LiveChain& chain) : liveChain(chain)
{
    presetLabel.setColour(juce::Label::textColourId, CustomLookAndFeel::greyMiddle());
    addAndMakeVisible(presetLabel);
    presetBox.setTextWhenNothingSelected("Choose a preset to load");
    presetBox.setTextWhenNoChoicesAvailable("No saved presets");
    presetBox.setTooltip("Replace the chain with a saved preset.");
    presetBox.onChange = [this] { loadSelectedPreset(); };
    addAndMakeVisible(presetBox);
    updatePresetButton.setTooltip("Save the modified chain over the loaded preset.");
    updatePresetButton.onClick = [this] { liveChain.updateCurrentPreset(); };
    addAndMakeVisible(updatePresetButton);
    savePresetButton.setTooltip("Save the plugins of the chain, their order, and their settings as a preset.");
    savePresetButton.onClick = [this] { liveChain.promptSavePreset(); };
    addAndMakeVisible(savePresetButton);
    deletePresetButton.setTooltip("Delete the selected preset.");
    deletePresetButton.onClick = [this] { deleteSelectedPreset(); };
    addAndMakeVisible(deletePresetButton);

    addButton.setTooltip("Add a plugin to the end of the chain.");
    addButton.onClick = [this] { showAddPluginMenu(); };
    rowContainer.addAndMakeVisible(addButton);
    clearChainButton.setTooltip("Remove every plugin from the chain.");
    clearChainButton.onClick = [this] { liveChain.clearChain(); };
    rowContainer.addAndMakeVisible(clearChainButton);

    hintLabel.setJustificationType(juce::Justification::centred);
    hintLabel.setColour(juce::Label::textColourId, CustomLookAndFeel::greyMiddle());
    addChildComponent(hintLabel);
    addAndMakeVisible(totalLatencyLabel);
    processingRateLabel.setColour(juce::Label::textColourId, CustomLookAndFeel::greyMiddle());
    addAndMakeVisible(processingRateLabel);

    viewport.setViewedComponent(&rowContainer, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollBarThickness(layout::scrollBarThickness);
    addAndMakeVisible(viewport);

    liveChain.addChangeListener(this);
    rebuildRows();
    refreshPresets();
}

/// Unregisters from the chain's change notifications.
PluginChainEditor::~PluginChainEditor()
{
    liveChain.removeChangeListener(this);
}

/// Lays out the preset row, the hint label, and the scrolling row list with its add and clear buttons.
void PluginChainEditor::resized()
{
    auto area = getLocalBounds().reduced(layout::pluginEditor::margin);

    auto presetRow = area.removeFromTop(layout::pluginEditor::presetRowHeight);
    area.removeFromTop(layout::pluginEditor::margin);
    presetLabel.setBounds(presetRow.removeFromLeft(layout::pluginEditor::presetLabelWidth));
    deletePresetButton.setBounds(presetRow.removeFromRight(layout::pluginEditor::presetButtonWidth));
    presetRow.removeFromRight(layout::pluginEditor::rowPadding);
    savePresetButton.setBounds(presetRow.removeFromRight(layout::pluginEditor::presetButtonWidth));
    presetRow.removeFromRight(layout::pluginEditor::rowPadding);
    updatePresetButton.setBounds(presetRow.removeFromRight(layout::pluginEditor::updatePresetButtonWidth));
    presetRow.removeFromRight(layout::pluginEditor::rowPadding);
    presetBox.setBounds(presetRow);

    auto footer = area.removeFromBottom(layout::pluginEditor::footerHeight);
    totalLatencyLabel.setBounds(footer.removeFromTop(layout::pluginEditor::footerLineHeight));
    processingRateLabel.setBounds(footer);

    viewport.setBounds(area);
    hintLabel.setBounds(area);

    // Reserve one extra row of space below the plugin rows for the add button.
    const auto rowsHeight = static_cast<int>(rows.size()) * layout::pluginEditor::rowHeight;
    rowContainer.setSize(viewport.getMaximumVisibleWidth(), rowsHeight + layout::pluginEditor::rowHeight);

    for (const auto& row : rows) {
        row->setSize(rowContainer.getWidth(), layout::pluginEditor::rowHeight);
    }

    layoutRows();
}

/// Schedules a deferred row rebuild when the chain changes.
/// The rebuild is asynchronous because the change may originate from a button on a row
/// that a synchronous rebuild would delete while its onClick is still executing.
void PluginChainEditor::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &liveChain) {
        // Defer the rebuild: the change may originate from a button on a row
        // that a synchronous rebuild would delete while its onClick is still executing.
        triggerAsyncUpdate();
    }
}

/// Performs the row rebuild deferred by changeListenerCallback(), and refreshes the preset list,
/// since saving or deleting a preset also notifies the listeners of the chain.
void PluginChainEditor::handleAsyncUpdate()
{
    if (displayedRevision != liveChain.getRevision()) {
        rebuildRows();
    } else {
        updateLatencyLabels();
    }
    refreshPresets();
}

/// Lists the saved presets and selects the one the chain was loaded from or saved as, without loading it again.
/// The selected preset reads "(modified)" after its name once the chain was modified,
/// and only then can Update save the chain over it.
void PluginChainEditor::refreshPresets()
{
    listedPresets = liveChain.getPresets();
    const auto currentName = liveChain.getCurrentPresetName();
    const bool modified = currentName.isNotEmpty() && liveChain.isPresetModified();

    presetBox.clear(juce::dontSendNotification);
    bool currentListed = false;
    for (std::size_t index = 0; index < listedPresets.size(); ++index) {
        const auto itemId = static_cast<int>(index) + 1;
        const auto& name = listedPresets[index].name;
        const bool current = currentName.isNotEmpty() && name == currentName;
        presetBox.addItem(current && modified ? utils::format("{} (modified)", name) : name, itemId);
        if (current) {
            presetBox.setSelectedId(itemId, juce::dontSendNotification);
            currentListed = true;
        }
    }

    updatePresetButton.setEnabled(currentListed && modified && liveChain.getNumSlots() > 0);
    savePresetButton.setEnabled(liveChain.getNumSlots() > 0);
    deletePresetButton.setEnabled(presetBox.getSelectedId() != 0);
}

/// Replaces the chain with the preset chosen in the list.
void PluginChainEditor::loadSelectedPreset()
{
    const auto index = presetBox.getSelectedId() - 1;
    if (!juce::isPositiveAndBelow(index, static_cast<int>(listedPresets.size()))) {
        return;
    }

    liveChain.loadPresetAndReport(listedPresets[static_cast<std::size_t>(index)].file);
    // A failed load leaves the chain unchanged, so the list shows the current preset again.
    refreshPresets();
}

/// Asks before deleting the preset chosen in the list.
void PluginChainEditor::deleteSelectedPreset()
{
    const auto index = presetBox.getSelectedId() - 1;
    if (juce::isPositiveAndBelow(index, static_cast<int>(listedPresets.size()))) {
        liveChain.confirmDeletePreset(listedPresets[static_cast<std::size_t>(index)]);
    }
}

/// Recreates the slot rows from the current chain state.
void PluginChainEditor::rebuildRows()
{
    displayedRevision = liveChain.getRevision();
    rows.clear();

    const auto slotIds = liveChain.getSlotIds();
    const auto numSlots = static_cast<int>(slotIds.size());
    rows.reserve(slotIds.size());

    for (int position = 0; position < numSlots; ++position) {
        auto row = std::make_unique<PluginChainSlotRow>(
            liveChain,
            slotIds[static_cast<std::size_t>(position)],
            position,
            [this](const int containerY) { return rowIndexForPosition(containerY); },
            [this](const PluginChainSlotRow& draggedRow, const int targetIndex) {
                rowDragged(draggedRow, targetIndex);
            },
            [this](const PluginChainSlotRow& droppedRow, const int targetIndex) { rowDropped(droppedRow, targetIndex); }
        );
        row->updateMoveButtonStates(numSlots);
        rowContainer.addAndMakeVisible(row.get());
        rows.push_back(std::move(row));
    }
    clearChainButton.setEnabled(numSlots > 0);

    updateHintLabel();
    updateLatencyLabels();
    resized();
}

/// Updates the pinned total and processing-rate context without rebuilding slot components.
void PluginChainEditor::updateLatencyLabels()
{
    for (const auto& row : rows) {
        row->updateLatency();
    }
    const auto total = liveChain.getTotalLatency();
    totalLatencyLabel.setText(
        total
            ? utils::format(
                  "Total latency: {}", plugin_processing::formatPluginLatency(*total, liveChain.getPreviewSampleRate())
              )
            : juce::String("Total latency: unavailable"),
        juce::dontSendNotification
    );
    processingRateLabel.setText(
        formatPluginPreviewRate(liveChain.getPreviewSampleRate(), liveChain.hasOpenPreviewDevice()),
        juce::dontSendNotification
    );
}

/// Positions all rows in the container.
/// While a row is being dragged it is excluded and a gap is left at the given visual index.
void PluginChainEditor::layoutRows(const PluginChainSlotRow* draggedRow, const int gapIndex)
{
    int visualIndex = 0;

    for (const auto& row : rows) {
        if (row.get() == draggedRow) {
            continue;
        }

        if (visualIndex == gapIndex) {
            ++visualIndex;
        }

        row->setTopLeftPosition(0, visualIndex * layout::pluginEditor::rowHeight);
        ++visualIndex;
    }

    const auto rowsHeight = static_cast<int>(rows.size()) * layout::pluginEditor::rowHeight;
    addButton.setBounds(
        layout::pluginEditor::rowPadding,
        rowsHeight + layout::pluginEditor::rowPadding,
        layout::pluginEditor::addButtonSize,
        layout::pluginEditor::addButtonSize
    );
    clearChainButton.setBounds(
        rowContainer.getWidth() - layout::pluginEditor::rowPadding - layout::pluginEditor::clearChainButtonWidth,
        rowsHeight + layout::pluginEditor::rowPadding,
        layout::pluginEditor::clearChainButtonWidth,
        layout::pluginEditor::addButtonSize
    );
}

/// Called by a row while it is being dragged to preview the drop position.
void PluginChainEditor::rowDragged(const PluginChainSlotRow& row, const int targetIndex)
{
    layoutRows(&row, targetIndex);
}

/// Called by a row when a drag ends to commit the reorder.
void PluginChainEditor::rowDropped(const PluginChainSlotRow& row, const int targetIndex)
{
    if (targetIndex == row.getChainPosition()) {
        // Nothing moved. Restore the resting layout.
        layoutRows();
        return;
    }

    // The chain change triggers an async rebuild that recreates the rows in final order.
    liveChain.moveSlot(row.getSlotId(), targetIndex);
}

/// Returns the visual row index for the given Y position in the row container.
int PluginChainEditor::rowIndexForPosition(const int containerY) const
{
    const auto maxIndex = juce::jmax(0, static_cast<int>(rows.size()) - 1);
    return juce::jlimit(0, maxIndex, containerY / layout::pluginEditor::rowHeight);
}

/// Pops up the known-plugins menu and appends the chosen plugin to the chain.
/// Uses desktop space so long plugin lists are not constrained by the chain editor window.
void PluginChainEditor::showAddPluginMenu()
{
    liveChain.getLibrary().reload();
    const auto types = liveChain.getLibrary().getKnownEffectTypes();

    juce::PopupMenu menu;
    juce::KnownPluginList::addToMenu(menu, types, juce::KnownPluginList::sortByManufacturer);

    const SafePointer safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton), [safeThis, types](const int result) {
        if (safeThis == nullptr || result == utils::MENU_RESULT_DISMISSED) {
            return;
        }

        const auto index = juce::KnownPluginList::getIndexChosenByMenu(types, result);
        if (index < 0 || index >= types.size()) {
            return;
        }

        safeThis->liveChain.addPlugin(types.getReference(index));
    });
}

/// Shows a hint label when the chain is empty, suggesting a plugin scan if no plugins are known yet.
void PluginChainEditor::updateHintLabel()
{
    if (!rows.empty()) {
        hintLabel.setVisible(false);
        return;
    }

    const bool hasKnownPlugins = !liveChain.getLibrary().getKnownEffectTypes().isEmpty();
    hintLabel.setText(
        hasKnownPlugins ? "No plugins in chain. Use the + button to get started."
                        : "No plugins found. Use the Plugins menu to scan for installed plugins.",
        juce::dontSendNotification
    );
    hintLabel.setVisible(true);
}
}  // namespace audiobatch
