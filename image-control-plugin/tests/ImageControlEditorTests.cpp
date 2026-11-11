/// Tests responsive editor layout and visibility-driven analysis with the real editor and worker.
/// CPU-only observations preserve the worker's single-consumer queue ownership and avoid GPU assertions.

#include "analysis/SpatialAnalyzer.h"
#include "analysis/StereoSampleFifo.h"
#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "ParameterHistory.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "tests/ControlLayoutTestSupport.h"
#include "tests/ViewInteractionTestSupport.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/DisplayDisclosure.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/RenderBenchmark.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SpectrogramParticles.h"
#include "ui/SpectrogramRenderer.h"
#include "ui/SpectrogramWorker.h"
#include "ui/SplitDivider.h"

#include <gtest/gtest.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>

namespace esgrove::image_control::test
{
/// Provides worker synchronization and observations while leaving its single-consumer queue ownership intact.
struct ImageControlEditorTestAccess {
    /// Selects the real editor whose settings and background worker are observed.
    explicit ImageControlEditorTestAccess(ImageControlEditor& observed) : editor(observed) {}

    /// Leaves worker shutdown and component destruction to the observed editor.
    ~ImageControlEditorTestAccess() = default;

    ImageControlEditorTestAccess(const ImageControlEditorTestAccess&) = delete;
    ImageControlEditorTestAccess(ImageControlEditorTestAccess&&) = delete;
    ImageControlEditorTestAccess& operator=(const ImageControlEditorTestAccess&) = delete;
    ImageControlEditorTestAccess& operator=(ImageControlEditorTestAccess&&) = delete;

    /// Reads the effective settings last synchronously handed to the worker.
    [[nodiscard]] bool analyzes() const
    {
        return editor.analysisSettings.analyze;
    }

    /// Reads particles that have reached the renderer, which need no OpenGL context to accumulate.
    [[nodiscard]] std::size_t pendingParticles()
    {
        return editor.spectrogramRenderer.pendingParticleCount();
    }

    /// Waits for a real worker pass instead of racing the FIFO's consumer index.
    [[nodiscard]] bool finishPass()
    {
        const auto ticket = editor.spectrogramWorker.requestPass();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (editor.spectrogramWorker.completedPassCount() < ticket) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    /// Detaches graphics and VBlank before host visibility is exercised, so tests need no GPU or refresh callbacks.
    void disableDisplayCallbacks()
    {
        editor.openGLContext.detach();
        editor.vblankAttachment = juce::VBlankAttachment {};
    }

    /// Applies the same settings path used by display refreshes, without requesting a graphics frame.
    void applySettings()
    {
        editor.applyViewSettings();
    }

    /// Reads the settings actually copied into the worker without becoming another audio FIFO consumer.
    [[nodiscard]] SpectrogramAnalysisSettings workerSettings()
    {
        const std::scoped_lock lock(editor.spectrogramWorker.settingsMutex);
        return editor.spectrogramWorker.settings;
    }

    /// Replaces only the worker copy with a sentinel to prove no-op refreshes do not republish settings.
    void markWorkerCopy(const double contrast)
    {
        const std::scoped_lock lock(editor.spectrogramWorker.settingsMutex);
        editor.spectrogramWorker.settings.contrast = contrast;
    }

    /// Observes the property invalidation flag without adding production instrumentation.
    [[nodiscard]] bool settingsDirty() const
    {
        return editor.viewSettingsDirty;
    }

    /// Applies the range-update path used by display refreshes, without requiring a VBlank or graphics frame.
    void updateBandRanges()
    {
        editor.updateBandRanges();
    }

    /// Editor under observation, which outlives this accessor.
    ImageControlEditor& editor;
};

namespace
{
/// Measures the widest actual expanded heading independently of its responsive layout decision.
int widestBandHeading(const ImageControlEditor& editor)
{
    int widest = 0;
    for (const auto* child : editor.getChildren()) {
        const auto* strip = dynamic_cast<const BandStrip*>(child);
        if (strip == nullptr) {
            continue;
        }
        int width = 20 + 20 + 60 + 6 + 8 * 2;
        for (auto* control : strip->getChildren()) {
            auto* label = dynamic_cast<juce::Label*>(control);
            if (label == nullptr
                || (strip->getTitle() != label->getText() + " band controls" && !label->getText().contains("Hz")))
            {
                continue;
            }
            auto& lookAndFeel = label->getLookAndFeel();
            width += static_cast<int>(std::ceil(
                         juce::GlyphArrangement::getStringWidth(lookAndFeel.getLabelFont(*label), label->getText())
                     ))
                + lookAndFeel.getLabelBorderSize(*label).getLeftAndRight();
        }
        widest = std::max(widest, width);
    }
    return widest;
}

/// Checks every production frequency label has the expected shared row after a live crossover update.
void expectBandRangeRow(const ImageControlEditor& editor, const int expectedY)
{
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
                EXPECT_EQ(label->getY(), expectedY);
            }
        }
    }
    EXPECT_EQ(rangeCount, static_cast<int>(BAND_COUNT));
}

