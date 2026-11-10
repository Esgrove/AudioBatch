/// Plugin slot controls, latency labels and drag-to-reorder gestures.

#include "gui/components/PluginChainSlotRow.h"

#include "gui/components/CustomLookAndFeel.h"
#include "gui/GuiLayout.h"
#include "gui/LiveChain.h"
#include "gui/LiveSlot.h"
#include "plugins/PluginChainPreparation.h"
#include "utils/StringFormat.h"

#include <functional>
#include <utility>

namespace audiobatch
{
/// Builds controls for the stable slot and delegates drop placement to the editor.
PluginChainSlotRow::PluginChainSlotRow(
    LiveChain& ownerChain,
    const PluginSlotId rowSlotId,
    const int rowChainPosition,
    std::function<int(int containerY)> dropIndexCallback,
    std::function<void(PluginChainSlotRow& row, int targetIndex)> draggedCallback,
    std::function<void(PluginChainSlotRow& row, int targetIndex)> droppedCallback
) :
    liveChain(ownerChain),
    slotId(rowSlotId),
    getDropIndex(std::move(dropIndexCallback)),
    onDragged(std::move(draggedCallback)),
    onDropped(std::move(droppedCallback)),
    chainPosition(rowChainPosition)
{
    const auto description = liveChain.getSlotDescription(slotId);
    const bool enabled = liveChain.isSlotEnabled(slotId);
    enabledToggle.setToggleState(enabled, juce::dontSendNotification);
    enabledToggle.setTooltip("Enable or disable this plugin in the chain.");
    enabledToggle.onClick = [this] { liveChain.setSlotEnabled(slotId, enabledToggle.getToggleState()); };
    addAndMakeVisible(enabledToggle);
    nameLabel.setText(
        utils::format("{}. {}", chainPosition + 1, formatPluginDescription(description)), juce::dontSendNotification
    );
    nameLabel.setJustificationType(juce::Justification::centredLeft);
    nameLabel.setColour(
        juce::Label::textColourId, enabled ? CustomLookAndFeel::greySuperLight() : CustomLookAndFeel::greyMiddle()
    );
    nameLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel);
    latencyLabel.setInterceptsMouseClicks(false, false);
    latencyLabel.setColour(juce::Label::textColourId, CustomLookAndFeel::greyMiddle());
    addAndMakeVisible(latencyLabel);
    updateLatency();
    editButton.setTooltip("Open this plugin's editor window.");
    editButton.onClick = [this] { liveChain.openEditorForSlot(slotId); };
    addAndMakeVisible(editButton);
    upButton.setButtonText(juce::String::fromUTF8("\xE2\x86\x91"));
    upButton.setTooltip("Move this plugin earlier in the chain.");
    upButton.onClick = [this] { moveBy(-1); };
    addAndMakeVisible(upButton);
    downButton.setButtonText(juce::String::fromUTF8("\xE2\x86\x93"));
    downButton.setTooltip("Move this plugin later in the chain.");
    downButton.onClick = [this] { moveBy(1); };
    addAndMakeVisible(downButton);
    removeButton.setTooltip("Remove this plugin from the chain.");
    removeButton.setColour(juce::TextButton::buttonColourId, CustomLookAndFeel::greyMediumDark());
    removeButton.onClick = [this] { liveChain.removeSlot(slotId); };
    addAndMakeVisible(removeButton);
}

/// Lays out the toggle, labels and action buttons.
void PluginChainSlotRow::resized()
{
    auto area = getLocalBounds().reduced(layout::pluginEditor::ROW_PADDING);
    enabledToggle.setBounds(area.removeFromLeft(layout::pluginEditor::ROW_ICON_BUTTON_WIDTH));
    removeButton.setBounds(area.removeFromRight(layout::pluginEditor::ROW_SQUARE_BUTTON_SIZE));
    area.removeFromRight(layout::pluginEditor::ROW_PADDING);
    downButton.setBounds(area.removeFromRight(layout::pluginEditor::ROW_SQUARE_BUTTON_SIZE));
    area.removeFromRight(layout::pluginEditor::ROW_PADDING);
    upButton.setBounds(area.removeFromRight(layout::pluginEditor::ROW_SQUARE_BUTTON_SIZE));
    area.removeFromRight(layout::pluginEditor::ROW_PADDING);
    editButton.setBounds(area.removeFromRight(layout::pluginEditor::ROW_EDIT_BUTTON_WIDTH));
    area.removeFromRight(layout::pluginEditor::ROW_PADDING);
    nameLabel.setBounds(area.removeFromTop(layout::pluginEditor::NAME_HEIGHT));
    latencyLabel.setBounds(area);
}

/// Refreshes measured latency without rebuilding controls or drag state.
void PluginChainSlotRow::updateLatency()
{
    const auto latency = liveChain.getSlotLatency(slotId);
    latencyLabel.setText(
        latency ? utils::format(
                      "Latency: {}", plugin_processing::formatPluginLatency(*latency, liveChain.getPreviewSampleRate())
                  )
                : juce::String("Latency: unavailable"),
        juce::dontSendNotification
    );
}

/// Highlights the background while dragging.
void PluginChainSlotRow::paint(juce::Graphics& graphics)
{
    graphics.setColour(dragging ? CustomLookAndFeel::greyMedium() : CustomLookAndFeel::greyMediumDark());
    graphics.fillRoundedRectangle(getLocalBounds().reduced(1).toFloat(), layout::pluginEditor::ROW_CORNER_RADIUS);
}

/// Starts dragging and raises the row above its neighbours.
void PluginChainSlotRow::mouseDown(const juce::MouseEvent& event)
{
    dragging = true;
    dragGrabOffsetY = event.getPosition().y;
    toFront(false);
    repaint();
}

/// Moves with the pointer and requests a drop-position preview.
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

/// Ends the gesture and commits the reorder through the editor.
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

/// Disables moves beyond either end of the chain.
void PluginChainSlotRow::updateMoveButtonStates(const int numRows)
{
    upButton.setEnabled(chainPosition > 0);
    downButton.setEnabled(chainPosition < numRows - 1);
}

/// Moves from the slot's current position, which may differ from the position when the row was built.
void PluginChainSlotRow::moveBy(const int offset)
{
    if (const auto current = liveChain.getSlotIndex(slotId)) {
        liveChain.moveSlot(slotId, *current + offset);
    }
}
}  // namespace audiobatch
