/// Shares PresetWorkflowTest, its production toolbar accessor, and bounded message-thread helpers for preset tests.

#pragma once

#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "ui/ProcessingToolbar.h"
#include "ui/ProcessingToolbarContent.h"
#include "UserPresets.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

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
    /// Observes a test-owned toolbar that outlives this non-owning accessor.
    explicit PresetWorkflowTestAccess(ProcessingToolbar& toolbarToTest) : toolbar(toolbarToTest) {}

    /// Leaves the toolbar and its owned implementation untouched.
    ~PresetWorkflowTestAccess() = default;

    /// Keeps the accessor attached to its original test-owned toolbar.
    PresetWorkflowTestAccess(const PresetWorkflowTestAccess&) = delete;
    /// Keeps the accessor attached to its original test-owned toolbar.
    PresetWorkflowTestAccess(PresetWorkflowTestAccess&&) = delete;
    /// Keeps the accessor attached to its original test-owned toolbar.
    PresetWorkflowTestAccess& operator=(const PresetWorkflowTestAccess&) = delete;
    /// Keeps the accessor attached to its original test-owned toolbar.
    PresetWorkflowTestAccess& operator=(PresetWorkflowTestAccess&&) = delete;

    /// Finds the implementation child retained by the public toolbar for its entire lifetime.
    [[nodiscard]] ProcessingToolbarContent& content() const
    {
        return *dynamic_cast<ProcessingToolbarContent*>(toolbar.getChildComponent(0));
    }

    /// Opens the same explicit command handler used by the production preset menu.
    void action(const PresetMenuAction action) const
    {
        content().handleMenuAction(static_cast<int>(action));
        content().updatePresetSelector();
    }

    /// Returns the member-owned modal dialog, if the current action requires one.
    [[nodiscard]] juce::AlertWindow* dialog() const
    {
        return content().dialog.get();
    }

    /// Checks the production constructor's default without opening a visible alert during tests.
    [[nodiscard]] bool usesVisibleDialogs() const
    {
        return content().dialogPresentation == PresetDialogPresentation::visible;
    }

    /// Raises a production warning without a filesystem failure or a native dialog driver.
    void warning(const juce::String& message) const
    {
        content().showError(message);
    }

    /// Captures the real message-thread request before testing an intervening state transition.
    [[nodiscard]] PresetRequest request(const PresetOperation operation) const
    {
        return content().makeRequest(operation);
    }

    /// Routes a detached completion through the same production acceptance guards used after worker publication.
    void complete(PresetRequest request, std::optional<UserPreset> preset) const
    {
        PresetCompletion completion;
        completion.request = std::move(request);
        completion.preset = std::move(preset);
        content().acceptCompletion(std::move(completion));
        toolbar.refresh();
    }

    /// Publishes a detached worker result through the real refresh path at a deterministic gesture boundary.
    void publish(PresetRequest request, std::optional<UserPreset> preset) const
    {
        auto& implementation = content();
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
    [[nodiscard]] bool busy() const
    {
        return content().busy;
    }

    /// Returns the production selector, including its inline overlay and accessible sound/library feedback.
    [[nodiscard]] PresetNameSelector& selector() const
    {
        return content().presetSelector;
    }

    /// Finds the actual child editor. The selector owns it for the toolbar's lifetime.
    [[nodiscard]] juce::TextEditor* nameInput() const
    {
        return dynamic_cast<juce::TextEditor*>(selector().findChildWithID("preset-name-input"));
    }

    /// Captures menu items without opening a platform-owned popup window.
    [[nodiscard]] juce::PopupMenu menu() const
    {
        return content().buildPresetMenu();
    }

    /// Reports the raw draft after the editor's asynchronous text-change callback runs.
    [[nodiscard]] juce::String draft() const
    {
        return content().draftName;
    }

    /// Reports inline feedback without requiring a warning dialog.
    [[nodiscard]] juce::String nameError() const
    {
        return content().nameError;
    }

    /// Reports whether a draft remains pending even after ordinary Enter hides its editor.
    [[nodiscard]] bool hasDraft() const
    {
        return content().nameEditContext.has_value();
    }

    /// Reports production Save admission independently of menu rendering.
    [[nodiscard]] bool canSave() const
    {
        return content().canSave();
    }

    /// Toolbar owned by the test, which outlives this accessor and retains its implementation child.
    ProcessingToolbar& toolbar;
};

/// Dispatches native callbacks for a bounded interval without running a second application or audio device.
inline void pumpPresetEvents()
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
inline void waitForPreset(ProcessingToolbar& toolbar, const std::function<bool()>& done)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    do {
        pumpPresetEvents();
        toolbar.refresh();
    } while (!done() && juce::Time::getMillisecondCounterHiRes() < deadline);
    EXPECT_TRUE(done());
}

/// Checks a real menu command's enabled state without creating its native popup.
inline void expectPresetMenuEnabled(ProcessingToolbar& toolbar, const PresetMenuAction action, const bool enabled)
{
    const auto menu = PresetWorkflowTestAccess(toolbar).menu();
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
/// Helpers run on the message thread, outside realtime processing, and join workers before deleting storage.
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
        waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).busy(); });
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
        auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
        ASSERT_NE(input, nullptr);
        ASSERT_TRUE(input->isVisible());
        input->setText(name);
        waitForPreset(*toolbar, [&] { return PresetWorkflowTestAccess(*toolbar).draft() == name; });
    }

    /// Sends Return through TextEditor so its posted callback, rather than a test-only commit, handles the draft.
    void acceptName() const
    {
        auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
        ASSERT_NE(input, nullptr);
        ASSERT_TRUE(input->isVisible());
        EXPECT_TRUE(input->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        waitForPreset(*toolbar, [&] {
            return !input->isVisible() || PresetWorkflowTestAccess(*toolbar).nameError().isNotEmpty();
        });
    }

    /// Sends Escape through TextEditor's asynchronous cancellation callback.
    void cancelName() const
    {
        auto* input = PresetWorkflowTestAccess(*toolbar).nameInput();
        ASSERT_NE(input, nullptr);
        ASSERT_TRUE(input->isVisible());
        EXPECT_TRUE(input->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        waitForPreset(*toolbar, [&] { return !PresetWorkflowTestAccess(*toolbar).hasDraft(); });
    }

    /// Accepts a real owned confirmation through its button callback, then pumps its asynchronous completion.
    void acceptDialog() const
    {
        const juce::Component::SafePointer<juce::AlertWindow> dialog(PresetWorkflowTestAccess(*toolbar).dialog());
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
}  // namespace esgrove::image_control::test
