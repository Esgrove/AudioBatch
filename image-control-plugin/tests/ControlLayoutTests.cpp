/// Tests SettingsPanel's coupled pan curve controls and SplitDivider's incremental screen-space movement.
/// Event-handler tests also verify that editor divider clamps persist through host state and reopening.
/// These tests inspect control values and layout bounds without desktop automation or pixel assertions.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ProcessingState.h"
#include "ui/BandStrip.h"
#include "ui/ControlSlider.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/SettingsPanel.h"
#include "ui/SpatialSpectrogramView.h"
#include "ui/SplitDivider.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#elif JUCE_WINDOWS
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Counts real slider notifications without replacing the panel's coupling callbacks.
class SliderChangeCounter final : private juce::Slider::Listener
{
public:
    /// Observes a control owned by a panel that outlives this counter.
    explicit SliderChangeCounter(juce::Slider& observed) : slider(observed)
    {
        slider.addListener(this);
    }

    /// Detaches before the panel destroys its slider.
    ~SliderChangeCounter() override
    {
        slider.removeListener(this);
    }

    SliderChangeCounter(const SliderChangeCounter&) = delete;
    SliderChangeCounter(SliderChangeCounter&&) = delete;
    SliderChangeCounter& operator=(const SliderChangeCounter&) = delete;
    SliderChangeCounter& operator=(SliderChangeCounter&&) = delete;

    /// Returns delivered value notifications, including asynchronously pushed peers.
    [[nodiscard]] int changeCount() const
    {
        return changes;
    }

private:
    /// Records delivery rather than polling values that can change before notification.
    void sliderValueChanged(juce::Slider* /*changed*/) override
    {
        ++changes;
    }

    /// Panel-owned slider, which remains alive until this observer detaches.
    juce::Slider& slider;
    /// Callback-owned count that advances even when the observer is read-only to the test.
    mutable int changes {0};
};

/// Finds a direct panel or editor child without depending on its position in the child list.
/// The returned observer is null when absent and otherwise remains valid while the parent owns the child.
template<typename Child>
Child* childOfType(const juce::Component& parent)
{
    for (auto* child : parent.getChildren()) {
        if (auto* match = dynamic_cast<Child*>(child)) {
            return match;
        }
    }
    return nullptr;
}

/// Finds an owned descendant by stable identity, returning a nullable observer valid until parent destruction.
/// An explicit stack avoids recursion through JUCE's implementation-owned viewport and control children.
template<typename Control>
Control* settingsControl(const juce::Component& parent, const juce::String& identifier)
{
    std::vector<juce::Component*> pending;
    for (auto* child : parent.getChildren()) {
        pending.push_back(child);
    }
    while (!pending.empty()) {
        auto* child = pending.back();
        pending.pop_back();
        if (child->getComponentID() == identifier) {
            return dynamic_cast<Control*>(child);
        }
        for (auto* descendant : child->getChildren()) {
            pending.push_back(descendant);
        }
    }
    return nullptr;
}

/// Delivers queued value bindings and peer-slider callbacks with a short, bounded native message pump.
/// JUCE's optional modal dispatch loop is disabled in the plugin, so tests pump only the platform queue.
void dispatchControlChanges()
{
#if JUCE_MAC
    for (int pass = 0; pass < 8; ++pass) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, static_cast<Boolean>(true));
    }
#elif JUCE_WINDOWS
    MSG message {};
    for (int pass = 0; pass < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0; ++pass) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#endif
}

/// Waits for one notification per coupled slider without accepting duplicates or assuming a platform queue depth.
bool waitForSliderChanges(const SliderChangeCounter& first, const SliderChangeCounter& second)
{
    constexpr double timeoutMilliseconds = 500.0;
    const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMilliseconds;
    do {
        dispatchControlChanges();
        if (first.changeCount() > 1 || second.changeCount() > 1) {
            return false;
        }
        if (first.changeCount() == 1 && second.changeCount() == 1) {
            return true;
        }
    } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    return false;
}

/// Waits for asynchronous focus scrolling with a fixed deadline instead of assuming a platform queue depth.
/// JUCE delivers a bounded number of messages per native run-loop pass, which varies with native peer setup.
bool waitForVisibleControl(const juce::Viewport& viewport, const juce::Component& control)
{
    constexpr double timeoutMilliseconds = 500.0;
    const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMilliseconds;
    do {
        dispatchControlChanges();
        const auto bounds = viewport.getLocalArea(&control, control.getLocalBounds());
        if (control.hasKeyboardFocus(false) && bounds.getY() >= 0 && bounds.getBottom() <= viewport.getViewHeight()) {
            return true;
        }
    } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    return false;
}

/// Creates a handler event at a fixed screen position even when layout has moved the component since mouse-down.
juce::MouseEvent mouseAtScreen(
    juce::Component& component,
    const juce::Point<int> screenPosition,
    const juce::Point<int> downScreenPosition,
    const juce::ModifierKeys modifiers = juce::ModifierKeys::leftButtonModifier
)
{
    const auto now = juce::Time::getCurrentTime();
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        component.getLocalPoint(nullptr, screenPosition).toFloat(),
        modifiers,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        &component,
        &component,
        now,
        component.getLocalPoint(nullptr, downScreenPosition).toFloat(),
        now,
        1,
        screenPosition != downScreenPosition,
    };
}
}  // namespace

