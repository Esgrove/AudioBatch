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
/// Keeps the opening Settings button available as a release-driven close action while the callout is modal.
class SettingsCallout final : public juce::CallOutBox
{
public:
    /// Retains only safe component observers, while the editor owns both this callout and its content.
    SettingsCallout(juce::Component& content, juce::Component& target, juce::Component& editor) :
        CallOutBox(content, target, &editor),
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

/// Fits natural settings content below its actual anchor, reserving the native callout's border on both edges.
int ImageControlEditor::settingsPanelHeight() const
{
    const auto anchor = getLocalArea(&settingsButton, settingsButton.getLocalBounds());
    const int availableHeight
        = getLocalBounds().getBottom() - anchor.getBottom() - settingsCallout->getBorderSize() * 2;
    return std::clamp(availableHeight, 1, settingsPanel->getNaturalHeight());
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
    settingsPanel = std::make_unique<SettingsPanel>(imageControlProcessor.getEditorState());
    const juce::Component::SafePointer<ImageControlEditor> safeThis(this);
    settingsCallout = std::make_unique<SettingsCallout>(*settingsPanel, settingsButton, *this);
    updateSettingsBounds();
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
        const auto anchor = getLocalArea(&settingsButton, settingsButton.getLocalBounds());
        settingsCallout->updatePosition(anchor, getLocalBounds().withTrimmedTop(anchor.getBottom()));
    }
}
}  // namespace esgrove::image_control
