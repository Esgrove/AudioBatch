/// Exercises production preset menu, inline naming, and confirmation workflows with isolated filesystem storage.

#include "Parameters.h"
#include "ProcessingState.h"
#include "tests/PresetWorkflowTestSupport.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/ProcessingToolbar.h"
#include "ui/ProcessingToolbarContent.h"

#include <gtest/gtest.h>

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control::test
{
namespace
{
/// Menu order keeps direct Save before Save As and separates naming, file exchange, and maintenance actions.
TEST_F(PresetWorkflowTest, PresetMenuOrdersCommandsAndReflectsSaveAdmission)
{
    openToolbar();
    const auto menu = PresetWorkflowTestAccess(*toolbar).menu();
    const juce::StringArray expected {
        "Save",
        "Save As...",
        "|",
        "Rename...",
        "Delete...",
        "|",
        "Import...",
        "Export...",
        "Reconnect...",
        "|",
        "Refresh preset list",
    };
    juce::StringArray actual;
    juce::PopupMenu::MenuItemIterator iterator(menu);
    while (iterator.next()) {
        const auto& item = iterator.getItem();
        actual.add(item.isSeparator ? "|" : item.text);
    }
    EXPECT_EQ(actual, expected);
    for (const auto action :
         {
             PresetMenuAction::update,
             PresetMenuAction::rename,
             PresetMenuAction::remove,
             PresetMenuAction::exportPreset,
             PresetMenuAction::reconnect,
         })
    {
        expectPresetMenuEnabled(*toolbar, action, false);
    }
    for (const auto action : {PresetMenuAction::saveAs, PresetMenuAction::importPreset, PresetMenuAction::refresh}) {
        expectPresetMenuEnabled(*toolbar, action, true);
    }
    saveAndLoad();
    toolbar->refresh();
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::update, false);
    for (const auto action :
         {
             PresetMenuAction::rename,
             PresetMenuAction::remove,
             PresetMenuAction::exportPreset,
             PresetMenuAction::reconnect,
         })
    {
        expectPresetMenuEnabled(*toolbar, action, true);
    }
    setOutput(-6.0F);
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::update, true);
    processor.getHistory().beginEdit("Active gesture");
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::update, false);
    processor.getHistory().endEdit();
    restore();
    toolbar->refresh();
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::update, false);
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::rename, false);
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::reconnect, true);
}

/// An unnamed sound has an explicit placeholder, and cancelling its inline draft never creates provenance.
TEST_F(PresetWorkflowTest, UnsavedSelectorUsesInlineNamingAndEscapeDiscardsOnlyTheDraft)
{
    openToolbar();
    auto& selector = PresetWorkflowTestAccess(*toolbar).selector();
    EXPECT_FALSE(selector.isTextEditable());
    EXPECT_EQ(selector.getText(), "Unsaved");
    EXPECT_FALSE(selector.isNameEditing());
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    const auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(input->isVisible());
    EXPECT_TRUE(input->getText().isEmpty());
    enterName("Unsaved draft");
    setOutput(-6.0F);
    const auto sound = processor.getSoundSnapshot();
    cancelName();
    EXPECT_EQ(selector.getText(), "Unsaved");
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).canSave());
    EXPECT_FALSE(processor.getPresetProvenance());
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    const auto listing = library.list();
    ASSERT_TRUE(listing);
    EXPECT_TRUE(listing->presets.empty());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
}

/// The name-label listener routes a child-origin click into inline editing without opening a native preset popup.
TEST_F(PresetWorkflowTest, MouseDownFromSelectorNameChildBeginsInlineEditingWithoutPopup)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    auto& selector = PresetWorkflowTestAccess(*toolbar).selector();
    juce::Label* nameLabel = nullptr;
    for (auto* child : selector.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child)) {
            nameLabel = label;
            break;
        }
    }
    ASSERT_NE(nameLabel, nullptr);
    EXPECT_FALSE(selector.isNameEditing());
    EXPECT_FALSE(selector.isPopupActive());
    const auto position = nameLabel->getLocalBounds().getCentre().toFloat();
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent event {
        juce::Desktop::getInstance().getMainMouseSource(),
        position,
        juce::ModifierKeys::leftButtonModifier,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        nameLabel,
        nameLabel,
        now,
        position,
        now,
        1,
        false,
    };
    selector.mouseDown(event);
    EXPECT_TRUE(selector.isNameEditing());
    EXPECT_FALSE(selector.isPopupActive());
    const auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->getText(), preset.name);
    EXPECT_EQ(selector.getPeer(), nullptr);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    cancelName();
}

