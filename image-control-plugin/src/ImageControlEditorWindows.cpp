/// Implements the ImageControl editor's owned settings callout and modeless keyboard shortcut reference window.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
#include "ui/ImageControlLookAndFeel.h"
#include "ui/KeyboardShortcutsPanel.h"
#include "ui/SettingsPanel.h"

#include <algorithm>
#include <memory>

namespace esgrove::image_control
{
namespace
{
/// Parent-contained callouts reserve their arrow and look-and-feel border on both edges.
constexpr int SETTINGS_MAX_HEIGHT {640};
constexpr int SETTINGS_BORDER_ALLOWANCE {48};

/// Keeps the opening Settings button available as a release-driven close action while the callout is modal.
class SettingsCallout final : public juce::CallOutBox
{
public:
    /// Retains only safe component observers, while the editor owns both this callout and its content.
    SettingsCallout(juce::Component& content, juce::Component& target, juce::Component& editor) :
        CallOutBox(content, target.getBounds(), &editor),
        button(&target)
    {
        setComponentID("display-settings-callout");
    }

    /// Lets only the opener complete its ordinary release-driven toggle while all other controls remain modal-blocked.
    bool canModalEventBeSentToComponent(const juce::Component* target) override
    {
        return button != nullptr && (target == button.getComponent() || button->isParentOf(target));
    }

private:
    /// Editor-owned opener, which can disappear during plugin teardown.
    juce::Component::SafePointer<juce::Component> button;
};

/// A modeless native reference window, hidden on close and destroyed with the plugin editor.
class ShortcutsWindow final : public juce::DocumentWindow
{
public:
    /// Owns only reference content, without retaining processor state or starting another modal workflow.
    explicit ShortcutsWindow(juce::Component& editor) :
        DocumentWindow("Keyboard shortcuts", juce::Colour(palette::GREY_SEMI_DARK), juce::DocumentWindow::closeButton)
    {
        setComponentID("keyboard-shortcuts-window");
        setTitle("ImageControl keyboard shortcuts");
        setLookAndFeel(&editor.getLookAndFeel());
        setUsingNativeTitleBar(true);
        setContentOwned(new KeyboardShortcutsPanel(), true);
        setResizable(true, false);
        constexpr int minimumWidth {480};
        constexpr int minimumHeight {360};
        constexpr int maximumWidth {900};
        constexpr int maximumHeight {1000};
        setResizeLimits(minimumWidth, minimumHeight, maximumWidth, maximumHeight);
        centreAroundComponent(&editor, getWidth(), getHeight());
    }

    /// Detaches the editor-owned palette while the editor and its look and feel are still alive.
    ~ShortcutsWindow() override
    {
        setLookAndFeel(nullptr);
    }

    ShortcutsWindow(const ShortcutsWindow&) = delete;
    ShortcutsWindow(ShortcutsWindow&&) = delete;
    ShortcutsWindow& operator=(const ShortcutsWindow&) = delete;
    ShortcutsWindow& operator=(ShortcutsWindow&&) = delete;

    /// Closing the reference never changes sound or the display preferences.
    void closeButtonPressed() override
    {
        setVisible(false);
    }

    /// Escape closes this modeless reference without affecting editor shortcuts.
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::escapeKey) {
            closeButtonPressed();
            return true;
        }
        return juce::DocumentWindow::keyPressed(key);
    }
};
}  // namespace

/// Returns the settings height bounded by the editor's available space and the callout border.
int ImageControlEditor::settingsPanelHeight() const
{
    return std::min(SETTINGS_MAX_HEIGHT, getHeight() - SETTINGS_BORDER_ALLOWANCE);
}

/// Opens one modeless, editor-owned shortcut reference without leaving Settings stacked underneath it.
void ImageControlEditor::openKeyboardShortcuts()
{
    if (settingsCallout != nullptr) {
        settingsCallout->dismiss();
    }
    if (shortcutsWindow == nullptr) {
        shortcutsWindow = std::make_unique<ShortcutsWindow>(*this);
    }
    shortcutsWindow->setVisible(true);
    shortcutsWindow->toFront(true);
}

/// Opens an editor-owned bounded callout, with synchronous teardown before the processor's settings disappear.
/// Dismissal restores Settings focus only when an outside click has not already focused another control.
void ImageControlEditor::openSettings()
{
    if (settingsCallout != nullptr) {
        settingsCallout->dismiss();
        return;
    }
    settingsPanel = std::make_unique<SettingsPanel>(imageControlProcessor.getEditorState(), settingsPanelHeight());
    const juce::Component::SafePointer<ImageControlEditor> safeThis(this);
    settingsPanel->setKeyboardShortcutsCallback([safeThis] {
        if (safeThis != nullptr) {
            safeThis->openKeyboardShortcuts();
        }
    });
    settingsCallout = std::make_unique<SettingsCallout>(*settingsPanel, settingsButton, *this);
    const juce::Component::SafePointer<juce::CallOutBox> safeCallout(settingsCallout.get());
    settingsCallout->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, safeCallout](int) {
                                         if (safeThis == nullptr || safeCallout == nullptr
                                             || safeThis->settingsCallout.get() != safeCallout.getComponent())
                                         {
                                             return;
                                         }
                                         const auto* focused = juce::Component::getCurrentlyFocusedComponent();
                                         const bool restoreFocus = focused == nullptr
                                             || focused == safeCallout.getComponent()
                                             || safeCallout->isParentOf(focused) || focused == safeThis.getComponent();
                                         safeThis->settingsCallout.reset();
                                         safeThis->settingsPanel.reset();
                                         if (restoreFocus && safeThis->isShowing()
                                             && (safeThis->shortcutsWindow == nullptr
                                                 || !safeThis->shortcutsWindow->isVisible()))
                                         {
                                             safeThis->settingsButton.grabKeyboardFocus();
                                         }
                                     }));
}

/// Keeps the editor-owned settings content inside the current window after display layout changes.
void ImageControlEditor::updateSettingsBounds()
{
    if (settingsCallout != nullptr && settingsPanel != nullptr) {
        settingsPanel->setSize(settingsPanel->getWidth(), settingsPanelHeight());
        settingsCallout->updatePosition(settingsButton.getBounds(), getLocalBounds());
    }
}
}  // namespace esgrove::image_control
