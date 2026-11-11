/// Tests contextual display geometry, disclosure affordances, and owned Settings navigation in the real editor.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "tests/CompactBandTestSupport.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/DisplayDisclosure.h"
#include "ui/ProcessingToolbar.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SplitDivider.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace esgrove::image_control::test
{
/// Hover affordances stay centered and focusable without requesting a strong selected outline.
TEST(EditingWorkflow, DisclosuresUseCenteredHoverOnlyIndicators)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    for (const auto* identifier : {"spectrogram-disclosure", "correlation-disclosure", "band-controls-disclosure"}) {
        auto* disclosure = dynamic_cast<DisplayDisclosure*>(editingControl(editor, identifier));
        ASSERT_NE(disclosure, nullptr);
        EXPECT_LE(std::abs(disclosure->getBounds().getCentreX() - editor.getWidth() / 2), 1);
        EXPECT_TRUE(disclosure->getWantsKeyboardFocus());
        EXPECT_FALSE(disclosure->hasFocusOutline());
        disclosure->setRevealed(false);
        EXPECT_FALSE(disclosure->isIndicatorVisible());
        disclosure->setRevealed(true);
        EXPECT_TRUE(disclosure->isIndicatorVisible());
        disclosure->onClick();
        disclosure->setRevealed(false);
        EXPECT_FALSE(disclosure->isIndicatorVisible());
        EXPECT_FALSE(disclosure->hasFocusOutline());
    }
}

/// Disclosure directions follow the section's collapse direction, and the spectrum arrow meets the plot border.
TEST(EditingWorkflow, DisclosureDirectionsAndSpectrumBorderFollowLayout)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    auto* view = editingChild<SpatialSpectrogramView>(editor);
    auto* spectrum = dynamic_cast<DisplayDisclosure*>(editingControl(editor, "spectrogram-disclosure"));
    auto* correlation = dynamic_cast<DisplayDisclosure*>(editingControl(editor, "correlation-disclosure"));
    auto* controls = dynamic_cast<DisplayDisclosure*>(editingControl(editor, "band-controls-disclosure"));
    ASSERT_NE(view, nullptr);
    ASSERT_NE(spectrum, nullptr);
    ASSERT_NE(correlation, nullptr);
    ASSERT_NE(controls, nullptr);
    for (const int width : {760, 1000, 2000}) {
        editor.setSize(width, 900);
        EXPECT_FALSE(spectrum->isIndicatorPointingDown());
        for (const auto* disclosure : {spectrum, correlation, controls}) {
            EXPECT_FLOAT_EQ(disclosure->getIndicatorBounds().getWidth(), 36.0F);
        }
        const float plotTop = static_cast<float>(view->getY()) + view->getPlotBounds().getY();
        const float triangleCentre = static_cast<float>(spectrum->getY()) + spectrum->getIndicatorBounds().getCentreY();
        EXPECT_NEAR(triangleCentre, plotTop, 0.5F);
        const auto* strip = editingChild<BandStrip>(editor);
        ASSERT_NE(strip, nullptr);
        EXPECT_NEAR(
            static_cast<float>(controls->getY()) + controls->getIndicatorBounds().getCentreY(),
            static_cast<float>(strip->getY()),
            0.5F
        );
        for (const auto* disclosure : {correlation, controls}) {
            EXPECT_TRUE(disclosure->isIndicatorPointingDown());
            disclosure->onClick();
            EXPECT_FALSE(disclosure->isIndicatorPointingDown());
            disclosure->onClick();
            EXPECT_TRUE(disclosure->isIndicatorPointingDown());
        }
    }
    spectrum->onClick();
    EXPECT_TRUE(spectrum->isIndicatorPointingDown());
}

