/// Exercises production preset toolbar workflows with real processor state and an isolated filesystem library.
/// Tests cover restored provenance, explicit non-loading reconnection, inline naming, confirmations, and stale results.

#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "ui/ImageControlLookAndFeel.h"
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

    /// Checks the production constructor's default without opening a visible alert during tests.
    static bool usesVisibleDialogs(ProcessingToolbar& toolbar)
    {
        return content(toolbar).dialogPresentation == PresetDialogPresentation::visible;
    }

    /// Raises a production warning without a filesystem failure or a native dialog driver.
    static void warning(ProcessingToolbar& toolbar, const juce::String& message)
    {
        content(toolbar).showError(message);
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

    /// Returns the production selector, including its inline overlay and accessible sound/library feedback.
    static PresetNameSelector& selector(ProcessingToolbar& toolbar)
    {
        return content(toolbar).presetSelector;
    }

    /// Finds the actual child editor. The selector owns it for the toolbar's lifetime.
    static juce::TextEditor* nameInput(ProcessingToolbar& toolbar)
    {
        return dynamic_cast<juce::TextEditor*>(selector(toolbar).findChildWithID("preset-name-input"));
    }

    /// Captures menu items without opening a platform-owned popup window.
    static juce::PopupMenu menu(ProcessingToolbar& toolbar)
    {
        return content(toolbar).buildPresetMenu();
    }

    /// Reports the raw draft after the editor's asynchronous text-change callback runs.
    static juce::String draft(ProcessingToolbar& toolbar)
    {
        return content(toolbar).draftName;
    }

    /// Reports inline feedback without requiring a warning dialog.
    static juce::String nameError(ProcessingToolbar& toolbar)
    {
        return content(toolbar).nameError;
    }

    /// Reports whether a draft remains pending even after ordinary Enter hides its editor.
    static bool hasDraft(ProcessingToolbar& toolbar)
    {
        return content(toolbar).nameEditContext.has_value();
    }

    /// Reports production Save admission independently of menu rendering.
    static bool canSave(ProcessingToolbar& toolbar)
    {
        return content(toolbar).canSave();
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

/// Checks a real menu command's enabled state without creating its native popup.
void expectPresetMenuEnabled(ProcessingToolbar& toolbar, const PresetMenuAction action, const bool enabled)
{
    const auto menu = PresetWorkflowTestAccess::menu(toolbar);
    juce::PopupMenu::MenuItemIterator iterator(menu);
    while (iterator.next()) {
        const auto& item = iterator.getItem();
        if (item.itemID == static_cast<int>(action)) {
            EXPECT_EQ(item.isEnabled, enabled) << item.text;
            return;
        }
    }
    ADD_FAILURE() << "Missing preset menu command " << static_cast<int>(action);
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
        toolbar = std::make_unique<ProcessingToolbar>(processor, directory, PresetDialogPresentation::nonRendering);
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

    /// Types through the actual overlay and waits for JUCE's posted text-change callback before continuing.
    void enterName(const juce::String& name) const
    {
        auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
        ASSERT_NE(input, nullptr);
        ASSERT_TRUE(input->isVisible());
        input->setText(name);
        waitForPreset(*toolbar, [&] { return PresetWorkflowTestAccess::draft(*toolbar) == name; });
    }

    /// Sends Return through TextEditor so its posted callback, rather than a test-only commit, handles the draft.
    void acceptName() const
    {
        auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
        ASSERT_NE(input, nullptr);
        ASSERT_TRUE(input->isVisible());
        EXPECT_TRUE(input->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        waitForPreset(*toolbar, [&] {
            return !input->isVisible() || PresetWorkflowTestAccess::nameError(*toolbar).isNotEmpty();
        });
    }

    /// Sends Escape through TextEditor's asynchronous cancellation callback.
    void cancelName() const
    {
        auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
        ASSERT_NE(input, nullptr);
        ASSERT_TRUE(input->isVisible());
        EXPECT_TRUE(input->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::hasDraft(*toolbar); });
    }

    /// Accepts a real owned confirmation through its button callback, then pumps its asynchronous completion.
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

/// Menu order keeps direct Save before Save As and separates naming, file exchange, and maintenance actions.
TEST_F(PresetWorkflowTest, PresetMenuOrdersCommandsAndReflectsSaveAdmission)
{
    openToolbar();
    const auto menu = PresetWorkflowTestAccess::menu(*toolbar);
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
    auto& selector = PresetWorkflowTestAccess::selector(*toolbar);
    EXPECT_FALSE(selector.isTextEditable());
    EXPECT_EQ(selector.getText(), "Unsaved");
    EXPECT_FALSE(selector.isNameEditing());
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    const auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(input->isVisible());
    EXPECT_TRUE(input->getText().isEmpty());
    enterName("Unsaved draft");
    setOutput(-6.0F);
    const auto sound = processor.getSoundSnapshot();
    cancelName();
    EXPECT_EQ(selector.getText(), "Unsaved");
    EXPECT_FALSE(PresetWorkflowTestAccess::canSave(*toolbar));
    EXPECT_FALSE(processor.getPresetProvenance());
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    const auto listing = library.list();
    ASSERT_TRUE(listing);
    EXPECT_TRUE(listing->presets.empty());
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
}

/// The name-label listener routes a child-origin click into inline editing without opening a native preset popup.
TEST_F(PresetWorkflowTest, MouseDownFromSelectorNameChildBeginsInlineEditingWithoutPopup)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    auto& selector = PresetWorkflowTestAccess::selector(*toolbar);
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
    const auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->getText(), preset.name);
    EXPECT_EQ(selector.getPeer(), nullptr);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
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
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::rename);
    auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->isVisible());
    input->setText("Rapid inline rename");
    EXPECT_EQ(PresetWorkflowTestAccess::draft(*toolbar), preset.name);
    EXPECT_TRUE(PresetWorkflowTestAccess::canSave(*toolbar));
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::update);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    EXPECT_EQ(provenance().name, "Rapid inline rename");
    EXPECT_EQ(selection().id, preset.id);
    EXPECT_EQ(selection().file, preset.file);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, sound));
    const auto saved = library.load(selection());
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->name, "Rapid inline rename");
    EXPECT_FALSE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_TRUE(PresetWorkflowTestAccess::nameError(*toolbar).isEmpty());
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
}

