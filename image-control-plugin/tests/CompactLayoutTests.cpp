/// Tests the full-strip and compact-value editor composition without screenshot or audio-device dependencies.
/// Compact controls release space to the spectrogram while retaining saved full-strip geometry and state.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SplitDivider.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Finds a direct production child by type, owned by its parent for the duration of each assertion.
template<typename Child>
Child* compactChild(juce::Component& parent)
{
    for (auto* child : parent.getChildren()) {
        if (auto* found = dynamic_cast<Child*>(child)) {
            return found;
        }
    }
    return nullptr;
}

/// Finds the contextual band action without depending on its visible presentation.
juce::Button* compactToggle(const juce::Component& parent)
{
    for (auto* child : parent.getChildren()) {
        if (child->getComponentID() == "band-controls-disclosure") {
            return dynamic_cast<juce::Button*>(child);
        }
    }
    return nullptr;
}
/// Measures the full inline heading width independently from the strip's responsive layout decision.
int expandedHeadingWidth(const BandStrip& strip)
{
    int width = 20 + 20 + 60 + 6 + 8 * 2;
    for (auto* child : strip.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child); label != nullptr
            && (strip.getTitle() == label->getText() + " band controls" || label->getText().contains(" Hz")
                || label->getText().contains(" kHz")))
        {
            auto& lookAndFeel = label->getLookAndFeel();
            width += static_cast<int>(std::ceil(
                         juce::GlyphArrangement::getStringWidth(lookAndFeel.getLabelFont(*label), label->getText())
                     ))
                + lookAndFeel.getLabelBorderSize(*label).getLeftAndRight();
        }
    }
    return width;
}

/// Checks the shared range row and slider alignment across every production strip without painting pixels.
void expectExpandedHeadingRows(const ImageControlEditor& editor, const bool inlineRanges)
{
    int midY = -1;
    int rangeCount = 0;
    for (const auto* child : editor.getChildren()) {
        const auto* strip = dynamic_cast<const BandStrip*>(child);
        if (strip == nullptr) {
            continue;
        }
        for (const auto* control : strip->getChildren()) {
            if (const auto* label = dynamic_cast<const juce::Label*>(control);
                label != nullptr && label->getText().contains("Hz"))
            {
                ++rangeCount;
                EXPECT_TRUE(label->isVisible());
                EXPECT_EQ(label->getY(), inlineRanges ? 10 : 30);
            }
            if (control->getTitle().endsWith(" Mid")) {
                if (midY < 0) {
                    midY = control->getY();
                }
                EXPECT_EQ(control->getY(), midY);
            }
        }
    }
    EXPECT_EQ(rangeCount, static_cast<int>(BAND_COUNT));
}

/// Checks compact column geometry and independent meter alignment through production component bounds.
void expectCompactColumns(const ImageControlEditor& editor, const int expectedHeight)
{
    std::vector<juce::Rectangle<int>> strips;
    for (const auto* child : editor.getChildren()) {
        if (dynamic_cast<const BandStrip*>(child) == nullptr) {
            continue;
        }
        EXPECT_EQ(child->getHeight(), expectedHeight);
        EXPECT_TRUE(editor.getLocalBounds().contains(child->getBounds()));
        for (const auto& bounds : strips) {
            EXPECT_FALSE(bounds.intersects(child->getBounds()));
        }
        strips.push_back(child->getBounds());
    }
    EXPECT_EQ(strips.size(), BAND_COUNT);
    const auto* toggle = compactToggle(editor);
    ASSERT_NE(toggle, nullptr);
    for (const auto* child : editor.getChildren()) {
        if (const auto* strip = dynamic_cast<const BandStrip*>(child)) {
            for (const auto* control : strip->getChildren()) {
                if (dynamic_cast<const ControlSlider*>(control) != nullptr && control->isVisible()) {
                    const auto bounds = editor.getLocalArea(control, control->getLocalBounds());
                    EXPECT_FALSE(bounds.intersects(toggle->getBounds()));
                }
            }
        }
    }
    for (const auto* child : editor.getChildren()) {
        if ((dynamic_cast<const BandCorrelationMeter*>(child) != nullptr && child->isVisible())
            || child->getComponentID() == "correlation-disclosure"
            || child->getComponentID() == "spectrogram-disclosure")
        {
            EXPECT_FALSE(child->getBounds().intersects(toggle->getBounds()));
        }
        if (dynamic_cast<const BandCorrelationMeter*>(child) == nullptr || !child->isVisible()) {
            continue;
        }
        const auto aligned = std::ranges::any_of(strips, [child](const auto& bounds) {
            return bounds.getX() == child->getX() && bounds.getWidth() == child->getWidth();
        });
        EXPECT_TRUE(aligned);
    }
}
}  // namespace

