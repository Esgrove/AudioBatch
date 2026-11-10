/// Implements the editor's compact vector header actions, keyboard focus order, history feedback, and shortcuts.
/// Display composition and saved geometry remain in ImageControlEditor.cpp.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ui/ImageControlLookAndFeel.h"

#include <array>

namespace esgrove::image_control
{
namespace
{
/// Font-independent header symbols use one restrained stroke and compact disclosure proportions.
constexpr float CHEVRON_WIDTH {10.0F};
constexpr float CHEVRON_HEIGHT {5.0F};
constexpr float ICON_STROKE_WIDTH {1.5F};
constexpr int CHEVRON_INSET {7};
/// Maximum host parameter label length used for a local history action.
constexpr int HISTORY_NAME_LENGTH {100};

/// Toggles a boolean parameter in a balanced host gesture and one local history edit.
void toggleParameter(juce::RangedAudioParameter& parameter, ParameterHistory& history)
{
    const auto generation = history.generation();
    history.beginEdit(parameter.getName(HISTORY_NAME_LENGTH));
    history.touch(parameter.paramID);
    parameter.beginChangeGesture();
    if (generation == history.generation()) {
        parameter.setValueNotifyingHost(parameter.getValue() >= 0.5F ? 0.0F : 1.0F);
    }
    parameter.endChangeGesture();
    if (generation == history.generation()) {
        history.endEdit();
    }
}
}  // namespace

/// Creates compact vector symbols with full button hit targets and stable, font-independent accessible identities.
void ImageControlEditor::configureHeaderIcons()
{
    settingsButton.setComponentID("display-settings");
    settingsButton.setTitle("Display settings");
    settingsButton.setDescription("Open display settings and controls help");
    settingsButton.setWantsKeyboardFocus(true);
    juce::Path gear;
    constexpr int gearPoints {32};
    for (int point = 0; point < gearPoints; ++point) {
        const float angle
            = static_cast<float>(point) * juce::MathConstants<float>::twoPi / static_cast<float>(gearPoints);
        const float radius = point % 4 < 2 ? 8.0F : 6.5F;
        const auto position = juce::Point<float>(10.0F, 10.0F).getPointOnCircumference(radius, angle);
        if (point == 0) {
            gear.startNewSubPath(position);
        } else {
            gear.lineTo(position);
        }
    }
    gear.closeSubPath();
    constexpr float gearHoleInset {7.0F};
    constexpr float gearHoleDiameter {6.0F};
    gear.addEllipse(gearHoleInset, gearHoleInset, gearHoleDiameter, gearHoleDiameter);
    juce::DrawablePath settingsIcon;
    settingsIcon.setPath(gear);
    settingsIcon.setFill(juce::Colours::transparentBlack);
    settingsIcon.setStrokeFill(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    settingsIcon.setStrokeThickness(ICON_STROKE_WIDTH);
    settingsButton.setImages(&settingsIcon);
    settingsButton.setEdgeIndent(4);
    settingsButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    settingsButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
    undoButton.setComponentID("processing-undo");
    undoButton.setTitle("Undo processing edit");
    undoButton.setDescription("Undo processing edit");
    undoButton.setWantsKeyboardFocus(true);
    redoButton.setComponentID("processing-redo");
    redoButton.setTitle("Redo processing edit");
    redoButton.setDescription("Redo processing edit");
    redoButton.setWantsKeyboardFocus(true);
    processingToolbarButton.setComponentID("processing-toolbar-toggle");
    processingToolbarButton.setTitle("A/B comparison and presets");
    processingToolbarButton.setDescription("Show or hide A/B comparison and presets");
    processingToolbarButton.setWantsKeyboardFocus(true);
    processingToolbarButton.setToggleable(true);
    int focusOrder = 1;
    for (auto* control :
         std::array<juce::Component*, 15> {
             &settingsButton,
             &preButton,
             &postButton,
             &undoButton,
             &redoButton,
             &monoButton,
             &deltaButton,
             &bypassButton,
             &outputSlider,
             &processingToolbarButton,
             &processingToolbar,
             &spectrogramDisclosure,
             &divider,
             &correlationDisclosure,
             &bandControlsDisclosure,
         })
    {
        control->setExplicitFocusOrder(focusOrder++);
    }

    constexpr juce::Point<float> arrowEnd {12.0F, 11.0F};
    constexpr juce::Point<float> arrowCurveFirst {15.0F, 4.0F};
    constexpr juce::Point<float> arrowCurveSecond {7.0F, 0.0F};
    constexpr juce::Point<float> arrowTip {1.0F, 6.0F};
    constexpr juce::Point<float> arrowHeadTop {1.0F, 2.0F};
    constexpr juce::Point<float> arrowHeadRight {5.0F, 6.0F};
    juce::Path arrowPath;
    arrowPath.startNewSubPath(arrowEnd);
    arrowPath.cubicTo(arrowCurveFirst, arrowCurveSecond, arrowTip);
    arrowPath.startNewSubPath(arrowHeadTop);
    arrowPath.lineTo(arrowTip);
    arrowPath.lineTo(arrowHeadRight);
    juce::DrawablePath arrow;
    arrow.setPath(arrowPath);
    arrow.setFill(juce::Colours::transparentBlack);
    arrow.setStrokeFill(juce::Colour(palette::GREY_SUPER_LIGHT));
    arrow.setStrokeType(
        juce::PathStrokeType(ICON_STROKE_WIDTH, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
    );
    undoButton.setImages(&arrow);
    arrowPath.applyTransform(juce::AffineTransform::scale(-1.0F, 1.0F));
    arrow.setPath(arrowPath);
    redoButton.setImages(&arrow);

    juce::Path chevronPath;
    chevronPath.startNewSubPath(0.0F, CHEVRON_HEIGHT);
    chevronPath.lineTo(CHEVRON_WIDTH * 0.5F, 0.0F);
    chevronPath.lineTo(CHEVRON_WIDTH, CHEVRON_HEIGHT);
    juce::DrawablePath collapseChevron;
    collapseChevron.setPath(chevronPath);
    collapseChevron.setFill(juce::Colours::transparentBlack);
    collapseChevron.setStrokeFill(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    collapseChevron.setStrokeThickness(ICON_STROKE_WIDTH);
    chevronPath.applyTransform(
        juce::AffineTransform::rotation(juce::MathConstants<float>::pi, CHEVRON_WIDTH * 0.5F, CHEVRON_HEIGHT * 0.5F)
    );
    juce::DrawablePath expandChevron;
    expandChevron.setPath(chevronPath);
    expandChevron.setFill(juce::Colours::transparentBlack);
    expandChevron.setStrokeFill(juce::Colour(palette::GREY_MIDDLE_LIGHT));
    expandChevron.setStrokeThickness(ICON_STROKE_WIDTH);
    processingToolbarButton.setImages(&expandChevron, nullptr, nullptr, nullptr, &collapseChevron);
    processingToolbarButton.setEdgeIndent(CHEVRON_INSET);
    processingToolbarButton.setColour(juce::DrawableButton::backgroundColourId, juce::Colours::transparentBlack);
    processingToolbarButton.setColour(juce::DrawableButton::backgroundOnColourId, juce::Colours::transparentBlack);
}

/// Keeps always-visible history availability and descriptive tooltips synchronized with processor-owned history.
void ImageControlEditor::updateHistoryButtons()
{
    const auto& history = imageControlProcessor.getHistory();
    undoButton.setEnabled(history.canUndo() && !history.isEditing());
    redoButton.setEnabled(history.canRedo() && !history.isEditing());
    undoButton.setTooltip(history.canUndo() ? "Undo " + history.undoDescription() : "No processing edit to undo");
    redoButton.setTooltip(history.canRedo() ? "Redo " + history.redoDescription() : "No processing edit to redo");
}

/// Routes local editing and display shortcuts without taking text-editor commands or unrelated host shortcuts.
bool ImageControlEditor::keyPressed(const juce::KeyPress& key)
{
    auto& history = imageControlProcessor.getHistory();
    const auto& parameters = imageControlProcessor.getParameters();
    const auto modifiers = key.getModifiers();
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const bool textEditing = dynamic_cast<const juce::TextEditor*>(focused) != nullptr;
    const auto letter = juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
    if (!textEditing && modifiers.isCommandDown() && !modifiers.isAltDown()) {
        const bool redoShortcut
            = (letter == 'z' && modifiers.isShiftDown()) || (letter == 'y' && !modifiers.isShiftDown());
        if (redoShortcut) {
            processingToolbar.redo();
            updateHistoryButtons();
            return true;
        }
        if (letter == 'z') {
            processingToolbar.undo();
            updateHistoryButtons();
            return true;
        }
    }
    if (textEditing) {
        return false;
    }
    const bool commandOnly = modifiers.isCommandDown() && !modifiers.isAltDown() && !modifiers.isShiftDown();
    if (commandOnly && letter == ',') {
        openSettings();
        return true;
    }
    if (key.getKeyCode() == juce::KeyPress::F1Key && !modifiers.isAnyModifierKeyDown()) {
        openKeyboardShortcuts();
        return true;
    }
    if (commandOnly && letter == 'd') {
        toggleParameter(*parameters.getParameter(DELTA_ID), history);
        return true;
    }
    if (commandOnly && letter == 'b') {
        toggleParameter(*parameters.getParameter(BYPASS_ID), history);
        return true;
    }
    if (letter == 's' && !modifiers.isCommandDown() && !modifiers.isCtrlDown() && !modifiers.isAltDown()) {
        const auto band = bandUnderPointer();
        if (!band.has_value()) {
            return false;
        }
        toggleParameter(*parameters.getParameter(soloParameterId(*band)), history);
        return true;
    }
    return false;
}
}  // namespace esgrove::image_control