/// Sets host crossover values without creating local editing history.
void setHeadingCrossovers(ImageControlProcessor& processor, const Crossovers& crossovers)
{
    for (std::size_t index = 0; index < CROSSOVER_COUNT; ++index) {
        setParameterInGesture(
            *processor.getParameters().getParameter(crossoverParameterId(index)), crossovers.at(index)
        );
    }
}
}  // namespace

/// The brand title uses the embedded text semibold face at its existing height, independent of host font matching.
TEST(ImageControlEditorLayout, TitleUsesEmbeddedSemiboldAtExistingHeight)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    juce::Label* title = nullptr;
    for (auto* child : editor.getChildren()) {
        if (auto* label = dynamic_cast<juce::Label*>(child); label != nullptr && label->getText() == "ImageControl") {
            title = label;
        }
    }
    ASSERT_NE(title, nullptr);
    const auto expected = semiboldTextFont(18.0F);
    EXPECT_EQ(title->getFont(), expected);
    EXPECT_EQ(title->getLookAndFeel().getLabelFont(*title), expected);
    EXPECT_NE(expected.getTypefacePtr(), textFont(18.0F).getTypefacePtr());
    const auto identity = expected.getTypefacePtr()->getName() + " " + expected.getTypefacePtr()->getStyle();
    EXPECT_TRUE(identity.containsIgnoreCase("SemiBold"));
}

/// Crossover text changes move every heading together at unchanged editor bounds, including while compacted.
TEST(ImageControlEditorLayout, CrossoverChangesSynchronizeEveryFrequencyRangeRow)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    const Crossovers shortRanges {80.0F, 120.0F, 800.0F};
    const Crossovers longRanges {1250.0F, 10'000.0F, 16'000.0F};
    setHeadingCrossovers(processor, shortRanges);
    access.updateBandRanges();
    const int requiredWidth = widestBandHeading(editor);
    int stripSpace = 0;
    for (const auto* child : editor.getChildren()) {
        if (dynamic_cast<const BandStrip*>(child) != nullptr) {
            stripSpace += child->getWidth();
        }
    }
    editor.setSize(editor.getWidth() - stripSpace + static_cast<int>(BAND_COUNT) * requiredWidth, editor.getHeight());
    juce::Button* toggle = nullptr;
    for (auto* child : editor.getChildren()) {
        if (child->getComponentID() == "band-controls-disclosure") {
            toggle = dynamic_cast<juce::Button*>(child);
        }
    }
    ASSERT_NE(toggle, nullptr);
    const auto bounds = editor.getBounds();
    for (const bool compact : {false, true}) {
        if (compact) {
            toggle->onClick();
        }
        for (const bool longText : {false, true, false}) {
            const auto& crossovers = longText ? longRanges : shortRanges;
            setHeadingCrossovers(processor, crossovers);
            access.updateBandRanges();
            EXPECT_EQ(editor.getBounds(), bounds);
            if (compact) {
                toggle->onClick();
            }
            expectBandRangeRow(editor, longText ? 30 : 10);
            if (compact) {
                toggle->onClick();
            }
        }
        if (compact) {
            toggle->onClick();
        }
    }
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// The real settings toggle grows only the plot and preserves window geometry, sound, and saved full-strip sizing.
/// Host state and editor reopening retain the opt-in, while older states deterministically restore standard axes.
TEST(ImageControlEditorLayout, MaximizedSpectrogramIsOptionalPersistentAndIndependentOfSound)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    EXPECT_FALSE(static_cast<bool>(state.getProperty(MAXIMIZE_SPECTROGRAM_PROPERTY)));
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    const SettingsPanel settings(state);
    juce::ToggleButton* toggle = nullptr;
    SpatialSpectrogramView* view = nullptr;
    juce::Component* disclosure = nullptr;
    for (auto* child : editor.getChildren()) {
        if (auto* candidate = dynamic_cast<SpatialSpectrogramView*>(child)) {
            view = candidate;
        }
        if (child->getComponentID() == "spectrogram-disclosure") {
            disclosure = child;
        }
    }
    // The toggle lives in the settings viewport's content, not directly on the panel.
    const auto* viewport = dynamic_cast<juce::Viewport*>(settings.getChildComponent(0));
    ASSERT_NE(viewport, nullptr);
    for (auto* child : viewport->getViewedComponent()->getChildren()) {
        if (child->getComponentID() == MAXIMIZE_SPECTROGRAM_PROPERTY) {
            toggle = dynamic_cast<juce::ToggleButton*>(child);
        }
    }
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(view, nullptr);
    ASSERT_NE(disclosure, nullptr);
    const auto standard = view->getPlotBounds();
    const auto window = editor.getBounds();
    const auto section = view->getBounds();
    const auto bandHeight = state.getProperty(BAND_SECTION_HEIGHT_PROPERTY);
    const auto sound = processor.getSoundSnapshot();
    for (const bool maximized : {true, false, true}) {
        toggle->setToggleState(maximized, juce::sendNotificationSync);
        access.applySettings();
        EXPECT_EQ(view->getPlotBounds(), maximized ? view->getLocalBounds().toFloat().reduced(2.0F) : standard);
        EXPECT_EQ(editor.getBounds(), window);
        EXPECT_EQ(view->getBounds(), section);
        EXPECT_EQ(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY), bandHeight);
        EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
        EXPECT_FALSE(processor.getHistory().canUndo());
        const auto* action = dynamic_cast<DisplayDisclosure*>(disclosure);
        ASSERT_NE(action, nullptr);
        EXPECT_NEAR(
            static_cast<float>(action->getY()) + action->getIndicatorBounds().getCentreY(),
            static_cast<float>(view->getY()) + view->getPlotBounds().getY(),
            0.5F
        );
    }
    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    ImageControlProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    EXPECT_TRUE(static_cast<bool>(restored.getEditorState().getProperty(MAXIMIZE_SPECTROGRAM_PROPERTY)));
    const ImageControlEditor reopened(restored);
    for (const auto* child : reopened.getChildren()) {
        if (const auto* plot = dynamic_cast<const SpatialSpectrogramView*>(child)) {
            EXPECT_EQ(plot->getPlotBounds(), plot->getLocalBounds().toFloat().reduced(2.0F));
        }
    }
    const auto xml = juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()));
    ASSERT_NE(xml, nullptr);
    auto* editorState = xml->getChildByName("EditorState");
    ASSERT_NE(editorState, nullptr);
    editorState->removeAttribute(MAXIMIZE_SPECTROGRAM_PROPERTY);
    juce::AudioProcessor::copyXmlToBinary(*xml, saved);
    processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    access.applySettings();
    EXPECT_FALSE(static_cast<bool>(state.getProperty(MAXIMIZE_SPECTROGRAM_PROPERTY)));
    EXPECT_FALSE(toggle->getToggleState());
    EXPECT_EQ(view->getPlotBounds(), standard);
}

