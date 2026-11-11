/// Exercises restored provenance, explicit reconnection, optimistic revisions, and stale preset completion results.

#include "Parameters.h"
#include "ProcessingState.h"
#include "tests/PresetWorkflowTestSupport.h"
#include "ui/ProcessingToolbarContent.h"

#include <gtest/gtest.h>

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace esgrove::image_control::test
{
namespace
{
/// Save rejects a draft invalidated by host restore even before the toolbar observes that restore in refresh.
TEST_F(PresetWorkflowTest, SaveBeforeRefreshAfterHostRestoreCannotWriteOrBindStaleDraft)
{
    const auto preset = saveAndLoad();
    openToolbar();
    for (const auto action : {PresetMenuAction::rename, PresetMenuAction::saveAs}) {
        SCOPED_TRACE(static_cast<int>(action));
        ASSERT_TRUE(processor.loadUserPreset(preset, processor.getHistory().generation(), 0));
        toolbar->refresh();
        PresetWorkflowTestAccess(*toolbar).action(action);
        enterName("Stale name must not be written");
        restore();
        const auto restoredSound = processor.getSoundSnapshot();
        ASSERT_FALSE(processor.getSelectedUserPreset());
        ASSERT_TRUE(PresetWorkflowTestAccess(*toolbar).hasDraft());
        EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).canSave());
        PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::update);
        EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).busy());
        EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).hasDraft());
        EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).selector().isNameEditing());
        waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
        EXPECT_FALSE(processor.getSelectedUserPreset());
        EXPECT_EQ(provenance().id, preset.id);
        EXPECT_EQ(provenance().name, preset.name);
        EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
        EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), restoredSound));
        const auto original = library.load(preset);
        ASSERT_TRUE(original);
        EXPECT_EQ(original->fingerprint, preset.fingerprint);
        const auto listing = library.list();
        ASSERT_TRUE(listing);
        ASSERT_EQ(listing->presets.size(), 1U);
        EXPECT_EQ(listing->presets.front().id, preset.id);
        EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    }
}

TEST_F(PresetWorkflowTest, RestoredNameAndBaselineRemainVisibleWithoutBindingOrLibraryFile)
{
    const auto name = juce::String::repeatedString("Full preset name ", 6).trim();
    const auto preset = saveAndLoad(name);
    setOutput(-6.0F);
    restore();
    ASSERT_FALSE(processor.getSelectedUserPreset());
    ASSERT_TRUE(preset.file.deleteFile());
    openToolbar();
    auto& selector = PresetWorkflowTestAccess(*toolbar).selector();
    EXPECT_EQ(selector.getText(), name + " *");
    EXPECT_TRUE(selector.getTitle().contains(name));
    EXPECT_TRUE(selector.getTooltip().contains(name));
    EXPECT_TRUE(selector.getDescription().contains("differs from the stored baseline"));
    EXPECT_TRUE(selector.getTooltip().contains("Unlinked"));
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_FALSE(processor.getSelectedUserPreset());
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::saveAs);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    const auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(input->isVisible());
    EXPECT_EQ(input->getText(), name);
}

TEST_F(PresetWorkflowTest, LibraryWarningsAndProgressNeverHideModifiedSoundStatus)
{
    const auto preset = saveAndLoad();
    setOutput(-3.0F);
    ASSERT_TRUE(directory.getChildFile("invalid.xml").replaceWithText("not XML"));
    openToolbar();
    auto& selector = PresetWorkflowTestAccess(*toolbar).selector();
    EXPECT_EQ(selector.getText(), preset.name + " *");
    EXPECT_TRUE(selector.getTooltip().contains("1 invalid"));
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::issues, true);
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::refresh);
    EXPECT_TRUE(selector.getTooltip().contains("Scanning..."));
    EXPECT_EQ(selector.getText(), preset.name + " *");
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
}

TEST_F(PresetWorkflowTest, ScanNeverSilentlyAdoptsNewLibraryRevisionOrBaseline)
{
    const auto preset = saveAndLoad();
    auto changed = preset.snapshot;
    changed.at(OUTPUT_GAIN_ID) = -12.0F;
    const auto updated = library.update(preset, changed);
    ASSERT_TRUE(updated);
    openToolbar();
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), preset.name);
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).selector().getTooltip().contains("File changed"));
}

