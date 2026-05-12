#include "PluginChain.h"

#include "CustomLookAndFeel.h"

namespace audiobatch::plugin_chain
{
constexpr auto pluginListPropertyKey = "knownPluginList";
constexpr auto selectedPluginIdPropertyKey = "selectedPluginIdentifier";
constexpr auto selectedPluginStatePropertyKey = "selectedPluginState";
constexpr auto deadMansPedalFileName = "audiobatch_plugin_scan_crash_log.txt";

constexpr int editMenuItemId = 1;
constexpr int clearMenuItemId = 2;
constexpr int scanMenuItemId = 3;
}  // namespace audiobatch::plugin_chain

using namespace audiobatch::plugin_chain;

PluginChain::PluginChain(juce::ApplicationProperties& applicationProperties) : appProperties(applicationProperties)
{
    juce::addDefaultFormatsToManager(formatManager);
    knownPluginList.addChangeListener(this);

    loadKnownPluginList();
    loadPersistedSelection();
}

PluginChain::~PluginChain()
{
    stopTimer();
    knownPluginList.removeChangeListener(this);

    if (editorWindow != nullptr) {
        editorWindow.deleteAndZero();
    }

    if (scanWindow != nullptr) {
        scanWindow.deleteAndZero();
    }

    editorInstance.reset();
}

PluginDescriptorRef PluginChain::getSelectedPlugin() const
{
    PluginDescriptorRef ref;

    if (selectedDescription.fileOrIdentifier.isEmpty()) {
        return ref;
    }

    ref.pluginFormatName = selectedDescription.pluginFormatName;
    ref.identifierString = selectedDescription.createIdentifierString();
    ref.name = selectedDescription.name;
    ref.manufacturer = selectedDescription.manufacturerName;
    ref.state = selectedState;
    return ref;
}

juce::PluginDescription PluginChain::getSelectedPluginDescription() const
{
    return selectedDescription;
}

void PluginChain::setSelectionChangedCallback(SelectionChangedCallback callback)
{
    selectionChangedCallback = std::move(callback);
}

void PluginChain::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &knownPluginList) {
        persistKnownPluginList();
    }
}

void PluginChain::timerCallback()
{
    // Watch for the user closing the editor window so we can capture plugin state.
    if (wasEditorOpen && editorWindow == nullptr) {
        wasEditorOpen = false;

        if (editorInstance != nullptr) {
            juce::MemoryBlock state;
            editorInstance->getStateInformation(state);

            if (state.getSize() > 0) {
                selectedState = std::move(state);
                persistSelection();
                notifySelectionChanged();
            }

            editorInstance.reset();
        }

        stopTimer();
    }
}

void PluginChain::showMenu(juce::Component& anchor)
{
    juce::PopupMenu menu;

    const bool hasSelection = selectedDescription.fileOrIdentifier.isNotEmpty();
    if (hasSelection) {
        menu.addSectionHeader(selectedDescription.name + " (" + selectedDescription.pluginFormatName + ")");
        menu.addItem(editMenuItemId, "Edit Plugin...");
        menu.addItem(clearMenuItemId, "Clear Plugin");
        menu.addSeparator();
    }

    juce::PopupMenu chooseSubmenu;
    const auto& types = knownPluginList.getTypes();
    juce::KnownPluginList::addToMenu(chooseSubmenu, types, juce::KnownPluginList::sortByManufacturer);
    menu.addSubMenu("Choose Plugin", chooseSubmenu, !types.isEmpty());

    menu.addItem(scanMenuItemId, "Scan for Plugins...");

    const juce::WeakReference<PluginChain> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&anchor).withParentComponent(anchor.getTopLevelComponent()),
        [safeThis, types](const int result) {
            if (safeThis == nullptr || result == 0) {
                return;
            }

            switch (result) {
                case editMenuItemId:
                    safeThis->openEditor();
                    return;
                case clearMenuItemId:
                    safeThis->clearSelection();
                    return;
                case scanMenuItemId:
                    safeThis->showScanWindow();
                    return;
                default:
                    break;
            }

            const auto index = juce::KnownPluginList::getIndexChosenByMenu(types, result);
            if (index < 0 || index >= types.size()) {
                return;
            }

            safeThis->selectPlugin(types.getReference(index));
        }
    );
}

