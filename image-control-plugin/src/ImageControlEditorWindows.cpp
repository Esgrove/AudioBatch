/// Implements the ImageControl editor's owned Settings callout and nested keyboard shortcuts navigation.

#include "ImageControlEditor.h"
#include "ImageControlProcessor.h"
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
}  // namespace

/// Returns the settings height bounded by the editor's available space and the callout border.
int ImageControlEditor::settingsPanelHeight() const
{
    return std::min(SETTINGS_MAX_HEIGHT, getHeight() - SETTINGS_BORDER_ALLOWANCE);
}

/// F1 enters the shortcuts submenu in Settings without creating another window or popup layer.
void ImageControlEditor::openKeyboardShortcuts()
{
    if (settingsCallout == nullptr) {
        openSettings();
    }
    if (settingsPanel != nullptr) {
        settingsPanel->showKeyboardShortcuts();
    }
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
                                         if (restoreFocus && safeThis->isShowing()) {
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