/// No-op property writes and refreshes do not copy settings into the worker again, but changed values still do.
TEST(ImageControlEditorLifecycle, UnchangedViewSettingsDoNotRepublishWorkerCopy)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    access.applySettings();
    EXPECT_FALSE(access.settingsDirty());
    auto* pre = buttonWithText(editor, "Pre");
    ASSERT_NE(pre, nullptr);
    ASSERT_TRUE(pre->isEnabled());
    pre->setEnabled(false);
    access.markWorkerCopy(-1.0);
    ASSERT_TRUE(access.finishPass());
    const auto epoch = processor.getAnalyzerFifo().getEpoch();
    auto& state = processor.getEditorState();
    state.setProperty(CONTRAST_PROPERTY, state.getProperty(CONTRAST_PROPERTY), nullptr);
    EXPECT_FALSE(access.settingsDirty());
    for (int refresh = 0; refresh < 32; ++refresh) {
        access.applySettings();
    }
    EXPECT_DOUBLE_EQ(access.workerSettings().contrast, -1.0);
    EXPECT_FALSE(pre->isEnabled());
    EXPECT_EQ(processor.getAnalyzerFifo().getEpoch(), epoch);
    state.setProperty(CONTRAST_PROPERTY, 25.0, nullptr);
    EXPECT_TRUE(access.settingsDirty());
    access.applySettings();
    EXPECT_DOUBLE_EQ(access.workerSettings().contrast, 0.25);
    EXPECT_TRUE(pre->isEnabled());
    EXPECT_FALSE(access.settingsDirty());
    EXPECT_EQ(processor.getAnalyzerFifo().getEpoch(), epoch);
}