/// Save reads the actual editor contents even before JUCE delivers its posted text-change notification.
TEST_F(PresetWorkflowTest, ImmediateSaveUsesLatestInlineTextBeforeTextChangeDispatch)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::rename);
    auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->isVisible());
    input->setText("Rapid inline rename");
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).draft(), preset.name);
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).canSave());
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::update);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_EQ(provenance().name, "Rapid inline rename");
    EXPECT_EQ(selection().id, preset.id);
    EXPECT_EQ(selection().file, preset.file);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, sound));
    const auto saved = library.load(selection());
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->name, "Rapid inline rename");
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).nameError().isEmpty());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
}

/// TextEditor's Return callback must capture its latest input without relying on an earlier text-change callback.
TEST_F(PresetWorkflowTest, ImmediateSaveAsReturnUsesLatestTextBeforeTextChangeDispatch)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::saveAs);
    auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->isVisible());
    ASSERT_TRUE(static_cast<bool>(input->onReturnKey));
    input->setText("Rapid Save As copy");
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).draft(), preset.name);
    input->onReturnKey();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_EQ(provenance().name, "Rapid Save As copy");
    EXPECT_NE(selection().id, preset.id);
    EXPECT_NE(selection().file, preset.file);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, sound));
    const auto original = library.load(preset);
    ASSERT_TRUE(original);
    EXPECT_EQ(original->fingerprint, preset.fingerprint);
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).nameError().isEmpty());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
}

/// Re-entering naming must preserve undispatched editor text and the explicit copy intent.
TEST_F(PresetWorkflowTest, ImmediateF2PreservesRawTextAndSaveAsIntent)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::saveAs);
    auto& selector = PresetWorkflowTestAccess(*toolbar).selector();
    auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->isVisible());
    input->setText("Copy survives immediate F2");
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).draft(), preset.name);
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    EXPECT_TRUE(input->isVisible());
    EXPECT_EQ(input->getText(), "Copy survives immediate F2");
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).draft(), "Copy survives immediate F2");
    acceptName();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_NE(selection().id, preset.id);
    EXPECT_NE(selection().file, preset.file);
    EXPECT_EQ(provenance().name, "Copy survives immediate F2");
    EXPECT_TRUE(sameProcessingState(provenance().baseline, sound));
    const auto original = library.load(preset);
    ASSERT_TRUE(original);
    EXPECT_EQ(original->fingerprint, preset.fingerprint);
    EXPECT_EQ(original->name, preset.name);
    EXPECT_TRUE(sameProcessingState(original->snapshot, preset.snapshot));
    const auto listing = library.list();
    ASSERT_TRUE(listing);
    EXPECT_EQ(listing->presets.size(), 2U);
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
}

/// Ordinary inline Enter validates the name before hiding its editor, without requiring an explicit Save.
TEST_F(PresetWorkflowTest, OrdinaryInlineEnterRetainsInvalidNameEditorAndFeedback)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    auto& selector = PresetWorkflowTestAccess(*toolbar).selector();
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    enterName("../invalid ordinary rename");
    acceptName();
    auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(input->isVisible());
    EXPECT_EQ(input->getText(), "../invalid ordinary rename");
    const auto error = PresetWorkflowTestAccess(*toolbar).nameError();
    ASSERT_TRUE(error.isNotEmpty());
    EXPECT_TRUE(selector.getTooltip().contains(error));
    EXPECT_TRUE(input->getTooltip().contains(error));
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).canSave());
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).busy());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    cancelName();
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).nameError().isEmpty());
    const auto original = library.load(preset);
    ASSERT_TRUE(original);
    EXPECT_EQ(original->fingerprint, preset.fingerprint);
}

/// Save As Enter creates a separate identity, while Escape leaves the source and current sound untouched.
TEST_F(PresetWorkflowTest, SaveAsEnterCreatesCopyAndEscapeDoesNotWrite)
{
    const auto preset = saveAndLoad();
    setOutput(-6.0F);
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::saveAs);
    enterName("Cancelled copy");
    cancelName();
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::saveAs);
    const auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->getText(), preset.name);
    enterName("Separate saved copy");
    acceptName();
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_NE(selection().id, preset.id);
    EXPECT_NE(selection().file, preset.file);
    EXPECT_EQ(provenance().name, "Separate saved copy");
    EXPECT_TRUE(sameProcessingState(selection().snapshot, sound));
    EXPECT_TRUE(sameProcessingState(provenance().baseline, sound));
    const auto original = library.load(preset);
    ASSERT_TRUE(original);
    EXPECT_EQ(original->fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(original->snapshot, preset.snapshot));
    const auto listing = library.list();
    ASSERT_TRUE(listing);
    EXPECT_EQ(listing->presets.size(), 2U);
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).canSave());
}