TEST(CompactLayout, ToggleReclaimsSpectrogramSpaceWithoutChangingWindowOrSavedBandHeight)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
    state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 360, nullptr);
    ImageControlEditor editor(processor);
    auto* spectrum = compactChild<SpatialSpectrogramView>(editor);
    auto* strip = compactChild<BandStrip>(editor);
    auto* divider = compactChild<SplitDivider>(editor);
    auto* toggle = compactToggle(editor);
    ASSERT_NE(spectrum, nullptr);
    ASSERT_NE(strip, nullptr);
    ASSERT_NE(divider, nullptr);
    ASSERT_NE(toggle, nullptr);
    const auto sound = processor.getSoundSnapshot();
    const auto originalSize = editor.getLocalBounds();
    const int plotHeight = spectrum->getHeight();
    const int bandHeight = strip->getHeight();
    for (int repetition = 0; repetition < 3; ++repetition) {
        toggle->onClick();
        EXPECT_TRUE(static_cast<bool>(state.getProperty(COMPACT_BAND_CONTROLS_PROPERTY)));
        EXPECT_EQ(editor.getLocalBounds(), originalSize);
        EXPECT_EQ(strip->getHeight(), BAND_STRIP_COMPACT_HEIGHT);
        EXPECT_EQ(spectrum->getHeight() - plotHeight, bandHeight - BAND_STRIP_COMPACT_HEIGHT);
        EXPECT_FALSE(divider->isVisible());
        EXPECT_FALSE(toggle->getToggleState());
        EXPECT_EQ(static_cast<int>(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), bandHeight);
        EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
        EXPECT_FALSE(processor.getHistory().canUndo());
        toggle->onClick();
        EXPECT_EQ(strip->getHeight(), bandHeight);
        EXPECT_EQ(spectrum->getHeight(), plotHeight);
        EXPECT_TRUE(divider->isVisible());
        EXPECT_TRUE(toggle->getToggleState());
        EXPECT_EQ(editor.getLocalBounds(), originalSize);
    }
}

TEST(CompactLayout, CompactRowsStayAlignedAndReachableAtMinimumWidthAndEveryDisplayCombination)
{
    for (int combination = 0; combination < 8; ++combination) {
        const bool toolbar = combination % 2 != 0;
        const bool correlation = (combination / 2) % 2 != 0;
        const bool spectrum = (combination / 4) % 2 != 0;
        ImageControlProcessor processor;
        auto& state = processor.getEditorState();
        state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
        state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 360, nullptr);
        state.setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
        state.setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, toolbar, nullptr);
        state.setProperty(SHOW_BAND_CORRELATION_PROPERTY, correlation, nullptr);
        state.setProperty(HIDE_SPECTROGRAM_PROPERTY, !spectrum, nullptr);
        ImageControlEditor editor(processor);
        auto* toggle = compactToggle(editor);
        ASSERT_NE(toggle, nullptr);
        EXPECT_TRUE(toggle->isVisible());
        EXPECT_TRUE(toggle->getWantsKeyboardFocus());
        for (const int width : {760, 940, 1200}) {
            editor.setSize(width, editor.getHeight());
            EXPECT_TRUE(editor.getLocalBounds().contains(toggle->getBounds()));
            expectCompactColumns(editor, BAND_STRIP_COMPACT_HEIGHT);
            EXPECT_EQ(static_cast<int>(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 360);
            EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
        }
        toggle->onClick();
        auto* strip = compactChild<BandStrip>(editor);
        ASSERT_NE(strip, nullptr);
        EXPECT_EQ(strip->getHeight(), 360);
        EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
    }
}

