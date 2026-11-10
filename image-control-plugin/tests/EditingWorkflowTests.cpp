/// Exercises composition of the editor's local editing toolbar and keyboard shortcuts.
/// Assertions inspect production controls, parameter values, and geometry rather than pixels.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "tests/TestSignals.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/ProcessingToolbar.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "UserPresets.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

namespace esgrove::image_control::test
{
namespace
{
/// Returns an editor-owned direct child, or null while that component type is absent.
/// Check the result before use. It remains valid only while the parent retains that child.
template<typename Child>
Child* editingChild(const juce::Component& parent)
{
    for (auto* child : parent.getChildren()) {
        if (auto* result = dynamic_cast<Child*>(child)) {
            return result;
        }
    }
    return nullptr;
}
/// Finds a control by stable ID through the toolbar's implementation, or null when absent.
/// The parent hierarchy owns the result. Check it before use and do not retain it after that control is removed.
juce::Component* editingControl(juce::Component& parent, const juce::String& identifier)
{
    std::vector<juce::Component*> pending {&parent};
    while (!pending.empty()) {
        auto* component = pending.back();
        pending.pop_back();
        if (component->getComponentID() == identifier) {
            return component;
        }
        for (auto* child : component->getChildren()) {
            pending.push_back(child);
        }
    }
    return nullptr;
}

/// Consumes asynchronous selector messages and disk results with a bounded native event pump.
void pumpEditing(ProcessingToolbar& toolbar, const std::function<bool()>& done)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    do {
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
        toolbar.refresh();
    } while (!done() && juce::Time::getMillisecondCounterHiRes() < deadline);
    EXPECT_TRUE(done());
}
}  // namespace

/// Both rows keep distinct, usable hit targets at minimum width and across the full-width layout threshold.
TEST(EditingWorkflow, ToolbarFitsMinimumWidthAndCompactLayout)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    auto* toolbar = editingChild<ProcessingToolbar>(editor);
    auto* undo = dynamic_cast<juce::DrawableButton*>(editingControl(editor, "processing-undo"));
    auto* redo = dynamic_cast<juce::DrawableButton*>(editingControl(editor, "processing-redo"));
    auto* toggle = dynamic_cast<juce::DrawableButton*>(editingControl(editor, "processing-toolbar-toggle"));
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(undo, nullptr);
    ASSERT_NE(redo, nullptr);
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(undo->getParentComponent(), &editor);
    EXPECT_EQ(redo->getParentComponent(), &editor);
    EXPECT_EQ(toggle->getParentComponent(), &editor);
    EXPECT_NE(undo->getNormalImage(), nullptr);
    EXPECT_NE(redo->getNormalImage(), nullptr);
    EXPECT_NE(toggle->getNormalImage(), nullptr);
    for (const int width : {760, 939, 940, 1000, 1200}) {
        editor.setSize(width, editor.getHeight());
        EXPECT_EQ(toolbar->getHeight(), PROCESSING_TOOLBAR_HEIGHT);
        EXPECT_TRUE(editor.getLocalBounds().contains(toolbar->getBounds()));
        std::vector<juce::Rectangle<int>> headerBounds;
        for (const auto* child : editor.getChildren()) {
            if (child->isVisible() && child->getY() < toolbar->getY()
                && (dynamic_cast<const juce::Button*>(child) != nullptr
                    || dynamic_cast<const juce::Label*>(child) != nullptr
                    || dynamic_cast<const juce::Slider*>(child) != nullptr))
            {
                EXPECT_FALSE(child->getBounds().isEmpty());
                EXPECT_TRUE(editor.getLocalBounds().contains(child->getBounds()));
                for (const auto bounds : headerBounds) {
                    EXPECT_FALSE(bounds.intersects(child->getBounds()));
                }
                headerBounds.push_back(child->getBounds());
            }
        }
        const auto* content = toolbar->getChildComponent(0);
        ASSERT_NE(content, nullptr);
        std::vector<juce::Rectangle<int>> rowBounds;
        for (const auto* child : content->getChildren()) {
            EXPECT_FALSE(child->getBounds().isEmpty());
            EXPECT_TRUE(content->getLocalBounds().contains(child->getBounds()));
            for (const auto bounds : rowBounds) {
                EXPECT_FALSE(bounds.intersects(child->getBounds()));
            }
            rowBounds.push_back(child->getBounds());
        }
        auto* strip = editingChild<BandStrip>(editor);
        ASSERT_NE(strip, nullptr);
        EXPECT_GE(strip->getY(), toolbar->getBottom());
        EXPECT_GE(strip->getHeight(), BAND_STRIP_MIN_HEIGHT);
    }
}