/// Ordinary name Enter is not a library lookup, even when its text exactly matches another preset.
TEST_F(PresetWorkflowTest, TypedLibraryNameNeverRecallsSoundOrChangesBinding)
{
    const auto preset = saveAndLoad();
    auto otherSound = preset.snapshot;
    otherSound.at(OUTPUT_GAIN_ID) = -18.0F;
    const auto other = library.saveAs("Existing other name", otherSound);
    ASSERT_TRUE(other);
    const auto sound = processor.getSoundSnapshot();
    const bool couldUndo = processor.getHistory().canUndo();
    openToolbar();
    auto& selector = PresetWorkflowTestAccess(*toolbar).selector();
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    enterName(other->name);
    acceptName();
    EXPECT_FALSE(selector.isNameEditing());
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).busy());
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_EQ(provenance().id, preset.id);
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_EQ(processor.getHistory().canUndo(), couldUndo);
    EXPECT_EQ(selector.getText(), other->name + " *");
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).canSave());
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::update, true);
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    cancelName();
    EXPECT_EQ(selector.getText(), preset.name);
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).canSave());
}

/// Background scans and header/row-sized layout changes preserve the overlay text, selection, and draft context.
TEST_F(PresetWorkflowTest, ScanAndLayoutRefreshPreserveInlineDraftAndCaret)
{
    const auto preset = saveAndLoad();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::rename);
    enterName("Draft survives refresh");
    auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    input->setHighlightedRegion(juce::Range<int>(2, 7));
    const auto highlighted = input->getHighlightedRegion();
    const auto caret = input->getCaretPosition();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::refresh);
    for (const int width : {PROCESSING_TOOLBAR_INLINE_MIN_WIDTH, PROCESSING_TOOLBAR_MAX_WIDTH, 744}) {
        toolbar->setSize(width, PROCESSING_TOOLBAR_HEIGHT);
        toolbar->refresh();
        EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).nameInput(), input);
        EXPECT_TRUE(input->isVisible());
        EXPECT_EQ(input->getText(), "Draft survives refresh");
        EXPECT_EQ(input->getCaretPosition(), caret);
        EXPECT_EQ(input->getHighlightedRegion(), highlighted);
    }
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_EQ(input->getText(), "Draft survives refresh");
    EXPECT_EQ(input->getCaretPosition(), caret);
    EXPECT_EQ(input->getHighlightedRegion(), highlighted);
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).canSave());
    cancelName();
}

/// Case-insensitive collisions preserve both revisions and remain reachable through tooltip and menu diagnostics.
TEST_F(PresetWorkflowTest, NameCollisionKeepsRenameAndSaveAsDraftsWithoutPopup)
{
    const auto preset = saveAndLoad();
    const auto other = library.saveAs("Occupied name", preset.snapshot);
    ASSERT_TRUE(other);
    openToolbar();
    for (const auto action : {PresetMenuAction::rename, PresetMenuAction::saveAs}) {
        PresetWorkflowTestAccess(*toolbar).action(action);
        enterName("OCCUPIED NAME");
        if (action == PresetMenuAction::rename) {
            acceptName();
            PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::update);
        } else {
            acceptName();
        }
        waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
        const auto error = PresetWorkflowTestAccess(*toolbar).nameError();
        EXPECT_TRUE(error.contains("already exists"));
        EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).selector().isNameEditing());
        EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).draft(), "OCCUPIED NAME");
        EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
        EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).selector().getTooltip().contains(error));
        const auto menu = PresetWorkflowTestAccess(*toolbar).menu();
        juce::PopupMenu::MenuItemIterator iterator(menu);
        bool foundError = false;
        while (iterator.next()) {
            const auto& item = iterator.getItem();
            if (item.text == error) {
                foundError = true;
                EXPECT_FALSE(item.isEnabled);
            }
        }
        EXPECT_TRUE(foundError);
        EXPECT_EQ(selection().fingerprint, preset.fingerprint);
        const auto untouched = library.load(*other);
        ASSERT_TRUE(untouched);
        EXPECT_EQ(untouched->fingerprint, other->fingerprint);
        cancelName();
        EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).nameError().isEmpty());
    }
}