TEST(CompactLayout, CompactChoiceSurvivesHostStateAndEditorReopening)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
    state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 360, nullptr);
    state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
    {
        ImageControlEditor editor(processor);
        EXPECT_EQ(compactChild<BandStrip>(editor)->getHeight(), BAND_STRIP_COMPACT_HEIGHT);
    }
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    ImageControlProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    ImageControlEditor reopened(restored);
    EXPECT_EQ(compactChild<BandStrip>(reopened)->getHeight(), BAND_STRIP_COMPACT_HEIGHT);
    auto* toggle = compactToggle(reopened);
    ASSERT_NE(toggle, nullptr);
    toggle->onClick();
    EXPECT_EQ(compactChild<BandStrip>(reopened)->getHeight(), 360);
    EXPECT_EQ(reopened.getHeight(), 900);
}

/// Geometry edits in the visual-first mode must not replace the remembered full-strip size.
TEST(CompactLayout, ResizingWithCompactControlsPreservesTheFullStripPreference)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
    state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 360, nullptr);
    ImageControlEditor editor(processor);
    auto* strip = compactChild<BandStrip>(editor);
    auto* spectrum = compactChild<SpatialSpectrogramView>(editor);
    ASSERT_NE(strip, nullptr);
    ASSERT_NE(spectrum, nullptr);
    for (const int height : {600, 799, 800, 1000, 764}) {
        editor.setSize(760, height);
        EXPECT_EQ(editor.getHeight(), height);
        EXPECT_EQ(strip->getHeight(), BAND_STRIP_COMPACT_HEIGHT);
        EXPECT_EQ(static_cast<int>(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 360);
        EXPECT_GE(spectrum->getHeight(), 160);
    }
    editor.setSize(760, 1000);
    auto* toggle = compactToggle(editor);
    ASSERT_NE(toggle, nullptr);
    toggle->onClick();
    EXPECT_EQ(strip->getHeight(), 360);
    EXPECT_EQ(editor.getHeight(), 1000);
}

/// A live editor must accept restored full-strip geometry rather than writing back its previous compact preference.
TEST(CompactLayout, ExistingCompactEditorUsesRestoredFullStripHeight)
{
    for (const bool hidden : {false, true}) {
        ImageControlProcessor source;
        auto& sourceState = source.getEditorState();
        sourceState.setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
        sourceState.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 304, nullptr);
        sourceState.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
        sourceState.setProperty(HIDE_SPECTROGRAM_PROPERTY, hidden, nullptr);
        juce::MemoryBlock saved;
        source.getStateInformation(saved);
        ImageControlProcessor processor;
        auto& state = processor.getEditorState();
        state.setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
        state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 360, nullptr);
        state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
        state.setProperty(HIDE_SPECTROGRAM_PROPERTY, hidden, nullptr);
        ImageControlEditor editor(processor);
        processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        auto* toggle = compactToggle(editor);
        ASSERT_NE(toggle, nullptr);
        toggle->onClick();
        auto* strip = compactChild<BandStrip>(editor);
        ASSERT_NE(strip, nullptr);
        EXPECT_EQ(strip->getHeight(), 304);
        EXPECT_EQ(static_cast<int>(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 304);
    }
}