/// TextEditor's Return callback must capture its latest input without relying on an earlier text-change callback.
TEST_F(PresetWorkflowTest, ImmediateSaveAsReturnUsesLatestTextBeforeTextChangeDispatch)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->isVisible());
    ASSERT_TRUE(static_cast<bool>(input->onReturnKey));
    input->setText("Rapid Save As copy");
    EXPECT_EQ(PresetWorkflowTestAccess::draft(*toolbar), preset.name);
    input->onReturnKey();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    EXPECT_EQ(provenance().name, "Rapid Save As copy");
    EXPECT_NE(selection().id, preset.id);
    EXPECT_NE(selection().file, preset.file);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, sound));
    const auto original = library.load(preset);
    ASSERT_TRUE(original);
    EXPECT_EQ(original->fingerprint, preset.fingerprint);
    EXPECT_FALSE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_TRUE(PresetWorkflowTestAccess::nameError(*toolbar).isEmpty());
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
}

/// Re-entering naming must preserve undispatched editor text and the explicit copy intent.
TEST_F(PresetWorkflowTest, ImmediateF2PreservesRawTextAndSaveAsIntent)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    auto& selector = PresetWorkflowTestAccess::selector(*toolbar);
    auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->isVisible());
    input->setText("Copy survives immediate F2");
    EXPECT_EQ(PresetWorkflowTestAccess::draft(*toolbar), preset.name);
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    EXPECT_TRUE(input->isVisible());
    EXPECT_EQ(input->getText(), "Copy survives immediate F2");
    EXPECT_EQ(PresetWorkflowTestAccess::draft(*toolbar), "Copy survives immediate F2");
    acceptName();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
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
    EXPECT_FALSE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
}

/// Save rejects a draft invalidated by host restore even before the toolbar observes that restore in refresh.
TEST_F(PresetWorkflowTest, SaveBeforeRefreshAfterHostRestoreCannotWriteOrBindStaleDraft)
{
    const auto preset = saveAndLoad();
    openToolbar();
    for (const auto action : {PresetMenuAction::rename, PresetMenuAction::saveAs}) {
        SCOPED_TRACE(static_cast<int>(action));
        ASSERT_TRUE(processor.loadUserPreset(preset, processor.getHistory().generation(), 0));
        toolbar->refresh();
        PresetWorkflowTestAccess::action(*toolbar, action);
        enterName("Stale name must not be written");
        restore();
        const auto restoredSound = processor.getSoundSnapshot();
        ASSERT_FALSE(processor.getSelectedUserPreset());
        ASSERT_TRUE(PresetWorkflowTestAccess::hasDraft(*toolbar));
        EXPECT_FALSE(PresetWorkflowTestAccess::canSave(*toolbar));
        PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::update);
        EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
        EXPECT_FALSE(PresetWorkflowTestAccess::hasDraft(*toolbar));
        EXPECT_FALSE(PresetWorkflowTestAccess::selector(*toolbar).isNameEditing());
        waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
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
        EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    }
}