/// Restore, comparison changes, and an explicit selector recall discard stale names rather than reusing them.
TEST_F(PresetWorkflowTest, StateTransitionsCancelInlineDrafts)
{
    const auto preset = saveAndLoad();
    auto otherSound = preset.snapshot;
    otherSound.at(OUTPUT_GAIN_ID) = -18.0F;
    const auto other = library.saveAs("Other recall target", otherSound);
    ASSERT_TRUE(other);
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::rename);
    enterName("Must not cross restore");
    restore();
    toolbar->refresh();
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).selector().isNameEditing());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), preset.name);
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::saveAs);
    enterName("Must not cross slots");
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    toolbar->refresh();
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).selector().isNameEditing());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), "Unsaved");
    ASSERT_TRUE(processor.selectComparisonSlot(0));
    toolbar->refresh();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::saveAs);
    enterName("Must not cross recall");
    auto& selector = PresetWorkflowTestAccess(*toolbar).selector();
    int target = 0;
    for (int index = 0; index < selector.getNumItems(); ++index) {
        if (selector.getItemText(index) == other->name) {
            target = selector.getItemId(index);
        }
    }
    ASSERT_GT(target, 0);
    selector.setSelectedId(target, juce::sendNotificationAsync);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).hasDraft(); });
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_FALSE(selector.isNameEditing());
    EXPECT_EQ(selection().id, other->id);
    EXPECT_EQ(provenance().name, other->name);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, otherSound));
    EXPECT_FLOAT_EQ(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), -18.0F);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
}

/// Ordinary plugin construction keeps dialogs visible, independently of neighboring non-rendering test instances.
TEST_F(PresetWorkflowTest, DialogVisibilityIsAnExplicitPerInstancePolicy)
{
    ProcessingToolbar production(processor, directory);
    EXPECT_TRUE(PresetWorkflowTestAccess(production).usesVisibleDialogs());
    openToolbar();
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).usesVisibleDialogs());
    EXPECT_TRUE(PresetWorkflowTestAccess(production).usesVisibleDialogs());
}

/// Test alerts retain messages, styling and asynchronous actions without desktop rendering or modal activation.
TEST_F(PresetWorkflowTest, NonRenderingWarningsRetainMessagesActionsAndPluginStyling)
{
    openToolbar();
    const auto message = juce::String("The preset file changed. Refresh the library and try again.");
    PresetWorkflowTestAccess(*toolbar).warning(message);
    auto* warning = PresetWorkflowTestAccess(*toolbar).dialog();
    ASSERT_NE(warning, nullptr);
    EXPECT_TRUE(warning->getDescription().contains(message));
    EXPECT_EQ(warning->getAlertType(), juce::MessageBoxIconType::WarningIcon);
    EXPECT_EQ(warning->getNumButtons(), 1);
    EXPECT_FALSE(warning->isCurrentlyModal());
    EXPECT_FALSE(warning->isVisible());
    EXPECT_FALSE(warning->isOnDesktop());
    EXPECT_EQ(warning->getPeer(), nullptr);
    juce::ModalComponentManager::getInstance()->bringModalComponentsToFront();
    EXPECT_EQ(warning->getPeer(), nullptr);
    EXPECT_FALSE(warning->hasKeyboardFocus(true));
    EXPECT_FLOAT_EQ(warning->getAlpha(), 0.0F);
    EXPECT_FALSE(warning->getWantsKeyboardFocus());
    EXPECT_FALSE(warning->hasKeyboardFocus(true));
    EXPECT_NE(dynamic_cast<ImageControlLookAndFeel*>(&warning->getLookAndFeel()), nullptr);
    acceptDialog();
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
}

/// Destructive and reconnection confirmations retain the same peerless presentation as failure reports.
TEST_F(PresetWorkflowTest, RemainingPresetConfirmationsAreNonRenderingInWorkflowTests)
{
    saveAndLoad();
    openToolbar();
    for (const auto action : {PresetMenuAction::remove, PresetMenuAction::reconnect}) {
        PresetWorkflowTestAccess(*toolbar).action(action);
        auto* current = PresetWorkflowTestAccess(*toolbar).dialog();
        ASSERT_NE(current, nullptr);
        EXPECT_FLOAT_EQ(current->getAlpha(), 0.0F);
        EXPECT_FALSE(current->isOnDesktop());
        EXPECT_FALSE(current->isVisible());
        EXPECT_EQ(current->getPeer(), nullptr);
        EXPECT_FALSE(current->getWantsKeyboardFocus());
        EXPECT_FALSE(current->hasKeyboardFocus(true));
        const juce::Component::SafePointer<juce::AlertWindow> safeDialog(current);
        auto* cancel = current->getButton(1);
        ASSERT_NE(cancel, nullptr);
        cancel->onClick();
        waitForPreset(*toolbar, [&] { return safeDialog == nullptr; });
    }
}

