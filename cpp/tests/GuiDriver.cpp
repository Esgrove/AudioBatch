/// Implementation of GuiDriver: desktop placement, component-id and role queries,
/// accessibility snapshots, and mouse, key, and file drop delivery through the window peer.

#include "GuiDriver.h"

#include "utils/StringFormat.h"
#include <fmt/base.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// Milliseconds added to the synthetic event clock between peer events, so JUCE sees ordered, distinct events.
constexpr juce::int64 EVENT_STEP_MILLISECONDS {20};
/// Milliseconds between the two clicks of a double click, inside JUCE's double-click interval.
constexpr juce::int64 DOUBLE_CLICK_GAP_MILLISECONDS {50};
/// Milliseconds after a click before the next one, outside JUCE's double-click interval.
constexpr juce::int64 SEPARATE_CLICK_GAP_MILLISECONDS {1000};
/// Opacity of the default nearly transparent window.
/// Windows hit testing skips fully transparent layered windows, so use its smallest nonzero alpha step.
/// macOS window-server hit testing also skips transparent windows, including alpha values below one percent.
/// Keep both windows hit testable so JUCE routes peer events through the real desktop window.
/// They also receive the developer's own pointer and wheel input, so leave the mouse alone during integration tests.
#if JUCE_WINDOWS
constexpr float HIDDEN_WINDOW_ALPHA {1.0f / 255.0f};
#else
constexpr float HIDDEN_WINDOW_ALPHA {0.01f};
#endif
/// Pointer positions between the start and the end of a drag.
constexpr int DRAG_STEPS {4};
/// JUCE roles with the names that `tests/gui_controls.json` uses for them.
constexpr std::array ROLE_NAMES {
    std::pair {juce::AccessibilityRole::button, "button"},
    std::pair {juce::AccessibilityRole::toggleButton, "toggleButton"},
    std::pair {juce::AccessibilityRole::radioButton, "radioButton"},
    std::pair {juce::AccessibilityRole::comboBox, "comboBox"},
    std::pair {juce::AccessibilityRole::slider, "slider"},
    std::pair {juce::AccessibilityRole::label, "label"},
    std::pair {juce::AccessibilityRole::table, "table"},
    std::pair {juce::AccessibilityRole::group, "group"},
    std::pair {juce::AccessibilityRole::popupMenu, "popupMenu"},
    std::pair {juce::AccessibilityRole::menuItem, "menuItem"},
    std::pair {juce::AccessibilityRole::row, "row"},
};

/// Collects every component below a root, including the root, with an explicit stack instead of recursion.
void collectComponents(juce::Component& root, std::vector<juce::Component*>& components)
{
    std::vector<juce::Component*> pending {&root};
    while (!pending.empty()) {
        auto* next = pending.back();
        pending.pop_back();
        components.push_back(next);
        pending.insert(pending.end(), next->getChildren().begin(), next->getChildren().end());
    }
}

/// Throws with a message that GoogleTest reports, for queries that cannot continue.
[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

/// Returns whether the developer opted in to watching the driven window, which is off unless the variable is "1".
bool visibleTestsRequested()
{
    return juce::SystemStats::getEnvironmentVariable(VISIBLE_GUI_TESTS_VARIABLE, {}) == "1";
}

/// Reads the pause after each visible action, falling back to the default for an unset, invalid, or negative value.
int visibleStepDelay()
{
    const auto text = juce::SystemStats::getEnvironmentVariable(VISIBLE_GUI_TESTS_STEP_DELAY_VARIABLE, {}).trim();
    if (text.isEmpty() || !text.containsOnly("0123456789")) {
        return DEFAULT_VISIBLE_STEP_DELAY_MILLISECONDS;
    }
    return juce::jmin(text.getIntValue(), MAXIMUM_VISIBLE_STEP_DELAY_MILLISECONDS);
}
}  // namespace

