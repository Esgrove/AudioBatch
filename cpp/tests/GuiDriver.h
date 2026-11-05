/// GUI integration test driver for the C++ application.
/// Declares GuiDriver, which places a real component on the desktop,
/// finds its controls by the stable component ids shared with the Rust application in `tests/gui_controls.json`,
/// reads their native accessibility state,
/// and drives them only through the window peer's mouse, key, and file drop entry points,
/// so every interaction passes through JUCE's hit testing, focus, and modal routing.
/// It never compares pixels or automates the desktop outside the test process.

#pragma once

#include <JuceHeader.h>

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

namespace audiobatch::tests
{
/// Environment variable that shows the driven window on screen when set to "1", so a developer can watch each step.
/// Unset or any other value keeps the default transparent window, which CI and normal runs use.
constexpr auto VISIBLE_GUI_TESTS_VARIABLE {"AUDIOBATCH_GUI_TESTS_VISIBLE"};
/// Environment variable with the pause after each driven action in milliseconds while the window is visible.
constexpr auto VISIBLE_GUI_TESTS_STEP_DELAY_VARIABLE {"AUDIOBATCH_GUI_TESTS_STEP_DELAY_MS"};
/// Pause after each driven action while the window is visible, when the delay variable is unset or invalid.
constexpr int DEFAULT_VISIBLE_STEP_DELAY_MILLISECONDS {400};
/// Longest accepted pause after a visible action.
/// The longest case pauses about thirty times, so this keeps a visible run inside the 60 second CTest timeout.
constexpr int MAXIMUM_VISIBLE_STEP_DELAY_MILLISECONDS {1500};

/// Owned facts about one control, read from its component and accessibility handler at query time.
struct ControlSnapshot {
    juce::Component* component {nullptr};
    juce::Rectangle<int> boundsInPeer;
    juce::String componentId;
    juce::String title;
    juce::String value;
    std::optional<juce::AccessibilityRole> role;
    bool checked {false};
    bool enabled {false};
    bool focused {false};
    bool showing {false};
};

/// Places a component in an undecorated desktop window for one test and drives it through that window's peer.
/// Must be used on the JUCE message thread, and the component must outlive the driver.
class GuiDriver final
{
public:
    explicit GuiDriver(juce::Component& root, juce::Rectangle<int> bounds = {0, 0, 1920, 1080});

    ~GuiDriver();

    GuiDriver(const GuiDriver&) = delete;
    GuiDriver(GuiDriver&&) = delete;
    GuiDriver& operator=(const GuiDriver&) = delete;
    GuiDriver& operator=(GuiDriver&&) = delete;

    [[nodiscard]] static bool dispatchUntil(
        const std::function<bool()>& complete,
        std::chrono::milliseconds timeout = std::chrono::seconds(10)
    );
    [[nodiscard]] static juce::String roleName(juce::AccessibilityRole role);

    static void settle();

    [[nodiscard]] ControlSnapshot find(const juce::String& controlId) const;
    [[nodiscard]] ControlSnapshot find(const juce::Component& scope, const juce::String& controlId) const;
    [[nodiscard]] juce::ComponentPeer& peer() const;
    [[nodiscard]] juce::StringArray registeredIds() const;
    [[nodiscard]] std::optional<ControlSnapshot> tryFind(const juce::String& controlId) const;
    [[nodiscard]] std::vector<ControlSnapshot> findAll(const juce::String& controlId) const;
    [[nodiscard]] std::vector<ControlSnapshot> findByRole(juce::AccessibilityRole role) const;

    void dropFiles(const juce::StringArray& files, juce::Point<int> positionInPeer) const;
    void press(const juce::KeyPress& key) const;
    void setValue(const juce::String& controlId, double value) const;
    void type(const juce::String& text) const;

    void click(const juce::String& controlId, juce::ModifierKeys modifiers = {});
    void clickAt(juce::Component& target, juce::Point<int> local, juce::ModifierKeys modifiers = {}, int clicks = 1);
    void doubleClick(const juce::String& controlId);
    void drag(const juce::String& controlId, juce::Point<int> distance);
    void focusWindow();
    void press(const juce::String& controlId, const juce::KeyPress& key);
    void rightClick(const juce::String& controlId);
    void rightClickAt(juce::Component& target, juce::Point<int> local);
    void scroll(const juce::String& controlId, float deltaY);

private:
    [[nodiscard]] ControlSnapshot snapshot(juce::Component& target) const;
    [[nodiscard]] juce::Point<float> pointInPeer(const juce::Component& target, juce::Point<int> local) const;

    void pauseForViewer(const juce::String& action) const;
    void requireReachable(const ControlSnapshot& control, juce::Point<float> position) const;
    void requireUnderMouse(const juce::Component& target) const;
    void sendButton(juce::Point<float> position, juce::ModifierKeys buttons, juce::ModifierKeys modifiers);
    void sendMove(juce::Point<float> position, juce::ModifierKeys modifiers);

    juce::Component& component;
    juce::int64 eventTime {0};
    /// Pause after each action while the window is visible, or zero for the default transparent window.
    int visibleStepDelayMilliseconds {0};
    bool visible {false};
};
}  // namespace audiobatch::tests
