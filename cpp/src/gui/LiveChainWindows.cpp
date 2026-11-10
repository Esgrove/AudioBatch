/// The menu and the windows of LiveChain.
/// Covers the chain popup menu, the per-slot plugin editor windows with the generic editor fallback,
/// the chain editor window, and the plugin manager window that lists and scans plugins.
/// The windows hide themselves when closed, so reopening one only makes it visible again.

#include "gui/components/CustomLookAndFeel.h"
#include "gui/components/PluginChainEditor.h"
#include "gui/GuiLayout.h"
#include "gui/LiveChain.h"
#include "gui/LiveSlot.h"
#include "utils/CallbackAccess.h"
#include "utils/GuiUtils.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <cstddef>
#include <memory>

namespace audiobatch
{
namespace
{
constexpr int EDIT_CHAIN_MENU_ITEM_ID {1};
constexpr int CLEAR_CHAIN_MENU_ITEM_ID {2};
constexpr int SCAN_MENU_ITEM_ID {3};
/// First id of the presets submenu in the chain popup menu, which uses the ids from it upwards.
/// The Add Plugin submenu ids of juce::KnownPluginList start far above any preset count.
constexpr int PRESETS_MENU_FIRST_ITEM_ID {10};
}  // namespace

/// Pops up the plugin menu anchored to the given component.
/// The menu includes the chain summary, Edit Chain, the Presets and Add Plugin submenus, Clear Chain, and a Scan entry.
/// Uses desktop space so plugin selection submenus can extend beyond the anchor window.
void LiveChain::showMenu(juce::Component& anchor)
{
    juce::PopupMenu menu;

    const auto summary = getChainSummary();
    menu.addSectionHeader(
        summary.isEmpty() ? juce::String("No plugins in chain") : utils::format("Chain: {}", summary)
    );

    menu.addItem(EDIT_CHAIN_MENU_ITEM_ID, "Edit Chain...");
    menu.addSubMenu("Presets", createPresetsMenu(PRESETS_MENU_FIRST_ITEM_ID));
    menu.addSeparator();

    juce::PopupMenu addSubmenu;
    library.reload();
    const auto types = library.getKnownEffectTypes();
    juce::KnownPluginList::addToMenu(addSubmenu, types, juce::KnownPluginList::sortByManufacturer);
    menu.addSubMenu("Add Plugin", addSubmenu, !types.isEmpty());

    menu.addItem(CLEAR_CHAIN_MENU_ITEM_ID, "Clear Chain", !chain.empty());
    menu.addItem(SCAN_MENU_ITEM_ID, "Scan for Plugins...");

    const juce::WeakReference safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&anchor), [safeThis, types](const int result) {
        if (safeThis == nullptr || result == utils::MENU_RESULT_DISMISSED) {
            return;
        }

        switch (result) {
            case EDIT_CHAIN_MENU_ITEM_ID:
                safeThis->showChainEditor();
                return;
            case CLEAR_CHAIN_MENU_ITEM_ID:
                safeThis->clearChain();
                return;
            case SCAN_MENU_ITEM_ID:
                safeThis->showScanWindow();
                return;
            default:
                break;
        }

        const auto index = juce::KnownPluginList::getIndexChosenByMenu(types, result);
        if (index < 0 && safeThis->handlePresetsMenuItem(result, PRESETS_MENU_FIRST_ITEM_ID)) {
            return;
        }
        if (index < 0 || index >= types.size()) {
            return;
        }

        safeThis->addPlugin(types.getReference(index));
    });
}

