/// Implements a self-contained, grouped keyboard and pointer reference with native accessible labels.

#include "ui/KeyboardShortcutsPanel.h"

#include "ui/ImageControlLookAndFeel.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace esgrove::image_control
{
namespace
{
/// One explicit action and gesture, independent of processor state and host shortcut registration.
struct Shortcut final {
    /// Non-null, non-owning JUCE text-boundary pointer to a static-lifetime identifier string literal.
    const char* identifier;
    /// Non-null, non-owning JUCE text-boundary pointer to a static-lifetime action string literal.
    const char* action;
    /// Non-null, non-owning JUCE text-boundary pointer to a static-lifetime gesture string literal.
    const char* keys;
};

/// One heading and its contiguous rows, used to keep reading and visual order identical.
struct ShortcutGroup final {
    /// Non-null, non-owning JUCE text-boundary pointer to a static-lifetime identifier string literal.
    const char* identifier;
    /// Non-null, non-owning JUCE text-boundary pointer to a static-lifetime heading string literal.
    const char* title;
    /// Number of contiguous shortcut rows belonging to this heading.
    std::size_t rows;
};

/// Native modifier names match the editor's local commands and the spectrogram's pointer gestures.
#if JUCE_MAC
constexpr const char* UNDO_KEYS {"Cmd+Z"};
constexpr const char* REDO_KEYS {"Cmd+Shift+Z / Cmd+Y"};
constexpr const char* BYPASS_KEYS {"Cmd+B"};
constexpr const char* DELTA_KEYS {"Cmd+D"};
constexpr const char* SETTINGS_KEYS {"Cmd+,"};
constexpr const char* MID_KEYS {"Cmd+drag / Cmd+wheel"};
constexpr const char* SIDE_KEYS {"Option+drag / wheel"};
#else
constexpr const char* UNDO_KEYS {"Ctrl+Z"};
constexpr const char* REDO_KEYS {"Ctrl+Shift+Z / Ctrl+Y"};
constexpr const char* BYPASS_KEYS {"Ctrl+B"};
constexpr const char* DELTA_KEYS {"Ctrl+D"};
constexpr const char* SETTINGS_KEYS {"Ctrl+,"};
constexpr const char* MID_KEYS {"Ctrl+drag / Ctrl+wheel"};
constexpr const char* SIDE_KEYS {"Alt+drag / wheel"};
#endif

constexpr std::array GROUPS {
    ShortcutGroup {.identifier = "editing", .title = "Editing", .rows = 5},
    ShortcutGroup {.identifier = "displaysNavigation", .title = "Displays / navigation", .rows = 5},
    ShortcutGroup {.identifier = "values", .title = "Values", .rows = 4},
    ShortcutGroup {.identifier = "spectrogram", .title = "Spectrogram", .rows = 4},
};

constexpr std::array SHORTCUTS {
    Shortcut {.identifier = "undo", .action = "Undo processing edit", .keys = UNDO_KEYS},
    Shortcut {.identifier = "redo", .action = "Redo processing edit", .keys = REDO_KEYS},
    Shortcut {.identifier = "audioBypass", .action = "Toggle audio Bypass", .keys = BYPASS_KEYS},
    Shortcut {.identifier = "delta", .action = "Toggle Delta audition", .keys = DELTA_KEYS},
    Shortcut {.identifier = "solo", .action = "Solo band under pointer", .keys = "S"},
    Shortcut {.identifier = "focus", .action = "Next / previous control", .keys = "Tab / Shift+Tab"},
    Shortcut {.identifier = "settings", .action = "Display settings", .keys = SETTINGS_KEYS},
    Shortcut {.identifier = "help", .action = "Keyboard shortcuts", .keys = "F1"},
    Shortcut {.identifier = "close", .action = "Back / close Settings", .keys = "Escape"},
    Shortcut {.identifier = "divider", .action = "Resize focused divider", .keys = "Up / Down (Shift: fine)"},
    Shortcut {.identifier = "adjust", .action = "Adjust focused value", .keys = "Arrows (Shift: fine)"},
    Shortcut {.identifier = "enter", .action = "Type a value", .keys = "Enter"},
    Shortcut {.identifier = "fine", .action = "Fine slider drag / wheel", .keys = "Shift+drag / Shift+wheel"},
    Shortcut {.identifier = "reset", .action = "Reset slider or crossover", .keys = "Double-click"},
    Shortcut {
        .identifier = "move",
        .action = "Hovered crossover / band (1-1000 Hz)",
        .keys = "Drag / arrows: 10% (Shift: 1%)",
    },
    Shortcut {.identifier = "mid", .action = "Change band Mid gain", .keys = MID_KEYS},
    Shortcut {.identifier = "side", .action = "Change band Side gain", .keys = SIDE_KEYS},
    Shortcut {.identifier = "fineGain", .action = "Fine band gain gesture", .keys = "Shift+drag / Shift+wheel"},
};

/// A compact reference fits its ordinary window and scrolls only when the host constrains its height.
constexpr int PANEL_WIDTH {520};
constexpr int PANEL_HEIGHT {500};
constexpr int MARGIN {12};
constexpr int TITLE_HEIGHT {24};
constexpr int DESCRIPTION_HEIGHT {20};
constexpr int GROUP_HEIGHT {24};
constexpr int ROW_HEIGHT {18};
constexpr int COLUMN_GAP {12};
constexpr int CONTENT_HEIGHT {
    MARGIN * 2 + TITLE_HEIGHT + DESCRIPTION_HEIGHT + static_cast<int>(GROUPS.size()) * GROUP_HEIGHT
        + static_cast<int>(SHORTCUTS.size()) * ROW_HEIGHT,
};
constexpr float TITLE_FONT_HEIGHT {15.0F};
constexpr float LABEL_FONT_HEIGHT {12.0F};

/// Creates native read-only text with meaningful accessible content and stable inspection identities.
void configureLabel(
    juce::Label& label,
    const juce::String& identifier,
    const juce::String& text,
    const juce::String& accessibleTitle,
    const bool heading = false
)
{
    label.setComponentID(identifier);
    label.setName(accessibleTitle);
    label.setTitle(accessibleTitle);
    label.setTooltip(accessibleTitle);
    label.setText(text, juce::dontSendNotification);
    label.setFont(heading ? textFont(LABEL_FONT_HEIGHT).boldened() : textFont(LABEL_FONT_HEIGHT));
    label.setColour(
        juce::Label::textColourId, juce::Colour(heading ? palette::GREY_MIDDLE_LIGHT : palette::GREY_LIGHT)
    );
    label.setBorderSize({});
    label.setJustificationType(juce::Justification::centredLeft);
    label.setEditable(false, false, false);
}
}  // namespace

/// Builds explicit reference rows on the message thread without binding or changing any plugin state.
KeyboardShortcutsPanel::KeyboardShortcutsPanel()
{
    setComponentID("keyboardShortcuts");
    setName("Keyboard shortcuts");
    setTitle("Keyboard shortcuts");
    content.setComponentID("keyboardShortcutsContent");
    viewport.setComponentID("keyboardShortcutsViewport");
    viewport.setName("Keyboard shortcuts reference");
    viewport.setTitle("Keyboard shortcuts reference");
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    addAndMakeVisible(viewport);

    configureLabel(titleLabel, "keyboardShortcutsTitle", "Keyboard shortcuts", "Keyboard shortcuts");
    titleLabel.setFont(textFont(TITLE_FONT_HEIGHT).boldened());
    configureLabel(
        descriptionLabel,
        "keyboardShortcutsDescription",
        "Editor controls while focused. Escape returns to Settings.",
        "Editor controls while focused. Escape returns to Settings."
    );
    descriptionLabel.setColour(juce::Label::textColourId, juce::Colour(palette::GREY_MIDDLE_LIGHT));
    content.addAndMakeVisible(titleLabel);
    content.addAndMakeVisible(descriptionLabel);

    static_assert(GROUP_COUNT == GROUPS.size());
    static_assert(SHORTCUT_COUNT == SHORTCUTS.size());
    std::size_t shortcutIndex = 0;
    for (std::size_t groupIndex = 0; groupIndex < GROUPS.size(); ++groupIndex) {
        const auto& group = GROUPS.at(groupIndex);
        auto& label = groupLabels.at(groupIndex);
        configureLabel(label, juce::String("shortcutGroup.") + group.identifier, group.title, group.title, true);
        content.addAndMakeVisible(label);
        for (std::size_t rowIndex = 0; rowIndex < group.rows; ++rowIndex) {
            const auto& shortcut = SHORTCUTS.at(shortcutIndex);
            const juce::String identifier = juce::String("shortcut.") + shortcut.identifier;
            const juce::String accessibleTitle = juce::String(shortcut.action) + ": " + shortcut.keys;
            auto& action = actionLabels.at(shortcutIndex);
            auto& keys = keyLabels.at(shortcutIndex);
            configureLabel(action, identifier + ".action", shortcut.action, accessibleTitle);
            configureLabel(keys, identifier + ".keys", shortcut.keys, accessibleTitle);
            keys.setJustificationType(juce::Justification::centredRight);
            content.addAndMakeVisible(action);
            content.addAndMakeVisible(keys);
            ++shortcutIndex;
        }
    }
    setSize(PANEL_WIDTH, PANEL_HEIGHT);
}

/// Uses the shared display surface rather than drawing a separate text-editor background.
void KeyboardShortcutsPanel::paint(juce::Graphics& graphics)
{
    graphics.fillAll(juce::Colour(palette::GREY_SEMI_DARK));
}

/// Aligns action and gesture columns, retaining all rows when a smaller window requires scrolling.
void KeyboardShortcutsPanel::resized()
{
    viewport.setBounds(getLocalBounds());
    constexpr int stackedWidthThreshold {460};
    const bool stacked = getWidth() < stackedWidthThreshold;
    const int rowHeight = stacked ? ROW_HEIGHT * 2 : ROW_HEIGHT;
    const int contentHeight = CONTENT_HEIGHT + (stacked ? static_cast<int>(SHORTCUTS.size()) * ROW_HEIGHT : 0);
    const int scrollBarWidth = getHeight() < contentHeight ? viewport.getScrollBarThickness() : 0;
    content.setSize(std::max(1, getWidth() - scrollBarWidth), contentHeight);
    auto area = content.getLocalBounds().reduced(MARGIN);
    titleLabel.setBounds(area.removeFromTop(TITLE_HEIGHT));
    descriptionLabel.setBounds(area.removeFromTop(DESCRIPTION_HEIGHT));
    std::size_t shortcutIndex = 0;
    for (std::size_t groupIndex = 0; groupIndex < GROUPS.size(); ++groupIndex) {
        groupLabels.at(groupIndex).setBounds(area.removeFromTop(GROUP_HEIGHT));
        for (std::size_t rowIndex = 0; rowIndex < GROUPS.at(groupIndex).rows; ++rowIndex) {
            auto row = area.removeFromTop(rowHeight);
            if (stacked) {
                actionLabels.at(shortcutIndex).setBounds(row.removeFromTop(ROW_HEIGHT));
                keyLabels.at(shortcutIndex).setJustificationType(juce::Justification::centredLeft);
            } else {
                actionLabels.at(shortcutIndex).setBounds(row.removeFromLeft((row.getWidth() - COLUMN_GAP) / 2));
                row.removeFromLeft(COLUMN_GAP);
                keyLabels.at(shortcutIndex).setJustificationType(juce::Justification::centredRight);
            }
            keyLabels.at(shortcutIndex).setBounds(row);
            ++shortcutIndex;
        }
    }
}
}  // namespace esgrove::image_control