/// Disclosure changes are immediate, preserve expanded size, and survive editor replacement in both analysis modes.
TEST(EditingWorkflow, ToolbarDisclosurePreservesExpandedHeightAndUpdatesCompactHeight)
{
    for (const bool hidden : {false, true}) {
        ImageControlProcessor processor;
        auto& state = processor.getEditorState();
        state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
        state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 304, nullptr);
        state.setProperty(HIDE_SPECTROGRAM_PROPERTY, hidden, nullptr);
        int collapsedHeight = 0;
        {
            ImageControlEditor editor(processor);
            editor.setSize(760, editor.getHeight());
            auto* toolbar = editingChild<ProcessingToolbar>(editor);
            auto* toggle = dynamic_cast<juce::Button*>(editingControl(editor, "processing-toolbar-toggle"));
            ASSERT_NE(toolbar, nullptr);
            ASSERT_NE(toggle, nullptr);
            const int expandedHeight = editor.getHeight();
            auto* strip = editingChild<BandStrip>(editor);
            ASSERT_NE(strip, nullptr);
            const auto stripHeight = strip->getHeight();
            const auto* expandedImage = dynamic_cast<juce::DrawableButton*>(toggle)->getCurrentImage();
            for (int round = 0; round < 3; ++round) {
                toggle->onClick();
                EXPECT_FALSE(toolbar->isVisible());
                EXPECT_FALSE(toggle->getToggleState());
                EXPECT_FALSE(static_cast<bool>(state.getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)));
                EXPECT_EQ(editor.getHeight(), hidden ? expandedHeight - PROCESSING_TOOLBAR_HEIGHT - 8 : expandedHeight);
                EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
                EXPECT_EQ(editingChild<BandStrip>(editor)->getHeight(), stripHeight);
                EXPECT_NE(dynamic_cast<juce::DrawableButton*>(toggle)->getCurrentImage(), expandedImage);
                collapsedHeight = editor.getHeight();
                toggle->onClick();
                EXPECT_TRUE(toolbar->isVisible());
                EXPECT_TRUE(toggle->getToggleState());
                EXPECT_EQ(editor.getHeight(), expandedHeight);
                EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
            }
            toggle->onClick();
        }
        ImageControlEditor reopened(processor);
        auto* toolbar = editingChild<ProcessingToolbar>(reopened);
        auto* toggle = dynamic_cast<juce::Button*>(editingControl(reopened, "processing-toolbar-toggle"));
        ASSERT_NE(toolbar, nullptr);
        ASSERT_NE(toggle, nullptr);
        EXPECT_FALSE(toolbar->isVisible());
        EXPECT_FALSE(toggle->getToggleState());
        EXPECT_EQ(reopened.getHeight(), collapsedHeight);
        EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
        // A restored property is applied by the same production settings path used on every display refresh.
        state.setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, true, nullptr);
        for (auto* child : reopened.getChildren()) {
            if (const auto* button = dynamic_cast<juce::Button*>(child);
                button != nullptr && button->getButtonText() == "Post")
            {
                button->onClick();
            }
        }
        EXPECT_TRUE(toolbar->isVisible());
        EXPECT_TRUE(toggle->getToggleState());
        EXPECT_EQ(reopened.getHeight(), hidden ? collapsedHeight + PROCESSING_TOOLBAR_HEIGHT + 8 : 900);
        EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
        if (hidden) {
            toggle->onClick();
            state.setProperty(HIDE_SPECTROGRAM_PROPERTY, false, nullptr);
            for (auto* child : reopened.getChildren()) {
                if (const auto* button = dynamic_cast<juce::Button*>(child);
                    button != nullptr && button->getButtonText() == "Post")
                {
                    button->onClick();
                }
            }
            EXPECT_EQ(reopened.getHeight(), 900);
            EXPECT_FALSE(toolbar->isVisible());
            EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
        }
    }
}

