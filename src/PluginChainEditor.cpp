/// Implementation of PluginChainEditor and its SlotRow component.
/// Covers building the rows and wiring their controls to the owning PluginChain,
/// laying out the top bar, hint label, and scrolling row list,
/// drag-to-reorder gestures with a live drop-position preview,
/// and deferred row rebuilds when the chain changes.

#include "PluginChainEditor.h"

#include "CustomLookAndFeel.h"

namespace audiobatch::plugin_chain_editor
{
constexpr int rowHeight = 36;
constexpr int rowPadding = 4;
constexpr int topBarHeight = 32;
constexpr int margin = 8;
}  // namespace audiobatch::plugin_chain_editor

using namespace audiobatch::plugin_chain_editor;

PluginChainEditor::SlotRow::SlotRow(PluginChainEditor& ownerEditor, const int slotIndex) :
    editor(ownerEditor),
    index(slotIndex)
{
    const auto description = editor.pluginChain.getSlotDescription(index);
    const bool enabled = editor.pluginChain.isSlotEnabled(index);

    enabledToggle.setToggleState(enabled, juce::dontSendNotification);
    enabledToggle.setTooltip("Enable or disable this plugin in the chain.");
    enabledToggle.onClick = [this] { editor.pluginChain.setSlotEnabled(index, enabledToggle.getToggleState()); };
    addAndMakeVisible(enabledToggle);

    nameLabel.setText(
        description.name + " (" + description.pluginFormatName + ") - " + description.manufacturerName,
        juce::dontSendNotification
    );
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setColour(
        juce::Label::textColourId,
        enabled ? juce::CustomLookAndFeel::greySuperLight : juce::CustomLookAndFeel::greyMiddle
    );
    nameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel);

    editButton.setTooltip("Open this plugin's editor window.");
    editButton.onClick = [this] { editor.pluginChain.openEditorForSlot(index); };
    addAndMakeVisible(editButton);

    upButton.setTooltip("Move this plugin earlier in the chain.");
    upButton.onClick = [this] { editor.pluginChain.moveSlot(index, index - 1); };
    addAndMakeVisible(upButton);

    downButton.setTooltip("Move this plugin later in the chain.");
    downButton.onClick = [this] { editor.pluginChain.moveSlot(index, index + 1); };
    addAndMakeVisible(downButton);

    removeButton.setTooltip("Remove this plugin from the chain.");
    removeButton.setColour(juce::TextButton::buttonColourId, juce::CustomLookAndFeel::greyMediumDark);
    removeButton.onClick = [this] { editor.pluginChain.removeSlot(index); };
    addAndMakeVisible(removeButton);
}

void PluginChainEditor::SlotRow::resized()
{
    auto area = getLocalBounds().reduced(rowPadding);

    enabledToggle.setBounds(area.removeFromLeft(24));
    removeButton.setBounds(area.removeFromRight(24));
    area.removeFromRight(rowPadding);
    downButton.setBounds(area.removeFromRight(24));
    upButton.setBounds(area.removeFromRight(24));
    area.removeFromRight(rowPadding);
    editButton.setBounds(area.removeFromRight(48));
    area.removeFromRight(rowPadding);
    nameLabel.setBounds(area);
}

void PluginChainEditor::SlotRow::paint(juce::Graphics& graphics)
{
    graphics.setColour(dragging ? juce::CustomLookAndFeel::greyMedium : juce::CustomLookAndFeel::greyMediumDark);
    graphics.fillRoundedRectangle(getLocalBounds().reduced(1).toFloat(), 4.0f);
}

void PluginChainEditor::SlotRow::mouseDown(const juce::MouseEvent& event)
{
    dragging = true;
    dragGrabOffsetY = event.getPosition().y;
    toFront(false);
    repaint();
}

void PluginChainEditor::SlotRow::mouseDrag(const juce::MouseEvent& event)
{
    if (!dragging) {
        return;
    }

    const auto containerY = event.getEventRelativeTo(getParentComponent()).getPosition().y - dragGrabOffsetY;
    const auto maxY = juce::jmax(0, getParentComponent()->getHeight() - getHeight());
    setTopLeftPosition(getX(), juce::jlimit(0, maxY, containerY));

    const auto targetIndex = editor.rowIndexForPosition(getY() + getHeight() / 2);
    editor.rowDragged(*this, targetIndex);
}

void PluginChainEditor::SlotRow::mouseUp(const juce::MouseEvent& event)
{
    juce::ignoreUnused(event);

    if (!dragging) {
        return;
    }

    dragging = false;
    repaint();

    const auto targetIndex = editor.rowIndexForPosition(getY() + getHeight() / 2);
    editor.rowDropped(*this, targetIndex);
}