/// Every property used by the worker remains live, including after state recall and processor sample-rate changes.
TEST(ImageControlEditorLifecycle, CachedViewSettingsFollowPropertiesRestoreAndSampleRate)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    access.applySettings();
    juce::MemoryBlock original;
    processor.getStateInformation(original);
    auto& state = processor.getEditorState();
    state.setProperty(PERSISTENCE_PROPERTY, 1.25, nullptr);
    state.setProperty(CONTRAST_PROPERTY, 35.0, nullptr);
    state.setProperty(PAN_MIDPOINT_PROPERTY, 15.0, nullptr);
    state.setProperty(PAN_EDGE_PROPERTY, 40.0, nullptr);
    state.setProperty(MEL_SCALE_PROPERTY, true, nullptr);
    state.setProperty(PHASE_COLOURS_PROPERTY, true, nullptr);
    access.applySettings();
    const auto changed = access.workerSettings();
    EXPECT_DOUBLE_EQ(changed.persistenceSeconds, 1.25);
    EXPECT_DOUBLE_EQ(changed.contrast, 0.35);
    EXPECT_EQ(changed.scale, FrequencyScale::mel);
    EXPECT_EQ(changed.colours, SpectrogramColours::phase);
    EXPECT_TRUE(changed.positionCurve == StereoPositionCurve(15.0, 40.0));
    processor.setStateInformation(original.getData(), static_cast<int>(original.getSize()));
    access.applySettings();
    const auto restored = access.workerSettings();
    EXPECT_DOUBLE_EQ(restored.persistenceSeconds, DEFAULT_PERSISTENCE_SECONDS);
    EXPECT_DOUBLE_EQ(restored.contrast, DEFAULT_CONTRAST_PERCENT / MAX_CONTRAST_PERCENT);
    EXPECT_EQ(restored.scale, FrequencyScale::logarithmic);
    EXPECT_EQ(restored.colours, SpectrogramColours::frequency);
    EXPECT_TRUE(restored.positionCurve == StereoPositionCurve {});
    processor.setRateAndBufferSizeDetails(96'000.0, 256);
    processor.prepareToPlay(96'000.0, 256);
    access.applySettings();
    EXPECT_DOUBLE_EQ(access.workerSettings().sampleRate, 96'000.0);
    access.markWorkerCopy(-1.0);
    access.applySettings();
    EXPECT_DOUBLE_EQ(access.workerSettings().contrast, -1.0);
    processor.releaseResources();
}

/// Host visibility updates the real worker without a VBlank or a GPU, retaining drain-only behavior while hidden.
TEST(ImageControlEditorLifecycle, VisibilityDisablesAndReenablesAnalysisWithoutDisplayRefreshes)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    EXPECT_FALSE(editor.isShowing());
    EXPECT_FALSE(access.analyzes());
    ASSERT_TRUE(access.finishPass());

    std::vector<float> tone(8192);
    for (std::size_t sample = 0; sample < tone.size(); ++sample) {
        tone.at(sample) = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(sample) / ANALYZER_DEFAULT_SAMPLE_RATE)
        );
    }
    auto& fifo = processor.getAnalyzerFifo();
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);

    juce::Component host;
    host.addToDesktop(0);
    host.setVisible(true);
    host.addAndMakeVisible(editor);
    ASSERT_TRUE(editor.isShowing());
    EXPECT_TRUE(access.analyzes());
    ASSERT_TRUE(access.finishPass());
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    ASSERT_TRUE(access.finishPass());
    EXPECT_GT(access.pendingParticles(), 0U);

    const auto shownEpoch = fifo.getEpoch();
    host.setVisible(false);
    EXPECT_FALSE(access.analyzes());
    EXPECT_NE(fifo.getEpoch(), shownEpoch);
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);
    const std::vector<float> full(static_cast<std::size_t>(ANALYZER_FIFO_FRAMES - 1), 0.0F);
    EXPECT_EQ(fifo.push(full, full), ANALYZER_FIFO_FRAMES - 1);
    ASSERT_TRUE(access.finishPass());

    // Resuming invalidates a hidden backlog without analyzing it under the visible settings.
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    const auto hiddenEpoch = fifo.getEpoch();
    host.setVisible(true);
    EXPECT_TRUE(access.analyzes());
    EXPECT_NE(fifo.getEpoch(), hiddenEpoch);
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);
    ASSERT_EQ(fifo.push(tone, tone), static_cast<int>(tone.size()));
    ASSERT_TRUE(access.finishPass());
    EXPECT_GT(access.pendingParticles(), 0U);

    editor.setVisible(false);
    EXPECT_FALSE(access.analyzes());
    editor.setVisible(true);
    EXPECT_TRUE(access.analyzes());
    host.removeChildComponent(&editor);
    EXPECT_FALSE(editor.isShowing());
    EXPECT_FALSE(access.analyzes());
    ASSERT_TRUE(access.finishPass());
    EXPECT_EQ(access.pendingParticles(), 0U);
}

/// Hidden and bypassed display settings remain authoritative when a peer becomes visible before another refresh.
TEST(ImageControlEditorLifecycle, VisibilityCannotOverrideHiddenOrBypassedAnalysis)
{
    for (const auto* property : {HIDE_SPECTROGRAM_PROPERTY, BYPASS_RENDERING_PROPERTY}) {
        ImageControlProcessor processor;
        processor.getEditorState().setProperty(property, true, nullptr);
        ImageControlEditor editor(processor);
        ImageControlEditorTestAccess access(editor);
        access.disableDisplayCallbacks();
        editor.setVisible(false);
        editor.addToDesktop(0);
        editor.setVisible(true);
        EXPECT_TRUE(editor.isShowing());
        EXPECT_FALSE(access.analyzes());
        editor.setVisible(false);
        processor.getEditorState().setProperty(property, false, nullptr);
        editor.setVisible(true);
        EXPECT_TRUE(access.analyzes());
        access.applySettings();
        EXPECT_TRUE(access.analyzes());
        processor.getEditorState().setProperty(property, true, nullptr);
        access.applySettings();
        EXPECT_FALSE(access.analyzes());
    }
}

