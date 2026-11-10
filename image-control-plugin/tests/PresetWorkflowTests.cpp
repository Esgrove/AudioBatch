/// Exercises production preset toolbar workflows with real processor state and an isolated filesystem library.
/// Tests cover restored provenance, explicit non-loading reconnection, inline naming, confirmations, and stale results.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "ui/ProcessingToolbar.h"
#include "ui/ProcessingToolbarContent.h"
#include "UserPresets.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

namespace esgrove::image_control::test
{
/// Reaches the actual action handlers and completion boundaries without driving platform-owned popup windows.
struct PresetWorkflowTestAccess final {
    /// Finds the implementation child retained by the public toolbar for its entire lifetime.
    static ProcessingToolbarContent& content(ProcessingToolbar& toolbar)
    {
        return *dynamic_cast<ProcessingToolbarContent*>(toolbar.getChildComponent(0));
    }

    /// Opens the same explicit command handler used by the production preset menu.
    static void action(ProcessingToolbar& toolbar, const PresetMenuAction action)
    {
        content(toolbar).handleMenuAction(static_cast<int>(action));
        content(toolbar).updatePresetSelector();
    }

    /// Returns the member-owned modal dialog, if the current action requires one.
    static juce::AlertWindow* dialog(ProcessingToolbar& toolbar)
    {
        return content(toolbar).dialog.get();
    }

    /// Captures the real message-thread request before testing an intervening state transition.
    static PresetRequest request(ProcessingToolbar& toolbar, const PresetOperation operation)
    {
        return content(toolbar).makeRequest(operation);
    }

    /// Routes a detached completion through the same production acceptance guards used after worker publication.
    static void complete(ProcessingToolbar& toolbar, PresetRequest request, std::optional<UserPreset> preset)
    {
        PresetCompletion completion;
        completion.request = std::move(request);
        completion.preset = std::move(preset);
        content(toolbar).acceptCompletion(std::move(completion));
        toolbar.refresh();
    }

    /// Publishes a detached worker result through the real refresh path at a deterministic gesture boundary.
    static void publish(ProcessingToolbar& toolbar, PresetRequest request, std::optional<UserPreset> preset)
    {
        auto& implementation = content(toolbar);
        PresetCompletion completion;
        completion.request = std::move(request);
        completion.preset = std::move(preset);
        {
            const std::scoped_lock lock(implementation.mailbox->mutex);
            implementation.mailbox->completion = std::move(completion);
        }
        implementation.busy = true;
        toolbar.refresh();
    }

    /// Reports whether filesystem work is still outstanding, without observing its mailbox contents.
    static bool busy(ProcessingToolbar& toolbar)
    {
        return content(toolbar).busy;
    }

    /// Returns a real sound-status control for non-pixel presentation and tooltip assertions.
    static juce::Label& soundStatus(ProcessingToolbar& toolbar)
    {
        return content(toolbar).presetStatus;
    }

    /// Returns library progress/diagnostics, which must never replace the modified-state label.
    static juce::Label& libraryStatus(ProcessingToolbar& toolbar)
    {
        return content(toolbar).libraryStatus;
    }

    /// Returns the production name-only selector, including its full accessible identity and tooltip.
    static juce::ComboBox& selector(ProcessingToolbar& toolbar)
    {
        return content(toolbar).presetSelector;
    }
};

namespace
{
/// Dispatches native callbacks for a bounded interval without running a second application or audio device.
void pumpPresetEvents()
{
#if JUCE_MAC
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, static_cast<Boolean>(true));
#elif JUCE_WINDOWS
    MSG message {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    juce::Thread::sleep(1);
#endif
}

/// Waits for a production toolbar condition, with bounded event dispatch and message-thread completion consumption.
void waitForPreset(ProcessingToolbar& toolbar, const std::function<bool()>& done)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    do {
        pumpPresetEvents();
        toolbar.refresh();
    } while (!done() && juce::Time::getMillisecondCounterHiRes() < deadline);
    EXPECT_TRUE(done());
}

/// Owns isolated library storage and the real processor before constructing its toolbar.
class PresetWorkflowTest : public testing::Test
{
public:
    /// Creates a real preset and atomically loads its durable provenance into the current comparison slot.
    UserPreset saveAndLoad(const juce::String& name = "Stored identity")
    {
        auto sound = processor.getSoundSnapshot();
        sound.at(OUTPUT_GAIN_ID) = -9.0F;
        auto preset = library.saveAs(name, sound);
        EXPECT_TRUE(preset.has_value());
        if (!preset) {
            return {};
        }
        EXPECT_TRUE(processor.loadUserPreset(*preset, processor.getHistory().generation(), 0));
        return *preset;
    }

