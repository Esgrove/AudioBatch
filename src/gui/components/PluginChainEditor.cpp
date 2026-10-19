/// Implementation of PluginChainEditor and its PluginChainSlotRow component.
/// Covers building the rows and wiring their controls to the owning PluginChain,
/// laying out the hint label and the scrolling row list with its add button,
/// drag-to-reorder gestures with a live drop-position preview,
/// and deferred row rebuilds when the chain changes.

#include "gui/components/PluginChainEditor.h"

#include "gui/components/CustomLookAndFeel.h"
#include "utils/StringFormat.h"

#include <utility>

namespace audiobatch
{
/// Builds the row for the given slot and wires its controls to the owning chain.
PluginChainSlotRow::PluginChainSlotRow(
    PluginChain& ownerChain,
    const int slotIndex,
    std::function<int(int containerY)> dropIndexCallback,
    std::function<void(PluginChainSlotRow& row, int targetIndex)> draggedCallback,
    std::function<void(PluginChainSlotRow& row, int targetIndex)> droppedCallback
) :
    pluginChain(ownerChain),
    index(slotIndex),
    getDropIndex(std::move(dropIndexCallback)),
    onDragged(std::move(draggedCallback)),
    onDropped(std::move(droppedCallback))
{
    const auto description = pluginChain.getSlotDescription(index);
    const bool enabled = pluginChain.isSlotEnabled(index);

    enabledToggle.setToggleState(enabled, juce::dontSendNotification);
    enabledToggle.setTooltip("Enable or disable this plugin in the chain.");
    enabledToggle.onClick = [this] { pluginChain.setSlotEnabled(index, enabledToggle.getToggleState()); };
    addAndMakeVisible(enabledToggle);

    nameLabel.setText(
        utils::format("{}. {}", index + 1, formatPluginDescription(description)), juce::dontSendNotification
    );
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setColour(
        juce::Label::textColourId,
        enabled ? juce::CustomLookAndFeel::greySuperLight() : juce::CustomLookAndFeel::greyMiddle()
    );
    nameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel);
    latencyLabel.setInterceptsMouseClicks(false, false);
    latencyLabel.setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddle());
    addAndMakeVisible(latencyLabel);
    updateLatency();

    editButton.setTooltip("Open this plugin's editor window.");
    editButton.onClick = [this] { pluginChain.openEditorForSlot(index); };
    addAndMakeVisible(editButton);

    upButton.setButtonText(juce::String::fromUTF8("\xE2\x86\x91"));
    upButton.setTooltip("Move this plugin earlier in the chain.");
    upButton.onClick = [this] { pluginChain.moveSlot(index, index - 1); };
    addAndMakeVisible(upButton);

    downButton.setButtonText(juce::String::fromUTF8("\xE2\x86\x93"));
    downButton.setTooltip("Move this plugin later in the chain.");
    downButton.onClick = [this] { pluginChain.moveSlot(index, index + 1); };
    addAndMakeVisible(downButton);

    removeButton.setTooltip("Remove this plugin from the chain.");
    removeButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMediumDark());
    removeButton.onClick = [this] { pluginChain.removeSlot(index); };
    addAndMakeVisible(removeButton);
}

/// Lays out the toggle, name label, and action buttons within the row.
void PluginChainSlotRow::resized()
{
    auto area = getLocalBounds().reduced(layout::pluginEditor::rowPadding);

    enabledToggle.setBounds(area.removeFromLeft(layout::pluginEditor::rowIconButtonWidth));
    removeButton.setBounds(area.removeFromRight(layout::pluginEditor::rowIconButtonWidth));
    area.removeFromRight(layout::pluginEditor::rowPadding);
    downButton.setBounds(area.removeFromRight(layout::pluginEditor::rowIconButtonWidth));
    upButton.setBounds(area.removeFromRight(layout::pluginEditor::rowIconButtonWidth));
    area.removeFromRight(layout::pluginEditor::rowPadding);
    editButton.setBounds(area.removeFromRight(layout::pluginEditor::rowEditButtonWidth));
    area.removeFromRight(layout::pluginEditor::rowPadding);
    nameLabel.setBounds(area.removeFromTop(layout::pluginEditor::nameHeight));
    latencyLabel.setBounds(area);
}