/// Adds the root to the desktop as an undecorated, always-on-top window inside the primary display,
/// then pumps the message loop until the peer has laid it out.
/// On macOS, JUCE asks the window server which window is at each event position,
/// so the window must be on screen, frontmost, and accept mouse events for its own peer events to reach it.
/// By default the window is nearly transparent, at the smallest opacity that keeps platform hit testing enabled.
/// With `AUDIOBATCH_GUI_TESTS_VISIBLE=1` it stays opaque and pauses after each action, so a developer can follow it.
/// The requested size is clamped to the display's user area, which keeps the wide layout on common displays.
GuiDriver::GuiDriver(juce::Component& root, const juce::Rectangle<int> bounds) :
    component(root),
    eventTime(juce::Time::currentTimeMillis()),
    visible(visibleTestsRequested())
{
    if (visible) {
        visibleStepDelayMilliseconds = visibleStepDelay();
    }
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const auto area = display == nullptr ? bounds : display->userBounds.toNearestInt();
    component.setBounds(
        area.getX(),
        area.getY(),
        juce::jmin(bounds.getWidth(), area.getWidth()),
        juce::jmin(bounds.getHeight(), area.getHeight())
    );
    component.addToDesktop(juce::ComponentPeer::windowIsTemporary);
    component.setAlwaysOnTop(true);
    component.setAlpha(visible ? 1.0f : HIDDEN_WINDOW_ALPHA);
    component.setVisible(true);
    component.toFront(true);
    settle();
    if (component.getPeer() == nullptr) {
        fail("GuiDriver could not create a window peer");
    }
    focusWindow();
    pauseForViewer("window opened");
}

/// Removes the window, so the component can be destroyed by its owner without a peer.
GuiDriver::~GuiDriver()
{
    component.removeFromDesktop();
}

/// Gives the test window keyboard focus within the test process, which JUCE requires before it routes key presses.
/// It never activates the process, so the developer's frontmost application keeps the system focus.
void GuiDriver::focusWindow()
{
    peer().grabFocus();
    settle();
    if (!dispatchUntil([this] { return peer().isFocused(); }, std::chrono::seconds(2))) {
        fail("The test window could not take keyboard focus");
    }
}

/// Returns the platform-independent name of a JUCE role, as the shared manifest spells it.
juce::String GuiDriver::roleName(const juce::AccessibilityRole role)
{
    for (const auto& [candidate, name] : ROLE_NAMES) {
        if (candidate == role) {
            return name;
        }
    }
    return "other";
}

/// Requires exactly one control with the id below the root, listing the registered ids when it is missing or repeated.
ControlSnapshot GuiDriver::find(const juce::String& controlId) const
{
    return find(component, controlId);
}

/// Requires exactly one control with the id below a scope, such as a popup or a separate window.
ControlSnapshot GuiDriver::find(const juce::Component& scope, const juce::String& controlId) const
{
    std::vector<juce::Component*> components;
    collectComponents(const_cast<juce::Component&>(scope), components);
    std::vector<ControlSnapshot> matches;
    for (auto* candidate : components) {
        if (candidate->getComponentID() == controlId) {
            matches.push_back(snapshot(*candidate));
        }
    }
    if (matches.size() != 1) {
        fail(
            utils::format(
                "Expected one control {}, found {}. Registered ids: {}",
                controlId,
                matches.size(),
                registeredIds().joinIntoString(", ")
            )
                .toStdString()
        );
    }
    return matches.front();
}

/// Returns every control with the id, including hidden ones, in tree order.
std::vector<ControlSnapshot> GuiDriver::findAll(const juce::String& controlId) const
{
    std::vector<juce::Component*> components;
    collectComponents(component, components);
    std::vector<ControlSnapshot> matches;
    for (auto* candidate : components) {
        if (candidate->getComponentID() == controlId) {
            matches.push_back(snapshot(*candidate));
        }
    }
    return matches;
}

/// Returns every component whose native accessibility handler reports the role, in tree order.
std::vector<ControlSnapshot> GuiDriver::findByRole(const juce::AccessibilityRole role) const
{
    std::vector<juce::Component*> components;
    collectComponents(component, components);
    std::vector<ControlSnapshot> matches;
    for (auto* candidate : components) {
        const auto control = snapshot(*candidate);
        if (control.role == role) {
            matches.push_back(control);
        }
    }
    return matches;
}

/// Returns the control when exactly one exists, and nothing when it is absent.
std::optional<ControlSnapshot> GuiDriver::tryFind(const juce::String& controlId) const
{
    auto matches = findAll(controlId);
    if (matches.empty()) {
        return std::nullopt;
    }
    if (matches.size() > 1) {
        fail(utils::format("Control id {} is ambiguous", controlId).toStdString());
    }
    return matches.front();
}

