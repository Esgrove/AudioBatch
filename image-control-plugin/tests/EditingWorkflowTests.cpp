/// Exercises composition of the editor's local editing toolbar and keyboard shortcuts.
/// Assertions inspect production controls, parameter values, and geometry rather than pixels.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "tests/TestSignals.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/DisplayDisclosure.h"
#include "ui/ProcessingToolbar.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SplitDivider.h"
#include "UserPresets.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
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
/// First whole-editor width fitting the compact 328-pixel preset group and a usable 190-pixel output slider.
constexpr int INLINE_PRESET_EDITOR_WIDTH {1114};

/// Observes host gesture boundaries and detaches before the processor-owned parameter disappears.
class EditingGestureCounter final : private juce::AudioProcessorParameter::Listener
{
public:
    /// Observes a parameter that outlives this temporary test listener.
    explicit EditingGestureCounter(juce::RangedAudioParameter& observed) : parameter(observed)
    {
        parameter.addListener(this);
    }

    /// Removes the listener even when an assertion exits the test early.
    ~EditingGestureCounter() override
    {
        parameter.removeListener(this);
    }

    EditingGestureCounter(const EditingGestureCounter&) = delete;
    EditingGestureCounter(EditingGestureCounter&&) = delete;
    EditingGestureCounter& operator=(const EditingGestureCounter&) = delete;
    EditingGestureCounter& operator=(EditingGestureCounter&&) = delete;

    /// Returns host begin and end notifications in their delivery order.
    [[nodiscard]] const std::vector<bool>& gestureBoundaries() const
    {
        return boundaries;
    }

private:
    /// Values are checked directly through the production parameter and slider.
    void parameterValueChanged(int /*index*/, float /*value*/) override {}

    /// Records each boundary without inspecting history during synchronous host callbacks.
    void parameterGestureChanged(int /*index*/, const bool beginning) override
    {
        boundaries.push_back(beginning);
    }

    /// Processor-owned parameter that stays alive until this listener detaches.
    juce::RangedAudioParameter& parameter;
    /// Ordered host begin and end notifications.
    mutable std::vector<bool> boundaries;
};

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