/// Ordinary inline Enter validates the name before hiding its editor, without requiring an explicit Save.
TEST_F(PresetWorkflowTest, OrdinaryInlineEnterRetainsInvalidNameEditorAndFeedback)
{
    const auto preset = saveAndLoad();
    const auto sound = processor.getSoundSnapshot();
    openToolbar();
    auto& selector = PresetWorkflowTestAccess::selector(*toolbar);
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    enterName("../invalid ordinary rename");
    acceptName();
    auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(input->isVisible());
    EXPECT_EQ(input->getText(), "../invalid ordinary rename");
    const auto error = PresetWorkflowTestAccess::nameError(*toolbar);
    ASSERT_TRUE(error.isNotEmpty());
    EXPECT_TRUE(selector.getTooltip().contains(error));
    EXPECT_TRUE(input->getTooltip().contains(error));
    EXPECT_TRUE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_FALSE(PresetWorkflowTestAccess::canSave(*toolbar));
    EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    cancelName();
    EXPECT_TRUE(PresetWorkflowTestAccess::nameError(*toolbar).isEmpty());
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
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    enterName("Cancelled copy");
    cancelName();
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    const auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->getText(), preset.name);
    enterName("Separate saved copy");
    acceptName();
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
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
    EXPECT_FALSE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_FALSE(PresetWorkflowTestAccess::canSave(*toolbar));
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
    auto& selector = PresetWorkflowTestAccess::selector(*toolbar);
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    enterName(other->name);
    acceptName();
    EXPECT_FALSE(selector.isNameEditing());
    EXPECT_TRUE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_EQ(provenance().id, preset.id);
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_EQ(processor.getHistory().canUndo(), couldUndo);
    EXPECT_EQ(selector.getText(), other->name + " *");
    EXPECT_TRUE(PresetWorkflowTestAccess::canSave(*toolbar));
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::update, true);
    EXPECT_TRUE(selector.keyPressed(juce::KeyPress(juce::KeyPress::F2Key)));
    cancelName();
    EXPECT_EQ(selector.getText(), preset.name);
    EXPECT_FALSE(PresetWorkflowTestAccess::canSave(*toolbar));
}

/// Background scans and header/row-sized layout changes preserve the overlay text, selection, and draft context.
TEST_F(PresetWorkflowTest, ScanAndLayoutRefreshPreserveInlineDraftAndCaret)
{
    const auto preset = saveAndLoad();
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::rename);
    enterName("Draft survives refresh");
    auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
    ASSERT_NE(input, nullptr);
    input->setHighlightedRegion(juce::Range<int>(2, 7));
    const auto highlighted = input->getHighlightedRegion();
    const auto caret = input->getCaretPosition();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::refresh);
    for (const int width : {PROCESSING_TOOLBAR_INLINE_MIN_WIDTH, PROCESSING_TOOLBAR_MAX_WIDTH, 744}) {
        toolbar->setSize(width, PROCESSING_TOOLBAR_HEIGHT);
        toolbar->refresh();
        EXPECT_EQ(PresetWorkflowTestAccess::nameInput(*toolbar), input);
        EXPECT_TRUE(input->isVisible());
        EXPECT_EQ(input->getText(), "Draft survives refresh");
        EXPECT_EQ(input->getCaretPosition(), caret);
        EXPECT_EQ(input->getHighlightedRegion(), highlighted);
    }
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    EXPECT_EQ(input->getText(), "Draft survives refresh");
    EXPECT_EQ(input->getCaretPosition(), caret);
    EXPECT_EQ(input->getHighlightedRegion(), highlighted);
    EXPECT_TRUE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(PresetWorkflowTestAccess::canSave(*toolbar));
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
        PresetWorkflowTestAccess::action(*toolbar, action);
        enterName("OCCUPIED NAME");
        if (action == PresetMenuAction::rename) {
            acceptName();
            PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::update);
        } else {
            acceptName();
        }
        waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
        const auto error = PresetWorkflowTestAccess::nameError(*toolbar);
        EXPECT_TRUE(error.contains("already exists"));
        EXPECT_TRUE(PresetWorkflowTestAccess::selector(*toolbar).isNameEditing());
        EXPECT_EQ(PresetWorkflowTestAccess::draft(*toolbar), "OCCUPIED NAME");
        EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
        EXPECT_TRUE(PresetWorkflowTestAccess::selector(*toolbar).getTooltip().contains(error));
        const auto menu = PresetWorkflowTestAccess::menu(*toolbar);
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
        EXPECT_TRUE(PresetWorkflowTestAccess::nameError(*toolbar).isEmpty());
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
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::rename);
    enterName("Must not cross restore");
    restore();
    toolbar->refresh();
    EXPECT_FALSE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_FALSE(PresetWorkflowTestAccess::selector(*toolbar).isNameEditing());
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name);
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    enterName("Must not cross slots");
    ASSERT_TRUE(processor.selectComparisonSlot(1));
    toolbar->refresh();
    EXPECT_FALSE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_FALSE(PresetWorkflowTestAccess::selector(*toolbar).isNameEditing());
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), "Unsaved");
    ASSERT_TRUE(processor.selectComparisonSlot(0));
    toolbar->refresh();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    enterName("Must not cross recall");
    auto& selector = PresetWorkflowTestAccess::selector(*toolbar);
    int target = 0;
    for (int index = 0; index < selector.getNumItems(); ++index) {
        if (selector.getItemText(index) == other->name) {
            target = selector.getItemId(index);
        }
    }
    ASSERT_GT(target, 0);
    selector.setSelectedId(target, juce::sendNotificationAsync);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::hasDraft(*toolbar); });
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    EXPECT_FALSE(selector.isNameEditing());
    EXPECT_EQ(selection().id, other->id);
    EXPECT_EQ(provenance().name, other->name);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, otherSound));
    EXPECT_FLOAT_EQ(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), -18.0F);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
}