TEST_F(PresetWorkflowTest, ExplicitUuidReconnectionBindsFreshRevisionWithoutLoadingOrChangingBaseline)
{
    const auto preset = saveAndLoad();
    setOutput(-6.0F);
    restore();
    auto changed = preset.snapshot;
    changed.at(OUTPUT_GAIN_ID) = -12.0F;
    const auto updated = library.update(preset, changed);
    ASSERT_TRUE(updated);
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    EXPECT_FALSE(processor.getSelectedUserPreset());
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::reconnect);
    EXPECT_FALSE(processor.getSelectedUserPreset());
    const auto* confirmation = PresetWorkflowTestAccess(*toolbar).dialog();
    ASSERT_NE(confirmation, nullptr);
    EXPECT_TRUE(confirmation->getDescription().contains("differs from the project's stored preset baseline"));
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    ASSERT_TRUE(processor.getSelectedUserPreset());
    EXPECT_EQ(selection().fingerprint, updated->fingerprint);
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), preset.name + " *");
    EXPECT_FALSE(processor.getHistory().canUndo());
}

TEST_F(PresetWorkflowTest, SameNameDifferentUuidCannotReconnect)
{
    const auto preset = saveAndLoad();
    restore();
    ASSERT_TRUE(library.remove(preset).wasOk());
    const auto replacement = library.saveAs(preset.name, preset.snapshot);
    ASSERT_TRUE(replacement);
    ASSERT_NE(replacement->id, preset.id);
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::reconnect);
    ASSERT_NE(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog()->getNumButtons(), 1);
    EXPECT_FALSE(processor.getSelectedUserPreset());
    EXPECT_EQ(provenance().id, preset.id);
}

TEST_F(PresetWorkflowTest, ReconnectionChecksExactRevisionAgainAfterConfirmation)
{
    const auto preset = saveAndLoad();
    restore();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::reconnect);
    ASSERT_NE(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    ASSERT_TRUE(library.update(preset, preset.snapshot));
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_FALSE(processor.getSelectedUserPreset());
    EXPECT_EQ(provenance().id, preset.id);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    ASSERT_NE(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog()->getNumButtons(), 1);
}

/// A direct Save still validates its captured revision and retains failed input without a popup.
TEST_F(PresetWorkflowTest, SaveRejectsChangedRevisionAndKeepsNameDraftInline)
{
    const auto preset = saveAndLoad("Explicit save target");
    setOutput(-6.0F);
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::rename);
    enterName("Pending renamed target");
    acceptName();
    const auto external = library.update(preset, preset.snapshot);
    ASSERT_TRUE(external);
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::update);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    const auto current = library.load(*external);
    ASSERT_TRUE(current);
    EXPECT_TRUE(sameProcessingState(current->snapshot, preset.snapshot));
    EXPECT_EQ(current->name, preset.name);
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).draft(), "Pending renamed target");
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).selector().isNameEditing());
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).nameError().isNotEmpty());
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).selector().getTooltip().contains(
        PresetWorkflowTestAccess(*toolbar).nameError()
    ));
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
}

/// Explicit Save publishes the current sound baseline immediately, independently of chronological sound undo.
TEST_F(PresetWorkflowTest, DirectSaveCapturesNewBaselineAndUndoDoesNotRewindIt)
{
    const auto preset = saveAndLoad();
    auto sound = processor.getSoundSnapshot();
    sound.at(OUTPUT_GAIN_ID) = -6.0F;
    ASSERT_TRUE(processor.loadSoundSnapshot(sound, "Local sound change"));
    const auto savedSound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::update);
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).dialog(), nullptr);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    ASSERT_TRUE(processor.getSelectedUserPreset());
    EXPECT_EQ(selection().id, preset.id);
    EXPECT_EQ(selection().file, preset.file);
    EXPECT_NE(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, savedSound));
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), preset.name);
    toolbar->undo();
    EXPECT_TRUE(sameProcessingState(provenance().baseline, savedSound));
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), preset.name + " *");
}

TEST_F(PresetWorkflowTest, DeleteDetachesFileButRetainsPersistentNameAndLatestBaseline)
{
    const auto preset = saveAndLoad();
    setOutput(-6.0F);
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess(*toolbar).action(PresetMenuAction::remove);
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
    EXPECT_FALSE(preset.file.exists());
    EXPECT_FALSE(processor.getSelectedUserPreset());
    ASSERT_TRUE(processor.getPresetProvenance());
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_EQ(PresetWorkflowTestAccess(*toolbar).selector().getText(), preset.name + " *");
}