/// Refreshes only measured latency text, leaving row controls and drag state intact.
void PluginChainSlotRow::updateLatency()
{
    const auto latency = pluginChain.getSlotLatency(index);
    latencyLabel.setText(
        latency
            ? utils::format(
                  "Latency: {}", plugin_processing::formatPluginLatency(*latency, pluginChain.getPreviewSampleRate())
              )
            : juce::String("Latency: unavailable"),
        juce::dontSendNotification
    );
}

/// Fills the row background, using a lighter shade while the row is being dragged.
void PluginChainSlotRow::paint(juce::Graphics& graphics)
{
    graphics.setColour(dragging ? juce::CustomLookAndFeel::greyMedium() : juce::CustomLookAndFeel::greyMediumDark());
    graphics.fillRoundedRectangle(getLocalBounds().reduced(1).toFloat(), layout::pluginEditor::rowCornerRadius);
}

/// Starts a drag-reorder gesture and brings the row to the front.
void PluginChainSlotRow::mouseDown(const juce::MouseEvent& event)
{
    dragging = true;
    dragGrabOffsetY = event.getPosition().y;
    toFront(false);
    repaint();
}

/// Moves the row with the pointer and asks the editor to preview the drop position.
void PluginChainSlotRow::mouseDrag(const juce::MouseEvent& event)
{
    if (!dragging) {
        return;
    }

    const auto containerY = event.getEventRelativeTo(getParentComponent()).getPosition().y - dragGrabOffsetY;
    const auto maximumY = juce::jmax(0, getParentComponent()->getHeight() - getHeight());
    setTopLeftPosition(getX(), juce::jlimit(0, maximumY, containerY));

    const auto targetIndex = getDropIndex(getY() + getHeight() / 2);
    onDragged(*this, targetIndex);
}

/// Ends the drag gesture and asks the editor to commit the reorder.
void PluginChainSlotRow::mouseUp(const juce::MouseEvent& event)
{
    juce::ignoreUnused(event);

    if (!dragging) {
        return;
    }

    dragging = false;
    repaint();

    const auto targetIndex = getDropIndex(getY() + getHeight() / 2);
    onDropped(*this, targetIndex);
}

/// Updates the enabled state of the reorder buttons for the row's position in the chain.
void PluginChainSlotRow::updateMoveButtonStates(const int numRows)
{
    upButton.setEnabled(index > 0);
    downButton.setEnabled(index < numRows - 1);
}

/// Builds the initial rows for the given chain
/// and registers as a change listener so the rows follow external chain changes.
PluginChainEditor::PluginChainEditor(PluginChain& chain) : pluginChain(chain)
{
    addButton.setTooltip("Add a plugin to the end of the chain.");
    addButton.onClick = [this] { showAddPluginMenu(); };
    rowContainer.addAndMakeVisible(addButton);

    hintLabel.setJustificationType(juce::Justification::centred);
    hintLabel.setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddle());
    addChildComponent(hintLabel);
    addAndMakeVisible(totalLatencyLabel);
    processingRateLabel.setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddle());
    addAndMakeVisible(processingRateLabel);

    viewport.setViewedComponent(&rowContainer, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollBarThickness(layout::scrollBarThickness);
    addAndMakeVisible(viewport);

    pluginChain.addChangeListener(this);
    rebuildRows();
}

/// Unregisters from the chain's change notifications.
PluginChainEditor::~PluginChainEditor()
{
    pluginChain.removeChangeListener(this);
}