/// Checks visible siblings for usable, contained, disjoint hit targets in the selected part of their parent.
void expectEditingControlsContained(const juce::Component& parent, const int maximumTop, const int minimumHeight)
{
    std::vector<juce::Rectangle<int>> bounds;
    for (const auto* child : parent.getChildren()) {
        if (!child->isVisible() || child->getY() >= maximumTop) {
            continue;
        }
        SCOPED_TRACE(child->getComponentID().toStdString());
        EXPECT_FALSE(child->getBounds().isEmpty());
        EXPECT_TRUE(parent.getLocalBounds().contains(child->getBounds()));
        EXPECT_GE(child->getHeight(), minimumHeight);
        for (const auto previous : bounds) {
            EXPECT_FALSE(previous.intersects(child->getBounds()));
        }
        bounds.push_back(child->getBounds());
    }
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
    for (const int width :
         {
             760,
             939,
             940,
             1000,
             INLINE_PRESET_EDITOR_WIDTH - 1,
             INLINE_PRESET_EDITOR_WIDTH,
             INLINE_PRESET_EDITOR_WIDTH + 1,
             1200,
         })
    {
        SCOPED_TRACE(width);
        editor.setSize(width, editor.getHeight());
        const bool inHeader = width >= INLINE_PRESET_EDITOR_WIDTH;
        EXPECT_EQ(toolbar->getHeight(), inHeader ? 28 : PROCESSING_TOOLBAR_HEIGHT);
        EXPECT_TRUE(editor.getLocalBounds().contains(toolbar->getBounds()));
        std::vector<juce::Rectangle<int>> headerBounds;
        for (const auto* child : editor.getChildren()) {
            if (child->isVisible() && child->getY() < 36
                && (child == toolbar || dynamic_cast<const juce::Button*>(child) != nullptr
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
            if (!child->isVisible()) {
                continue;
            }
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

/// Host resize constraints use the same square maximum for both editor dimensions.
TEST(EditingWorkflow, SquareSizeLimit)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    const auto* constrainer = editor.getConstrainer();
    ASSERT_NE(constrainer, nullptr);
    EXPECT_EQ(constrainer->getMaximumWidth(), 4096);
    EXPECT_EQ(constrainer->getMaximumHeight(), 4096);
    editor.setSize(4096, 4096);
    EXPECT_EQ(editor.getWidth(), 4096);
    EXPECT_EQ(editor.getHeight(), 4096);
}

/// Header pressure compresses gaps smoothly while spare space stays before the right-anchored output group.
TEST(EditingWorkflow, ToolbarCompressesContinuouslyBeforeShrinkingOutput)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    auto* settings = editingControl(editor, "display-settings");
    auto* redo = editingControl(editor, "processing-redo");
    auto* output = editingChild<ControlSlider>(editor);
    auto* toolbarToggle = editingControl(editor, "processing-toolbar-toggle");
    juce::Button* bypass = nullptr;
    juce::Button* delta = nullptr;
    juce::Button* mono = nullptr;
    for (auto* child : editor.getChildren()) {
        if (auto* button = dynamic_cast<juce::Button*>(child)) {
            if (button->getButtonText() == "Mono") {
                mono = button;
            } else if (button->getButtonText() == "Delta") {
                delta = button;
            } else if (button->getButtonText() == "Bypass") {
                bypass = button;
            }
        }
    }
    ASSERT_NE(settings, nullptr);
    ASSERT_NE(redo, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(toolbarToggle, nullptr);
    ASSERT_NE(mono, nullptr);
    ASSERT_NE(delta, nullptr);
    ASSERT_NE(bypass, nullptr);
    int previousSettingsX = -1;
    int previousOutputWidth = -1;
    for (int width = 760; width < INLINE_PRESET_EDITOR_WIDTH; ++width) {
        editor.setSize(width, editor.getHeight());
        EXPECT_GE(mono->getX() - redo->getRight(), 2);
        if (output->getWidth() < 240) {
            EXPECT_LE(mono->getX() - redo->getRight(), 8);
        }
        EXPECT_EQ(toolbarToggle->getRight(), width - 8);
        EXPECT_GE(delta->getX() - mono->getRight(), 2);
        EXPECT_LE(delta->getX() - mono->getRight(), 8);
        EXPECT_GE(bypass->getX() - delta->getRight(), 2);
        EXPECT_LE(bypass->getX() - delta->getRight(), 8);
        EXPECT_GE(output->getWidth(), 190);
        EXPECT_LE(output->getWidth(), 240);
        if (previousSettingsX >= 0) {
            EXPECT_LE(std::abs(settings->getX() - previousSettingsX), 1);
            EXPECT_LE(std::abs(output->getWidth() - previousOutputWidth), 2);
        }
        previousSettingsX = settings->getX();
        previousOutputWidth = output->getWidth();
    }
    std::vector<int> rightInsets;
    for (const int width : {1600, 2400, 4096}) {
        editor.setSize(width, editor.getHeight());
        EXPECT_EQ(output->getWidth(), 240);
        EXPECT_FALSE(toolbarToggle->isVisible());
        EXPECT_TRUE(toolbarToggle->getBounds().isEmpty());
        EXPECT_EQ(output->getRight(), width - 8);
        std::vector<int> insets;
        for (const auto* control : std::array<const juce::Component*, 4> {mono, delta, bypass, output}) {
            insets.push_back(width - control->getRight());
        }
        if (rightInsets.empty()) {
            rightInsets = insets;
        } else {
            EXPECT_EQ(insets, rightInsets);
        }
    }
}

/// Each placement's header edges grow monotonically one pixel at a time and retrace identical geometry.
TEST(EditingWorkflow, HeaderResizingNeverOscillatesOrChangesButtonWidthsBackwards)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    for (const auto& [minimumWidth, maximumWidth] :
         std::array {std::pair {760, INLINE_PRESET_EDITOR_WIDTH - 1}, std::pair {INLINE_PRESET_EDITOR_WIDTH, 1800}})
    {
        editor.setSize(minimumWidth, editor.getHeight());
        std::vector<juce::Component*> controls;
        for (auto* child : editor.getChildren()) {
            if (child->isVisible() && child->getY() < 36
                && (dynamic_cast<juce::Button*>(child) != nullptr || dynamic_cast<juce::Slider*>(child) != nullptr
                    || dynamic_cast<juce::Label*>(child) != nullptr
                    || dynamic_cast<ProcessingToolbar*>(child) != nullptr))
            {
                controls.push_back(child);
            }
        }
        ASSERT_FALSE(controls.empty());
        std::vector<std::vector<juce::Rectangle<int>>> layouts;
        for (int width = minimumWidth; width <= maximumWidth; ++width) {
            editor.setSize(width, editor.getHeight());
            std::vector<juce::Rectangle<int>> bounds;
            for (std::size_t index = 0; index < controls.size(); ++index) {
                const auto current = controls.at(index)->getBounds();
                SCOPED_TRACE(controls.at(index)->getName().toStdString());
                SCOPED_TRACE(width);
                if (!layouts.empty()) {
                    const auto previous = layouts.back().at(index);
                    EXPECT_GE(current.getX(), previous.getX());
                    EXPECT_LE(current.getX(), previous.getX() + 1);
                    EXPECT_GE(current.getRight(), previous.getRight());
                    EXPECT_LE(current.getRight(), previous.getRight() + 1);
                    EXPECT_GE(current.getWidth(), previous.getWidth());
                    EXPECT_LE(current.getWidth(), previous.getWidth() + 1);
                }
                bounds.push_back(current);
            }
            layouts.push_back(bounds);
        }
        for (int width = maximumWidth; width >= minimumWidth; --width) {
            editor.setSize(width, editor.getHeight());
            const auto& expected = layouts.at(static_cast<std::size_t>(width - minimumWidth));
            for (std::size_t index = 0; index < controls.size(); ++index) {
                EXPECT_EQ(controls.at(index)->getBounds(), expected.at(index));
            }
        }
    }
}

/// A/B and presets reuse their live controls inline, restoring the second row only when it was requested.
TEST(EditingWorkflow, PresetsMoveIntoHeaderAndRestoreRememberedRowVisibility)
{
    for (int combination = 0; combination < 4; ++combination) {
        const bool rowShown = combination % 2 != 0;
        const bool spectrumHidden = combination / 2 != 0;
        ImageControlProcessor processor;
        auto& state = processor.getEditorState();
        state.setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, rowShown, nullptr);
        state.setProperty(HIDE_SPECTROGRAM_PROPERTY, spectrumHidden, nullptr);
        state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
        state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 304, nullptr);
        ImageControlEditor editor(processor);
        auto* toolbar = editingChild<ProcessingToolbar>(editor);
        auto* spectrum = editingChild<SpatialSpectrogramView>(editor);
        auto* selector = editingControl(editor, "preset-selector");
        auto* slotA = editingControl(editor, "comparison-a");
        auto* toggle = editingControl(editor, "processing-toolbar-toggle");
        ASSERT_NE(toolbar, nullptr);
        ASSERT_NE(spectrum, nullptr);
        ASSERT_NE(selector, nullptr);
        ASSERT_NE(slotA, nullptr);
        ASSERT_NE(toggle, nullptr);
        const auto sound = processor.getSoundSnapshot();
        const int initialHeight = editor.getHeight();
        const int initialPlotHeight = spectrum->getHeight();
        for (const int width : {2000, 4096, 760, 2000, 760}) {
            SCOPED_TRACE(width);
            editor.setSize(width, editor.getHeight());
            const bool inlineControls = width >= 2000;
            EXPECT_EQ(toolbar->isVisible(), inlineControls || rowShown);
            EXPECT_EQ(toggle->isVisible(), !inlineControls);
            EXPECT_EQ(static_cast<bool>(state.getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)), rowShown);
            EXPECT_EQ(editingControl(editor, "preset-selector"), selector);
            EXPECT_EQ(editingControl(editor, "comparison-a"), slotA);
            EXPECT_EQ(toolbar->getParentComponent(), &editor);
            EXPECT_LE(selector->getWidth(), 320);
            if (toolbar->isVisible()) {
                EXPECT_GE(selector->getWidth(), 100);
            }
            if (inlineControls) {
                EXPECT_LT(toolbar->getY(), 36);
                EXPECT_LE(toolbar->getWidth(), PROCESSING_TOOLBAR_MAX_WIDTH);
                EXPECT_TRUE(std::ranges::all_of(editor.getChildren(), [toolbar](const auto* child) {
                    return child == toolbar || !child->isVisible() || child->getY() >= 36
                        || !toolbar->getBounds().intersects(child->getBounds());
                }));
            } else if (rowShown) {
                EXPECT_GE(toolbar->getY(), 36);
                EXPECT_EQ(toolbar->getHeight(), PROCESSING_TOOLBAR_HEIGHT);
            }
            const int reclaimed = inlineControls && rowShown ? PROCESSING_TOOLBAR_HEIGHT + 8 : 0;
            if (spectrumHidden) {
                EXPECT_EQ(editor.getHeight(), initialHeight - reclaimed);
            } else {
                EXPECT_EQ(editor.getHeight(), initialHeight);
                EXPECT_EQ(spectrum->getHeight(), initialPlotHeight + reclaimed);
            }
            EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
            EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_WIDTH_PROPERTY)), width);
            EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
            EXPECT_FALSE(processor.getHistory().canUndo());
        }
    }
}