    /// Returns durable provenance only after reporting an explicit test failure when it is absent.
    PresetProvenance provenance() const
    {
        const auto result = processor.getPresetProvenance();
        if (!result) {
            ADD_FAILURE() << "Expected persistent preset provenance";
            return {};
        }
        return *result;
    }

    /// Returns the exact transient revision only after checking that the production binding exists.
    UserPreset selection() const
    {
        const auto result = processor.getSelectedUserPreset();
        if (!result) {
            ADD_FAILURE() << "Expected an exact library preset binding";
            return {};
        }
        return *result;
    }

    /// Saves and restores the same host state, deliberately invalidating the session-only file binding.
    void restore()
    {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }

    /// Builds the real toolbar with injected storage and waits for its independent initial scan.
    void openToolbar()
    {
        toolbar = std::make_unique<ProcessingToolbar>(processor, directory);
        toolbar->setSize(744, PROCESSING_TOOLBAR_HEIGHT);
        waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    }

    /// Changes sound via host automation, which does not create local undo entries.
    void setOutput(const float value)
    {
        auto* output = processor.getParameters().getParameter(OUTPUT_GAIN_ID);
        ASSERT_NE(output, nullptr);
        output->setValueNotifyingHost(output->convertTo0to1(value));
        if (toolbar) {
            toolbar->refresh();
        }
    }

    /// Accepts a real owned confirmation through its button callback, then pumps its asynchronous modal completion.
    void acceptDialog() const
    {
        const juce::Component::SafePointer<juce::AlertWindow> dialog(PresetWorkflowTestAccess::dialog(*toolbar));
        ASSERT_NE(dialog, nullptr);
        auto* accept = dialog->getButton(0);
        ASSERT_NE(accept, nullptr);
        accept->onClick();
        waitForPreset(*toolbar, [&] { return dialog == nullptr; });
    }

    /// Processor and library schema outlive the toolbar's joined worker.
    ImageControlProcessor processor;
    /// Unique scratch library, never production storage.
    juce::File directory {
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("ImageControl-PresetWorkflow-" + juce::Uuid().toString()),
    };
    /// Synchronous second library instance used to exercise real optimistic revision checks.
    UserPresets library {processor.getParameters(), directory};
    /// Public production toolbar, destroyed before its processor and schema.
    std::unique_ptr<ProcessingToolbar> toolbar;

protected:
    /// Joins filesystem jobs before removing only this fixture's uniquely owned directory.
    void TearDown() override
    {
        toolbar.reset();
        EXPECT_TRUE(directory.deleteRecursively());
    }
};

TEST_F(PresetWorkflowTest, RestoredNameAndBaselineRemainVisibleWithoutBindingOrLibraryFile)
{
    const auto name = juce::String::repeatedString("Full preset name ", 6).trim();
    const auto preset = saveAndLoad(name);
    setOutput(-6.0F);
    restore();
    ASSERT_FALSE(processor.getSelectedUserPreset());
    ASSERT_TRUE(preset.file.deleteFile());
    openToolbar();
    auto& selector = PresetWorkflowTestAccess::selector(*toolbar);
    auto& status = PresetWorkflowTestAccess::soundStatus(*toolbar);
    EXPECT_EQ(selector.getText(), name);
    EXPECT_TRUE(selector.getTitle().contains(name));
    EXPECT_TRUE(selector.getTooltip().contains(name));
    EXPECT_EQ(status.getText(), "Modified");
    EXPECT_TRUE(status.getTooltip().contains(name));
    EXPECT_TRUE(status.hitTest(1, 1));
    EXPECT_TRUE(PresetWorkflowTestAccess::libraryStatus(*toolbar).hitTest(1, 1));
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_FALSE(processor.getSelectedUserPreset());
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    auto* dialog = PresetWorkflowTestAccess::dialog(*toolbar);
    ASSERT_NE(dialog, nullptr);
    EXPECT_EQ(dialog->getTextEditorContents("presetName"), name);
}

TEST_F(PresetWorkflowTest, LibraryWarningsAndProgressNeverHideModifiedSoundStatus)
{
    const auto preset = saveAndLoad();
    setOutput(-3.0F);
    ASSERT_TRUE(directory.getChildFile("invalid.xml").replaceWithText("not XML"));
    openToolbar();
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Modified");
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name);
    EXPECT_EQ(PresetWorkflowTestAccess::libraryStatus(*toolbar).getText(), "1 invalid");
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::refresh);
    EXPECT_EQ(PresetWorkflowTestAccess::libraryStatus(*toolbar).getText(), "Scanning...");
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Modified");
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
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
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Saved");
    EXPECT_EQ(PresetWorkflowTestAccess::libraryStatus(*toolbar).getText(), "File changed");
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
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::reconnect);
    EXPECT_FALSE(processor.getSelectedUserPreset());
    const auto* confirmation = PresetWorkflowTestAccess::dialog(*toolbar);
    ASSERT_NE(confirmation, nullptr);
    EXPECT_TRUE(confirmation->getDescription().contains("differs from the project's stored preset baseline"));
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    ASSERT_TRUE(processor.getSelectedUserPreset());
    EXPECT_EQ(selection().fingerprint, updated->fingerprint);
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Modified");
    EXPECT_FALSE(processor.getHistory().canUndo());
}

