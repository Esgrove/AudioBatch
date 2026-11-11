/// Shares component lookup and screen-space pointer events between control and editor layout tests.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace esgrove::image_control::test
{
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

/// Creates a handler event at a fixed screen position even when layout has moved the component since mouse-down.
inline juce::MouseEvent mouseAtScreen(
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
}  // namespace esgrove::image_control::test
