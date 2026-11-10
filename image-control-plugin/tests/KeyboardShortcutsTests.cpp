/// Tests the nested shortcuts reference through native labels, stable identities, and responsive layout contracts.

#include "ui/KeyboardShortcutsPanel.h"

#include <gtest/gtest.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <utility>
#include <vector>

namespace esgrove::image_control::test
{
namespace
{
/// Finds a native reference label by identity without relying on JUCE's viewport implementation children.
/// The nullable result observes a component owned by the panel and valid for the panel's lifetime.
juce::Label* shortcutLabel(const juce::Component& parent, const juce::String& identifier)
{
    std::vector<juce::Component*> pending;
    for (auto* child : parent.getChildren()) {
        pending.push_back(child);
    }
    while (!pending.empty()) {
        auto* child = pending.back();
        pending.pop_back();
        if (child->getComponentID() == identifier) {
            return dynamic_cast<juce::Label*>(child);
        }
        for (auto* descendant : child->getChildren()) {
            pending.push_back(descendant);
        }
    }
    return nullptr;
}

/// Returns the panel-owned reference viewport without relying on its order among root children.
/// The nullable observer remains valid while the panel owns it.
juce::Viewport* shortcutViewport(const KeyboardShortcutsPanel& panel)
{
    for (auto* child : panel.getChildren()) {
        if (auto* viewport = dynamic_cast<juce::Viewport*>(child)) {
            return viewport;
        }
    }
    return nullptr;
}
}  // namespace

/// All categories use read-only native labels, not a selectable editor containing a text wall.
TEST(KeyboardShortcutsPanel, GroupsHaveMeaningfulNativeLabelsAndStableIdentities)
{
    KeyboardShortcutsPanel panel;
    panel.addToDesktop(0);
    EXPECT_EQ(panel.getComponentID(), "keyboardShortcuts");
    EXPECT_EQ(panel.getTitle(), "Keyboard shortcuts");
    const std::array groups {
        std::pair {"editing", "Editing"},
        std::pair {"displaysNavigation", "Displays / navigation"},
        std::pair {"values", "Values"},
        std::pair {"spectrogram", "Spectrogram"},
    };
    int previousBottom = 0;
    for (const auto& [identifier, title] : groups) {
        auto* heading = shortcutLabel(panel, juce::String("shortcutGroup.") + identifier);
        ASSERT_NE(heading, nullptr);
        EXPECT_EQ(heading->getText(), title);
        EXPECT_EQ(heading->getTitle(), title);
        EXPECT_GE(heading->getY(), previousBottom);
        previousBottom = heading->getBottom();
        auto* accessibility = heading->getAccessibilityHandler();
        ASSERT_NE(accessibility, nullptr);
        EXPECT_EQ(accessibility->getTitle(), heading->getTitle());
    }
    auto* description = shortcutLabel(panel, "keyboardShortcutsDescription");
    ASSERT_NE(description, nullptr);
    EXPECT_EQ(description->getText(), "Editor controls while focused. Escape returns to Settings.");
    auto* viewport = shortcutViewport(panel);
    ASSERT_NE(viewport, nullptr);
    auto* content = viewport->getViewedComponent();
    ASSERT_NE(content, nullptr);
    for (const auto* child : content->getChildren()) {
        EXPECT_NE(dynamic_cast<const juce::Label*>(child), nullptr);
        EXPECT_EQ(dynamic_cast<const juce::TextEditor*>(child), nullptr);
    }
}

/// Action and gesture labels retain native label titles and full contextual help while remaining read-only.
TEST(KeyboardShortcutsPanel, RowsNameActualEditorActionsInTwoColumns)
{
    KeyboardShortcutsPanel panel;
    panel.addToDesktop(0);
    const std::array identifiers {
        "undo",
        "redo",
        "audioBypass",
        "delta",
        "solo",
        "focus",
        "settings",
        "help",
        "close",
        "divider",
        "adjust",
        "enter",
        "fine",
        "reset",
        "move",
        "mid",
        "side",
        "fineGain",
    };
    for (const auto* identifier : identifiers) {
        SCOPED_TRACE(identifier);
        const juce::String prefix = juce::String("shortcut.") + identifier;
        auto* action = shortcutLabel(panel, prefix + ".action");
        auto* keys = shortcutLabel(panel, prefix + ".keys");
        ASSERT_NE(action, nullptr);
        ASSERT_NE(keys, nullptr);
        EXPECT_FALSE(action->isEditable());
        EXPECT_FALSE(keys->isEditable());
        EXPECT_TRUE(action->getText().isNotEmpty());
        EXPECT_TRUE(keys->getText().isNotEmpty());
        EXPECT_EQ(action->getTitle(), action->getText() + ": " + keys->getText());
        EXPECT_EQ(keys->getTitle(), action->getTitle());
        EXPECT_EQ(action->getY(), keys->getY());
        EXPECT_EQ(action->getHeight(), keys->getHeight());
        EXPECT_LT(action->getRight(), keys->getX());
        auto* accessibility = keys->getAccessibilityHandler();
        ASSERT_NE(accessibility, nullptr);
        EXPECT_EQ(accessibility->getTitle(), keys->getText());
        EXPECT_EQ(accessibility->getHelp(), action->getTitle());
    }
    EXPECT_EQ(shortcutLabel(panel, "shortcut.audioBypass.action")->getText(), "Toggle audio Bypass");
    EXPECT_EQ(shortcutLabel(panel, "shortcut.solo.keys")->getText(), "S");
    EXPECT_EQ(shortcutLabel(panel, "shortcut.help.keys")->getText(), "F1");
    EXPECT_EQ(shortcutLabel(panel, "shortcut.close.action")->getText(), "Back / close Settings");
    EXPECT_EQ(shortcutLabel(panel, "shortcut.close.keys")->getText(), "Escape");
    EXPECT_EQ(shortcutLabel(panel, "shortcut.enter.keys")->getText(), "Enter");
    EXPECT_EQ(shortcutLabel(panel, "shortcut.move.keys")->getText(), "Drag sideways");
}

/// Modifier names match the target platform instead of presenting mixed-platform Command/Ctrl strings.
TEST(KeyboardShortcutsPanel, UsesNativePlatformModifierNames)
{
    const KeyboardShortcutsPanel panel;
#if JUCE_MAC
    const juce::String command = "Cmd";
    const juce::String alternate = "Option";
    const juce::String foreignCommand = "Ctrl";
    const juce::String foreignAlternate = "Alt";
#else
    const juce::String command = "Ctrl";
    const juce::String alternate = "Alt";
    const juce::String foreignCommand = "Cmd";
    const juce::String foreignAlternate = "Option";
#endif
    const std::array commands {
        std::pair {"undo", "+Z"},
        std::pair {"redo", "+Shift+Z / "},
        std::pair {"audioBypass", "+B"},
        std::pair {"delta", "+D"},
        std::pair {"settings", "+,"},
        std::pair {"mid", "+drag / "},
    };
    for (const auto& [identifier, suffix] : commands) {
        auto* keys = shortcutLabel(panel, juce::String("shortcut.") + identifier + ".keys");
        ASSERT_NE(keys, nullptr);
        EXPECT_TRUE(keys->getText().startsWith(command + suffix));
        EXPECT_FALSE(keys->getText().contains(foreignCommand));
    }
    auto* side = shortcutLabel(panel, "shortcut.side.keys");
    ASSERT_NE(side, nullptr);
    EXPECT_EQ(side->getText(), alternate + "+drag / wheel");
    EXPECT_FALSE(side->getText().contains(foreignAlternate));
}

/// The natural panel fits every reference row, and a bounded submenu scrolls rather than clips.
TEST(KeyboardShortcutsPanel, NaturalPanelFitsAndSmallerHeightRetainsCompleteReference)
{
    KeyboardShortcutsPanel panel;
    EXPECT_EQ(panel.getWidth(), 520);
    EXPECT_EQ(panel.getHeight(), 500);
    auto* viewport = shortcutViewport(panel);
    ASSERT_NE(viewport, nullptr);
    auto* content = viewport->getViewedComponent();
    ASSERT_NE(content, nullptr);
    EXPECT_LE(content->getHeight(), viewport->getViewHeight());
    const int naturalContentHeight = content->getHeight();
    panel.setSize(520, 240);
    EXPECT_EQ(content->getHeight(), naturalContentHeight);
    EXPECT_GT(content->getHeight(), viewport->getViewHeight());
    EXPECT_LE(content->getWidth(), viewport->getViewWidth());
    for (const auto* child : content->getChildren()) {
        SCOPED_TRACE(child->getComponentID().toStdString());
        EXPECT_TRUE(content->getLocalBounds().contains(child->getBounds()));
        EXPECT_GT(child->getHeight(), 0);
    }
    auto* lastKeys = shortcutLabel(panel, "shortcut.fineGain.keys");
    ASSERT_NE(lastKeys, nullptr);
    viewport->setViewPosition(0, content->getHeight());
    EXPECT_GT(viewport->getViewPositionY(), 0);
    const auto visibleBounds = viewport->getLocalArea(lastKeys, lastKeys->getLocalBounds());
    EXPECT_GE(visibleBounds.getY(), 0);
    EXPECT_LE(visibleBounds.getBottom(), viewport->getViewHeight());
}

/// Narrow Settings content stacks each action above its gesture and reserves both lines in the scroll height.
TEST(KeyboardShortcutsPanel, NarrowSubmenuStacksCompleteRowsBelowResponsiveThreshold)
{
    KeyboardShortcutsPanel panel;
    auto* viewport = shortcutViewport(panel);
    ASSERT_NE(viewport, nullptr);
    auto* content = viewport->getViewedComponent();
    ASSERT_NE(content, nullptr);
    const int naturalContentHeight = content->getHeight();
    const std::array identifiers {
        "undo",
        "redo",
        "audioBypass",
        "delta",
        "solo",
        "focus",
        "settings",
        "help",
        "close",
        "divider",
        "adjust",
        "enter",
        "fine",
        "reset",
        "move",
        "mid",
        "side",
        "fineGain",
    };
    for (const int width : {390, 459, 460, 520}) {
        SCOPED_TRACE(width);
        panel.setSize(width, 240);
        const bool stacked = width < 460;
        EXPECT_EQ(
            content->getHeight(), naturalContentHeight + (stacked ? static_cast<int>(identifiers.size()) * 18 : 0)
        );
        EXPECT_LE(content->getWidth(), viewport->getViewWidth());
        for (const auto* identifier : identifiers) {
            SCOPED_TRACE(identifier);
            const auto prefix = juce::String("shortcut.") + identifier;
            auto* action = shortcutLabel(panel, prefix + ".action");
            auto* keys = shortcutLabel(panel, prefix + ".keys");
            ASSERT_NE(action, nullptr);
            ASSERT_NE(keys, nullptr);
            EXPECT_TRUE(content->getLocalBounds().contains(action->getBounds()));
            EXPECT_TRUE(content->getLocalBounds().contains(keys->getBounds()));
            EXPECT_EQ(action->getHeight(), 18);
            EXPECT_EQ(keys->getHeight(), 18);
            EXPECT_FALSE(action->getBounds().intersects(keys->getBounds()));
            if (stacked) {
                EXPECT_EQ(action->getBottom(), keys->getY());
                EXPECT_EQ(action->getX(), keys->getX());
                EXPECT_EQ(action->getWidth(), keys->getWidth());
                EXPECT_EQ(keys->getJustificationType(), juce::Justification::centredLeft);
            } else {
                EXPECT_EQ(action->getY(), keys->getY());
                EXPECT_LT(action->getRight(), keys->getX());
                EXPECT_EQ(keys->getJustificationType(), juce::Justification::centredRight);
            }
        }
        auto* lastKeys = shortcutLabel(panel, "shortcut.fineGain.keys");
        ASSERT_NE(lastKeys, nullptr);
        viewport->setViewPosition(0, content->getHeight());
        EXPECT_GT(viewport->getViewPositionY(), 0);
        const auto visibleBounds = viewport->getLocalArea(lastKeys, lastKeys->getLocalBounds());
        EXPECT_GE(visibleBounds.getY(), 0);
        EXPECT_LE(visibleBounds.getBottom(), viewport->getViewHeight());
    }
}
}  // namespace esgrove::image_control::test