/// The owned popup grows below its real anchor before scrolling and clamps to natural content after every resize.
TEST(ImageControlEditor, SettingsCalloutUsesAnchorChromeAndNaturalContentHeight)
{
    ImageControlProcessor processor;
    processor.getEditorState().setProperty(EDITOR_HEIGHT_PROPERTY, 600, nullptr);
    ImageControlEditor editor(processor);
    auto* opener = settingsControl<juce::Button>(editor, "display-settings");
    ASSERT_NE(opener, nullptr);
    opener->onClick();
    auto* callout = childOfType<juce::CallOutBox>(editor);
    ASSERT_NE(callout, nullptr);
    auto* panel = childOfType<SettingsPanel>(*callout);
    ASSERT_NE(panel, nullptr);
    auto* viewport = settingsControl<juce::Viewport>(*panel, "displaySettingsViewport");
    auto* reference = settingsControl<juce::Component>(*panel, "keyboardShortcuts");
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(reference, nullptr);
    auto* content = viewport->getViewedComponent();
    ASSERT_NE(content, nullptr);
    const int naturalHeight = panel->getNaturalHeight();
    const int border = callout->getBorderSize();
    const auto anchor = editor.getLocalArea(opener, opener->getLocalBounds());
    const int exactFitHeight = anchor.getBottom() + border * 2 + naturalHeight;
    int previousHeight = 0;
    for (const int height : {600, 680, exactFitHeight - 1, exactFitHeight, 1100, 600, 1100}) {
        SCOPED_TRACE(height);
        editor.setSize(editor.getWidth(), height);
        const auto currentAnchor = editor.getLocalArea(opener, opener->getLocalBounds());
        const int available = editor.getHeight() - currentAnchor.getBottom() - border * 2;
        EXPECT_EQ(panel->getHeight(), std::min(available, naturalHeight));
        EXPECT_EQ(callout->getHeight(), panel->getHeight() + border * 2);
        EXPECT_EQ(panel->getY(), border);
        EXPECT_EQ(panel->getNaturalHeight(), naturalHeight);
        EXPECT_EQ(content->getHeight(), naturalHeight);
        EXPECT_TRUE(editor.getLocalBounds().contains(callout->getBounds()));
        EXPECT_GE(callout->getY(), currentAnchor.getBottom());
        EXPECT_EQ(viewport->getVerticalScrollBar().isVisible(), available < naturalHeight);
        if (height == 680) {
            EXPECT_GT(panel->getHeight(), previousHeight);
        }
        if (height == 1100) {
            EXPECT_GT(panel->getHeight(), 640);
            EXPECT_EQ(content->getWidth(), viewport->getWidth());
        }
        previousHeight = panel->getHeight();
    }
    panel->showKeyboardShortcuts();
    EXPECT_TRUE(reference->isVisible());
    editor.setSize(editor.getWidth(), 600);
    const auto resizedAnchor = editor.getLocalArea(opener, opener->getLocalBounds());
    EXPECT_EQ(panel->getHeight(), 600 - resizedAnchor.getBottom() - border * 2);
    EXPECT_TRUE(panel->getLocalBounds().contains(reference->getBounds()));
    EXPECT_TRUE(editor.getLocalBounds().contains(callout->getBounds()));
    panel->showPreferences();
    EXPECT_TRUE(viewport->isVisible());
    EXPECT_TRUE(viewport->getVerticalScrollBar().isVisible());
    editor.setSize(editor.getWidth(), 1100);
    EXPECT_EQ(panel->getHeight(), naturalHeight);
    EXPECT_FALSE(viewport->getVerticalScrollBar().isVisible());
    EXPECT_EQ(childOfType<juce::CallOutBox>(editor), callout);
    EXPECT_EQ(childOfType<SettingsPanel>(*callout), panel);
    for (const bool compact : {false, true}) {
        SCOPED_TRACE(compact);
        processor.getEditorState().setProperty(COMPACT_BAND_CONTROLS_PROPERTY, compact, nullptr);
        processor.getEditorState().setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
        const auto compactAnchor = editor.getLocalArea(opener, opener->getLocalBounds());
        const int available = editor.getHeight() - compactAnchor.getBottom() - border * 2;
        EXPECT_EQ(panel->getHeight(), std::min(available, naturalHeight));
        EXPECT_TRUE(editor.getLocalBounds().contains(callout->getBounds()));
        EXPECT_EQ(viewport->getVerticalScrollBar().isVisible(), available < naturalHeight);
        panel->showKeyboardShortcuts();
        EXPECT_TRUE(panel->getLocalBounds().contains(reference->getBounds()));
        panel->showPreferences();
    }
}