/// Saving while inline preserves the user's narrow-row preference across host state and editor replacement.
TEST(EditingWorkflow, InlinePlacementDoesNotReplaceSavedRowPreference)
{
    for (const bool rowShown : {false, true}) {
        ImageControlProcessor source;
        source.getEditorState().setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, rowShown, nullptr);
        source.getEditorState().setProperty(EDITOR_WIDTH_PROPERTY, 2000, nullptr);
        {
            const ImageControlEditor editor(source);
            auto* toolbar = editingChild<ProcessingToolbar>(editor);
            ASSERT_NE(toolbar, nullptr);
            EXPECT_LT(toolbar->getY(), 36);
            EXPECT_TRUE(toolbar->isVisible());
        }
        juce::MemoryBlock saved;
        source.getStateInformation(saved);
        ImageControlProcessor restored;
        restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        ImageControlEditor editor(restored);
        auto* toolbar = editingChild<ProcessingToolbar>(editor);
        ASSERT_NE(toolbar, nullptr);
        EXPECT_EQ(editor.getWidth(), 2000);
        EXPECT_TRUE(toolbar->isVisible());
        EXPECT_LT(toolbar->getY(), 36);
        editor.setSize(760, editor.getHeight());
        EXPECT_EQ(toolbar->isVisible(), rowShown);
        EXPECT_EQ(static_cast<bool>(restored.getEditorState().getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)), rowShown);
    }
}