/// Ordinary plugin construction keeps dialogs visible, independently of neighboring non-rendering test instances.
TEST_F(PresetWorkflowTest, DialogVisibilityIsAnExplicitPerInstancePolicy)
{
    ProcessingToolbar production(processor, directory);
    EXPECT_TRUE(PresetWorkflowTestAccess::usesVisibleDialogs(production));
    openToolbar();
    EXPECT_FALSE(PresetWorkflowTestAccess::usesVisibleDialogs(*toolbar));
    EXPECT_TRUE(PresetWorkflowTestAccess::usesVisibleDialogs(production));
}

/// Test alerts retain messages, styling and asynchronous actions without desktop rendering or modal activation.
TEST_F(PresetWorkflowTest, NonRenderingWarningsRetainMessagesActionsAndPluginStyling)
{
    openToolbar();
    const auto message = juce::String("The preset file changed. Refresh the library and try again.");
    PresetWorkflowTestAccess::warning(*toolbar, message);
    auto* warning = PresetWorkflowTestAccess::dialog(*toolbar);
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
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
}

/// Destructive and reconnection confirmations retain the same peerless presentation as failure reports.
TEST_F(PresetWorkflowTest, RemainingPresetConfirmationsAreNonRenderingInWorkflowTests)
{
    saveAndLoad();
    openToolbar();
    for (const auto action : {PresetMenuAction::remove, PresetMenuAction::reconnect}) {
        PresetWorkflowTestAccess::action(*toolbar, action);
        auto* current = PresetWorkflowTestAccess::dialog(*toolbar);
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
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::remove);
    const juce::Component::SafePointer<juce::AlertWindow> pending(PresetWorkflowTestAccess::dialog(*toolbar));
    ASSERT_NE(pending, nullptr);
    auto* accept = pending->getButton(0);
    ASSERT_NE(accept, nullptr);
    accept->onClick();
    EXPECT_NE(pending, nullptr);
    EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
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
    EXPECT_EQ(selector.getText(), name + " *");
    EXPECT_TRUE(selector.getTitle().contains(name));
    EXPECT_TRUE(selector.getTooltip().contains(name));
    EXPECT_TRUE(selector.getDescription().contains("differs from the stored baseline"));
    EXPECT_TRUE(selector.getTooltip().contains("Unlinked"));
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_FALSE(processor.getSelectedUserPreset());
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    const auto* input = PresetWorkflowTestAccess::nameInput(*toolbar);
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
    auto& selector = PresetWorkflowTestAccess::selector(*toolbar);
    EXPECT_EQ(selector.getText(), preset.name + " *");
    EXPECT_TRUE(selector.getTooltip().contains("1 invalid"));
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::issues, true);
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::refresh);
    EXPECT_TRUE(selector.getTooltip().contains("Scanning..."));
    EXPECT_EQ(selector.getText(), preset.name + " *");
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
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name);
    EXPECT_TRUE(PresetWorkflowTestAccess::selector(*toolbar).getTooltip().contains("File changed"));
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
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name + " *");
    EXPECT_FALSE(processor.getHistory().canUndo());
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
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::reconnect);
    acceptDialog();
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::rename);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    enterName("Renamed identity");
    acceptName();
    EXPECT_TRUE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_EQ(selection().fingerprint, updated->fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, preset.snapshot));
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), "Renamed identity *");
    EXPECT_TRUE(PresetWorkflowTestAccess::canSave(*toolbar));
    setOutput(-3.0F);
    const auto sound = processor.getSoundSnapshot();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::update);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
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
    EXPECT_FALSE(PresetWorkflowTestAccess::hasDraft(*toolbar));
    EXPECT_FALSE(PresetWorkflowTestAccess::canSave(*toolbar));
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), "Renamed identity");
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