/// Extreme divider drags save the effective clamp, not an unreachable request, and reopening restores that layout.
TEST(ImageControlEditor, DividerClampsPersistThroughHostStateAndReopening)
{
    for (const int movement : {10'000, -10'000}) {
        SCOPED_TRACE(movement);
        ImageControlProcessor processor;
        auto& state = processor.getEditorState();
        state.setProperty(EDITOR_WIDTH_PROPERTY, 1100, nullptr);
        state.setProperty(EDITOR_HEIGHT_PROPERTY, 800, nullptr);
        state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 320, nullptr);
        juce::MemoryBlock saved;
        int savedBandHeight = 0;
        {
            const ImageControlEditor editor(processor);
            auto* divider = childOfType<SplitDivider>(editor);
            auto* strip = childOfType<BandStrip>(editor);
            auto* spectrogram = childOfType<SpatialSpectrogramView>(editor);
            ASSERT_NE(divider, nullptr);
            ASSERT_NE(strip, nullptr);
            ASSERT_NE(spectrogram, nullptr);
            const auto start = divider->localPointToGlobal(juce::Point<int> {5, 5});
            const auto end = start.translated(0, movement);
            divider->mouseDown(mouseAtScreen(*divider, start, start));
            divider->mouseDrag(mouseAtScreen(*divider, end, start));
            divider->mouseDrag(mouseAtScreen(*divider, end, start));
            divider->mouseUp(mouseAtScreen(*divider, end, start));
            savedBandHeight = strip->getHeight();
            EXPECT_EQ(static_cast<int>(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), savedBandHeight);
            if (movement > 0) {
                EXPECT_EQ(savedBandHeight, BAND_STRIP_MIN_HEIGHT);
            } else {
                EXPECT_GT(savedBandHeight, BAND_STRIP_MIN_HEIGHT);
                EXPECT_EQ(spectrogram->getHeight(), 160);
            }
            processor.getStateInformation(saved);
        }

        ImageControlProcessor restored;
        restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        const ImageControlEditor reopened(restored);
        auto* strip = childOfType<BandStrip>(reopened);
        ASSERT_NE(strip, nullptr);
        EXPECT_EQ(strip->getHeight(), savedBandHeight);
        EXPECT_EQ(reopened.getWidth(), 1100);
        EXPECT_EQ(reopened.getHeight(), 800);
        EXPECT_EQ(
            static_cast<int>(restored.getEditorState().getProperty(BAND_SECTION_HEIGHT_PROPERTY)), savedBandHeight
        );
    }
}
/// Native peer dispatch must consult the hover listener before the focused production slider consumes arrows.
TEST(ImageControlEditor, SpectrumHoverArrowsPrecedeFocusedSliderAndExcludeTextAndModals)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    SpatialSpectrogramView* view = nullptr;
    for (auto* child : editor.getChildren()) {
        if (auto* candidate = dynamic_cast<SpatialSpectrogramView*>(child)) {
            view = candidate;
        }
    }
    auto* slider = dynamic_cast<ControlSlider*>(childWithTitle(editor, "Low Width"));
    ASSERT_NE(view, nullptr);
    ASSERT_NE(slider, nullptr);
    auto* peer = editor.getPeer();
    ASSERT_NE(peer, nullptr);
    slider->grabKeyboardFocus();
    ASSERT_TRUE(slider->hasKeyboardFocus(false));
    hoverFrequency(*view, 100.0);
    EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 110.0F, 0.001F);
    EXPECT_NEAR(parameterValue(processor, widthParameterId(0)), 1.0F, 0.001F);
    EXPECT_TRUE(slider->hasKeyboardFocus(false));

    hoverFrequency(*view, 200.0);
    EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::leftKey)));
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(0)), 99.0F, 0.002F);
    EXPECT_NEAR(parameterValue(processor, crossoverParameterId(1)), 450.0F, 0.002F);
    view->mouseExit(mouseEvent(*view, {}, {}, {}));
    EXPECT_TRUE(peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey)));
    EXPECT_NEAR(parameterValue(processor, widthParameterId(0)), 1.02F, 0.001F);

    hoverFrequency(*view, 100.0);
    ASSERT_TRUE(slider->keyPressed(juce::KeyPress(juce::KeyPress::returnKey)));
    juce::TextEditor* text = nullptr;
    for (auto* child : slider->getChildren()) {
        if (const auto* label = dynamic_cast<juce::Label*>(child)) {
            text = label->getCurrentTextEditor();
        }
    }
    ASSERT_NE(text, nullptr);
    text->grabKeyboardFocus();
    const auto before = processor.getCrossovers();
    peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey));
    EXPECT_EQ(processor.getCrossovers(), before);
    EXPECT_TRUE(text->hasKeyboardFocus(false));
    text->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component modal;
    modal.setSize(80, 40);
    modal.setWantsKeyboardFocus(true);
    editor.addAndMakeVisible(modal);
    modal.enterModalState(false);
    peer->handleKeyPress(juce::KeyPress(juce::KeyPress::rightKey));
    EXPECT_EQ(processor.getCrossovers(), before);
    modal.exitModalState(0);
}

TEST(ImageControlEditor, CoreProfileRequested)
{
    ImageControlProcessor processor;
    ImageControlEditor editor(processor);
    editor.addToDesktop(0);
    editor.setVisible(true);
    const auto* context = juce::OpenGLContext::getContextAttachedTo(editor);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->getPreferredAPI(), juce::OpenGLAPI::openGL);
    EXPECT_EQ(context->getPreferredVersion(), (juce::OpenGLVersion {4, 1}));
    EXPECT_EQ(context->getPreferredProfile(), juce::OpenGLProfile::core);
}