/// Global toggle shortcuts participate in the same history as slider and bulk editing commands.
TEST(EditingWorkflow, KeyboardToggleUndoAndRedoUseLocalHistory)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    ImageControlEditor editor(processor);
    auto* bypass = processor.getParameters().getParameter(BYPASS_ID);
    ASSERT_NE(bypass, nullptr);
    const auto original = bypass->getValue();
    EXPECT_TRUE(editor.keyPressed(juce::KeyPress('b', juce::ModifierKeys::commandModifier, 0)));
    EXPECT_FALSE(juce::exactlyEqual(bypass->getValue(), original));
    EXPECT_TRUE(processor.getHistory().canUndo());
    EXPECT_TRUE(editor.keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)));
    EXPECT_FLOAT_EQ(bypass->getValue(), original);
    EXPECT_TRUE(editor.keyPressed(
        juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
    ));
    EXPECT_FALSE(juce::exactlyEqual(bypass->getValue(), original));
    auto* toggle = dynamic_cast<juce::Button*>(editingControl(editor, "processing-toolbar-toggle"));
    auto* undo = dynamic_cast<juce::Button*>(editingControl(editor, "processing-undo"));
    auto* redo = dynamic_cast<juce::Button*>(editingControl(editor, "processing-redo"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(undo, nullptr);
    ASSERT_NE(redo, nullptr);
    toggle->onClick();
    EXPECT_TRUE(undo->isVisible());
    EXPECT_TRUE(redo->isVisible());
    EXPECT_TRUE(undo->isEnabled());
    EXPECT_FALSE(redo->isEnabled());
    EXPECT_EQ(undo->getTooltip(), "Undo Bypass");
    undo->onClick();
    EXPECT_FLOAT_EQ(bypass->getValue(), original);
    EXPECT_FALSE(undo->isEnabled());
    EXPECT_TRUE(redo->isEnabled());
    EXPECT_EQ(redo->getTooltip(), "Redo Bypass");
    EXPECT_TRUE(editor.keyPressed(
        juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)
    ));
    EXPECT_FALSE(juce::exactlyEqual(bypass->getValue(), original));
    processor.getHistory().beginEdit("Open gesture");
    toggle->onClick();
    EXPECT_FALSE(undo->isEnabled());
    EXPECT_FALSE(redo->isEnabled());
    processor.getHistory().endEdit();
}

/// Both redo bindings share the editor's local history, including when the optional second row is hidden.
TEST(EditingWorkflow, UndoAndBothRedoShortcutsWorkWithEitherToolbarVisibility)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    ImageControlEditor editor(processor);
    auto* bypass = processor.getParameters().getParameter(BYPASS_ID);
    auto* toggle = dynamic_cast<juce::Button*>(editingControl(editor, "processing-toolbar-toggle"));
    ASSERT_NE(bypass, nullptr);
    ASSERT_NE(toggle, nullptr);
    const auto original = bypass->getValue();
    ASSERT_TRUE(editor.keyPressed(juce::KeyPress('b', juce::ModifierKeys::commandModifier, 0)));
    const auto edited = bypass->getValue();
    const std::vector<juce::KeyPress> redoKeys {
        juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0),
        juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0),
    };
    for (int visibility = 0; visibility < 2; ++visibility) {
        for (const auto& redoKey : redoKeys) {
            EXPECT_TRUE(editor.keyPressed(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)));
            EXPECT_FLOAT_EQ(bypass->getValue(), original);
            EXPECT_TRUE(editor.keyPressed(redoKey));
            EXPECT_FLOAT_EQ(bypass->getValue(), edited);
            EXPECT_FALSE(editor.keyPressed(juce::KeyPress('y', juce::ModifierKeys::noModifiers, 0)));
            EXPECT_FALSE(editor.keyPressed(juce::KeyPress('z', juce::ModifierKeys::noModifiers, 0)));
            EXPECT_FALSE(editor.keyPressed(
                juce::KeyPress('y', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)
            ));
            EXPECT_FLOAT_EQ(bypass->getValue(), edited);
        }
        toggle->onClick();
    }
}