void PluginChainEditor::SlotRow::updateMoveButtonStates(const int numRows)
{
    upButton.setEnabled(index > 0);
    downButton.setEnabled(index < numRows - 1);
}

PluginChainEditor::PluginChainEditor(PluginChain& chain) : pluginChain(chain)
{
    addButton.setTooltip("Add a plugin to the end of the chain.");
    addButton.onClick = [this] { showAddPluginMenu(); };
    addAndMakeVisible(addButton);

    scanButton.setTooltip("Open the plugin scanner to find installed plugins.");
    scanButton.onClick = [this] { pluginChain.showScanWindow(); };
    addAndMakeVisible(scanButton);

    hintLabel.setJustificationType(juce::Justification::centred);
    hintLabel.setColour(juce::Label::textColourId, juce::CustomLookAndFeel::greyMiddle);
    addChildComponent(hintLabel);

    viewport.setViewedComponent(&rowContainer, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollBarThickness(8);
    addAndMakeVisible(viewport);

    pluginChain.addChangeListener(this);
    rebuildRows();
}

PluginChainEditor::~PluginChainEditor()
{
    pluginChain.removeChangeListener(this);
}

void PluginChainEditor::resized()
{
    auto area = getLocalBounds().reduced(margin);

    auto topBar = area.removeFromTop(topBarHeight);
    addButton.setBounds(topBar.removeFromLeft(110));
    topBar.removeFromLeft(margin);
    scanButton.setBounds(topBar.removeFromLeft(80));

    area.removeFromTop(margin);
    viewport.setBounds(area);
    hintLabel.setBounds(area);

    rowContainer.setSize(viewport.getMaximumVisibleWidth(), juce::jmax(1, static_cast<int>(rows.size())) * rowHeight);

    for (auto& row : rows) {
        row->setSize(rowContainer.getWidth(), rowHeight);
    }

    layoutRows();
}

void PluginChainEditor::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &pluginChain) {
        // Defer the rebuild: the change may originate from a button on a row
        // that a synchronous rebuild would delete while its onClick is still executing.
        triggerAsyncUpdate();
    }
}

void PluginChainEditor::handleAsyncUpdate()
{
    rebuildRows();
}

void PluginChainEditor::rebuildRows()
{
    rows.clear();

    const auto numSlots = pluginChain.getNumSlots();
    rows.reserve(static_cast<std::size_t>(numSlots));

    for (int index = 0; index < numSlots; ++index) {
        auto row = std::make_unique<SlotRow>(*this, index);
        row->updateMoveButtonStates(numSlots);
        rowContainer.addAndMakeVisible(row.get());
        rows.push_back(std::move(row));
    }

    updateHintLabel();
    resized();
}

void PluginChainEditor::layoutRows(const SlotRow* draggedRow, const int gapIndex)
{
    int visualIndex = 0;

    for (auto& row : rows) {
        if (row.get() == draggedRow) {
            continue;
        }

        if (visualIndex == gapIndex) {
            ++visualIndex;
        }

        row->setTopLeftPosition(0, visualIndex * rowHeight);
        ++visualIndex;
    }
}

void PluginChainEditor::rowDragged(SlotRow& row, const int targetIndex)
{
    layoutRows(&row, targetIndex);
}

void PluginChainEditor::rowDropped(SlotRow& row, const int targetIndex)
{
    if (targetIndex == row.getSlotIndex()) {
        // Nothing moved. Restore the resting layout.
        layoutRows();
        return;
    }

    // The chain change triggers an async rebuild that recreates the rows in final order.
    pluginChain.moveSlot(row.getSlotIndex(), targetIndex);
}

int PluginChainEditor::rowIndexForPosition(const int containerY) const
{
    const auto maxIndex = juce::jmax(0, static_cast<int>(rows.size()) - 1);
    return juce::jlimit(0, maxIndex, containerY / rowHeight);
}

void PluginChainEditor::showAddPluginMenu()
{
    const auto& types = pluginChain.getKnownPluginList().getTypes();

    juce::PopupMenu menu;
    juce::KnownPluginList::addToMenu(menu, types, juce::KnownPluginList::sortByManufacturer);

    const juce::Component::SafePointer<PluginChainEditor> safeThis(this);
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

void PluginChainEditor::updateHintLabel()
{
    if (!rows.empty()) {
        hintLabel.setVisible(false);
        return;
    }

    const bool hasKnownPlugins = !pluginChain.getKnownPluginList().getTypes().isEmpty();
    hintLabel.setText(
        hasKnownPlugins ? "No plugins in chain. Use Add Plugin to get started."
                        : "No plugins found. Use Scan... to find installed plugins.",
        juce::dontSendNotification
    );
    hintLabel.setVisible(true);
}