TEST_F(PresetWorkflowTest, RenameAfterReconnectionRetainsBaselineButBindsActualSavedSound)
{
    const auto preset = saveAndLoad();
    restore();
    auto changed = preset.snapshot;
    changed.at(OUTPUT_GAIN_ID) = -12.0F;
    const auto updated = library.update(preset, changed);
    ASSERT_TRUE(updated);
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::reconnect);
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::rename);
    auto* dialog = PresetWorkflowTestAccess::dialog(*toolbar);
    ASSERT_NE(dialog, nullptr);
    auto* input = dialog->getTextEditor("presetName");
    ASSERT_NE(input, nullptr);
    input->setText("Renamed identity");
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    ASSERT_TRUE(processor.getPresetProvenance());
    EXPECT_EQ(provenance().name, "Renamed identity");
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    ASSERT_TRUE(processor.getSelectedUserPreset());
    EXPECT_TRUE(sameProcessingState(selection().snapshot, changed));
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Saved");
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
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::reconnect);
    ASSERT_NE(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar)->getNumButtons(), 1);
    EXPECT_FALSE(processor.getSelectedUserPreset());
    EXPECT_EQ(provenance().id, preset.id);
}

TEST_F(PresetWorkflowTest, ReconnectionChecksExactRevisionAgainAfterConfirmation)
{
    const auto preset = saveAndLoad();
    restore();
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::reconnect);
    ASSERT_NE(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    ASSERT_TRUE(library.update(preset, preset.snapshot));
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    EXPECT_FALSE(processor.getSelectedUserPreset());
    EXPECT_EQ(provenance().id, preset.id);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    ASSERT_NE(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar)->getNumButtons(), 1);
}

TEST_F(PresetWorkflowTest, UpdateNamesItsCapturedTargetAndRejectsRevisionChangedDuringConfirmation)
{
    const auto preset = saveAndLoad("Explicit update target");
    setOutput(-6.0F);
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::update);
    auto* confirmation = PresetWorkflowTestAccess::dialog(*toolbar);
    ASSERT_NE(confirmation, nullptr);
    EXPECT_EQ(confirmation->getName(), "Update Saved Preset");
    EXPECT_EQ(confirmation->getNumButtons(), 2);
    const auto external = library.update(preset, preset.snapshot);
    ASSERT_TRUE(external);
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    const auto current = library.load(*external);
    ASSERT_TRUE(current);
    EXPECT_TRUE(sameProcessingState(current->snapshot, preset.snapshot));
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Modified");
}

TEST_F(PresetWorkflowTest, ConfirmedUpdateCapturesNewBaselineAndUndoDoesNotRewindIt)
{
    const auto preset = saveAndLoad();
    auto sound = processor.getSoundSnapshot();
    sound.at(OUTPUT_GAIN_ID) = -6.0F;
    ASSERT_TRUE(processor.loadSoundSnapshot(sound, "Local sound change"));
    const auto savedSound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::update);
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    ASSERT_TRUE(processor.getSelectedUserPreset());
    EXPECT_NE(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, savedSound));
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Saved");
    toolbar->undo();
    EXPECT_TRUE(sameProcessingState(provenance().baseline, savedSound));
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Modified");
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name);
}