/// Font-independent header buttons retain native accessible names and a toggle state for row disclosure.
TEST(EditingWorkflow, HeaderSymbolsExposeAccessibleNamesAndActions)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    for (const auto& [identifier, title] :
         std::vector<std::pair<juce::String, juce::String>> {
             {"processing-undo", "Undo processing edit"},
             {"processing-redo", "Redo processing edit"},
             {"processing-toolbar-toggle", "A/B comparison and presets"},
         })
    {
        auto* control = editingControl(editor, identifier);
        ASSERT_NE(control, nullptr);
        auto* handler = control->getAccessibilityHandler();
        ASSERT_NE(handler, nullptr);
        EXPECT_EQ(handler->getRole(), juce::AccessibilityRole::button);
        EXPECT_EQ(handler->getTitle(), title);
    }
    auto* toggle = editingControl(editor, "processing-toolbar-toggle");
    auto* toolbar = editingChild<ProcessingToolbar>(editor);
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(toolbar, nullptr);
    EXPECT_TRUE(toggle->getAccessibilityHandler()->getActions().invoke(juce::AccessibilityActionType::toggle));
    EXPECT_FALSE(toolbar->isVisible());
}

/// Contextual display actions preserve the expanded layout and remain reachable in every visibility combination.
TEST(EditingWorkflow, DisplayDisclosuresPreserveGeometryAndRestoreHiddenSections)
{
    for (const bool toolbarShown : {false, true}) {
        for (const bool spectrumShown : {false, true}) {
            for (const bool correlationShown : {false, true}) {
                ImageControlProcessor processor;
                auto& state = processor.getEditorState();
                state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
                state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 304, nullptr);
                state.setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, toolbarShown, nullptr);
                state.setProperty(HIDE_SPECTROGRAM_PROPERTY, !spectrumShown, nullptr);
                state.setProperty(SHOW_BAND_CORRELATION_PROPERTY, correlationShown, nullptr);
                ImageControlEditor editor(processor);
                auto* spectrum = dynamic_cast<juce::Button*>(editingControl(editor, "spectrogram-disclosure"));
                auto* correlation = dynamic_cast<juce::Button*>(editingControl(editor, "correlation-disclosure"));
                auto* settings = dynamic_cast<juce::Button*>(editingControl(editor, "display-settings"));
                ASSERT_NE(spectrum, nullptr);
                ASSERT_NE(correlation, nullptr);
                ASSERT_NE(settings, nullptr);
                for (const int width : {760, 939, 940, 1000, 1200}) {
                    editor.setSize(width, editor.getHeight());
                    EXPECT_TRUE(editor.getLocalBounds().contains(spectrum->getBounds()));
                    EXPECT_TRUE(editor.getLocalBounds().contains(correlation->getBounds()));
                    EXPECT_TRUE(spectrum->isVisible());
                    EXPECT_TRUE(correlation->isVisible());
                    EXPECT_TRUE(spectrum->getWantsKeyboardFocus());
                    EXPECT_TRUE(correlation->getWantsKeyboardFocus());
                    EXPECT_EQ(spectrum->getToggleState(), spectrumShown);
                    EXPECT_EQ(correlation->getToggleState(), correlationShown);
                    const auto* strip = editingChild<BandStrip>(editor);
                    ASSERT_NE(strip, nullptr);
                    EXPECT_EQ(strip->getHeight(), 304);
                }
                spectrum->onClick();
                EXPECT_EQ(static_cast<bool>(state.getProperty(HIDE_SPECTROGRAM_PROPERTY)), spectrumShown);
                spectrum->onClick();
                EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
                correlation->onClick();
                EXPECT_EQ(static_cast<bool>(state.getProperty(SHOW_BAND_CORRELATION_PROPERTY)), !correlationShown);
                correlation->onClick();
                EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
                EXPECT_EQ(editingChild<BandStrip>(editor)->getHeight(), 304);
            }
        }
    }
}

/// The minimum expanded editor still fits full-size strips and a usable spectrogram with both restore handles.
TEST(EditingWorkflow, MinimumHeightFitsAllOptionalRows)
{
    for (const bool toolbar : {false, true}) {
        for (const bool correlation : {false, true}) {
            ImageControlProcessor processor;
            auto& state = processor.getEditorState();
            state.setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, toolbar, nullptr);
            state.setProperty(SHOW_BAND_CORRELATION_PROPERTY, correlation, nullptr);
            ImageControlEditor editor(processor);
            editor.setSize(760, 600);
            const auto* strip = editingChild<BandStrip>(editor);
            const auto* spectrum = editingChild<SpatialSpectrogramView>(editor);
            ASSERT_NE(strip, nullptr);
            ASSERT_NE(spectrum, nullptr);
            EXPECT_GE(strip->getHeight(), BAND_STRIP_MIN_HEIGHT);
            EXPECT_GE(spectrum->getHeight(), 160);
            EXPECT_TRUE(editor.getLocalBounds().contains(strip->getBounds()));
            EXPECT_TRUE(editor.getLocalBounds().contains(spectrum->getBounds()));
        }
    }
}