/// Lists the non-empty component ids below the root, for failure messages.
juce::StringArray GuiDriver::registeredIds() const
{
    std::vector<juce::Component*> components;
    collectComponents(component, components);
    juce::StringArray ids;
    for (const auto* candidate : components) {
        if (candidate->getComponentID().isNotEmpty()) {
            ids.add(candidate->getComponentID());
        }
    }
    ids.sort(true);
    return ids;
}

/// Returns the peer of the desktop window, which exists for the driver's whole lifetime.
juce::ComponentPeer& GuiDriver::peer() const
{
    auto* windowPeer = component.getPeer();
    if (windowPeer == nullptr) {
        fail("The driven component lost its window peer");
    }
    return *windowPeer;
}

/// Pumps native dispatch, timers, and worker callbacks until the condition holds or the deadline passes.
bool GuiDriver::dispatchUntil(const std::function<bool()>& complete, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!complete() && std::chrono::steady_clock::now() < deadline) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    }
    return complete();
}

/// Runs queued messages and asynchronous updates for a short bounded slice after an interaction.
void GuiDriver::settle()
{
    auto delivered = std::make_shared<bool>(false);
    juce::MessageManager::callAsync([delivered] { *delivered = true; });
    juce::ignoreUnused(dispatchUntil([&] { return *delivered; }, std::chrono::seconds(2)));
}

/// Clicks the centre of a control with the left button, after requiring that it is showing, enabled, and on top.
void GuiDriver::click(const juce::String& controlId, const juce::ModifierKeys modifiers)
{
    const auto control = find(controlId);
    clickAt(*control.component, control.component->getLocalBounds().getCentre(), modifiers);
}

/// Clicks a component-local point with the left button through the peer, so hit testing chooses the receiver.
void GuiDriver::clickAt(
    juce::Component& target,
    const juce::Point<int> local,
    const juce::ModifierKeys modifiers,
    const int clicks
)
{
    const auto position = pointInPeer(target, local);
    requireReachable(snapshot(target), position);
    eventTime += SEPARATE_CLICK_GAP_MILLISECONDS;
    sendMove(position, modifiers);
    requireUnderMouse(target);
    for (int click = 0; click < clicks; ++click) {
        if (click > 0) {
            eventTime += DOUBLE_CLICK_GAP_MILLISECONDS;
        }
        sendButton(position, juce::ModifierKeys::leftButtonModifier, modifiers);
        sendButton(position, {}, modifiers);
    }
    settle();
    pauseForViewer(clicks > 1 ? "double click" : "click");
}

/// Sends two quick left clicks at the centre of a control.
void GuiDriver::doubleClick(const juce::String& controlId)
{
    const auto control = find(controlId);
    clickAt(*control.component, control.component->getLocalBounds().getCentre(), {}, 2);
}

/// Presses the left button at the centre of a control, moves by the distance in steps, and releases.
void GuiDriver::drag(const juce::String& controlId, const juce::Point<int> distance)
{
    const auto control = find(controlId);
    const auto start = pointInPeer(*control.component, control.component->getLocalBounds().getCentre());
    requireReachable(control, start);
    eventTime += SEPARATE_CLICK_GAP_MILLISECONDS;
    sendMove(start, {});
    requireUnderMouse(*control.component);
    sendButton(start, juce::ModifierKeys::leftButtonModifier, {});
    for (int step = 1; step <= DRAG_STEPS; ++step) {
        const auto fraction = static_cast<float>(step) / static_cast<float>(DRAG_STEPS);
        const auto position = start + distance.toFloat() * fraction;
        eventTime += EVENT_STEP_MILLISECONDS;
        juce::ModifierKeys::currentModifiers = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        peer().handleMouseEvent(
            juce::MouseInputSource::InputSourceType::mouse,
            position,
            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
            juce::MouseInputSource::defaultPressure,
            juce::MouseInputSource::defaultOrientation,
            eventTime
        );
    }
    sendButton(start + distance.toFloat(), {}, {});
    settle();
    pauseForViewer("drag");
}

/// Moves dragged files over the window and drops them, as the operating system's drag session would.
void GuiDriver::dropFiles(const juce::StringArray& files, const juce::Point<int> positionInPeer) const
{
    juce::ComponentPeer::DragInfo info;
    info.files = files;
    info.position = positionInPeer;
    peer().handleDragMove(info);
    peer().handleDragDrop(info);
    settle();
    pauseForViewer("file drop");
}