TEST_F(PresetWorkflowTest, StaleCompletionsCannotReplaceRestoredSlotsOrNewerBaselines)
{
    const auto preset = saveAndLoad();
    openToolbar();
    const auto request = PresetWorkflowTestAccess(*toolbar).request(PresetOperation::update);
    auto modified = preset;
    modified.name = "Stale result";
    modified.snapshot.at(OUTPUT_GAIN_ID) = -12.0F;
    restore();
    PresetWorkflowTestAccess(*toolbar).complete(request, modified);
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_FALSE(processor.getSelectedUserPreset());
    const auto slotRequest = PresetWorkflowTestAccess(*toolbar).request(PresetOperation::saveAs);
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    PresetWorkflowTestAccess(*toolbar).complete(slotRequest, modified);
    EXPECT_FALSE(processor.getPresetProvenance());
    ASSERT_TRUE(processor.selectComparisonSlot(0));
    const auto baselineRequest = PresetWorkflowTestAccess(*toolbar).request(PresetOperation::saveAs);
    processor.setSelectedUserPreset(modified);
    PresetWorkflowTestAccess(*toolbar).complete(baselineRequest, preset);
    EXPECT_EQ(provenance().name, modified.name);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, modified.snapshot));
    const auto bindingRequest = PresetWorkflowTestAccess(*toolbar).request(PresetOperation::saveAs);
    auto rebound = modified;
    rebound.fingerprint = "different exact library revision";
    ASSERT_TRUE(processor.reconnectSelectedUserPreset(
        rebound, processor.getHistory().generation(), processor.getActiveComparisonSlot()
    ));
    PresetWorkflowTestAccess(*toolbar).complete(bindingRequest, preset);
    EXPECT_EQ(selection().fingerprint, rebound.fingerprint);
    EXPECT_EQ(provenance().name, modified.name);
}

/// Completed writes remain pending during a local gesture, then publish their exact revision without changing sound.
TEST_F(PresetWorkflowTest, SuccessfulMetadataCompletionsWaitForActiveGesture)
{
    const auto preset = saveAndLoad();
    openToolbar();
    for (const auto operation :
         {
             PresetOperation::saveAs,
             PresetOperation::update,
             PresetOperation::rename,
             PresetOperation::reconnect,
             PresetOperation::remove,
         })
    {
        SCOPED_TRACE(static_cast<int>(operation));
        const auto request = PresetWorkflowTestAccess(*toolbar).request(operation);
        auto result = selection();
        if (operation == PresetOperation::saveAs) {
            result.id = juce::Uuid().toString();
            result.name = "Saved during gesture";
        } else if (operation == PresetOperation::rename) {
            result.name = "Renamed during gesture";
        }
        if (operation != PresetOperation::reconnect) {
            result.fingerprint += "-new-revision";
        }
        const auto previous = processor.getPresetPresentation();
        processor.getHistory().beginEdit("Active sound gesture");
        setOutput(-3.0F);
        const auto sound = processor.getSoundSnapshot();
        PresetWorkflowTestAccess(*toolbar).publish(request, result);
        EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).busy());
        EXPECT_EQ(selection().fingerprint, previous.selection.value_or(UserPreset {}).fingerprint);
        EXPECT_EQ(provenance().name, previous.provenance.value_or(PresetProvenance {}).name);
        toolbar->refresh();
        EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).busy());
        processor.getHistory().endEdit();
        toolbar->refresh();
        EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).busy());
        EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
        if (operation == PresetOperation::remove) {
            EXPECT_FALSE(processor.getSelectedUserPreset());
            EXPECT_EQ(provenance().name, previous.provenance.value_or(PresetProvenance {}).name);
        } else {
            EXPECT_EQ(selection().fingerprint, result.fingerprint);
            EXPECT_EQ(
                provenance().name,
                operation == PresetOperation::reconnect ? previous.provenance.value_or(PresetProvenance {}).name
                                                        : result.name
            );
        }
    }
    EXPECT_TRUE(preset.file.exists());
}

/// A deferred successful write cannot replace a later host restore, even after the local gesture is invalidated.
TEST_F(PresetWorkflowTest, DeferredMetadataCompletionRevalidatesHostRestore)
{
    const auto preset = saveAndLoad();
    openToolbar();
    const auto request = PresetWorkflowTestAccess(*toolbar).request(PresetOperation::rename);
    auto renamed = preset;
    renamed.name = "Stale deferred rename";
    renamed.fingerprint += "-new-revision";
    processor.getHistory().beginEdit("Active gesture");
    PresetWorkflowTestAccess(*toolbar).publish(request, renamed);
    EXPECT_TRUE(PresetWorkflowTestAccess(*toolbar).busy());
    restore();
    toolbar->refresh();
    EXPECT_FALSE(PresetWorkflowTestAccess(*toolbar).busy());
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_FALSE(processor.getSelectedUserPreset());
}

TEST_F(PresetWorkflowTest, ImportCompletionDoesNotChangeActiveSoundOrProvenance)
{
    const auto preset = saveAndLoad();
    openToolbar();
    auto imported = preset;
    imported.id = juce::Uuid().toString();
    imported.name = "Imported library entry";
    imported.snapshot.at(OUTPUT_GAIN_ID) = -12.0F;
    const auto sound = processor.getSoundSnapshot();
    PresetWorkflowTestAccess(*toolbar).complete(
        PresetWorkflowTestAccess(*toolbar).request(PresetOperation::importPreset), imported
    );
    EXPECT_EQ(provenance().id, preset.id);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
}
}  // namespace
}  // namespace esgrove::image_control::test