/// F1 enters the bounded Settings submenu without replacing the callout or adding a desktop window.
TEST(EditingWorkflow, SettingsToggleAndF1ShareNestedShortcutsCallout)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    auto* toolbar = editingChild<ProcessingToolbar>(editor);
    auto* button = dynamic_cast<juce::Button*>(editingControl(editor, "display-settings"));
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(button, nullptr);
    const auto& desktop = juce::Desktop::getInstance();
    const int desktopComponents = desktop.getNumComponents();
    button->onClick();
    auto* callout = editingChild<juce::CallOutBox>(editor);
    ASSERT_NE(callout, nullptr);
    auto* settings = editingChild<SettingsPanel>(*callout);
    ASSERT_NE(settings, nullptr);
    EXPECT_LE(settings->getHeight(), editor.getHeight() - 40);
    EXPECT_TRUE(editor.getLocalBounds().contains(callout->getBounds()));
    EXPECT_EQ(editingControl(*settings, "displayControlsHelp"), nullptr);
    const juce::Component::SafePointer<juce::CallOutBox> safeCallout(callout);
    const auto calloutBounds = callout->getBounds();
    const auto panelBounds = settings->getBounds();
    auto* output = editingChild<ControlSlider>(editor);
    ASSERT_NE(output, nullptr);
    EXPECT_TRUE(callout->canModalEventBeSentToComponent(button));
    EXPECT_FALSE(callout->canModalEventBeSentToComponent(output));
    auto* reference = editingControl(*settings, "keyboardShortcuts");
    auto* preferences = editingControl(*settings, "displaySettingsViewport");
    ASSERT_NE(reference, nullptr);
    ASSERT_NE(preferences, nullptr);
    EXPECT_FALSE(reference->isVisible());
    for (int activation = 0; activation < 2; ++activation) {
        EXPECT_TRUE(editor.keyPressed(juce::KeyPress(juce::KeyPress::F1Key)));
        EXPECT_EQ(editingChild<juce::CallOutBox>(editor), callout);
        EXPECT_EQ(editingChild<SettingsPanel>(*callout), settings);
        EXPECT_TRUE(reference->isVisible());
        EXPECT_FALSE(preferences->isVisible());
        EXPECT_FALSE(reference->isOnDesktop());
        EXPECT_EQ(callout->getBounds(), calloutBounds);
        EXPECT_EQ(settings->getBounds(), panelBounds);
        EXPECT_EQ(desktop.getNumComponents(), desktopComponents);
    }
    for (int index = 0; index < desktop.getNumComponents(); ++index) {
        EXPECT_NE(desktop.getComponent(index)->getComponentID(), "keyboard-shortcuts-window");
    }
    EXPECT_TRUE(settings->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    EXPECT_EQ(editingChild<juce::CallOutBox>(editor), callout);
    EXPECT_FALSE(reference->isVisible());
    EXPECT_TRUE(preferences->isVisible());
    EXPECT_FALSE(settings->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    EXPECT_TRUE(callout->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    pumpEditing(*toolbar, [&] { return safeCallout == nullptr; });
    EXPECT_EQ(editingChild<juce::CallOutBox>(editor), nullptr);

    EXPECT_TRUE(editor.keyPressed(juce::KeyPress(juce::KeyPress::F1Key)));
    auto* first = editingChild<juce::CallOutBox>(editor);
    ASSERT_NE(first, nullptr);
    auto* openedReference = editingControl(*first, "keyboardShortcuts");
    ASSERT_NE(openedReference, nullptr);
    EXPECT_TRUE(openedReference->isVisible());
    EXPECT_EQ(desktop.getNumComponents(), desktopComponents);
    const juce::Component::SafePointer<juce::CallOutBox> safeFirst(first);
    // A rapid double activation cannot queue a replacement while dismissal is pending.
    button->onClick();
    button->onClick();
    pumpEditing(*toolbar, [&] { return safeFirst == nullptr; });
    EXPECT_EQ(editingChild<juce::CallOutBox>(editor), nullptr);
}

/// Footer and Back navigation retain the same callout, preference scroll position, and unmodified sound.
TEST(EditingWorkflow, SettingsFooterAndBackPreserveOwnedCalloutAndPreferences)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    auto* button = dynamic_cast<juce::Button*>(editingControl(editor, "display-settings"));
    ASSERT_NE(button, nullptr);
    button->onClick();
    auto* callout = editingChild<juce::CallOutBox>(editor);
    ASSERT_NE(callout, nullptr);
    auto* settings = editingChild<SettingsPanel>(*callout);
    auto* shortcuts = dynamic_cast<juce::Button*>(editingControl(*callout, "showKeyboardShortcuts"));
    auto* back = dynamic_cast<juce::Button*>(editingControl(*callout, "keyboardShortcutsBack"));
    auto* preferences = dynamic_cast<juce::Viewport*>(editingControl(*callout, "displaySettingsViewport"));
    auto* reference = editingControl(*callout, "keyboardShortcuts");
    ASSERT_NE(settings, nullptr);
    ASSERT_NE(shortcuts, nullptr);
    ASSERT_NE(back, nullptr);
    ASSERT_NE(preferences, nullptr);
    ASSERT_NE(reference, nullptr);
    preferences->setViewPosition(0, preferences->getViewedComponent()->getHeight());
    shortcuts->grabKeyboardFocus();
    const auto scrollPosition = preferences->getViewPositionY();
    ASSERT_GT(scrollPosition, 0);
    const auto settingsState = processor.getEditorState().createCopy();
    const auto sound = processor.getSoundSnapshot();
    const auto bounds = callout->getBounds();
    shortcuts->onClick();
    EXPECT_EQ(editingChild<juce::CallOutBox>(editor), callout);
    EXPECT_TRUE(reference->isVisible());
    EXPECT_FALSE(preferences->isVisible());
    EXPECT_TRUE(back->hasKeyboardFocus(false));
    EXPECT_EQ(callout->getBounds(), bounds);
    back->onClick();
    EXPECT_EQ(editingChild<juce::CallOutBox>(editor), callout);
    EXPECT_FALSE(reference->isVisible());
    EXPECT_TRUE(preferences->isVisible());
    EXPECT_EQ(preferences->getViewPositionY(), scrollPosition);
    EXPECT_TRUE(shortcuts->hasKeyboardFocus(false));
    EXPECT_EQ(callout->getBounds(), bounds);
    EXPECT_TRUE(processor.getEditorState().isEquivalentTo(settingsState));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Deferred dismissal does not take focus away from a control selected outside the settings callout.
TEST(EditingWorkflow, SettingsDismissalPreservesNewControlFocus)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    auto* toolbar = editingChild<ProcessingToolbar>(editor);
    ASSERT_NE(toolbar, nullptr);
    EXPECT_TRUE(editor.keyPressed(juce::KeyPress(',', juce::ModifierKeys::commandModifier, 0)));
    auto* callout = editingChild<juce::CallOutBox>(editor);
    ASSERT_NE(callout, nullptr);
    const juce::Component::SafePointer<juce::CallOutBox> safeCallout(callout);
    auto* slider = editingChild<ControlSlider>(editor);
    ASSERT_NE(slider, nullptr);
    callout->exitModalState(0);
    slider->grabKeyboardFocus();
    pumpEditing(*toolbar, [&] { return safeCallout == nullptr; });
    EXPECT_TRUE(slider->hasKeyboardFocus(true));
}

/// Closing Settings or its shortcuts submenu destroys editor-owned content before processor state disappears.
TEST(EditingWorkflow, OpenSettingsAndShortcutsTearDownSynchronouslyWithEditor)
{
    for (const bool submenu : {false, true}) {
        SCOPED_TRACE(submenu);
        juce::Component::SafePointer<juce::CallOutBox> callout;
        juce::Component::SafePointer<SettingsPanel> panel;
        juce::Component::SafePointer<juce::Component> reference;
        auto processor = std::make_unique<ImageControlProcessor>();
        auto editor = std::make_unique<ImageControlEditor>(*processor);
        editor->addToDesktop(0);
        editor->setVisible(true);
        const auto key = submenu ? juce::KeyPress(juce::KeyPress::F1Key)
                                 : juce::KeyPress(',', juce::ModifierKeys::commandModifier, 0);
        EXPECT_TRUE(editor->keyPressed(key));
        callout = editingChild<juce::CallOutBox>(*editor);
        ASSERT_NE(callout, nullptr);
        panel = editingChild<SettingsPanel>(*callout);
        ASSERT_NE(panel, nullptr);
        reference = editingControl(*panel, "keyboardShortcuts");
        ASSERT_NE(reference, nullptr);
        EXPECT_EQ(reference->isVisible(), submenu);
        editor.reset();
        EXPECT_EQ(callout, nullptr);
        EXPECT_EQ(panel, nullptr);
        EXPECT_EQ(reference, nullptr);
        processor = nullptr;
    }
    juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
}

/// Restored preset feedback uses the existing title space only when the selector row is hidden.
TEST(EditingWorkflow, RestoredPresetIdentityAppearsOnceAndFitsLongNames)
{
    ImageControlProcessor source;
    UserPreset preset;
    preset.id = juce::Uuid().toString();
    preset.name = juce::String::fromUTF8("Wide ambience and detailed spatial movement \xe2\x80\x94 \xc3\xa4\xc3\xb6");
    preset.snapshot = source.getSoundSnapshot();
    source.setSelectedUserPreset(preset);
    source.getEditorState().setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, false, nullptr);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    ImageControlProcessor restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    ImageControlEditor editor(restored);
    auto* identity = dynamic_cast<juce::Label*>(editingControl(editor, "header-preset-name"));
    auto* disclosure = dynamic_cast<juce::Button*>(editingControl(editor, "processing-toolbar-toggle"));
    ASSERT_NE(identity, nullptr);
    ASSERT_NE(disclosure, nullptr);
    EXPECT_TRUE(identity->isVisible());
    EXPECT_EQ(identity->getText(), preset.name);
    EXPECT_EQ(identity->getTooltip(), preset.name + " · Slot A");
    for (const int width : {760, 940, 1200}) {
        editor.setSize(width, editor.getHeight());
        EXPECT_TRUE(editor.getLocalBounds().contains(identity->getBounds()));
        for (auto* child : editor.getChildren()) {
            if (child != identity && child->isVisible() && dynamic_cast<juce::Button*>(child) != nullptr) {
                EXPECT_FALSE(identity->getBounds().intersects(child->getBounds()));
            }
        }
    }
    disclosure->onClick();
    EXPECT_FALSE(identity->isVisible());
    disclosure->onClick();
    EXPECT_TRUE(identity->isVisible());
}