/// Every inline control stays contained while resizing through the packing boundary, with no duplicate row.
TEST(EditingWorkflow, InlinePresetThresholdKeepsControlsContainedAndFocusIntact)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    auto* toolbar = editingChild<ProcessingToolbar>(editor);
    auto* slotA = editingControl(editor, "comparison-a");
    auto* selector = editingControl(editor, "preset-selector");
    auto* toggle = editingControl(editor, "processing-toolbar-toggle");
    auto* redo = editingControl(editor, "processing-redo");
    auto* output = editingChild<ControlSlider>(editor);
    juce::Button* mono = nullptr;
    for (auto* child : editor.getChildren()) {
        if (auto* button = dynamic_cast<juce::Button*>(child); button != nullptr && button->getButtonText() == "Mono") {
            mono = button;
        }
    }
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(slotA, nullptr);
    ASSERT_NE(selector, nullptr);
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(redo, nullptr);
    ASSERT_NE(output, nullptr);
    ASSERT_NE(mono, nullptr);
    EXPECT_EQ(PROCESSING_TOOLBAR_INLINE_MIN_WIDTH, 328);
    EXPECT_EQ(PROCESSING_TOOLBAR_MAX_WIDTH, 548);
    slotA->grabKeyboardFocus();
    bool sawInline = false;
    bool sawRow = false;
    for (const int width :
         {
             INLINE_PRESET_EDITOR_WIDTH - 1,
             INLINE_PRESET_EDITOR_WIDTH,
             INLINE_PRESET_EDITOR_WIDTH + 1,
             INLINE_PRESET_EDITOR_WIDTH + 49,
             INLINE_PRESET_EDITOR_WIDTH + 50,
             INLINE_PRESET_EDITOR_WIDTH + 51,
             INLINE_PRESET_EDITOR_WIDTH + 269,
             INLINE_PRESET_EDITOR_WIDTH + 270,
             INLINE_PRESET_EDITOR_WIDTH + 271,
             4096,
             INLINE_PRESET_EDITOR_WIDTH,
             INLINE_PRESET_EDITOR_WIDTH - 1,
         })
    {
        SCOPED_TRACE(width);
        editor.setSize(width, editor.getHeight());
        EXPECT_TRUE(slotA->hasKeyboardFocus(false));
        const bool inHeader = toolbar->getY() < 36;
        EXPECT_EQ(inHeader, width >= INLINE_PRESET_EDITOR_WIDTH);
        sawInline = sawInline || inHeader;
        sawRow = sawRow || !inHeader;
        EXPECT_EQ(toggle->isVisible(), !inHeader);
        EXPECT_GE(selector->getWidth(), 100);
        EXPECT_LE(selector->getWidth(), 320);
        if (inHeader) {
            EXPECT_EQ(toolbar->getX() - redo->getRight(), 4);
            EXPECT_EQ(mono->getX() - toolbar->getRight(), 4);
            const int extraWidth = width - INLINE_PRESET_EDITOR_WIDTH;
            const int outputGrowth = std::min(extraWidth, 50);
            const int selectorGrowth = std::min(extraWidth - outputGrowth, 220);
            EXPECT_EQ(toolbar->getWidth(), 328 + selectorGrowth);
            EXPECT_EQ(selector->getWidth(), 100 + selectorGrowth);
            EXPECT_EQ(output->getWidth(), 190 + outputGrowth);
            EXPECT_EQ(output->getRight(), width - 8);
            EXPECT_TRUE(toggle->getBounds().isEmpty());
        }
        expectEditingControlsContained(editor, 36, 1);
        auto* content = toolbar->getChildComponent(0);
        ASSERT_NE(content, nullptr);
        expectEditingControlsContained(*content, content->getHeight(), 24);
    }
    EXPECT_TRUE(sawInline);
    EXPECT_TRUE(sawRow);
    editor.setSize(760, editor.getHeight());
    auto* disclosure = dynamic_cast<juce::Button*>(toggle);
    ASSERT_NE(disclosure, nullptr);
    disclosure->onClick();
    editor.setSize(2000, editor.getHeight());
    slotA->grabKeyboardFocus();
    editor.setSize(760, editor.getHeight());
    EXPECT_FALSE(toolbar->isVisible());
    EXPECT_TRUE(toggle->hasKeyboardFocus(false));
    editor.setSize(INLINE_PRESET_EDITOR_WIDTH, editor.getHeight());
    EXPECT_TRUE(toolbar->isVisible());
    EXPECT_FALSE(toggle->isVisible());
    EXPECT_TRUE(toolbar->hasKeyboardFocus(true));
    EXPECT_FALSE(static_cast<bool>(processor.getEditorState().getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)));
}

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