/// Closing the toolbar invalidates queued test-dialog results before they can start filesystem commands.
TEST_F(PresetWorkflowTest, PeerlessDialogCompletionIsCancelledOnToolbarDestruction)
{
    const auto preset = saveAndLoad();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::remove);
    const juce::Component::SafePointer<juce::AlertWindow> pending(PresetWorkflowTestAccess(*toolbar).dialog());
    ASSERT_NE(pending, nullptr);
    auto* accept = pending->getButton(0);
    ASSERT_NE(accept, nullptr);
    accept->onClick();
    EXPECT_NE(pending, nullptr);
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).busy());
    toolbar.reset();
    EXPECT_EQ(pending, nullptr);
    for (int attempt = 0; attempt < 10; ++attempt) {
        pumpPresetEvents();
    }
    const auto listing = library.list();
    ASSERT_TRUE(listing.has_value());
    ASSERT_EQ(listing->presets.size(), 1U);
    EXPECT_EQ(listing->presets.front().fingerprint, preset.fingerprint);
    EXPECT_EQ(provenance().id, preset.id);
}

/// Alert palette and fonts are explicit plugin choices rather than a host's default look and feel.
TEST_F(PresetWorkflowTest, AlertLookAndFeelUsesPluginPaletteAndTypography)
{
    ImageControlLookAndFeel look;
    EXPECT_EQ(look.findColour(juce::AlertWindow::backgroundColourId), juce::Colour(palette::GREY_SEMI_DARK));
    EXPECT_EQ(look.findColour(juce::AlertWindow::textColourId), juce::Colour(palette::GREY_SUPER_LIGHT));
    EXPECT_EQ(look.findColour(juce::AlertWindow::outlineColourId), juce::Colour(palette::GREY_MEDIUM));
    EXPECT_EQ(look.getAlertWindowTitleFont().getTypefaceName(), textFont(16.0F).getTypefaceName());
    EXPECT_EQ(look.getAlertWindowMessageFont().getTypefaceName(), textFont(13.0F).getTypefaceName());
    EXPECT_EQ(look.getAlertWindowFont().getTypefaceName(), textFont(13.0F).getTypefaceName());
    EXPECT_LE(look.getAlertWindowButtonHeight(), 30);
}

/// Rename remains a local draft until Save replaces both the reconnected revision's name and sound.
TEST_F(PresetWorkflowTest, InlineRenameAfterReconnectionSavesCurrentSoundAndBaselineWithSameIdentity)
{
    const auto preset = saveAndLoad();
    setOutput(-6.0F);
    restore();
    auto changed = preset.snapshot;
    changed.at(OUTPUT_GAIN_ID) = -12.0F;
    const auto updated = library.update(preset, changed);
    ASSERT_TRUE(updated);
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::reconnect);
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::rename);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    enterName("Renamed identity");
    acceptName();
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).busy());
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_EQ(selection().fingerprint, updated->fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), "Renamed identity *");
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).canSave());
    setOutput(-3.0F);
    const auto sound = processor.getSoundSnapshot();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::update);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    ASSERT_TRUE(processor.getPresetProvenance());
    EXPECT_EQ(provenance().name, "Renamed identity");
    EXPECT_EQ(provenance().id, preset.id);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, sound));
    ASSERT_TRUE(processor.getSelectedUserPreset());
    EXPECT_EQ(selection().id, preset.id);
    EXPECT_EQ(selection().file, preset.file);
    EXPECT_NE(selection().fingerprint, updated->fingerprint);
    EXPECT_TRUE(sameProcessingState(selection().snapshot, sound));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    const auto saved = library.load(selection());
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->name, "Renamed identity");
    EXPECT_TRUE(sameProcessingState(saved->snapshot, sound));
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).hasDraft());
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).canSave());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), "Renamed identity");
}

/// Invalid names stay editable and explain themselves inline until corrected or cancelled.
TEST_F(PresetWorkflowTest, InvalidNameRemainsInlineUntilCorrected)
{
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::saveAs);
    enterName("../invalid");
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).canSave());
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::update, false);
    acceptName();
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).selector().isNameEditing());
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).draft(), "../invalid");
    const auto error = PresetWorkflowTestAccess(*toolbar).nameError();
    EXPECT_TRUE(error.isNotEmpty());
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).selector().getTooltip().contains(error));
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).busy());
    enterName("Valid corrected name");
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).nameError().isEmpty());
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).canSave());
    acceptName();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    ASSERT_TRUE(processor.getPresetProvenance());
    EXPECT_EQ(provenance().name, "Valid corrected name");
}
}  // namespace
}  // namespace esgrove::image_control::test