/// Restoring the same parameter value must still discard unfinished numeric text before changing the band mode.
TEST(CompactLayout, ModeChangingHostRestoreDiscardsUnchangedParameterText)
{
    ImageControlProcessor source;
    source.getEditorState().setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
    juce::MemoryBlock saved;
    source.getStateInformation(saved);
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    auto* strip = compactChild<BandStrip>(editor);
    ASSERT_NE(strip, nullptr);
    ControlSlider* mid = nullptr;
    for (auto* child : strip->getChildren()) {
        if (child->getTitle() == "Low Mid") {
            mid = dynamic_cast<ControlSlider*>(child);
        }
    }
    ASSERT_NE(mid, nullptr);
    mid->grabKeyboardFocus();
    EXPECT_TRUE(mid->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    juce::TextEditor* input = nullptr;
    for (auto* child : mid->getChildren()) {
        if (const auto* label = dynamic_cast<juce::Label*>(child)) {
            input = label->getCurrentTextEditor();
        }
    }
    ASSERT_NE(input, nullptr);
    input->setText("-3 dB", false);
    processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    // Route through the real layout-settings path before the next display refresh.
    for (auto* child : editor.getChildren()) {
        if (const auto* button = dynamic_cast<juce::Button*>(child);
            button != nullptr && button->getButtonText() == "Post")
        {
            button->onClick();
        }
    }
    EXPECT_EQ(strip->getHeight(), BAND_STRIP_COMPACT_HEIGHT);
    EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(midGainParameterId(0))), 0.0F, 0.001F);
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Changing vertical space never adds a second concise row or interrupts a focused value.
TEST(CompactLayout, ConciseControlsStaySingleRowAcrossEditorHeights)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.setSize(760, 799);
    auto* strip = compactChild<BandStrip>(editor);
    ASSERT_NE(strip, nullptr);
    ControlSlider* pan = nullptr;
    for (auto* child : strip->getChildren()) {
        if (child->getTitle() == "Low Pan") {
            pan = dynamic_cast<ControlSlider*>(child);
        }
    }
    ASSERT_NE(pan, nullptr);
    EXPECT_TRUE(pan->isVisible());
    pan->grabKeyboardFocus();
    const auto sound = processor.getSoundSnapshot();
    for (const int height : {800, 799, 900, 600}) {
        editor.setSize(760, height);
        EXPECT_EQ(editor.getHeight(), height);
        EXPECT_EQ(strip->getHeight(), BAND_STRIP_COMPACT_HEIGHT);
        EXPECT_TRUE(pan->hasKeyboardFocus(true));
        EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
}