TEST(ImageControlEditor, MonoIsComposedBeforeDeltaAndBypassAtMinimumWidthAndRetainedOnReopen)
{
    ImageControlProcessor processor;
    auto& parameter = *processor.getParameters().getParameter(MONO_ID);
    GestureCounter gestures;
    {
        ImageControlEditor editor(processor);
        editor.setSize(760, 560);
        auto* mono = buttonWithText(editor, "Mono");
        auto* delta = buttonWithText(editor, "Delta");
        auto* bypass = buttonWithText(editor, "Bypass");
        ASSERT_NE(mono, nullptr);
        ASSERT_NE(delta, nullptr);
        ASSERT_NE(bypass, nullptr);
        EXPECT_LE(mono->getRight(), delta->getX());
        EXPECT_LE(delta->getRight(), bypass->getX());
        EXPECT_TRUE(editor.getLocalBounds().contains(mono->getBounds()));
        for (auto* child : editor.getChildren()) {
            if (child != mono && dynamic_cast<juce::Button*>(child) != nullptr) {
                EXPECT_FALSE(mono->getBounds().intersects(child->getBounds()));
            }
        }
        editor.addToDesktop(0);
        auto* handler = mono->getAccessibilityHandler();
        ASSERT_NE(handler, nullptr);
        parameter.addListener(&gestures);
        const bool toggled = handler->getActions().invoke(juce::AccessibilityActionType::toggle);
        parameter.removeListener(&gestures);
        ASSERT_TRUE(toggled);
        EXPECT_FLOAT_EQ(parameter.getValue(), 1.0F);
        EXPECT_EQ(gestures.events, (std::vector<bool> {true, false}));
        parameter.setValueNotifyingHost(0.0F);
        EXPECT_FALSE(mono->getToggleState());
        parameter.setValueNotifyingHost(1.0F);
        EXPECT_TRUE(mono->getToggleState());
        EXPECT_FALSE(delta->getToggleState());
        EXPECT_FALSE(bypass->getToggleState());
    }
    const ImageControlEditor reopened(processor);
    auto* mono = buttonWithText(reopened, "Mono");
    ASSERT_NE(mono, nullptr);
    EXPECT_TRUE(mono->getToggleState());
}

TEST(ImageControlEditor, CompactCorrelationRowKeepsExpandedHeightAndAlignsWithStrips)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(EDITOR_HEIGHT_PROPERTY, 900, nullptr);
    state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 260, nullptr);
    state.setProperty(HIDE_SPECTROGRAM_PROPERTY, true, nullptr);
    const ImageControlEditor editor(processor);
    const int withCorrelation = editor.getHeight();
    EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
    const SettingsPanel settings(state);
    auto* show = buttonWithText(settings, "Show band correlation");
    ASSERT_NE(show, nullptr);
    auto* post = buttonWithText(editor, "Post");
    ASSERT_NE(post, nullptr);
    show->setToggleState(false, juce::sendNotificationSync);
    // Analyzer tap actions apply all display settings through the real editor composition.
    post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
    // Hidden correlation retains an 18-pixel labelled restore handle in the same section.
    EXPECT_EQ(withCorrelation - editor.getHeight(), BAND_CORRELATION_HEIGHT - 18);
    EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
    show->setToggleState(true, juce::sendNotificationSync);
    post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
    EXPECT_EQ(editor.getHeight(), withCorrelation);
    auto* meter = childWithTitle(editor, "Low correlation");
    ASSERT_NE(meter, nullptr);
    auto* mid = childWithTitle(editor, "Low Mid");
    ASSERT_NE(mid, nullptr);
    auto* strip = mid->getParentComponent();
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(meter->getX(), strip->getX());
    EXPECT_EQ(meter->getWidth(), strip->getWidth());
    EXPECT_LE(meter->getBottom(), strip->getY());
    EXPECT_GE(strip->getHeight(), BAND_STRIP_MIN_HEIGHT);
    auto* width = childWithTitle(editor, "Low Width");
    auto* recover = childWithTitle(editor, "Low Recover");
    auto* pan = childWithTitle(editor, "Low Pan");
    ASSERT_NE(width, nullptr);
    ASSERT_NE(recover, nullptr);
    ASSERT_NE(pan, nullptr);
    EXPECT_LE(width->getBottom(), recover->getY());
    EXPECT_LE(recover->getBottom(), pan->getY());
    EXPECT_TRUE(strip->getLocalBounds().contains(pan->getBounds()));
    state.setProperty(HIDE_SPECTROGRAM_PROPERTY, false, nullptr);
    post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
    EXPECT_EQ(editor.getHeight(), 900);
    EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 900);
}

/// Applying settings through the real Post action updates all meters without clearing their histories.
TEST(ImageControlEditor, SmoothCorrelationNumbersPropagatesToEveryMeterAndRestoresOnReopen)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    {
        ImageControlEditor editor(processor);
        editor.addToDesktop(0);
        editor.setVisible(true);
        const SettingsPanel settings(state);
        auto* smooth = buttonWithText(settings, "Smooth correlation numbers");
        auto* post = buttonWithText(editor, "Post");
        ASSERT_NE(smooth, nullptr);
        ASSERT_NE(post, nullptr);
        std::array<BandCorrelationMeter*, BAND_COUNT> meters {};
        constexpr std::array titles {
            "Low correlation",
            "Low Mid correlation",
            "High Mid correlation",
            "High correlation",
        };
        for (std::size_t band = 0; band < BAND_COUNT; ++band) {
            auto* meter = dynamic_cast<BandCorrelationMeter*>(childWithTitle(editor, titles.at(band)));
            ASSERT_NE(meter, nullptr);
            meters.at(band) = meter;
            meter->setCorrelation(-0.75F);
            meter->setCorrelation(0.75F);
            EXPECT_EQ(meter->readoutText(), "-0.75");
        }
        for (const bool enabled : {false, true, false}) {
            smooth->setToggleState(enabled, juce::sendNotificationSync);
            // Analyzer tap actions apply all display settings through the real editor composition.
            post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
            for (const auto* meter : meters) {
                EXPECT_EQ(meter->readoutText(), enabled ? "-0.75" : "+0.75");
                const auto trend = meter->trendValue();
                const auto range = meter->recentRange();
                if (!trend.has_value() || !range.has_value()) {
                    FAIL() << "Changing readout mode must preserve correlation history";
                }
                EXPECT_FLOAT_EQ(trend.value(), -0.75F);
                EXPECT_FLOAT_EQ(range.value().getStart(), -0.75F);
                EXPECT_FLOAT_EQ(range.value().getEnd(), 0.75F);
            }
        }
    }
    const ImageControlEditor reopened(processor);
    auto* meter = dynamic_cast<BandCorrelationMeter*>(childWithTitle(reopened, "Low correlation"));
    ASSERT_NE(meter, nullptr);
    meter->setCorrelation(-0.75F);
    meter->setCorrelation(0.75F);
    EXPECT_EQ(meter->readoutText(), "+0.75");
}