/// Sends a key press to the window's current keyboard focus, as the platform peer does for a key-down event.
void GuiDriver::press(const juce::KeyPress& key) const
{
    peer().handleKeyPress(key);
    settle();
    pauseForViewer("key press");
}

/// Focuses a control through a click and then sends a key press to it.
void GuiDriver::press(const juce::String& controlId, const juce::KeyPress& key)
{
    const auto control = find(controlId);
    if (!control.component->getWantsKeyboardFocus()) {
        fail(utils::format("Control {} does not take keyboard focus", controlId).toStdString());
    }
    click(controlId);
    if (!control.component->hasKeyboardFocus(true)) {
        control.component->grabKeyboardFocus();
    }
    press(key);
}

/// Sets a ranged control, such as a slider, through its native accessibility value interface.
void GuiDriver::setValue(const juce::String& controlId, const double value) const
{
    const auto control = find(controlId);
    if (!control.showing || !control.enabled) {
        fail(utils::format("Control {} cannot take a value", controlId).toStdString());
    }
    const auto* handler = control.component->getAccessibilityHandler();
    auto* valueInterface = handler == nullptr ? nullptr : handler->getValueInterface();
    if (valueInterface == nullptr || valueInterface->isReadOnly()) {
        fail(utils::format("Control {} has no writable value", controlId).toStdString());
    }
    valueInterface->setValue(value);
    settle();
    pauseForViewer("value change");
}

/// Opens a control's context menu with a right click at its centre.
void GuiDriver::rightClick(const juce::String& controlId)
{
    const auto control = find(controlId);
    rightClickAt(*control.component, control.component->getLocalBounds().getCentre());
}

/// Right-clicks a component-local point through the peer.
/// A context menu it opens is a separate window that macOS closes while the test process is in the background,
/// so tests assert the menu's contents through the view's menu builder instead of its window.
void GuiDriver::rightClickAt(juce::Component& target, const juce::Point<int> local)
{
    const auto position = pointInPeer(target, local);
    requireReachable(snapshot(target), position);
    eventTime += SEPARATE_CLICK_GAP_MILLISECONDS;
    sendMove(position, {});
    requireUnderMouse(target);
    sendButton(position, juce::ModifierKeys::rightButtonModifier, {});
    sendButton(position, {}, {});
    juce::PopupMenu::dismissAllActiveMenus();
    settle();
    pauseForViewer("right click");
}

/// Sends one mouse-wheel notch over the centre of a control through the peer, positive for up.
void GuiDriver::scroll(const juce::String& controlId, const float deltaY)
{
    const auto control = find(controlId);
    scrollAt(*control.component, control.component->getLocalBounds().getCentre(), {0.0f, deltaY});
}

/// Sends one wheel notch at a component-local point through the peer, with keyboard modifiers held.
/// The modifiers are released again afterwards, so they never reach a later action.
void GuiDriver::scrollAt(
    juce::Component& target,
    const juce::Point<int> local,
    const juce::Point<float> delta,
    const juce::ModifierKeys modifiers
)
{
    const auto position = pointInPeer(target, local);
    requireReachable(snapshot(target), position);
    sendMove(position, modifiers);
    requireUnderMouse(target);
    const juce::MouseWheelDetails wheel {
        .deltaX = delta.getX(),
        .deltaY = delta.getY(),
        .isReversed = false,
        .isSmooth = false,
        .isInertial = false,
    };
    eventTime += EVENT_STEP_MILLISECONDS;
    peer().handleMouseWheel(juce::MouseInputSource::InputSourceType::mouse, position, eventTime, wheel);
    juce::ModifierKeys::currentModifiers = {};
    settle();
    pauseForViewer("wheel notch");
}

/// Types characters into the current keyboard focus, one key press per character.
void GuiDriver::type(const juce::String& text) const
{
    for (const auto character : text) {
        peer().handleKeyPress(juce::KeyPress(static_cast<int>(character), {}, character));
    }
    settle();
    pauseForViewer("typing");
}