TEST_F(PresetWorkflowTest, InvalidNameStaysInSameDialogUntilCorrectedOrCancelled)
{
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    auto* dialog = PresetWorkflowTestAccess::dialog(*toolbar);
    ASSERT_NE(dialog, nullptr);
    auto* input = dialog->getTextEditor("presetName");
    ASSERT_NE(input, nullptr);
    input->setText("../invalid");
    auto* accept = dialog->getButton(0);
    ASSERT_NE(accept, nullptr);
    accept->onClick();
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), dialog);
    EXPECT_TRUE(dialog->isCurrentlyModal());
    EXPECT_EQ(input->getText(), "../invalid");
    EXPECT_FALSE(input->getDescription().isEmpty());
    EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
    input->setText("Valid corrected name");
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    ASSERT_TRUE(processor.getPresetProvenance());
    EXPECT_EQ(provenance().name, "Valid corrected name");
}

TEST_F(PresetWorkflowTest, DeleteDetachesFileButRetainsPersistentNameAndLatestBaseline)
{
    const auto preset = saveAndLoad();
    setOutput(-6.0F);
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::remove);
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    EXPECT_FALSE(preset.file.exists());
    EXPECT_FALSE(processor.getSelectedUserPreset());
    ASSERT_TRUE(processor.getPresetProvenance());
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name);
    EXPECT_EQ(PresetWorkflowTestAccess::soundStatus(*toolbar).getText(), "Modified");
}

TEST_F(PresetWorkflowTest, StaleCompletionsCannotReplaceRestoredSlotsOrNewerBaselines)
{
    const auto preset = saveAndLoad();
    openToolbar();
    const auto request = PresetWorkflowTestAccess::request(*toolbar, PresetOperation::update);
    auto modified = preset;
    modified.name = "Stale result";
    modified.snapshot.at(OUTPUT_GAIN_ID) = -12.0F;
    restore();
    PresetWorkflowTestAccess::complete(*toolbar, request, modified);
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_FALSE(processor.getSelectedUserPreset());
    const auto slotRequest = PresetWorkflowTestAccess::request(*toolbar, PresetOperation::saveAs);
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    PresetWorkflowTestAccess::complete(*toolbar, slotRequest, modified);
    EXPECT_FALSE(processor.getPresetProvenance());
    ASSERT_TRUE(processor.selectComparisonSlot(0));
    const auto baselineRequest = PresetWorkflowTestAccess::request(*toolbar, PresetOperation::saveAs);
    processor.setSelectedUserPreset(modified);
    PresetWorkflowTestAccess::complete(*toolbar, baselineRequest, preset);
    EXPECT_EQ(provenance().name, modified.name);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, modified.snapshot));
    const auto bindingRequest = PresetWorkflowTestAccess::request(*toolbar, PresetOperation::saveAs);
    auto rebound = modified;
    rebound.fingerprint = "different exact library revision";
    ASSERT_TRUE(processor.reconnectSelectedUserPreset(
        rebound, processor.getHistory().generation(), processor.getActiveComparisonSlot()
    ));
    PresetWorkflowTestAccess::complete(*toolbar, bindingRequest, preset);
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
        const auto request = PresetWorkflowTestAccess::request(*toolbar, operation);
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
        PresetWorkflowTestAccess::publish(*toolbar, request, result);
        EXPECT_TRUE(PresetWorkflowTestAccess::busy(*toolbar));
        EXPECT_EQ(selection().fingerprint, previous.selection.value_or(UserPreset {}).fingerprint);
        EXPECT_EQ(provenance().name, previous.provenance.value_or(PresetProvenance {}).name);
        toolbar->refresh();
        EXPECT_TRUE(PresetWorkflowTestAccess::busy(*toolbar));
        processor.getHistory().endEdit();
        toolbar->refresh();
        EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
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
    const auto request = PresetWorkflowTestAccess::request(*toolbar, PresetOperation::rename);
    auto renamed = preset;
    renamed.name = "Stale deferred rename";
    renamed.fingerprint += "-new-revision";
    processor.getHistory().beginEdit("Active gesture");
    PresetWorkflowTestAccess::publish(*toolbar, request, renamed);
    EXPECT_TRUE(PresetWorkflowTestAccess::busy(*toolbar));
    restore();
    toolbar->refresh();
    EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
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
    PresetWorkflowTestAccess::complete(
        *toolbar, PresetWorkflowTestAccess::request(*toolbar, PresetOperation::importPreset), imported
    );
    EXPECT_EQ(provenance().id, preset.id);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
}
}  // namespace
}  // namespace esgrove::image_control::test