TEST(ImageControlEditor, HiddenAndClosedMetersStopPublishingMeasurements)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 256);
    const auto render = [&processor] {
        juce::AudioBuffer<float> block(2, 256);
        juce::MidiBuffer midi;
        for (int pass = 0; pass < 200; ++pass) {
            for (int sample = 0; sample < 256; ++sample) {
                const float value = sample % 2 == 0 ? 0.25F : -0.25F;
                block.setSample(0, sample, value);
                block.setSample(1, sample, value);
            }
            processor.processBlock(block, midi);
        }
    };
    const auto expectUnavailable = [&processor] {
        for (const float value : processor.getBandCorrelations()) {
            EXPECT_TRUE(std::isnan(value));
        }
    };
    {
        ImageControlEditor editor(processor);
        editor.addToDesktop(0);
        editor.setVisible(true);
        render();
        EXPECT_NEAR(processor.getBandCorrelations().back(), 1.0F, 1.0e-6F);
        auto* meter = dynamic_cast<BandCorrelationMeter*>(childWithTitle(editor, "High correlation"));
        ASSERT_NE(meter, nullptr);
        meter->setCorrelation(-0.8F);
        meter->setCorrelation(0.8F, 0.05, 0.3);
        ASSERT_TRUE(meter->trendValue().has_value());
        editor.setVisible(false);
        EXPECT_FALSE(meter->trendValue().has_value());
        EXPECT_FALSE(meter->recentRange().has_value());
        EXPECT_EQ(meter->valueText(), "Unavailable");
        render();
        expectUnavailable();
        editor.setVisible(true);
        meter->setCorrelation(0.5F);
        processor.getEditorState().setProperty(SHOW_BAND_CORRELATION_PROPERTY, false, nullptr);
        auto* post = buttonWithText(editor, "Post");
        ASSERT_NE(post, nullptr);
        post->setToggleState(!post->getToggleState(), juce::sendNotificationSync);
        EXPECT_FALSE(meter->trendValue().has_value());
        EXPECT_FALSE(meter->recentRange().has_value());
        render();
        expectUnavailable();
    }
    render();
    expectUnavailable();
    processor.releaseResources();
}
/// The benchmark drives real editor-state listeners without host restore or sound/analysis reset.
TEST(ImageControlEditorBenchmark, LiveSweepUsesNormalStateListenersWithoutResettingAnalysis)
{
    ImageControlProcessor processor;
    processor.prepareToPlay(48'000.0, 512);
    ImageControlEditor editor(processor);
    ImageControlEditorTestAccess access(editor);
    access.disableDisplayCallbacks();
    const auto& state = processor.getEditorState();
    const auto originalEpoch = processor.getAnalyzerFifo().getEpoch();
    const auto originalSound = processor.getSoundSnapshot();
    const auto report = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("image-control-sweep", ".json");
    RenderBenchmark benchmark(report, true);
    benchmark.applySweepAt(state, originalEpoch, 0.0);
    access.applySettings();
    EXPECT_NEAR(static_cast<double>(state.getProperty(PERSISTENCE_PROPERTY)), 0.1, 1.0e-6);
    EXPECT_NEAR(static_cast<double>(state.getProperty(CONTRAST_PROPERTY)), 10.0, 1.0e-6);
    benchmark.applySweepAt(state, processor.getAnalyzerFifo().getEpoch(), 2.0);
    access.applySettings();
    EXPECT_NEAR(static_cast<double>(state.getProperty(PERSISTENCE_PROPERTY)), 2.5, 1.0e-6);
    EXPECT_NEAR(static_cast<double>(state.getProperty(CONTRAST_PROPERTY)), 90.0, 1.0e-6);
    benchmark.applySweepAt(state, processor.getAnalyzerFifo().getEpoch(), 4.0);
    access.applySettings();
    EXPECT_EQ(processor.getAnalyzerFifo().getEpoch(), originalEpoch);
    EXPECT_EQ(processor.getSoundSnapshot(), originalSound);
    ASSERT_TRUE(benchmark.flush());
    const auto sweep = juce::JSON::parse(report.loadFileAsString()).getProperty("appearance_sweep", {});
    EXPECT_EQ(static_cast<int>(sweep.getProperty("accepted_updates", 0)), 3);
    EXPECT_EQ(static_cast<int>(sweep.getProperty("generation_changes", -1)), 0);
    EXPECT_TRUE(report.deleteFile());
}
}  // namespace esgrove::image_control::test