/// Every interactive setting has a stable id and a useful accessible title, including both explicit choices.
TEST(SettingsPanel, ControlsAreNamedAndExplicitChoicesFollowLiveState)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    SettingsPanel panel(state, 300);
    const std::array ids {
        PERSISTENCE_PROPERTY,
        CONTRAST_PROPERTY,
        PAN_MIDPOINT_PROPERTY,
        PAN_EDGE_PROPERTY,
        PHASE_COLOURS_PROPERTY,
        MEL_SCALE_PROPERTY,
        SOLID_RENDERING_PROPERTY,
        HIDE_SPECTROGRAM_PROPERTY,
        MAXIMIZE_SPECTROGRAM_PROPERTY,
        SHOW_BAND_CORRELATION_PROPERTY,
        SMOOTH_CORRELATION_READOUT_PROPERTY,
        BYPASS_RENDERING_PROPERTY,
        SHOW_FPS_PROPERTY,
        "resetDisplayAppearance",
        "showKeyboardShortcuts",
    };
    panel.addToDesktop(0);
    for (const auto* identifier : ids) {
        SCOPED_TRACE(identifier);
        auto* control = settingsControl<juce::Component>(panel, identifier);
        ASSERT_NE(control, nullptr);
        EXPECT_TRUE(control->getName().isNotEmpty());
        EXPECT_TRUE(control->getTitle().isNotEmpty());
        EXPECT_TRUE(control->hasFocusOutline());
        auto* accessibility = control->getAccessibilityHandler();
        ASSERT_NE(accessibility, nullptr);
        EXPECT_EQ(accessibility->getTitle(), control->getTitle());
    }
    for (const auto* property : {PHASE_COLOURS_PROPERTY, MEL_SCALE_PROPERTY, SOLID_RENDERING_PROPERTY}) {
        auto* choice = settingsControl<juce::ComboBox>(panel, property);
        ASSERT_NE(choice, nullptr);
        EXPECT_EQ(choice->getNumItems(), 2);
        EXPECT_EQ(choice->getSelectedId(), 1);
        state.setProperty(property, true, nullptr);
        EXPECT_EQ(choice->getSelectedId(), 2);
        choice->setSelectedId(1, juce::sendNotificationSync);
        EXPECT_FALSE(static_cast<bool>(state.getProperty(property)));
    }
    EXPECT_EQ(
        settingsControl<juce::ToggleButton>(panel, BYPASS_RENDERING_PROPERTY)->getButtonText(),
        "Stop spectrogram analysis"
    );
}

/// Percentage readouts keep the unit adjacent while the numeric editor retains a separate noneditable suffix.
TEST(SettingsPanel, PercentageReadoutsKeepPercentAdjacentToNumber)
{
    ImageControlProcessor processor;
    ImageControlLookAndFeel lookAndFeel;
    SettingsPanel panel(processor.getEditorState());
    panel.setLookAndFeel(&lookAndFeel);
    auto* slider = settingsControl<ControlSlider>(panel, CONTRAST_PROPERTY);
    ASSERT_NE(slider, nullptr);
    EXPECT_EQ(slider->getTextValueSuffix(), "%");
    for (const double value : {0.0, 50.0, 100.0}) {
        const auto text = slider->getTextFromValue(value);
        EXPECT_TRUE(text.endsWithChar('%'));
        EXPECT_FALSE(text.contains(" %"));
        EXPECT_DOUBLE_EQ(slider->getValueFromText(text), value);
    }
    EXPECT_EQ(slider->numericValueSuffix(), "%");
    EXPECT_FALSE(slider->numericValueText().containsChar('%'));
}

/// Strict settings entry consumes one finite token and one optional matching unit, never a valid prefix of junk.
TEST(SettingsPanel, NumericEntryIsFiniteFullTokenAndPreservesInvalidValues)
{
    ImageControlProcessor processor;
    const SettingsPanel panel(processor.getEditorState());
    const std::array settings {
        std::pair<const char*, const char*> {PERSISTENCE_PROPERTY, "s"},
        std::pair<const char*, const char*> {CONTRAST_PROPERTY, "%"},
        std::pair<const char*, const char*> {PAN_MIDPOINT_PROPERTY, "dB"},
        std::pair<const char*, const char*> {PAN_EDGE_PROPERTY, "dB"},
    };
    for (const auto& [identifier, suffix] : settings) {
        SCOPED_TRACE(identifier);
        auto* slider = settingsControl<ControlSlider>(panel, identifier);
        ASSERT_NE(slider, nullptr);
        const double original = slider->getValue();
        for (const juce::String invalid :
             {
                 "",
                 " ",
                 "garbage",
                 "nan",
                 "inf",
                 "-inf",
                 "1e9999",
                 "12junk",
                 "++12",
                 "+-12",
                 "12 34",
                 "12,5",
                 "12 dB junk",
                 "0x10",
             })
        {
            SCOPED_TRACE(invalid.toStdString());
            EXPECT_DOUBLE_EQ(slider->getValueFromText(invalid), original);
        }
        const juce::String unit(suffix);
        EXPECT_DOUBLE_EQ(slider->getValueFromText("12 " + unit + " " + unit), original);
        EXPECT_DOUBLE_EQ(slider->getValueFromText("12" + unit + unit), original);
        EXPECT_DOUBLE_EQ(slider->getValueFromText("  +1.25  "), 1.25);
        EXPECT_DOUBLE_EQ(slider->getValueFromText("  1.25 " + unit + "  "), 1.25);
        EXPECT_DOUBLE_EQ(slider->getValueFromText("1.25" + unit.toUpperCase()), 1.25);
        EXPECT_DOUBLE_EQ(slider->getValueFromText("1.25e1 " + unit), 12.5);
        slider->setValue(slider->getValueFromText("-999"), juce::sendNotificationSync);
        EXPECT_DOUBLE_EQ(slider->getValue(), slider->getMinimum());
        slider->setValue(slider->getValueFromText("99999"), juce::sendNotificationSync);
        EXPECT_DOUBLE_EQ(slider->getValue(), slider->getMaximum());
    }
}