/// A direct Save still validates its captured revision and retains failed input without a popup.
TEST_F(PresetWorkflowTest, SaveRejectsChangedRevisionAndKeepsNameDraftInline)
{
    const auto preset = saveAndLoad("Explicit save target");
    setOutput(-6.0F);
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::rename);
    enterName("Pending renamed target");
    acceptName();
    const auto external = library.update(preset, preset.snapshot);
    ASSERT_TRUE(external);
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::update);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    const auto current = library.load(*external);
    ASSERT_TRUE(current);
    EXPECT_TRUE(sameProcessingState(current->snapshot, preset.snapshot));
    EXPECT_EQ(current->name, preset.name);
    EXPECT_EQ(selection().fingerprint, preset.fingerprint);
    EXPECT_EQ(provenance().name, preset.name);
    EXPECT_EQ(PresetWorkflowTestAccess::draft(*toolbar), "Pending renamed target");
    EXPECT_TRUE(PresetWorkflowTestAccess::selector(*toolbar).isNameEditing());
    EXPECT_TRUE(PresetWorkflowTestAccess::nameError(*toolbar).isNotEmpty());
    EXPECT_TRUE(
        PresetWorkflowTestAccess::selector(*toolbar).getTooltip().contains(
            PresetWorkflowTestAccess::nameError(*toolbar)
        )
    );
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
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
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::update);
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess::busy(*toolbar); });
    ASSERT_TRUE(processor.getSelectedUserPreset());
    EXPECT_EQ(selection().id, preset.id);
    EXPECT_EQ(selection().file, preset.file);
    EXPECT_NE(selection().fingerprint, preset.fingerprint);
    EXPECT_TRUE(sameProcessingState(provenance().baseline, savedSound));
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name);
    toolbar->undo();
    EXPECT_TRUE(sameProcessingState(provenance().baseline, savedSound));
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name + " *");
}

/// Invalid names stay editable and explain themselves inline until corrected or cancelled.
TEST_F(PresetWorkflowTest, InvalidNameRemainsInlineUntilCorrected)
{
    openToolbar();
    PresetWorkflowTestAccess::action(*toolbar, PresetMenuAction::saveAs);
    enterName("../invalid");
    EXPECT_FALSE(PresetWorkflowTestAccess::canSave(*toolbar));
    expectPresetMenuEnabled(*toolbar, PresetMenuAction::update, false);
    acceptName();
    EXPECT_EQ(PresetWorkflowTestAccess::dialog(*toolbar), nullptr);
    EXPECT_TRUE(PresetWorkflowTestAccess::selector(*toolbar).isNameEditing());
    EXPECT_EQ(PresetWorkflowTestAccess::draft(*toolbar), "../invalid");
    const auto error = PresetWorkflowTestAccess::nameError(*toolbar);
    EXPECT_TRUE(error.isNotEmpty());
    EXPECT_TRUE(PresetWorkflowTestAccess::selector(*toolbar).getTooltip().contains(error));
    EXPECT_FALSE(PresetWorkflowTestAccess::busy(*toolbar));
    enterName("Valid corrected name");
    EXPECT_TRUE(PresetWorkflowTestAccess::nameError(*toolbar).isEmpty());
    EXPECT_TRUE(PresetWorkflowTestAccess::canSave(*toolbar));
    acceptName();
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
    EXPECT_EQ(PresetWorkflowTestAccess::selector(*toolbar).getText(), preset.name + " *");
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