void PluginChain::showScanWindow()
{
    if (scanWindow != nullptr) {
        scanWindow->toFront(true);
        return;
    }

    const auto deadMansPedalFile
        = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile(deadMansPedalFileName);

    auto* listComponent = new juce::PluginListComponent(
        formatManager, knownPluginList, deadMansPedalFile, appProperties.getUserSettings(), true
    );
    listComponent->setSize(720, 520);

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = "Plugin Manager";
    launchOptions.content.setOwned(listComponent);
    launchOptions.escapeKeyTriggersCloseButton = true;
    launchOptions.useNativeTitleBar = true;
    launchOptions.resizable = true;
    launchOptions.dialogBackgroundColour = juce::CustomLookAndFeel::greySemiDark;

    scanWindow = launchOptions.create();

    if (scanWindow != nullptr) {
        scanWindow->setVisible(true);
        scanWindow->toFront(true);
    }
}

void PluginChain::openEditor()
{
    if (selectedDescription.fileOrIdentifier.isEmpty()) {
        return;
    }

    if (editorWindow != nullptr) {
        editorWindow->toFront(true);
        return;
    }

    juce::String errorMessage;
    constexpr double editorSampleRate = 48000.0;
    constexpr int editorBlockSize = 512;

    editorInstance
        = formatManager.createPluginInstance(selectedDescription, editorSampleRate, editorBlockSize, errorMessage);

    if (editorInstance == nullptr) {
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Plugin Error",
                "Could not instantiate plugin:\n\n" + errorMessage,
                "OK"
            ),
            nullptr
        );
        return;
    }

    // Restore previously saved state, if any, so the editor opens at the user's saved settings.
    if (selectedState.getSize() > 0) {
        editorInstance->setStateInformation(selectedState.getData(), static_cast<int>(selectedState.getSize()));
    }

    auto* editor = editorInstance->createEditorIfNeeded();

    if (editor == nullptr) {
        // No custom editor. Fall back to the generic parameter editor.
        editor = new juce::GenericAudioProcessorEditor(*editorInstance);
        editor->setSize(450, 600);
    }

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = selectedDescription.name;
    launchOptions.content.setOwned(editor);
    launchOptions.escapeKeyTriggersCloseButton = true;
    launchOptions.useNativeTitleBar = true;
    launchOptions.resizable = false;
    launchOptions.dialogBackgroundColour = juce::CustomLookAndFeel::greySemiDark;

    editorWindow = launchOptions.create();

    if (editorWindow != nullptr) {
        editorWindow->setVisible(true);
        editorWindow->toFront(true);
        wasEditorOpen = true;
        startTimerHz(4);
    }
}

void PluginChain::clearSelection()
{
    selectedDescription = {};
    selectedState.reset();
    persistSelection();
    notifySelectionChanged();
}

void PluginChain::selectPlugin(const juce::PluginDescription& description)
{
    selectedDescription = description;
    selectedState.reset();
    persistSelection();
    notifySelectionChanged();
}

void PluginChain::loadPersistedSelection()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    const auto pluginXml = settings->getValue(selectedPluginIdPropertyKey);
    if (pluginXml.isEmpty()) {
        return;
    }

    if (const auto element = juce::parseXML(pluginXml); element != nullptr) {
        selectedDescription.loadFromXml(*element);
    }

    const auto stateBase64 = settings->getValue(selectedPluginStatePropertyKey);
    if (stateBase64.isNotEmpty()) {
        juce::MemoryOutputStream stream(selectedState, false);
        if (!juce::Base64::convertFromBase64(stream, stateBase64)) {
            selectedState.reset();
        }
    }
}

void PluginChain::persistSelection()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    if (selectedDescription.fileOrIdentifier.isEmpty()) {
        settings->removeValue(selectedPluginIdPropertyKey);
        settings->removeValue(selectedPluginStatePropertyKey);
        settings->saveIfNeeded();
        return;
    }

    const auto element = selectedDescription.createXml();
    if (element != nullptr) {
        settings->setValue(selectedPluginIdPropertyKey, element->toString());
    }

    if (selectedState.getSize() > 0) {
        const auto stateBase64 = juce::Base64::toBase64(selectedState.getData(), selectedState.getSize());
        settings->setValue(selectedPluginStatePropertyKey, stateBase64);
    } else {
        settings->removeValue(selectedPluginStatePropertyKey);
    }

    settings->saveIfNeeded();
}

void PluginChain::persistKnownPluginList()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    if (const auto xml = knownPluginList.createXml(); xml != nullptr) {
        settings->setValue(pluginListPropertyKey, xml->toString());
        settings->saveIfNeeded();
    }
}

void PluginChain::loadKnownPluginList()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    const auto xmlText = settings->getValue(pluginListPropertyKey);
    if (xmlText.isEmpty()) {
        return;
    }

    if (const auto element = juce::parseXML(xmlText); element != nullptr) {
        knownPluginList.recreateFromXml(*element);
    }
}

void PluginChain::notifySelectionChanged()
{
    if (selectionChangedCallback != nullptr) {
        selectionChangedCallback(getSelectedPlugin());
    }
}