/// A recalled sound reaches the same production DSP path with the editor closed and different host block sizes.
TEST(EditingWorkflow, SnapshotRecallMatchesRealtimeAndOfflineHeadlessRendering)
{
    ImageControlProcessor realtime;
    ImageControlProcessor offline;
    auto sound = realtime.getSoundSnapshot();
    sound.at(widthParameterId(0)) = 0.5F;
    sound.at(recoverParameterId(0)) = 40.0F;
    sound.at(midGainParameterId(1)) = -3.0F;
    sound.at(panParameterId(2)) = 25.0F;
    sound.at(OUTPUT_GAIN_ID) = -6.0F;
    ASSERT_TRUE(realtime.loadSoundSnapshot(sound, "Recall sound"));
    ASSERT_TRUE(offline.loadSoundSnapshot(sound, "Recall sound"));
    realtime.setNonRealtime(false);
    offline.setNonRealtime(true);
    realtime.prepareToPlay(TEST_SAMPLE_RATE, 37);
    offline.prepareToPlay(TEST_SAMPLE_RATE, 511);
    const auto left = makeNoise(0.3F, 16'384, 114);
    const auto right = makeNoise(0.3F, 16'384, 115);
    auto first = makeBuffer({left, right});
    auto second = makeBuffer({left, right});
    const auto render = [](ImageControlProcessor& processor, juce::AudioBuffer<float>& buffer, const int size) {
        juce::MidiBuffer midi;
        for (int offset = 0; offset < buffer.getNumSamples(); offset += size) {
            juce::AudioBuffer<float> block(
                buffer.getArrayOfWritePointers(), 2, offset, (std::min)(size, buffer.getNumSamples() - offset)
            );
            processor.processBlock(block, midi);
        }
    };
    render(realtime, first, 37);
    render(offline, second, 511);
    for (int channel = 0; channel < 2; ++channel) {
        for (int sample = 0; sample < first.getNumSamples(); ++sample) {
            ASSERT_EQ(first.getSample(channel, sample), second.getSample(channel, sample));
        }
    }
    realtime.releaseResources();
    offline.releaseResources();
}

/// Asynchronous preset selection reaches production recall, shows automation dirtiness, and keeps provenance on reopen.
TEST(EditingWorkflow, PresetSelectorLoadsSoundAndRetainsBaselineAcrossToolbarReopen)
{
    ImageControlProcessor processor;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("ImageControl-Toolbar-" + juce::Uuid().toString());
    UserPresets const library(processor.getParameters(), directory);
    auto sound = processor.getSoundSnapshot();
    sound.at(OUTPUT_GAIN_ID) = -9.0F;
    const auto saved = library.saveAs("Narrow test", sound);
    ASSERT_TRUE(saved.has_value());
    {
        ProcessingToolbar toolbar(processor, directory, PresetDialogPresentation::nonRendering);
        toolbar.setSize(744, PROCESSING_TOOLBAR_HEIGHT);
        auto* selector = dynamic_cast<juce::ComboBox*>(editingControl(toolbar, "preset-selector"));
        ASSERT_NE(selector, nullptr);
        pumpEditing(toolbar, [&] { return selector->getNumItems() == 1 && selector->isEnabled(); });
        selector->setSelectedId(1, juce::sendNotificationSync);
        toolbar.setVisible(false);
        pumpEditing(toolbar, [&] { return processor.getSelectedUserPreset().has_value(); });
        EXPECT_NEAR(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), -9.0F, 0.001F);
        EXPECT_TRUE(processor.getHistory().canUndo());
        auto* output = processor.getParameters().getParameter(OUTPUT_GAIN_ID);
        output->setValueNotifyingHost(output->convertTo0to1(-6.0F));
        toolbar.refresh();
        EXPECT_EQ(selector->getText(), "Narrow test");
        auto* status = dynamic_cast<juce::Label*>(editingControl(toolbar, "preset-status"));
        ASSERT_NE(status, nullptr);
        EXPECT_EQ(status->getText(), "Modified");
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), 0.0F, 0.001F);
    }
    {
        ProcessingToolbar reopened(processor, directory, PresetDialogPresentation::nonRendering);
        auto* selector = dynamic_cast<juce::ComboBox*>(editingControl(reopened, "preset-selector"));
        ASSERT_NE(selector, nullptr);
        EXPECT_EQ(selector->getText(), "Narrow test");
        auto* status = dynamic_cast<juce::Label*>(editingControl(reopened, "preset-status"));
        ASSERT_NE(status, nullptr);
        EXPECT_EQ(status->getText(), "Modified");
    }
    EXPECT_TRUE(directory.deleteRecursively());
}
/// A collapsed row still rejects a pending load after header/shortcut history commands restore the captured sound.
TEST(EditingWorkflow, HistoryCommandsRejectPendingPresetLoadsWhileCollapsed)
{
    ImageControlProcessor processor;
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("ImageControl-PendingPreset-" + juce::Uuid().toString());
    UserPresets const library(processor.getParameters(), directory);
    auto sound = processor.getSoundSnapshot();
    sound.at(OUTPUT_GAIN_ID) = -9.0F;
    ASSERT_TRUE(library.saveAs("Pending load", sound).has_value());
    sound.at(OUTPUT_GAIN_ID) = -6.0F;
    ASSERT_TRUE(processor.loadSoundSnapshot(sound, "Previous edit"));
    {
        ProcessingToolbar toolbar(processor, directory, PresetDialogPresentation::nonRendering);
        auto* selector = dynamic_cast<juce::ComboBox*>(editingControl(toolbar, "preset-selector"));
        auto* status = dynamic_cast<juce::Label*>(editingControl(toolbar, "preset-library-status"));
        ASSERT_NE(selector, nullptr);
        ASSERT_NE(status, nullptr);
        pumpEditing(toolbar, [&] { return selector->getNumItems() == 1 && selector->isEnabled(); });
        selector->setSelectedId(1, juce::sendNotificationSync);
        toolbar.setVisible(false);
        toolbar.undo();
        toolbar.redo();
        pumpEditing(toolbar, [&] { return status->getText() != "Loading..."; });
        EXPECT_NEAR(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), -6.0F, 0.001F);
        EXPECT_FALSE(processor.getSelectedUserPreset().has_value());
    }
    EXPECT_TRUE(directory.deleteRecursively());
}
}  // namespace esgrove::image_control::test