/// Preset identity stays in the selector, without changing the brand header when its row is hidden.
TEST(EditingWorkflow, RestoredPresetIdentityStaysInSelectorAndFitsLongNames)
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
    auto* toolbar = editingChild<ProcessingToolbar>(editor);
    auto* title = editingChild<juce::Label>(editor);
    auto* selector = dynamic_cast<juce::ComboBox*>(editingControl(editor, "preset-selector"));
    auto* disclosure = dynamic_cast<juce::Button*>(editingControl(editor, "processing-toolbar-toggle"));
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NE(selector, nullptr);
    ASSERT_NE(disclosure, nullptr);
    EXPECT_EQ(title->getText(), "ImageControl");
    for (const int width : {760, 940, INLINE_PRESET_EDITOR_WIDTH - 1, INLINE_PRESET_EDITOR_WIDTH, 2000, 760}) {
        editor.setSize(width, editor.getHeight());
        EXPECT_EQ(editingControl(editor, "header-preset-name"), nullptr);
        EXPECT_EQ(toolbar->isVisible(), width >= INLINE_PRESET_EDITOR_WIDTH);
        EXPECT_EQ(selector->getText(), preset.name);
        EXPECT_LE(selector->getWidth(), 320);
        EXPECT_EQ(title->getText(), "ImageControl");
        EXPECT_FLOAT_EQ(title->getFont().getHeight(), 18.0F);
        EXPECT_EQ(title->getBounds().getCentreY(), 22);
        EXPECT_TRUE(editor.getLocalBounds().contains(title->getBounds()));
        for (const auto* child : editor.getChildren()) {
            if (child != title && child->isVisible() && child->getY() < 36) {
                EXPECT_FALSE(title->getBounds().intersects(child->getBounds()));
            }
        }
    }
    disclosure->onClick();
    EXPECT_TRUE(toolbar->isVisible());
    EXPECT_EQ(selector->getText(), preset.name);
    EXPECT_EQ(editingControl(editor, "header-preset-name"), nullptr);
    disclosure->onClick();
    EXPECT_FALSE(toolbar->isVisible());
    EXPECT_EQ(editingControl(editor, "header-preset-name"), nullptr);
}