/// Opens the editor window for the slot.
/// Multiple slot editors can be open at the same time.
/// Uses the same format-specific state restoration as processing, then creates the editor.
/// Closing the window retains the instance so parameter changes affect the same live processor.
void LiveChain::openEditorForSlot(const PluginSlotId slotId)
{
    const auto index = getSlotIndex(slotId);
    if (!index.has_value()) {
        return;
    }

    auto& entry = chain[static_cast<std::size_t>(*index)];

    if (entry.description.fileOrIdentifier.isEmpty()) {
        return;
    }

    if (entry.editorWindow != nullptr && entry.editorWindow->isVisible()) {
        entry.editorWindow->toFront(true);
        return;
    }

    // A window the user closed is only hidden, not deleted, and the next display update may not have reaped it.
    // Capture its state and destroy it now so a fresh editor can open below.
    // Neither that nor creating an unattached processor touches live preview, so only preparation excludes callbacks.
    const bool stateCaptured = entry.wasEditorOpen && reapClosedEditor(entry);
    const juce::WeakReference safeThis(this);
    bool prepared = instantiateLiveEntry(slotId, previewSampleRate, previewBlockSize);
    if (safeThis == nullptr) {
        return;
    }
    const auto current = getSlotIndex(slotId);
    if (!current) {
        return;
    }
    auto& currentEntry = chain[static_cast<std::size_t>(*current)];
    if (prepared) {
        const utils::CallbackControlGuard control(renderingAccess);
        prepared = prepareLiveEntry(currentEntry);
    }

    if (stateCaptured) {
        persistChain();
        sendChangeMessage();
    }

    if (!prepared) {
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Plugin Error",
                utils::format("Could not prepare plugin:\n\n{}", currentEntry.preparationError),
                "OK"
            ),
            nullptr
        );
        return;
    }

    // Creating the editor and its window can take a long time for a large plugin interface,
    // so it runs without the callback exclusion. The processor is only destroyed on the message thread.
    std::unique_ptr<juce::AudioProcessorEditor> editor(currentEntry.instance->createEditorAndMakeActive());

    if (editor == nullptr) {
        // No custom editor. Fall back to the generic parameter editor.
        editor = std::make_unique<juce::GenericAudioProcessorEditor>(*currentEntry.instance);
        editor->setSize(layout::pluginEditor::GENERIC_EDITOR_WIDTH, layout::pluginEditor::GENERIC_EDITOR_HEIGHT);
    }

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = currentEntry.description.name;
    launchOptions.escapeKeyTriggersCloseButton = true;
    launchOptions.useNativeTitleBar = true;
    launchOptions.resizable = editor->isResizable();
    launchOptions.content.setOwned(editor.release());
    launchOptions.useBottomRightCornerResizer = false;
    launchOptions.dialogBackgroundColour = CustomLookAndFeel::greySemiDark();

    currentEntry.editorWindow = launchOptions.create();

    if (currentEntry.editorWindow != nullptr) {
        // A slot whose editor was open before reopens it where it was, kept on a connected display.
        if (currentEntry.editorPosition.has_value()) {
            const auto* peer = currentEntry.editorWindow->getPeer();
            const auto frameSize
                = peer != nullptr ? peer->getFrameSizeIfPresent() : juce::ComponentPeer::OptionalBorderSize {};
            const auto frame = frameSize ? *frameSize : juce::BorderSize<int> {};
            currentEntry.editorWindow->setBounds(
                utils::boundsOnScreen(
                    currentEntry.editorWindow->getBounds().withPosition(*currentEntry.editorPosition), frame
                )
            );
        }
        currentEntry.editorWindow->setVisible(true);
        currentEntry.editorWindow->toFront(true);
        currentEntry.wasEditorOpen = true;
    }
    sendChangeMessage();
}

/// Opens the chain editor window for adding, reordering, enabling, and removing plugins.
void LiveChain::showChainEditor()
{
    const juce::WeakReference safeThis(this);
    const auto inspectedChain = preparePreview(previewSampleRate, previewBlockSize, true);
    if (safeThis == nullptr) {
        return;
    }
    if (inspectedChain.preparation.errorMessage.isNotEmpty()) {
        utils::logWarn("Plugin latency inspection: {}", inspectedChain.preparation.errorMessage);
    }
    if (chainEditorWindow != nullptr) {
        // The window hides itself when closed, so reopening only needs to make it visible again.
        chainEditorWindow->setVisible(true);
        chainEditorWindow->toFront(true);
        return;
    }

    auto editor = std::make_unique<PluginChainEditor>(*this);
    editor->setSize(layout::pluginEditor::CHAIN_WINDOW_WIDTH, layout::pluginEditor::CHAIN_WINDOW_HEIGHT);

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = "Plugin Chain";
    launchOptions.content.setOwned(editor.release());
    launchOptions.escapeKeyTriggersCloseButton = true;
    launchOptions.useNativeTitleBar = true;
    launchOptions.resizable = true;
    launchOptions.dialogBackgroundColour = CustomLookAndFeel::greySemiDark();

    chainEditorWindow = launchOptions.create();

    if (chainEditorWindow != nullptr) {
        chainEditorWindow->setVisible(true);
        chainEditorWindow->toFront(true);
    }
}

/// Opens the plugin manager window that lists the known plugins and scans for new ones.
void LiveChain::showScanWindow()
{
    if (scanWindow != nullptr) {
        // The window hides itself when closed, so reopening only needs to make it visible again,
        // after picking up any list a CLI rescan saved while it was hidden.
        library.reload();
        scanWindow->setVisible(true);
        scanWindow->toFront(true);
        return;
    }

    auto listComponent = library.createScanComponent();
    listComponent->setSize(layout::pluginEditor::MANAGER_WINDOW_WIDTH, layout::pluginEditor::MANAGER_WINDOW_HEIGHT);

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = "Plugin Manager";
    launchOptions.content.setOwned(listComponent.release());
    launchOptions.escapeKeyTriggersCloseButton = true;
    launchOptions.useNativeTitleBar = true;
    launchOptions.resizable = true;
    launchOptions.dialogBackgroundColour = CustomLookAndFeel::greySemiDark();

    scanWindow = launchOptions.create();

    if (scanWindow != nullptr) {
        scanWindow->setVisible(true);
        scanWindow->toFront(true);
    }
}
}  // namespace audiobatch