/// Reset is intentionally narrower than a plugin reset and cannot change audition, layout, or analysis enablement.
TEST(SettingsPanel, AppearanceResetLeavesSoundVisibilityLayoutLinksAndStoppedAnalysisUntouched)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(PERSISTENCE_PROPERTY, MAX_PERSISTENCE_SECONDS, nullptr);
    state.setProperty(CONTRAST_PROPERTY, 1.0, nullptr);
    state.setProperty(PAN_MIDPOINT_PROPERTY, MAX_PAN_MIDPOINT_DB, nullptr);
    state.setProperty(PAN_EDGE_PROPERTY, MAX_PAN_EDGE_DB, nullptr);
    for (const auto* property :
         {
             PHASE_COLOURS_PROPERTY,
             MEL_SCALE_PROPERTY,
             SOLID_RENDERING_PROPERTY,
             SHOW_FPS_PROPERTY,
             HIDE_SPECTROGRAM_PROPERTY,
             MAXIMIZE_SPECTROGRAM_PROPERTY,
             BYPASS_RENDERING_PROPERTY,
         })
    {
        state.setProperty(property, true, nullptr);
    }
    state.setProperty(SMOOTH_CORRELATION_READOUT_PROPERTY, false, nullptr);
    state.setProperty(SHOW_BAND_CORRELATION_PROPERTY, false, nullptr);
    state.setProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY, false, nullptr);
    state.setProperty(ANALYZE_OUTPUT_PROPERTY, false, nullptr);
    state.setProperty(linkPropertyName(0), true, nullptr);
    state.setProperty(EDITOR_HEIGHT_PROPERTY, 800, nullptr);
    state.setProperty(BAND_SECTION_HEIGHT_PROPERTY, 310, nullptr);
    auto& gain = *processor.getParameters().getParameter(midGainParameterId(0));
    gain.setValueNotifyingHost(gain.convertTo0to1(-7.0F));
    const SettingsPanel panel(state);
    auto* reset = settingsControl<juce::TextButton>(panel, "resetDisplayAppearance");
    ASSERT_NE(reset, nullptr);
    reset->onClick();
    dispatchControlChanges();
    EXPECT_DOUBLE_EQ(static_cast<double>(state.getProperty(PERSISTENCE_PROPERTY)), DEFAULT_PERSISTENCE_SECONDS);
    EXPECT_DOUBLE_EQ(static_cast<double>(state.getProperty(CONTRAST_PROPERTY)), DEFAULT_CONTRAST_PERCENT);
    EXPECT_DOUBLE_EQ(static_cast<double>(state.getProperty(PAN_MIDPOINT_PROPERTY)), DEFAULT_PAN_MIDPOINT_DB);
    EXPECT_DOUBLE_EQ(static_cast<double>(state.getProperty(PAN_EDGE_PROPERTY)), DEFAULT_PAN_EDGE_DB);
    for (const auto* property :
         {PHASE_COLOURS_PROPERTY, MEL_SCALE_PROPERTY, SOLID_RENDERING_PROPERTY, SHOW_FPS_PROPERTY})
    {
        EXPECT_FALSE(static_cast<bool>(state.getProperty(property)));
    }
    EXPECT_TRUE(static_cast<bool>(state.getProperty(SMOOTH_CORRELATION_READOUT_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(state.getProperty(HIDE_SPECTROGRAM_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(state.getProperty(MAXIMIZE_SPECTROGRAM_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(state.getProperty(BYPASS_RENDERING_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(state.getProperty(SHOW_BAND_CORRELATION_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(state.getProperty(SHOW_PROCESSING_TOOLBAR_PROPERTY)));
    EXPECT_FALSE(static_cast<bool>(state.getProperty(ANALYZE_OUTPUT_PROPERTY)));
    EXPECT_TRUE(static_cast<bool>(state.getProperty(linkPropertyName(0))));
    EXPECT_EQ(static_cast<int>(state.getProperty(EDITOR_HEIGHT_PROPERTY)), 800);
    EXPECT_EQ(static_cast<int>(state.getProperty(BAND_SECTION_HEIGHT_PROPERTY)), 310);
    EXPECT_FLOAT_EQ(plainValue(gain), -7.0F);
}

/// Height limits bound the viewport to natural content, with a readable footer gap and no unused scroll gutter.
TEST(SettingsPanel, ViewportBoundsNaturalContentWithoutEmbeddedHelp)
{
    ImageControlProcessor processor;
    for (const int maximumHeight : {1, 180, 320, 640, 2000}) {
        SCOPED_TRACE(maximumHeight);
        const SettingsPanel panel(processor.getEditorState(), maximumHeight);
        EXPECT_LE(panel.getHeight(), maximumHeight);
        auto* viewport = childOfType<juce::Viewport>(panel);
        ASSERT_NE(viewport, nullptr);
        EXPECT_EQ(viewport->getBounds(), panel.getLocalBounds());
        auto* content = viewport->getViewedComponent();
        ASSERT_NE(content, nullptr);
        EXPECT_EQ(content->getHeight(), panel.getNaturalHeight());
        EXPECT_EQ(panel.getHeight(), std::min(maximumHeight, panel.getNaturalHeight()));
        EXPECT_EQ(
            viewport->getVerticalScrollBar().isVisible(),
            maximumHeight < panel.getNaturalHeight() && maximumHeight > viewport->getScrollBarThickness()
        );
        EXPECT_FALSE(viewport->getHorizontalScrollBar().isVisible());
        EXPECT_EQ(content->getWidth(), viewport->getViewWidth());
        EXPECT_GE(content->getHeight(), panel.getHeight());
        EXPECT_LE(content->getWidth(), viewport->getWidth());
        for (const auto* child : content->getChildren()) {
            SCOPED_TRACE(child->getComponentID().toStdString());
            EXPECT_TRUE(content->getLocalBounds().contains(child->getBounds()));
            EXPECT_GT(child->getHeight(), 0);
            EXPECT_EQ(dynamic_cast<const juce::TextEditor*>(child), nullptr);
        }
        EXPECT_EQ(settingsControl<juce::Component>(panel, "displayControlsHelp"), nullptr);
        auto* shortcuts = settingsControl<juce::TextButton>(panel, "showKeyboardShortcuts");
        auto* reset = settingsControl<juce::TextButton>(panel, "resetDisplayAppearance");
        ASSERT_NE(shortcuts, nullptr);
        ASSERT_NE(reset, nullptr);
        EXPECT_GE(shortcuts->getY() - reset->getBottom(), 8);
        EXPECT_EQ(panel.getNaturalHeight() - shortcuts->getBottom(), shortcuts->getX());
        viewport->setViewPosition(0, content->getHeight());
        if (maximumHeight < content->getHeight()) {
            EXPECT_GT(viewport->getViewPositionY(), 0);
        } else {
            EXPECT_EQ(viewport->getViewPositionY(), 0);
        }
        if (maximumHeight >= shortcuts->getHeight()) {
            const auto bounds = viewport->getLocalArea(shortcuts, shortcuts->getLocalBounds());
            EXPECT_GE(bounds.getY(), 0);
            EXPECT_LE(bounds.getBottom(), viewport->getViewHeight());
        }
        if (maximumHeight >= content->getHeight()) {
            EXPECT_EQ(panel.getHeight(), content->getHeight());
        }
    }
}

/// Growing and shrinking the real viewport crosses the scroll threshold exactly at the measured natural height.
TEST(SettingsPanel, ResizeUsesFullWidthAndOnlyScrollsBelowNaturalHeight)
{
    ImageControlProcessor processor;
    SettingsPanel panel(processor.getEditorState(), 240);
    auto* viewport = settingsControl<juce::Viewport>(panel, "displaySettingsViewport");
    ASSERT_NE(viewport, nullptr);
    auto* content = viewport->getViewedComponent();
    ASSERT_NE(content, nullptr);
    const int naturalHeight = panel.getNaturalHeight();
    for (const int height : {naturalHeight - 1, naturalHeight, naturalHeight + 100, 300, naturalHeight}) {
        SCOPED_TRACE(height);
        panel.setSize(panel.getWidth(), height);
        EXPECT_EQ(panel.getNaturalHeight(), naturalHeight);
        EXPECT_EQ(content->getHeight(), naturalHeight);
        EXPECT_EQ(content->getWidth(), viewport->getViewWidth());
        EXPECT_EQ(viewport->getVerticalScrollBar().isVisible(), height < naturalHeight);
        EXPECT_FALSE(viewport->getHorizontalScrollBar().isVisible());
        viewport->setViewPosition(0, naturalHeight);
        EXPECT_EQ(viewport->getViewPositionY(), std::max(0, naturalHeight - height));
        if (height >= naturalHeight) {
            EXPECT_EQ(content->getWidth(), panel.getWidth());
        }
    }
}

/// The footer and Back action swap nested views without changing settings, sound, bounds, or preference scroll.
TEST(SettingsPanel, KeyboardShortcutsFooterEntersNestedViewAndReturnsWithoutChangingSettings)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    const SettingsPanel panel(state, 300);
    auto* shortcuts = settingsControl<juce::TextButton>(panel, "showKeyboardShortcuts");
    auto* back = settingsControl<juce::TextButton>(panel, "keyboardShortcutsBack");
    auto* reference = settingsControl<juce::Component>(panel, "keyboardShortcuts");
    auto* viewport = settingsControl<juce::Viewport>(panel, "displaySettingsViewport");
    auto* contrast = settingsControl<ControlSlider>(panel, CONTRAST_PROPERTY);
    ASSERT_NE(shortcuts, nullptr);
    ASSERT_NE(back, nullptr);
    ASSERT_NE(reference, nullptr);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(contrast, nullptr);
    EXPECT_EQ(shortcuts->getButtonText(), "Keyboard shortcuts");
    EXPECT_TRUE(shortcuts->getWantsKeyboardFocus());
    EXPECT_TRUE(back->getWantsKeyboardFocus());
    EXPECT_TRUE(back->getTitle().contains("Settings"));
    EXPECT_FALSE(back->isVisible());
    EXPECT_FALSE(reference->isVisible());
    viewport->setViewPosition(0, viewport->getViewedComponent()->getHeight());
    const int scrollPosition = viewport->getViewPositionY();
    ASSERT_GT(scrollPosition, 0);
    const auto bounds = panel.getBounds();
    const auto before = state.createCopy();
    const auto sound = processor.getSoundSnapshot();
    const auto contrastTitle = contrast->getTitle();
    shortcuts->onClick();
    EXPECT_FALSE(viewport->isVisible());
    EXPECT_TRUE(reference->isVisible());
    EXPECT_TRUE(back->isVisible());
    EXPECT_EQ(reference->getParentComponent(), &panel);
    EXPECT_EQ(panel.getBounds(), bounds);
    EXPECT_EQ(settingsControl<ControlSlider>(panel, CONTRAST_PROPERTY), contrast);
    EXPECT_EQ(contrast->getTitle(), contrastTitle);
    EXPECT_TRUE(panel.getLocalBounds().contains(reference->getBounds()));
    EXPECT_TRUE(panel.getLocalBounds().contains(back->getBounds()));
    EXPECT_FALSE(reference->getBounds().intersects(back->getBounds()));
    EXPECT_TRUE(state.isEquivalentTo(before));
    back->onClick();
    EXPECT_TRUE(viewport->isVisible());
    EXPECT_FALSE(reference->isVisible());
    EXPECT_FALSE(back->isVisible());
    EXPECT_EQ(viewport->getViewPositionY(), scrollPosition);
    EXPECT_EQ(panel.getBounds(), bounds);
    EXPECT_TRUE(state.isEquivalentTo(before));
    EXPECT_TRUE(sameProcessingState(processor.getSoundSnapshot(), sound));
    EXPECT_FALSE(processor.getHistory().canUndo());
}

/// Escape and Alt+Left return one level, leaving an outer owner to handle Escape from preferences.
TEST(SettingsPanel, SubmenuBackKeysLeavePreferenceDismissalToOwner)
{
    ImageControlProcessor processor;
    SettingsPanel panel(processor.getEditorState(), 300);
    auto* reference = settingsControl<juce::Component>(panel, "keyboardShortcuts");
    auto* viewport = settingsControl<juce::Viewport>(panel, "displaySettingsViewport");
    ASSERT_NE(reference, nullptr);
    ASSERT_NE(viewport, nullptr);
    for (const auto& key :
         std::array {
             juce::KeyPress(juce::KeyPress::escapeKey),
             juce::KeyPress(juce::KeyPress::leftKey, juce::ModifierKeys::altModifier, 0),
         })
    {
        panel.showKeyboardShortcuts();
        EXPECT_TRUE(reference->isVisible());
        EXPECT_FALSE(panel.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
        EXPECT_TRUE(reference->isVisible());
        EXPECT_TRUE(panel.keyPressed(key));
        EXPECT_FALSE(reference->isVisible());
        EXPECT_TRUE(viewport->isVisible());
        EXPECT_FALSE(panel.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
    }
}

/// Large section headings replace introductory copy and retain compact top space and explicit following gaps.
TEST(SettingsPanel, DisplayOnlyGroupsHaveReadableTypographyAndFullAccessibleControlTitles)
{
    ImageControlProcessor processor;
    const SettingsPanel panel(processor.getEditorState());
    EXPECT_EQ(settingsControl<juce::Label>(panel, "displaySettingsTitle"), nullptr);
    EXPECT_EQ(settingsControl<juce::Label>(panel, "displaySettingsDescription"), nullptr);
    const auto* viewport = childOfType<juce::Viewport>(panel);
    ASSERT_NE(viewport, nullptr);
    const auto* content = viewport->getViewedComponent();
    ASSERT_NE(content, nullptr);
    const std::array groups {
        std::pair {"displayAppearanceSection", "Appearance"},
        std::pair {"displayStereoPositionSection", "Stereo position"},
        std::pair {"displayCorrelationSection", "Correlation"},
        std::pair {"displayBehaviorSection", "Display behavior"},
    };
    int previousY = 0;
    for (const auto& [identifier, text] : groups) {
        auto* heading = settingsControl<juce::Label>(panel, identifier);
        ASSERT_NE(heading, nullptr);
        EXPECT_EQ(heading->getText(), text);
        EXPECT_FLOAT_EQ(heading->getFont().getHeight(), 18.0F);
        EXPECT_TRUE(heading->getFont().isBold());
        EXPECT_EQ(heading->getJustificationType(), juce::Justification(juce::Justification::centredLeft));
        EXPECT_GE(static_cast<float>(heading->getHeight()), heading->getFont().getHeight());
        EXPECT_GT(heading->getY(), previousY);
        if (previousY == 0) {
            EXPECT_EQ(heading->getY(), 4);
        }
        int firstRowY = content->getHeight();
        int previousBottom = 0;
        for (const auto* child : content->getChildren()) {
            if (child == heading) {
                continue;
            }
            if (child->getY() >= heading->getBottom()) {
                firstRowY = std::min(firstRowY, child->getY());
            } else if (child->getBottom() <= heading->getY()) {
                previousBottom = std::max(previousBottom, child->getBottom());
            }
        }
        EXPECT_GE(firstRowY - heading->getBottom(), 8);
        EXPECT_LE(firstRowY - heading->getBottom(), 10);
        if (previousY != 0) {
            EXPECT_GE(heading->getY() - previousBottom, 12);
        }
        previousY = heading->getY();
    }
    const auto* midpoint = settingsControl<ControlSlider>(panel, PAN_MIDPOINT_PROPERTY);
    const auto* edge = settingsControl<ControlSlider>(panel, PAN_EDGE_PROPERTY);
    ASSERT_NE(midpoint, nullptr);
    ASSERT_NE(edge, nullptr);
    EXPECT_EQ(midpoint->getTitle(), "Display position midpoint (50%)");
    EXPECT_EQ(edge->getTitle(), "Display position edge (99%)");
}

/// Resizing inside the shortcuts submenu keeps hidden preferences laid out and retains their legal scroll position.
TEST(SettingsPanel, SubmenuResizePreservesPreferenceScrollAndControlLifetime)
{
    ImageControlProcessor processor;
    SettingsPanel panel(processor.getEditorState(), 300);
    auto* viewport = settingsControl<juce::Viewport>(panel, "displaySettingsViewport");
    auto* shortcuts = settingsControl<juce::TextButton>(panel, "showKeyboardShortcuts");
    auto* reference = settingsControl<juce::Component>(panel, "keyboardShortcuts");
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(shortcuts, nullptr);
    ASSERT_NE(reference, nullptr);
    viewport->setViewPosition(0, 120);
    const int scrollPosition = viewport->getViewPositionY();
    ASSERT_GT(scrollPosition, 0);
    const juce::Component::SafePointer<juce::TextButton> safeShortcuts(shortcuts);
    panel.showKeyboardShortcuts();
    panel.setSize(panel.getWidth(), 260);
    EXPECT_TRUE(reference->isVisible());
    EXPECT_TRUE(panel.getLocalBounds().contains(reference->getBounds()));
    EXPECT_EQ(viewport->getHeight(), 260);
    panel.showPreferences();
    EXPECT_EQ(viewport->getViewPositionY(), scrollPosition);
    EXPECT_EQ(safeShortcuts.getComponent(), shortcuts);
    EXPECT_EQ(settingsControl<juce::TextButton>(panel, "showKeyboardShortcuts"), shortcuts);
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

/// Keyboard traversal reveals distant controls rather than leaving focus below the popup's visible viewport.
TEST(SettingsPanel, FocusedControlsScrollIntoView)
{
    ImageControlProcessor processor;
    SettingsPanel panel(processor.getEditorState(), 240);
    panel.addToDesktop(0);
    panel.setVisible(true);
    panel.toFront(true);
    auto* viewport = childOfType<juce::Viewport>(panel);
    auto* shortcuts = settingsControl<juce::TextButton>(panel, "showKeyboardShortcuts");
    auto* contrast = settingsControl<ControlSlider>(panel, CONTRAST_PROPERTY);
    ASSERT_NE(viewport, nullptr);
    ASSERT_NE(shortcuts, nullptr);
    ASSERT_NE(contrast, nullptr);
    shortcuts->grabKeyboardFocus();
    EXPECT_TRUE(waitForVisibleControl(*viewport, *shortcuts));
    ASSERT_TRUE(shortcuts->hasKeyboardFocus(false));
    auto visibleBounds = viewport->getLocalArea(shortcuts, shortcuts->getLocalBounds());
    EXPECT_GE(visibleBounds.getY(), 0);
    EXPECT_LE(visibleBounds.getBottom(), viewport->getViewHeight());
    EXPECT_GT(viewport->getViewPositionY(), 0);
    const int footerPosition = viewport->getViewPositionY();
    contrast->grabKeyboardFocus();
    EXPECT_TRUE(waitForVisibleControl(*viewport, *contrast));
    ASSERT_TRUE(contrast->hasKeyboardFocus(false));
    EXPECT_LT(viewport->getViewPositionY(), footerPosition);
    visibleBounds = viewport->getLocalArea(contrast, contrast->getLocalBounds());
    EXPECT_GE(visibleBounds.getY(), 0);
    EXPECT_LE(visibleBounds.getBottom(), viewport->getViewHeight());
}

/// Legal slider intervals remain reachable even when a fine range-based nudge would otherwise round to no change.
TEST(ControlSlider, KeyboardNudgesRespectMinimumIntervalsAndBalanceGestures)
{
    for (const int modifiers : {0, static_cast<int>(juce::ModifierKeys::shiftModifier)}) {
        ControlSlider slider;
        slider.setRange(3.0, 24.0, 1.0);
        slider.setValue(12.0, juce::dontSendNotification);
        std::vector<bool> gestures;
        slider.onDragStart = [&] { gestures.push_back(true); };
        slider.onDragEnd = [&] { gestures.push_back(false); };
        EXPECT_TRUE(slider.keyPressed(juce::KeyPress(juce::KeyPress::rightKey, modifiers, 0)));
        EXPECT_DOUBLE_EQ(slider.getValue(), 13.0);
        EXPECT_TRUE(slider.keyPressed(juce::KeyPress(juce::KeyPress::downKey, modifiers, 0)));
        EXPECT_DOUBLE_EQ(slider.getValue(), 12.0);
        EXPECT_EQ(gestures, (std::vector<bool> {true, false, true, false}));
        slider.setValue(slider.getMaximum(), juce::dontSendNotification);
        EXPECT_TRUE(slider.keyPressed(juce::KeyPress(juce::KeyPress::upKey, modifiers, 0)));
        EXPECT_DOUBLE_EQ(slider.getValue(), slider.getMaximum());
    }
}

/// Command, Control, and Alt arrow shortcuts belong to the host and must not start slider gestures.
TEST(ControlSlider, ModifiedArrowsAreNotConsumed)
{
    ControlSlider slider;
    slider.setRange(0.0, 100.0, 1.0);
    slider.setValue(50.0, juce::dontSendNotification);
    int starts = 0;
    int ends = 0;
    slider.onDragStart = [&] { ++starts; };
    slider.onDragEnd = [&] { ++ends; };
    for (const int modifier :
         {juce::ModifierKeys::commandModifier, juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::altModifier})
    {
        for (const int shift : {0, static_cast<int>(juce::ModifierKeys::shiftModifier)}) {
            for (const int arrow :
                 {juce::KeyPress::leftKey, juce::KeyPress::rightKey, juce::KeyPress::upKey, juce::KeyPress::downKey})
            {
                EXPECT_FALSE(
                    slider.keyPressed(juce::KeyPress(arrow, juce::ModifierKeys(modifier).withFlags(shift), 0))
                );
                EXPECT_DOUBLE_EQ(slider.getValue(), 50.0);
            }
        }
    }
    EXPECT_EQ(starts, 0);
    EXPECT_EQ(ends, 0);
}

/// Raising the midpoint pushes the edge once, while a noncolliding edit leaves the edge untouched.
TEST(SettingsPanel, PanMidpointPushesTheEdgeWithoutRecursiveNotifications)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(PAN_MIDPOINT_PROPERTY, MIN_PAN_MIDPOINT_DB, nullptr);
    state.setProperty(PAN_EDGE_PROPERTY, MIN_PAN_EDGE_DB, nullptr);
    const SettingsPanel panel(state);
    auto* midpoint = settingsControl<juce::Slider>(panel, PAN_MIDPOINT_PROPERTY);
    auto* edge = settingsControl<juce::Slider>(panel, PAN_EDGE_PROPERTY);
    ASSERT_NE(midpoint, nullptr);
    ASSERT_NE(edge, nullptr);
    dispatchControlChanges();
    const SliderChangeCounter midpointChanges(*midpoint);
    const SliderChangeCounter edgeChanges(*edge);

    midpoint->setValue(MAX_PAN_MIDPOINT_DB, juce::sendNotificationSync);
    EXPECT_TRUE(waitForSliderChanges(midpointChanges, edgeChanges));
    EXPECT_DOUBLE_EQ(midpoint->getValue(), MAX_PAN_MIDPOINT_DB);
    EXPECT_DOUBLE_EQ(edge->getValue(), MAX_PAN_MIDPOINT_DB + MIN_PAN_CURVE_GAP_DB);
    EXPECT_DOUBLE_EQ(static_cast<double>(state.getProperty(PAN_MIDPOINT_PROPERTY)), midpoint->getValue());
    EXPECT_DOUBLE_EQ(static_cast<double>(state.getProperty(PAN_EDGE_PROPERTY)), edge->getValue());
    EXPECT_EQ(midpointChanges.changeCount(), 1);
    EXPECT_EQ(edgeChanges.changeCount(), 1);

    midpoint->setValue(MAX_PAN_MIDPOINT_DB, juce::sendNotificationSync);
    dispatchControlChanges();
    EXPECT_EQ(midpointChanges.changeCount(), 1);
    EXPECT_EQ(edgeChanges.changeCount(), 1);

    midpoint->setValue(MIN_PAN_MIDPOINT_DB, juce::sendNotificationSync);
    dispatchControlChanges();
    EXPECT_DOUBLE_EQ(edge->getValue(), MAX_PAN_MIDPOINT_DB + MIN_PAN_CURVE_GAP_DB);
    EXPECT_EQ(midpointChanges.changeCount(), 2);
    EXPECT_EQ(edgeChanges.changeCount(), 1);
}

/// Lowering the edge pushes the midpoint once, and the bound pair survives panel destruction and host restore.
TEST(SettingsPanel, PanEdgePushesTheMidpointAndSavesTheCoupledValues)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(PAN_MIDPOINT_PROPERTY, MAX_PAN_MIDPOINT_DB, nullptr);
    state.setProperty(PAN_EDGE_PROPERTY, MAX_PAN_EDGE_DB, nullptr);
    {
        const SettingsPanel panel(state);
        auto* midpoint = settingsControl<juce::Slider>(panel, PAN_MIDPOINT_PROPERTY);
        auto* edge = settingsControl<juce::Slider>(panel, PAN_EDGE_PROPERTY);
        ASSERT_NE(midpoint, nullptr);
        ASSERT_NE(edge, nullptr);
        dispatchControlChanges();
        const SliderChangeCounter midpointChanges(*midpoint);
        const SliderChangeCounter edgeChanges(*edge);

        edge->setValue(MIN_PAN_EDGE_DB, juce::sendNotificationSync);
        EXPECT_TRUE(waitForSliderChanges(midpointChanges, edgeChanges));
        EXPECT_DOUBLE_EQ(edge->getValue(), MIN_PAN_EDGE_DB);
        EXPECT_DOUBLE_EQ(midpoint->getValue(), MIN_PAN_EDGE_DB - MIN_PAN_CURVE_GAP_DB);
        EXPECT_DOUBLE_EQ(static_cast<double>(state.getProperty(PAN_MIDPOINT_PROPERTY)), midpoint->getValue());
        EXPECT_DOUBLE_EQ(static_cast<double>(state.getProperty(PAN_EDGE_PROPERTY)), edge->getValue());
        EXPECT_EQ(midpointChanges.changeCount(), 1);
        EXPECT_EQ(edgeChanges.changeCount(), 1);
        dispatchControlChanges();
        EXPECT_EQ(midpointChanges.changeCount(), 1);
        EXPECT_EQ(edgeChanges.changeCount(), 1);
    }

    juce::MemoryBlock saved;
    processor.getStateInformation(saved);
    ImageControlProcessor restored;
    restored.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
    const SettingsPanel reopened(restored.getEditorState());
    auto* midpoint = settingsControl<juce::Slider>(reopened, PAN_MIDPOINT_PROPERTY);
    auto* edge = settingsControl<juce::Slider>(reopened, PAN_EDGE_PROPERTY);
    ASSERT_NE(midpoint, nullptr);
    ASSERT_NE(edge, nullptr);
    EXPECT_DOUBLE_EQ(midpoint->getValue(), MIN_PAN_EDGE_DB - MIN_PAN_CURVE_GAP_DB);
    EXPECT_DOUBLE_EQ(edge->getValue(), MIN_PAN_EDGE_DB);
}

/// The divider is named and keyboard-reachable, with coarse and fine vertical steps using the same callback.
TEST(SplitDivider, KeyboardResizeUsesNamedFocusableControlAndRejectsHostShortcuts)
{
    SplitDivider divider;
    EXPECT_EQ(divider.getComponentID(), "displayBandDivider");
    EXPECT_TRUE(divider.getTitle().contains("divider"));
    EXPECT_TRUE(divider.getWantsKeyboardFocus());
    EXPECT_TRUE(divider.hasFocusOutline());
    std::vector<int> movements;
    divider.setDragCallback([&](const int delta) { movements.push_back(delta); });
    EXPECT_TRUE(divider.keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
    EXPECT_TRUE(divider.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    EXPECT_TRUE(divider.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0)));
    EXPECT_TRUE(divider.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)));
    EXPECT_EQ(movements, (std::vector<int> {8, -8, 1, -1}));
    for (const int modifier :
         {juce::ModifierKeys::commandModifier, juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::altModifier})
    {
        EXPECT_FALSE(divider.keyPressed(juce::KeyPress(juce::KeyPress::upKey, modifier, 0)));
        EXPECT_FALSE(divider.keyPressed(
            juce::KeyPress(
                juce::KeyPress::downKey, juce::ModifierKeys(modifier).withFlags(juce::ModifierKeys::shiftModifier), 0
            )
        ));
    }
    EXPECT_FALSE(divider.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)));
    divider.setEnabled(false);
    EXPECT_FALSE(divider.keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
    EXPECT_EQ(movements.size(), 4U);
}

/// Only a genuine left-button drag can resize, including synthetic handlers dispatched before mouse-down.
TEST(SplitDivider, PointerResizeRejectsNonLeftButtonsAndUnstartedDrags)
{
    SplitDivider divider;
    divider.setBounds(0, 100, 400, 16);
    std::vector<int> movements;
    divider.setDragCallback([&](const int delta) { movements.push_back(delta); });
    const auto start = divider.localPointToGlobal(juce::Point<int> {5, 5});
    const auto end = start.translated(0, 20);
    divider.mouseDrag(mouseAtScreen(divider, end, start));
    for (const int buttons :
         {
             static_cast<int>(juce::ModifierKeys::rightButtonModifier),
             static_cast<int>(juce::ModifierKeys::middleButtonModifier),
             juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::rightButtonModifier,
             juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::backButtonModifier,
         })
    {
        divider.mouseDown(mouseAtScreen(divider, start, start, buttons));
        divider.mouseDrag(mouseAtScreen(divider, end, start, buttons));
        divider.mouseUp(mouseAtScreen(divider, end, start, buttons));
    }
    EXPECT_TRUE(movements.empty());
    divider.mouseDown(mouseAtScreen(divider, start, start));
    divider.mouseDrag(mouseAtScreen(divider, end, start));
    EXPECT_EQ(movements, (std::vector<int> {20}));
    EXPECT_FALSE(divider.keyPressed(juce::KeyPress(juce::KeyPress::downKey)));
    divider.mouseUp(mouseAtScreen(divider, end, start));
    divider.mouseDrag(mouseAtScreen(divider, end.translated(0, 10), start));
    EXPECT_EQ(movements, (std::vector<int> {20}));
}

/// JUCE manages one shared outline rather than each control painting an unrelated focus style.
TEST(ControlSlider, LookAndFeelCreatesSharedFocusOutline)
{
    ImageControlLookAndFeel lookAndFeel;
    ControlSlider slider;
    EXPECT_TRUE(slider.hasFocusOutline());
    EXPECT_NE(lookAndFeel.createFocusOutlineForComponent(slider), nullptr);
    juce::TextButton button("Settings");
    EXPECT_NE(lookAndFeel.createFocusOutlineForComponent(button), nullptr);
    juce::ToggleButton toggle("Show correlation");
    EXPECT_NE(lookAndFeel.createFocusOutlineForComponent(toggle), nullptr);
    juce::DrawableButton drawable("Undo", juce::DrawableButton::ImageFitted);
    EXPECT_NE(lookAndFeel.createFocusOutlineForComponent(drawable), nullptr);
}

/// Relayout moves the divider under the pointer, so local coordinates cannot stand in for incremental screen movement.
TEST(SplitDivider, ReportsIncrementalScreenMovementAndIgnoresStationaryEvents)
{
    juce::Component parent;
    parent.setBounds(40, 70, 400, 400);
    SplitDivider divider;
    parent.addAndMakeVisible(divider);
    divider.setBounds(0, 100, 400, 16);
    std::vector<int> movements;
    divider.setDragCallback([&](const int delta) {
        movements.push_back(delta);
        divider.setTopLeftPosition(divider.getX(), divider.getY() + delta);
    });
    const auto start = divider.localPointToGlobal(juce::Point<int> {5, 5});
    divider.mouseDown(mouseAtScreen(divider, start, start));
    divider.mouseDrag(mouseAtScreen(divider, start, start));
    EXPECT_TRUE(movements.empty());
    divider.mouseDrag(mouseAtScreen(divider, start.translated(0, 20), start));
    EXPECT_EQ(movements, (std::vector<int> {20}));
    divider.mouseDrag(mouseAtScreen(divider, start.translated(0, 20), start));
    EXPECT_EQ(movements, (std::vector<int> {20}));
    divider.mouseDrag(mouseAtScreen(divider, start.translated(0, 35), start));
    divider.mouseDrag(mouseAtScreen(divider, start.translated(0, 28), start));
    divider.mouseUp(mouseAtScreen(divider, start.translated(0, 28), start));
    EXPECT_EQ(movements, (std::vector<int> {20, 15, -7}));
    EXPECT_EQ(divider.getY(), 128);
}

/// Missing callbacks still advance the last screen position, and each mouse-down starts a fresh movement origin.
TEST(SplitDivider, TracksMovementWithoutACallbackAndResetsTheOriginForEachDrag)
{
    SplitDivider divider;
    divider.setBounds(10, 100, 400, 16);
    const auto start = divider.localPointToGlobal(juce::Point<int> {5, 5});
    divider.mouseDown(mouseAtScreen(divider, start, start));
    divider.mouseDrag(mouseAtScreen(divider, start.translated(0, 20), start));
    std::vector<int> movements;
    divider.setDragCallback([&](const int delta) { movements.push_back(delta); });
    divider.mouseDrag(mouseAtScreen(divider, start.translated(0, 25), start));
    divider.mouseUp(mouseAtScreen(divider, start.translated(0, 25), start));
    EXPECT_EQ(movements, (std::vector<int> {5}));

    const auto restart = start.translated(0, -30);
    divider.mouseDown(mouseAtScreen(divider, restart, restart));
    divider.mouseDrag(mouseAtScreen(divider, restart, restart));
    EXPECT_EQ(movements, (std::vector<int> {5}));
    divider.mouseDrag(mouseAtScreen(divider, restart.translated(0, -4), restart));
    divider.mouseUp(mouseAtScreen(divider, restart.translated(0, -4), restart));
    EXPECT_EQ(movements, (std::vector<int> {5, -4}));
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
}  // namespace esgrove::image_control::test