/// Same-value host recall discards Output's native numeric draft without adding gestures or blocking fresh edits.
TEST(EditingWorkflow, HostRestoreDiscardsUnchangedOutputText)
{
    for (const bool compact : {false, true}) {
        SCOPED_TRACE(compact);
        ImageControlProcessor source;
        source.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
        source.getEditorState().setProperty(COMPACT_BAND_CONTROLS_PROPERTY, compact, nullptr);
        juce::MemoryBlock saved;
        source.getStateInformation(saved);
        ImageControlProcessor processor;
        processor.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
        ImageControlEditor editor(processor);
        editor.addToDesktop(0);
        editor.setVisible(true);
        auto* output = editingChild<ControlSlider>(editor);
        auto* toolbar = editingChild<ProcessingToolbar>(editor);
        auto* strip = editingChild<BandStrip>(editor);
        ASSERT_NE(output, nullptr);
        ASSERT_NE(toolbar, nullptr);
        ASSERT_NE(strip, nullptr);
        auto& parameter = *processor.getParameters().getParameter(OUTPUT_GAIN_ID);
        const EditingGestureCounter gestures(parameter);
        const auto original = plainValue(parameter);
        output->grabKeyboardFocus();
        ASSERT_TRUE(output->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        auto* label = editingChild<juce::Label>(*output);
        ASSERT_NE(label, nullptr);
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        label->getCurrentTextEditor()->setText("-3.25", false);
        EXPECT_FLOAT_EQ(plainValue(parameter), original);
        processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        // Route through the production settings path without requiring a display refresh.
        for (auto* child : editor.getChildren()) {
            if (const auto* button = dynamic_cast<juce::Button*>(child);
                button != nullptr && button->getButtonText() == "Post")
            {
                button->onClick();
            }
        }
        ASSERT_EQ(label->getCurrentTextEditor(), nullptr);
        EXPECT_EQ(strip->getHeight() == BAND_STRIP_COMPACT_HEIGHT, compact);
        EXPECT_FLOAT_EQ(plainValue(parameter), original);
        EXPECT_NEAR(output->getValue(), static_cast<double>(original), 0.0001);
        EXPECT_FALSE(processor.getHistory().canUndo());
        EXPECT_FALSE(processor.getHistory().canRedo());
        EXPECT_FALSE(processor.getHistory().isEditing());
        EXPECT_TRUE(gestures.gestureBoundaries().empty());

        output->grabKeyboardFocus();
        ASSERT_TRUE(output->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        ASSERT_NE(label->getCurrentTextEditor(), nullptr);
        label->getCurrentTextEditor()->setText("-6.5", false);
        ASSERT_TRUE(label->getCurrentTextEditor()->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
        pumpEditing(*toolbar, [&] { return label->getCurrentTextEditor() == nullptr; });
        EXPECT_NEAR(plainValue(parameter), -6.5F, 0.0001F);
        EXPECT_TRUE(processor.getHistory().canUndo());
        EXPECT_FALSE(processor.getHistory().isEditing());
        EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true, false}));
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_FLOAT_EQ(plainValue(parameter), original);
        EXPECT_TRUE(processor.getHistory().redo());
        EXPECT_NEAR(plainValue(parameter), -6.5F, 0.0001F);
        EXPECT_EQ(gestures.gestureBoundaries(), (std::vector<bool> {true, false, true, false, true, false}));
    }
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
        EXPECT_EQ(selector->getText(), "Unsaved");
        EXPECT_EQ(selector->getTextWhenNothingSelected(), "Unsaved");
        selector->setSelectedId(1, juce::sendNotificationSync);
        toolbar.setVisible(false);
        pumpEditing(toolbar, [&] { return processor.getSelectedUserPreset().has_value(); });
        EXPECT_EQ(selector->getText(), "Narrow test");
        EXPECT_NEAR(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), -9.0F, 0.001F);
        EXPECT_TRUE(processor.getHistory().canUndo());
        auto* output = processor.getParameters().getParameter(OUTPUT_GAIN_ID);
        output->setValueNotifyingHost(output->convertTo0to1(-6.0F));
        toolbar.refresh();
        EXPECT_EQ(selector->getText(), "Narrow test *");
        EXPECT_EQ(editingControl(toolbar, "preset-status"), nullptr);
        EXPECT_TRUE(processor.getHistory().undo());
        EXPECT_NEAR(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), 0.0F, 0.001F);
    }
    {
        ProcessingToolbar reopened(processor, directory, PresetDialogPresentation::nonRendering);
        auto* selector = dynamic_cast<juce::ComboBox*>(editingControl(reopened, "preset-selector"));
        ASSERT_NE(selector, nullptr);
        EXPECT_EQ(selector->getText(), "Narrow test *");
        EXPECT_EQ(editingControl(reopened, "preset-status"), nullptr);
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
        ASSERT_NE(selector, nullptr);
        pumpEditing(toolbar, [&] { return selector->getNumItems() == 1 && selector->isEnabled(); });
        selector->setSelectedId(1, juce::sendNotificationSync);
        toolbar.setVisible(false);
        toolbar.undo();
        toolbar.redo();
        // Rejection opens an error dialog, so completion clears progress without re-enabling the selector.
        pumpEditing(toolbar, [&] { return !selector->getTooltip().contains("Loading..."); });
        EXPECT_FALSE(toolbar.isVisible());
        EXPECT_NEAR(processor.getSoundSnapshot().at(OUTPUT_GAIN_ID), -6.0F, 0.001F);
        EXPECT_FALSE(processor.getSelectedUserPreset().has_value());
    }
    EXPECT_TRUE(directory.deleteRecursively());
}
}  // namespace esgrove::image_control::test