/// Lays out the hint label and the scrolling row list with its add button.
void PluginChainEditor::resized()
{
    auto area = getLocalBounds().reduced(layout::pluginEditor::margin);
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
    if (source == &pluginChain) {
        // Defer the rebuild: the change may originate from a button on a row
        // that a synchronous rebuild would delete while its onClick is still executing.
        triggerAsyncUpdate();
    }
}

/// Performs the row rebuild deferred by changeListenerCallback().
void PluginChainEditor::handleAsyncUpdate()
{
    if (displayedRevision != pluginChain.getRevision()) {
        rebuildRows();
    } else {
        updateLatencyLabels();
    }
}

/// Recreates the slot rows from the current chain state.
void PluginChainEditor::rebuildRows()
{
    displayedRevision = pluginChain.getRevision();
    rows.clear();

    const auto numSlots = pluginChain.getNumSlots();
    rows.reserve(static_cast<std::size_t>(numSlots));

    for (int index = 0; index < numSlots; ++index) {
        auto row = std::make_unique<PluginChainSlotRow>(
            pluginChain,
            index,
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
    const auto total = pluginChain.getTotalLatency();
    totalLatencyLabel.setText(
        total ? utils::format(
                    "Total latency: {}",
                    plugin_processing::formatPluginLatency(*total, pluginChain.getPreviewSampleRate())
                )
              : juce::String("Total latency: unavailable"),
        juce::dontSendNotification
    );
    processingRateLabel.setText(
        utils::format("Preview rate: {:.0f} Hz. No device: 48 kHz inspection.", pluginChain.getPreviewSampleRate()),
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

    // Keep the add button centered below the last row.
    const auto rowsHeight = static_cast<int>(rows.size()) * layout::pluginEditor::rowHeight;
    const auto addButtonX = (rowContainer.getWidth() - layout::pluginEditor::addButtonSize) / 2;
    addButton.setBounds(
        addButtonX,
        rowsHeight + layout::pluginEditor::rowPadding,
        layout::pluginEditor::addButtonSize,
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
    if (targetIndex == row.getSlotIndex()) {
        // Nothing moved. Restore the resting layout.
        layoutRows();
        return;
    }

    // The chain change triggers an async rebuild that recreates the rows in final order.
    pluginChain.moveSlot(row.getSlotIndex(), targetIndex);
}

/// Returns the visual row index for the given Y position in the row container.
int PluginChainEditor::rowIndexForPosition(const int containerY) const
{
    const auto maxIndex = juce::jmax(0, static_cast<int>(rows.size()) - 1);
    return juce::jlimit(0, maxIndex, containerY / layout::pluginEditor::rowHeight);
}

/// Pops up the known-plugins menu and appends the chosen plugin to the chain.
void PluginChainEditor::showAddPluginMenu()
{
    const auto& types = pluginChain.getKnownPluginList().getTypes();

    juce::PopupMenu menu;
    juce::KnownPluginList::addToMenu(menu, types, juce::KnownPluginList::sortByManufacturer);

    const SafePointer safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&addButton).withParentComponent(getTopLevelComponent()),
        [safeThis, types](const int result) {
            if (safeThis == nullptr || result == 0) {
                return;
            }

            const auto index = juce::KnownPluginList::getIndexChosenByMenu(types, result);
            if (index < 0 || index >= types.size()) {
                return;
            }

            safeThis->pluginChain.addPlugin(types.getReference(index));
        }
    );
}

/// Shows a hint label when the chain is empty, suggesting a plugin scan if no plugins are known yet.
void PluginChainEditor::updateHintLabel()
{
    if (!rows.empty()) {
        hintLabel.setVisible(false);
        return;
    }

    const bool hasKnownPlugins = !pluginChain.getKnownPluginList().getTypes().isEmpty();
    hintLabel.setText(
        hasKnownPlugins ? "No plugins in chain. Use the + button to get started."
                        : "No plugins found. Use the Plugins menu to scan for installed plugins.",
        juce::dontSendNotification
    );
    hintLabel.setVisible(true);
}
}  // namespace audiobatch