/// Reads one component's identity, geometry, and native accessibility state.
ControlSnapshot GuiDriver::snapshot(juce::Component& target) const
{
    ControlSnapshot control;
    control.component = &target;
    control.componentId = target.getComponentID();
    control.enabled = target.isEnabled();
    control.showing = target.isShowing();
    control.focused = target.hasKeyboardFocus(true);
    if (component.isParentOf(&target) || &target == &component) {
        control.boundsInPeer = component.getLocalArea(&target, target.getLocalBounds());
    } else {
        control.boundsInPeer = target.getScreenBounds();
    }
    if (const auto* handler = target.getAccessibilityHandler()) {
        control.role = handler->getRole();
        control.title = handler->getTitle();
        const auto state = handler->getCurrentState();
        control.checked = state.isChecked();
        if (const auto* value = handler->getValueInterface()) {
            control.value = value->getCurrentValueAsString();
        }
    }
    return control;
}

/// Converts a component-local point to the coordinates of the window peer.
juce::Point<float> GuiDriver::pointInPeer(const juce::Component& target, const juce::Point<int> local) const
{
    return component.getLocalPoint(&target, local).toFloat();
}

/// Keeps a visible window on screen for the configured pause while running the message loop,
/// so asynchronous updates are drawn and a developer can follow the action, and logs which action finished.
/// Does nothing for the default transparent window.
void GuiDriver::pauseForViewer(const juce::String& action) const
{
    if (!visible) {
        return;
    }
    fmt::print(stderr, "[gui] {}\n", action.toStdString());
    component.repaint();
    juce::ignoreUnused(dispatchUntil([] { return false; }, std::chrono::milliseconds(visibleStepDelayMilliseconds)));
}

/// Fails before sending input when the target is hidden, disabled, or covered by another component at the point,
/// so a test never passes because its event went somewhere else.
void GuiDriver::requireReachable(const ControlSnapshot& control, const juce::Point<float> position) const
{
    if (!control.showing) {
        fail(utils::format("Control {} is not showing", control.componentId).toStdString());
    }
    if (!control.enabled) {
        fail(utils::format("Control {} is disabled", control.componentId).toStdString());
    }
    const auto* hit = component.getComponentAt(position.roundToInt());
    if (hit != control.component && !control.component->isParentOf(hit)) {
        fail(
            utils::format(
                "Control {} is covered at {} by {}",
                control.componentId,
                position.roundToInt().toString(),
                hit == nullptr ? juce::String("nothing") : hit->getName()
            )
                .toStdString()
        );
    }
}

/// Fails when JUCE's mouse source did not route the last move to the target,
/// which happens when the platform does not deliver peer events to the window,
/// so a click that never reaches its control cannot pass silently.
void GuiDriver::requireUnderMouse(const juce::Component& target) const
{
    const auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    if (under != &target && !target.isParentOf(under)) {
        fail(
            utils::format(
                "The pointer did not reach {} in {} of window {}, it is over {}",
                target.getComponentID().isNotEmpty() ? target.getComponentID() : target.getName(),
                target.getScreenBounds().toString(),
                peer().getBounds().toString(),
                under == nullptr ? juce::String("nothing") : under->getName()
            )
                .toStdString()
        );
    }
}

/// Sends a button transition, which JUCE turns into a mouse down when buttons are set and a mouse up otherwise.
void GuiDriver::sendButton(
    const juce::Point<float> position,
    const juce::ModifierKeys buttons,
    const juce::ModifierKeys modifiers
)
{
    eventTime += EVENT_STEP_MILLISECONDS;
    const auto state = modifiers.withoutMouseButtons().withFlags(buttons.getRawFlags());
    // The platform peers update the shared modifier state before dispatching, and JUCE reads it for mouse-up events.
    juce::ModifierKeys::currentModifiers = state;
    peer().handleMouseEvent(
        juce::MouseInputSource::InputSourceType::mouse,
        position,
        state,
        juce::MouseInputSource::defaultPressure,
        juce::MouseInputSource::defaultOrientation,
        eventTime
    );
}

/// Moves the pointer without buttons, so hover and enter events reach the component under it first.
void GuiDriver::sendMove(const juce::Point<float> position, const juce::ModifierKeys modifiers)
{
    eventTime += EVENT_STEP_MILLISECONDS;
    juce::ModifierKeys::currentModifiers = modifiers.withoutMouseButtons();
    peer().handleMouseEvent(
        juce::MouseInputSource::InputSourceType::mouse,
        position,
        modifiers.withoutMouseButtons(),
        juce::MouseInputSource::defaultPressure,
        juce::MouseInputSource::defaultOrientation,
        eventTime
    );
}
}  // namespace audiobatch::tests