/// A small local proximity region controls a bounded eased fade without a whole-section hover or lingering tail.
TEST(EditingWorkflow, DisclosureProximityAndFadeStayLocalAndBounded)
{
    DisplayDisclosure disclosure("Spectrogram");
    disclosure.setVisible(true);
    disclosure.setSize(48, 20);
    const auto activation = disclosure.getActivationBounds();
    EXPECT_FLOAT_EQ(activation.getWidth(), 72.0F);
    EXPECT_FLOAT_EQ(activation.getHeight(), 20.0F);
    EXPECT_FLOAT_EQ(disclosure.getIndicatorOpacity(), 0.0F);
    disclosure.setPointerPosition(disclosure.getIndicatorBounds().getCentre());
    EXPECT_TRUE(disclosure.isIndicatorVisible());
    EXPECT_FLOAT_EQ(disclosure.getIndicatorOpacity(), 0.0F);
    disclosure.advance(0.035);
    const float first = disclosure.getIndicatorOpacity();
    EXPECT_GT(first, 0.0F);
    disclosure.advance(0.035);
    EXPECT_GT(disclosure.getIndicatorOpacity(), first);
    EXPECT_LT(disclosure.getIndicatorOpacity(), 0.18F);
    disclosure.advance(0.07);
    EXPECT_NEAR(disclosure.getIndicatorOpacity(), 0.18F, 0.0001F);
    for (const auto position :
         {
             juce::Point<float>(activation.getCentreX(), activation.getBottom() + 1.0F),
             juce::Point<float>(activation.getRight() + 1.0F, activation.getCentreY()),
         })
    {
        disclosure.setPointerPosition(position);
        EXPECT_FALSE(disclosure.isIndicatorVisible());
        disclosure.advance(0.05);
        EXPECT_GT(disclosure.getIndicatorOpacity(), 0.0F);
        EXPECT_LT(disclosure.getIndicatorOpacity(), 0.18F);
        disclosure.advance(0.05);
        EXPECT_FLOAT_EQ(disclosure.getIndicatorOpacity(), 0.0F);
        disclosure.setPointerPosition(activation.getCentre());
        disclosure.advance(0.14);
    }
    disclosure.setSize(1000, 18);
    disclosure.setPointerPosition({20.0F, 9.0F});
    EXPECT_FALSE(disclosure.isIndicatorVisible());
    disclosure.advance(0.1);
    EXPECT_FLOAT_EQ(disclosure.getIndicatorOpacity(), 0.0F);
    disclosure.setPointerPosition(disclosure.getIndicatorBounds().getCentre());
    EXPECT_TRUE(disclosure.isIndicatorVisible());
    disclosure.advance(0.14);
    disclosure.setPointerPosition({500.0F, 80.0F});
    EXPECT_FALSE(disclosure.isIndicatorVisible());
    disclosure.advance(0.1);
    EXPECT_FLOAT_EQ(disclosure.getIndicatorOpacity(), 0.0F);
}

/// Divider visibility follows interaction rather than an always-visible resize handle.
TEST(EditingWorkflow, DividerGripIsHiddenUntilHoverOrDrag)
{
    SplitDivider divider;
    divider.setSize(200, 16);
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent event {
        juce::Desktop::getInstance().getMainMouseSource(),
        {100.0F, 8.0F},
        juce::ModifierKeys::leftButtonModifier,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        &divider,
        &divider,
        now,
        {100.0F, 8.0F},
        now,
        1,
        false,
    };
    EXPECT_FALSE(divider.isGripVisible());
    divider.mouseEnter(event);
    EXPECT_TRUE(divider.isGripVisible());
    divider.mouseExit(event);
    EXPECT_FALSE(divider.isGripVisible());
    divider.mouseDown(event);
    EXPECT_TRUE(divider.isGripVisible());
    divider.mouseExit(event);
    EXPECT_TRUE(divider.isGripVisible());
    divider.mouseUp(event);
    EXPECT_FALSE(divider.isGripVisible());
}

/// Equal compact plot margins retain the same geometry for renderer and pointer mapping.
TEST(EditingWorkflow, SpectrogramHasEqualReducedSideInsets)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    auto* view = editingChild<SpatialSpectrogramView>(editor);
    ASSERT_NE(view, nullptr);
    for (const int width : {760, 940, 1200}) {
        editor.setSize(width, editor.getHeight());
        const auto plot = view->getPlotBounds();
        EXPECT_FLOAT_EQ(plot.getX(), static_cast<float>(view->getWidth()) - plot.getRight());
        EXPECT_FLOAT_EQ(plot.getX(), 16.0F);
    }
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

/// Settings uses available below-toolbar space before introducing a scrollbar and stops at natural content height.
TEST(EditingWorkflow, SettingsExpandsToAvailableBottomBeforeScrolling)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    auto* settings = dynamic_cast<juce::Button*>(editingControl(editor, "display-settings"));
    ASSERT_NE(settings, nullptr);
    settings->onClick();
    auto* panel = editingChild<SettingsPanel>(editor);
    auto* callout = dynamic_cast<juce::CallOutBox*>(editingControl(editor, "display-settings-callout"));
    if (panel == nullptr && callout != nullptr) {
        panel = editingChild<SettingsPanel>(*callout);
    }
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(callout, nullptr);
    auto* viewport = dynamic_cast<juce::Viewport*>(editingControl(*panel, "settings-viewport"));
    if (viewport == nullptr) {
        viewport = editingChild<juce::Viewport>(*panel);
    }
    ASSERT_NE(viewport, nullptr);
    for (const int height : {900, 600, 764, 1200, 600}) {
        editor.setSize(1000, height);
        const int available = height - settings->getBottom() - callout->getBorderSize() * 2;
        EXPECT_EQ(panel->getHeight(), std::min(available, panel->getNaturalHeight()));
        EXPECT_TRUE(editor.getLocalBounds().contains(callout->getBounds()));
        EXPECT_EQ(viewport->getVerticalScrollBar().isVisible(), available < panel->getNaturalHeight());
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
}  // namespace esgrove::image_control::test