/// Expanding inline control names with width leaves unfinished numeric input and keyboard focus intact.
TEST(CompactLayout, InlineNamesDoNotCommitOrDismissNumericEntry)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    editor.setSize(760, 799);
    auto* strip = compactChild<BandStrip>(editor);
    ASSERT_NE(strip, nullptr);
    ControlSlider* pan = nullptr;
    for (auto* child : strip->getChildren()) {
        if (child->getTitle() == "Low Pan") {
            pan = dynamic_cast<ControlSlider*>(child);
        }
    }
    ASSERT_NE(pan, nullptr);
    pan->grabKeyboardFocus();
    ASSERT_TRUE(pan->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    juce::TextEditor* input = nullptr;
    for (auto* child : pan->getChildren()) {
        if (const auto* label = dynamic_cast<juce::Label*>(child)) {
            input = label->getCurrentTextEditor();
        }
    }
    ASSERT_NE(input, nullptr);
    input->setText("25L", false);
    for (const int width : {1600, 760, 2000}) {
        editor.setSize(width, 800);
        EXPECT_TRUE(input->isShowing());
        EXPECT_EQ(input->getText(), "25L");
        EXPECT_TRUE(input->hasKeyboardFocus(true));
        EXPECT_NEAR(plainValue(*processor.getParameters().getParameter(panParameterId(0))), 0.0F, 0.001F);
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
    pan->hideTextBox(true);
}

/// Every expanded band uses the same heading row count at the widest measured text boundary in both directions.
TEST(CompactLayout, ExpandedFrequencyRangesWrapTogetherAcrossResizeAndCompactRoundTrips)
{
    for (const int bandHeight : {BAND_STRIP_MIN_HEIGHT, BAND_STRIP_MIN_HEIGHT + 80}) {
        ImageControlProcessor processor;
        processor.getEditorState().setProperty(BAND_SECTION_HEIGHT_PROPERTY, bandHeight, nullptr);
        processor.getEditorState().setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
        ImageControlEditor editor(processor);
        std::vector<int> thresholds;
        int stripSpace = 0;
        for (const auto* child : editor.getChildren()) {
            if (const auto* strip = dynamic_cast<const BandStrip*>(child)) {
                thresholds.push_back(expandedHeadingWidth(*strip));
                stripSpace += strip->getWidth();
            }
        }
        ASSERT_EQ(thresholds.size(), BAND_COUNT);
        const auto [smallestThreshold, sharedThreshold] = std::ranges::minmax(thresholds);
        ASSERT_LT(smallestThreshold, sharedThreshold);
        const int outsideStrips = editor.getWidth() - stripSpace;
        auto* toggle = compactToggle(editor);
        ASSERT_NE(toggle, nullptr);
        for (const bool compactRoundTrip : {false, true}) {
            if (compactRoundTrip) {
                toggle->onClick();
            }
            for (const int width :
                 {
                     smallestThreshold,
                     sharedThreshold - 1,
                     sharedThreshold,
                     sharedThreshold + 1,
                     sharedThreshold + 80,
                     sharedThreshold - 1,
                 })
            {
                SCOPED_TRACE(width);
                SCOPED_TRACE(compactRoundTrip);
                editor.setSize(outsideStrips + static_cast<int>(BAND_COUNT) * width, 900);
                if (compactRoundTrip) {
                    toggle->onClick();
                }
                expectExpandedHeadingRows(editor, width >= sharedThreshold);
                if (compactRoundTrip) {
                    toggle->onClick();
                }
            }
            if (compactRoundTrip) {
                toggle->onClick();
            }
        }
        EXPECT_FALSE(processor.getHistory().canUndo());
    }
}

/// The full-view action stays in reserved band-heading space, clear of analysis controls and readouts.
TEST(CompactLayout, FullControlsDisclosureDoesNotOverlapCorrelationOrSpectrogram)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    auto* toggle = compactToggle(editor);
    auto* spectrum = compactChild<SpatialSpectrogramView>(editor);
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(spectrum, nullptr);
    for (const int width : {760, 761, 939, 940, 1000, 1200}) {
        editor.setSize(width, editor.getHeight());
        EXPECT_FALSE(toggle->getBounds().intersects(spectrum->getBounds()));
        for (const auto* child : editor.getChildren()) {
            if (dynamic_cast<const BandCorrelationMeter*>(child) != nullptr
                || child->getComponentID() == "correlation-disclosure")
            {
                EXPECT_FALSE(toggle->getBounds().intersects(child->getBounds()));
            }
            if (const auto* strip = dynamic_cast<const BandStrip*>(child)) {
                for (const auto* control : strip->getChildren()) {
                    if (control->isVisible()) {
                        const auto bounds = editor.getLocalArea(control, control->getLocalBounds());
                        EXPECT_FALSE(toggle->getBounds().intersects(bounds));
                    }
                }
            }
        }
    }
}

