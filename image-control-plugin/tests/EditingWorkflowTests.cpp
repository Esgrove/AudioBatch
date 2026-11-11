/// Exercises composition of the editor's local editing toolbar and keyboard shortcuts.
/// Assertions inspect production controls, parameter values, and geometry rather than pixels.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "tests/CompactBandTestSupport.h"
#include "tests/TestSignals.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/ProcessingToolbar.h"
#include "ui/SpatialSpectrogramView.h"
#include "UserPresets.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
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

/// Wide headers share spare space on both sides of history/presets while monitoring remains right-anchored.
TEST(EditingWorkflow, WideHeaderExpandsTheGapBetweenPresetsAndMono)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    auto* toolbar = editingChild<ProcessingToolbar>(editor);
    auto* settings = editingControl(editor, "display-settings");
    auto* undo = editingControl(editor, "processing-undo");
    juce::Button* mono = nullptr;
    juce::Button* post = nullptr;
    for (auto* child : editor.getChildren()) {
        if (auto* button = dynamic_cast<juce::Button*>(child)) {
            if (button->getButtonText() == "Mono") {
                mono = button;
            } else if (button->getButtonText() == "Post") {
                post = button;
            }
        }
    }
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(settings, nullptr);
    ASSERT_NE(undo, nullptr);
    ASSERT_NE(mono, nullptr);
    ASSERT_NE(post, nullptr);
    int previousTrailingGap = 0;
    int previousLeadingGap = 0;
    int previousMonoInset = -1;
    for (const int width : {1600, 2000, 2400, 4096}) {
        editor.setSize(width, editor.getHeight());
        EXPECT_EQ(settings->getX(), 8 + 124 + 8);
        EXPECT_EQ(toolbar->getWidth(), PROCESSING_TOOLBAR_MAX_WIDTH);
        const int leadingGap = undo->getX() - post->getRight();
        const int trailingGap = mono->getX() - toolbar->getRight();
        EXPECT_GT(leadingGap, previousLeadingGap);
        EXPECT_GT(trailingGap, previousTrailingGap);
        EXPECT_LE(std::abs((leadingGap - 8) - (trailingGap - PROCESSING_TOOLBAR_CONTROL_GAP)), 1);
        const int monoInset = width - mono->getRight();
        if (previousMonoInset >= 0) {
            EXPECT_EQ(monoInset, previousMonoInset);
        }
        previousLeadingGap = leadingGap;
        previousTrailingGap = trailingGap;
        previousMonoInset = monoInset;
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
        // A native window on a scaled display can snap the requested size to its pixel grid.
        const int editorWidth = editor.getWidth();
        EXPECT_TRUE(slotA->hasKeyboardFocus(false));
        const bool inHeader = toolbar->getY() < 36;
        EXPECT_EQ(inHeader, editorWidth >= INLINE_PRESET_EDITOR_WIDTH);
        sawInline = sawInline || inHeader;
        sawRow = sawRow || !inHeader;
        EXPECT_EQ(toggle->isVisible(), !inHeader);
        EXPECT_GE(selector->getWidth(), 100);
        EXPECT_LE(selector->getWidth(), 320);
        if (inHeader) {
            EXPECT_EQ(toolbar->getX() - redo->getRight(), 4);
            const int extraWidth = editorWidth - INLINE_PRESET_EDITOR_WIDTH;
            const int outputGrowth = std::min(extraWidth, 50);
            const int selectorGrowth = std::min(extraWidth - outputGrowth, 220);
            const int spareSpace = extraWidth - outputGrowth - selectorGrowth;
            EXPECT_EQ(mono->getX() - toolbar->getRight(), 4 + spareSpace - spareSpace / 2);
            EXPECT_EQ(toolbar->getWidth(), 328 + selectorGrowth);
            EXPECT_EQ(selector->getWidth(), 100 + selectorGrowth);
            EXPECT_EQ(output->getWidth(), 190 + outputGrowth);
            EXPECT_EQ(output->getRight(), editorWidth - 8);
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
        juce::KeyPress(
            'z', juce::ModifierKeys(juce::ModifierKeys::commandModifier).withFlags(juce::ModifierKeys::shiftModifier), 0
        )
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
        juce::KeyPress(
            'z', juce::ModifierKeys(juce::ModifierKeys::commandModifier).withFlags(juce::ModifierKeys::shiftModifier), 0
        )
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
        juce::KeyPress(
            'z', juce::ModifierKeys(juce::ModifierKeys::commandModifier).withFlags(juce::ModifierKeys::shiftModifier), 0
        ),
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
                juce::KeyPress(
                    'y',
                    juce::ModifierKeys(juce::ModifierKeys::commandModifier).withFlags(juce::ModifierKeys::altModifier),
                    0
                )
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
