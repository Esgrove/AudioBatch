/// Tests SettingsPanel's coupled pan curve controls and SplitDivider's incremental screen-space movement.
/// Event-handler tests also verify that editor divider clamps persist through host state and reopening.
/// These tests inspect control values and layout bounds without desktop automation or pixel assertions.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "Parameters.h"
#include "ui/BandStrip.h"
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

/// Finds a panel-owned pan slider through its displayed unit and supported range.
/// The nullable observer remains valid until the panel is destroyed.
juce::Slider* panSlider(const SettingsPanel& panel, const double minimum, const double maximum)
{
    for (auto* child : panel.getChildren()) {
        if (auto* slider = dynamic_cast<juce::Slider*>(child); slider != nullptr
            && slider->getTextValueSuffix() == " dB" && juce::approximatelyEqual(slider->getMinimum(), minimum)
            && juce::approximatelyEqual(slider->getMaximum(), maximum))
        {
            return slider;
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

/// Creates a handler event at a fixed screen position even when layout has moved the component since mouse-down.
juce::MouseEvent mouseAtScreen(
    juce::Component& component,
    const juce::Point<int> screenPosition,
    const juce::Point<int> downScreenPosition
)
{
    const auto now = juce::Time::getCurrentTime();
    return {
        juce::Desktop::getInstance().getMainMouseSource(),
        component.getLocalPoint(nullptr, screenPosition).toFloat(),
        juce::ModifierKeys::leftButtonModifier,
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

/// Raising the midpoint pushes the edge once, while a noncolliding edit leaves the edge untouched.
TEST(SettingsPanel, PanMidpointPushesTheEdgeWithoutRecursiveNotifications)
{
    ImageControlProcessor processor;
    auto& state = processor.getEditorState();
    state.setProperty(PAN_MIDPOINT_PROPERTY, MIN_PAN_MIDPOINT_DB, nullptr);
    state.setProperty(PAN_EDGE_PROPERTY, MIN_PAN_EDGE_DB, nullptr);
    const SettingsPanel panel(state);
    auto* midpoint = panSlider(panel, MIN_PAN_MIDPOINT_DB, MAX_PAN_MIDPOINT_DB);
    auto* edge = panSlider(panel, MIN_PAN_EDGE_DB, MAX_PAN_EDGE_DB);
    ASSERT_NE(midpoint, nullptr);
    ASSERT_NE(edge, nullptr);
    dispatchControlChanges();
    const SliderChangeCounter midpointChanges(*midpoint);
    const SliderChangeCounter edgeChanges(*edge);

    midpoint->setValue(MAX_PAN_MIDPOINT_DB, juce::sendNotificationSync);
    dispatchControlChanges();
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
        auto* midpoint = panSlider(panel, MIN_PAN_MIDPOINT_DB, MAX_PAN_MIDPOINT_DB);
        auto* edge = panSlider(panel, MIN_PAN_EDGE_DB, MAX_PAN_EDGE_DB);
        ASSERT_NE(midpoint, nullptr);
        ASSERT_NE(edge, nullptr);
        dispatchControlChanges();
        const SliderChangeCounter midpointChanges(*midpoint);
        const SliderChangeCounter edgeChanges(*edge);

        edge->setValue(MIN_PAN_EDGE_DB, juce::sendNotificationSync);
        dispatchControlChanges();
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
    auto* midpoint = panSlider(reopened, MIN_PAN_MIDPOINT_DB, MAX_PAN_MIDPOINT_DB);
    auto* edge = panSlider(reopened, MIN_PAN_EDGE_DB, MAX_PAN_EDGE_DB);
    ASSERT_NE(midpoint, nullptr);
    ASSERT_NE(edge, nullptr);
    EXPECT_DOUBLE_EQ(midpoint->getValue(), MIN_PAN_EDGE_DB - MIN_PAN_CURVE_GAP_DB);
    EXPECT_DOUBLE_EQ(edge->getValue(), MIN_PAN_EDGE_DB);
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