/// Restoring compact mode invalidates a captured divider drag even before the next settings refresh.
TEST(CompactLayout, RestoredCompactModeRejectsCapturedDividerMovement)
{
    ImageControlProcessor source;
    source.getEditorState().setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
    source.getEditorState().setProperty(BAND_SECTION_HEIGHT_PROPERTY, 304, nullptr);
    juce::MemoryBlock saved;
    source.getStateInformation(saved);
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(BAND_SECTION_HEIGHT_PROPERTY, 360, nullptr);
    ImageControlEditor editor(processor);
    auto* divider = compactChild<SplitDivider>(editor);
    auto* toggle = compactToggle(editor);
    ASSERT_NE(divider, nullptr);
    ASSERT_NE(toggle, nullptr);
    const auto now = juce::Time::getCurrentTime();
    const juce::MouseEvent down {
        juce::Desktop::getInstance().getMainMouseSource(),
        {12.0F, 4.0F},
        juce::ModifierKeys::leftButtonModifier,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        divider,
        divider,
        now,
        {12.0F, 4.0F},
        now,
        1,
        false,
    };
    const auto move = down.withNewPosition(juce::Point<float> {12.0F, 40.0F});
    divider->mouseDown(down);
    processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    divider->mouseDrag(move);
    EXPECT_EQ(static_cast<int>(processor.getEditorState().getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 304);
    // Synchronize and expand, then verify an old captured event cannot revive the cancelled drag.
    toggle->onClick();
    EXPECT_TRUE(divider->isVisible());
    divider->mouseDrag(move);
    EXPECT_EQ(static_cast<int>(processor.getEditorState().getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 304);
    divider->mouseDown(down);
    toggle->onClick();
    EXPECT_FALSE(divider->isVisible());
    divider->mouseDrag(move);
    toggle->onClick();
    divider->mouseDrag(move);
    EXPECT_EQ(static_cast<int>(processor.getEditorState().getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 304);
    divider->mouseUp(move);
}

/// A collapsed analysis section restores from the same right-hand edge used to hide it.
TEST(CompactLayout, CollapsedAnalysisRowsRestoreAcrossTheirFullWidth)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    state.setProperty(SHOW_BAND_CORRELATION_PROPERTY, false, nullptr);
    ImageControlEditor editor(processor);
    editor.setVisible(true);
    for (const auto* identifier : {"spectrogram-disclosure", "correlation-disclosure"}) {
        juce::Button* disclosure = nullptr;
        for (auto* child : editor.getChildren()) {
            if (child->getComponentID() == identifier) {
                disclosure = dynamic_cast<juce::Button*>(child);
            }
        }
        ASSERT_NE(disclosure, nullptr);
        for (const int width : {760, 1000, 1200}) {
            editor.setSize(width, editor.getHeight());
            EXPECT_EQ(disclosure->getX(), 8);
            EXPECT_EQ(disclosure->getRight(), width - 8);
            const auto right = juce::Point<int> {disclosure->getRight() - 2, disclosure->getBounds().getCentreY()};
            EXPECT_EQ(editor.getComponentAt(right), disclosure);
            const auto centre = disclosure->getBounds().getCentre();
            EXPECT_EQ(editor.getComponentAt(centre), disclosure);
        }
        disclosure->onClick();
        EXPECT_TRUE(disclosure->getToggleState());
        disclosure->onClick();
        EXPECT_FALSE(disclosure->getToggleState());
    }
}

TEST(CompactLayout, LegacyHostStateDefaultsToFullControlsOnAnAlreadyCompactInstance)
{
    ImageControlProcessor source;
    juce::MemoryBlock saved;
    source.getStateInformation(saved);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
    ASSERT_NE(xml, nullptr);
    auto* editorState = xml->getChildByName("EditorState");
    ASSERT_NE(editorState, nullptr);
    editorState->removeAttribute(COMPACT_BAND_CONTROLS_PROPERTY);
    juce::AudioProcessor::copyXmlToBinary(*xml, saved);
    ImageControlProcessor restored;
    restored.getEditorState().setProperty(COMPACT_BAND_CONTROLS_PROPERTY, true, nullptr);
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    EXPECT_FALSE(static_cast<bool>(restored.getEditorState().getProperty(COMPACT_BAND_CONTROLS_PROPERTY)));
    ImageControlEditor editor(restored);
    EXPECT_GE(compactChild<BandStrip>(editor)->getHeight(), BAND_STRIP_MIN_HEIGHT);
}
}  // namespace esgrove::image_control::test
