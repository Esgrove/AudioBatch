/// Implementation of PluginChain.
/// Covers the popup menu, slot editing operations, and window handling for the per-slot plugin editors,
/// the chain editor, and the plugin scan dialog,
/// including capturing live plugin state from open editors before a run or save.
/// Also implements XML persistence of the chain and the known-plugins list in application settings,
/// with migration of the legacy single-plugin selection into a one-slot chain.

#include "PluginChain.h"

#include "CustomLookAndFeel.h"
#include "PluginChainEditor.h"
#include "StringFormat.h"
#include "utils.h"

#include <algorithm>

namespace audiobatch::plugin_chain
{
constexpr auto pluginChainPropertyKey = "pluginChain";
constexpr auto legacySelectedPluginIdPropertyKey = "selectedPluginIdentifier";
constexpr auto legacySelectedPluginStatePropertyKey = "selectedPluginState";

constexpr auto chainXmlTag = "PLUGIN_CHAIN";
constexpr auto slotXmlTag = "SLOT";
constexpr auto slotEnabledAttribute = "enabled";
constexpr auto slotStateAttribute = "state";

constexpr int editChainMenuItemId = 1;
constexpr int clearChainMenuItemId = 2;
constexpr int scanMenuItemId = 3;

constexpr double editorSampleRate = 48000.0;
constexpr int editorBlockSize = 512;

/// Initial width of the generic parameter editor shown for plugins without a custom editor.
constexpr int genericEditorWidth = 450;

/// Initial height of the generic parameter editor shown for plugins without a custom editor.
constexpr int genericEditorHeight = 600;

/// Initial width of the plugin chain editor window.
constexpr int chainEditorWidth = 560;

/// Initial height of the plugin chain editor window.
constexpr int chainEditorHeight = 420;

/// Initial width of the plugin manager window that lists and scans plugins.
constexpr int pluginManagerWidth = 720;

/// Initial height of the plugin manager window that lists and scans plugins.
constexpr int pluginManagerHeight = 520;

/// Builds the descriptor ref for one chain entry.
static PluginDescriptorRef makeDescriptorRef(const juce::PluginDescription& description, const juce::MemoryBlock& state)
{
    PluginDescriptorRef descriptorRef;
    descriptorRef.pluginFormatName = description.pluginFormatName;
    descriptorRef.identifierString = description.createIdentifierString();
    descriptorRef.name = description.name;
    descriptorRef.manufacturer = description.manufacturerName;
    descriptorRef.state = state;
    return descriptorRef;
}
}  // namespace audiobatch::plugin_chain

using namespace audiobatch::plugin_chain;

juce::String formatPluginDescription(const juce::PluginDescription& description)
{
    return utils::format("{} {} ({})", description.manufacturerName, description.name, description.pluginFormatName);
}

PluginChain::PluginChain(juce::ApplicationProperties& applicationProperties) : appProperties(applicationProperties)
{
    juce::addDefaultFormatsToManager(formatManager);
    knownPluginList.addChangeListener(this);

    // Load plugins in a separate worker process while scanning,
    // so a plugin that crashes during the scan is blacklisted instead of taking down the app.
    // The plugin scan lock is held for the whole scan, so a CLI rescan cannot overwrite its results.
    knownPluginList.setCustomScanner(
        std::make_unique<OutOfProcessPluginScanner>(
            [this] { return beginPluginScan(); }, [this] { pluginScanLock.release(); }
        )
    );

    loadKnownPluginList();
    loadPersistedChain();
}

PluginChain::~PluginChain()
{
    stopTimer();
    knownPluginList.removeChangeListener(this);

    for (auto& entry : chain) {
        closeEditorForEntry(entry);
    }

    if (chainEditorWindow != nullptr) {
        chainEditorWindow.deleteAndZero();
    }

    if (scanWindow != nullptr) {
        scanWindow.deleteAndZero();
    }
}

int PluginChain::getNumSlots() const noexcept
{
    return static_cast<int>(chain.size());
}

int PluginChain::getNumEnabledValidSlots() const noexcept
{
    int count = 0;
    for (const auto& entry : chain) {
        if (entry.enabled && entry.description.fileOrIdentifier.isNotEmpty()) {
            ++count;
        }
    }

    return count;
}

juce::PluginDescription PluginChain::getSlotDescription(const int index) const
{
    if (!juce::isPositiveAndBelow(index, getNumSlots())) {
        return {};
    }

    return chain[static_cast<std::size_t>(index)].description;
}

bool PluginChain::isSlotEnabled(const int index) const
{
    if (!juce::isPositiveAndBelow(index, getNumSlots())) {
        return false;
    }

    return chain[static_cast<std::size_t>(index)].enabled;
}

std::vector<PluginChain::EnabledChainPlugin> PluginChain::getEnabledPlugins()
{
    // Capture live state from open editors so a processing run uses the latest tweaks.
    captureOpenEditorStates();

    std::vector<EnabledChainPlugin> plugins;
    plugins.reserve(chain.size());

    for (const auto& entry : chain) {
        if (entry.enabled && entry.description.fileOrIdentifier.isNotEmpty()) {
            plugins.push_back(
                {.description = entry.description, .descriptorRef = makeDescriptorRef(entry.description, entry.state)}
            );
        }
    }

    return plugins;
}

juce::String PluginChain::getChainSummary() const
{
    if (chain.empty()) {
        return {};
    }

    juce::StringArray names;
    for (const auto& entry : chain) {
        names.add(entry.enabled ? entry.description.name : utils::format("({})", entry.description.name));
    }

    auto summary = names.joinIntoString(" > ");
    constexpr int maxSummaryLength = 60;
    if (summary.length() > maxSummaryLength) {
        summary = utils::format("{}...", summary.substring(0, maxSummaryLength));
    }

    return summary;
}

void PluginChain::setChainChangedCallback(ChainChangedCallback callback)
{
    chainChangedCallback = std::move(callback);
}

void PluginChain::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &knownPluginList) {
        persistKnownPluginList();
    }
}

void PluginChain::timerCallback()
{
    // Watch for the user closing editor windows so we can capture plugin state.
    bool stateCaptured = false;

    for (auto& entry : chain) {
        if (entry.wasEditorOpen && isEditorWindowClosed(entry) && reapClosedEditor(entry)) {
            stateCaptured = true;
        }
    }

    if (stateCaptured) {
        persistChain();
        notifyChainChanged();
    }

    if (!anyEditorOpen()) {
        stopTimer();
    }
}

bool PluginChain::isEditorWindowClosed(const ChainEntry& entry)
{
    return entry.editorWindow == nullptr || !entry.editorWindow->isVisible();
}

bool PluginChain::reapClosedEditor(ChainEntry& entry)
{
    entry.wasEditorOpen = false;

    bool stateCaptured = false;
    if (entry.editorInstance != nullptr) {
        juce::MemoryBlock state;
        entry.editorInstance->getStateInformation(state);

        if (state.getSize() > 0) {
            entry.state = std::move(state);
            stateCaptured = true;
        }
    }

    closeEditorForEntry(entry);
    return stateCaptured;
}

void PluginChain::showMenu(juce::Component& anchor)
{
    juce::PopupMenu menu;

    const auto summary = getChainSummary();
    menu.addSectionHeader(
        summary.isEmpty() ? juce::String("No plugins in chain") : utils::format("Chain: {}", summary)
    );

    menu.addItem(editChainMenuItemId, "Edit Chain...");
    menu.addSeparator();

    juce::PopupMenu addSubmenu;
    const auto& types = knownPluginList.getTypes();
    juce::KnownPluginList::addToMenu(addSubmenu, types, juce::KnownPluginList::sortByManufacturer);
    menu.addSubMenu("Add Plugin", addSubmenu, !types.isEmpty());

    menu.addItem(clearChainMenuItemId, "Clear Chain", !chain.empty());
    menu.addItem(scanMenuItemId, "Scan for Plugins...");

    const juce::WeakReference<PluginChain> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&anchor).withParentComponent(anchor.getTopLevelComponent()),
        [safeThis, types](const int result) {
            if (safeThis == nullptr || result == 0) {
                return;
            }

            switch (result) {
                case editChainMenuItemId:
                    safeThis->showChainEditor();
                    return;
                case clearChainMenuItemId:
                    safeThis->clearChain();
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

            safeThis->addPlugin(types.getReference(index));
        }
    );
}

void PluginChain::addPlugin(const juce::PluginDescription& description)
{
    ChainEntry entry;
    entry.description = description;
    chain.push_back(std::move(entry));

    persistChain();
    notifyChainChanged();

    // Automatically open the editor for a freshly added plugin so the user can dial it in right away.
    openEditorForSlot(getNumSlots() - 1);
}

void PluginChain::removeSlot(const int index)
{
    if (!juce::isPositiveAndBelow(index, getNumSlots())) {
        return;
    }

    auto& entry = chain[static_cast<std::size_t>(index)];
    closeEditorForEntry(entry);
    chain.erase(chain.begin() + index);

    if (!anyEditorOpen()) {
        stopTimer();
    }

    persistChain();
    notifyChainChanged();
}

void PluginChain::moveSlot(const int fromIndex, const int toIndex)
{
    const auto numSlots = getNumSlots();
    if (!juce::isPositiveAndBelow(fromIndex, numSlots) || !juce::isPositiveAndBelow(toIndex, numSlots)
        || fromIndex == toIndex)
    {
        return;
    }

    // Entries own their editor windows, so reordering carries any open editor along with its slot.
    if (fromIndex < toIndex) {
        std::rotate(chain.begin() + fromIndex, chain.begin() + fromIndex + 1, chain.begin() + toIndex + 1);
    } else {
        std::rotate(chain.begin() + toIndex, chain.begin() + fromIndex, chain.begin() + fromIndex + 1);
    }

    persistChain();
    notifyChainChanged();
}

void PluginChain::setSlotEnabled(const int index, const bool enabled)
{
    if (!juce::isPositiveAndBelow(index, getNumSlots())) {
        return;
    }

    auto& entry = chain[static_cast<std::size_t>(index)];
    if (entry.enabled == enabled) {
        return;
    }

    entry.enabled = enabled;
    persistChain();
    notifyChainChanged();
}

void PluginChain::clearChain()
{
    if (chain.empty()) {
        return;
    }

    for (auto& entry : chain) {
        closeEditorForEntry(entry);
    }

    chain.clear();
    stopTimer();

    persistChain();
    notifyChainChanged();
}

void PluginChain::openEditorForSlot(const int index)
{
    if (!juce::isPositiveAndBelow(index, getNumSlots())) {
        return;
    }

    auto& entry = chain[static_cast<std::size_t>(index)];

    if (entry.description.fileOrIdentifier.isEmpty()) {
        return;
    }

    if (entry.editorWindow != nullptr && entry.editorWindow->isVisible()) {
        entry.editorWindow->toFront(true);
        return;
    }

    // A window the user closed is only hidden, not deleted,
    // and the polling timer may not have reaped it yet.
    // Capture its state and destroy it now so a fresh editor can open below.
    if (entry.wasEditorOpen && reapClosedEditor(entry)) {
        persistChain();
        notifyChainChanged();
    }

    juce::String errorMessage;
    entry.editorInstance
        = formatManager.createPluginInstance(entry.description, editorSampleRate, editorBlockSize, errorMessage);

    if (entry.editorInstance == nullptr) {
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::WarningIcon,
                "Plugin Error",
                utils::format("Could not instantiate plugin:\n\n{}", errorMessage),
                "OK"
            ),
            nullptr
        );
        return;
    }

    // Restore previously saved state, if any, so the editor opens at the user's saved settings.
    if (entry.state.getSize() > 0) {
        entry.editorInstance->setStateInformation(entry.state.getData(), static_cast<int>(entry.state.getSize()));
    }

    auto* editor = entry.editorInstance->createEditorAndMakeActive();

    if (editor == nullptr) {
        // No custom editor. Fall back to the generic parameter editor.
        editor = new juce::GenericAudioProcessorEditor(*entry.editorInstance);
        editor->setSize(genericEditorWidth, genericEditorHeight);
    }

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = entry.description.name;
    launchOptions.content.setOwned(editor);
    launchOptions.escapeKeyTriggersCloseButton = true;
    launchOptions.useNativeTitleBar = true;
    launchOptions.resizable = false;
    launchOptions.dialogBackgroundColour = juce::CustomLookAndFeel::greySemiDark();

    entry.editorWindow = launchOptions.create();

    if (entry.editorWindow != nullptr) {
        entry.editorWindow->setVisible(true);
        entry.editorWindow->toFront(true);
        entry.wasEditorOpen = true;
        startTimerHz(4);
    } else {
        entry.editorInstance = nullptr;
    }
}

void PluginChain::showChainEditor()
{
    if (chainEditorWindow != nullptr) {
        // The window hides itself when closed, so reopening only needs to make it visible again.
        chainEditorWindow->setVisible(true);
        chainEditorWindow->toFront(true);
        return;
    }

    auto* editor = new PluginChainEditor(*this);
    editor->setSize(chainEditorWidth, chainEditorHeight);

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = "Plugin Chain";
    launchOptions.content.setOwned(editor);
    launchOptions.escapeKeyTriggersCloseButton = true;
    launchOptions.useNativeTitleBar = true;
    launchOptions.resizable = true;
    launchOptions.dialogBackgroundColour = juce::CustomLookAndFeel::greySemiDark();

    chainEditorWindow = launchOptions.create();

    if (chainEditorWindow != nullptr) {
        chainEditorWindow->setVisible(true);
        chainEditorWindow->toFront(true);
    }
}

void PluginChain::showScanWindow()
{
    // Pick up changes from a CLI rescan since the list was loaded,
    // so a scan from this window builds on them instead of saving over them.
    loadKnownPluginList();

    if (scanWindow != nullptr) {
        // The window hides itself when closed, so reopening only needs to make it visible again.
        scanWindow->setVisible(true);
        scanWindow->toFront(true);
        return;
    }

    const auto deadMansPedalFile = getPluginScanCrashLogFile();

    // The plugins themselves are loaded by the OutOfProcessPluginScanner worker process,
    // which scans on its own message thread.
    // Copy-protected plugins (for example PACE / iLok wrapped VST3s) need that,
    // because they dispatch licensing work to the main thread while their factory is still being enumerated.
    // allowPluginsWhichRequireAsynchronousInstantiation stays false,
    // so asynchronous-only plugins (AUv3) are still left out of the scan.
    // They are not usable by this app anyway,
    // since all instantiation goes through the synchronous createPluginInstance.
    constexpr bool allowAsynchronousInstantiation = false;
    auto* listComponent = new juce::PluginListComponent(
        formatManager,
        knownPluginList,
        deadMansPedalFile,
        appProperties.getUserSettings(),
        allowAsynchronousInstantiation
    );
    listComponent->setSize(pluginManagerWidth, pluginManagerHeight);

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = "Plugin Manager";
    launchOptions.content.setOwned(listComponent);
    launchOptions.escapeKeyTriggersCloseButton = true;
    launchOptions.useNativeTitleBar = true;
    launchOptions.resizable = true;
    launchOptions.dialogBackgroundColour = juce::CustomLookAndFeel::greySemiDark();

    scanWindow = launchOptions.create();

    if (scanWindow != nullptr) {
        scanWindow->setVisible(true);
        scanWindow->toFront(true);
    }
}

void PluginChain::captureOpenEditorStates()
{
    for (auto& entry : chain) {
        if (entry.editorInstance != nullptr && entry.editorWindow != nullptr) {
            juce::MemoryBlock state;
            entry.editorInstance->getStateInformation(state);

            if (state.getSize() > 0) {
                entry.state = std::move(state);
            }
        }
    }
}

void PluginChain::closeEditorForEntry(ChainEntry& entry)
{
    if (entry.editorWindow != nullptr) {
        entry.editorWindow.deleteAndZero();
    }

    entry.editorInstance = nullptr;
    entry.wasEditorOpen = false;
}

bool PluginChain::anyEditorOpen() const noexcept
{
    return std::ranges::any_of(chain, [](const auto& entry) { return entry.wasEditorOpen; });
}

void PluginChain::restoreState(ChainEntry& entry, const juce::String& stateBase64)
{
    if (stateBase64.isEmpty()) {
        return;
    }

    juce::MemoryOutputStream stream(entry.state, false);
    if (!juce::Base64::convertFromBase64(stream, stateBase64)) {
        entry.state.reset();
    }
}

std::optional<PluginChain::ChainEntry> PluginChain::parseSlot(const juce::XmlElement& slotElement)
{
    const auto* descriptionElement = slotElement.getFirstChildElement();
    if (descriptionElement == nullptr) {
        return std::nullopt;
    }

    ChainEntry entry;
    if (!entry.description.loadFromXml(*descriptionElement)) {
        return std::nullopt;
    }

    entry.enabled = slotElement.getBoolAttribute(slotEnabledAttribute, true);
    restoreState(entry, slotElement.getStringAttribute(slotStateAttribute));
    return entry;
}

std::optional<PluginChain::ChainEntry> PluginChain::parseLegacyEntry(
    const juce::String& pluginXml,
    const juce::String& stateBase64
)
{
    const auto element = juce::parseXML(pluginXml);
    if (element == nullptr) {
        return std::nullopt;
    }

    ChainEntry entry;
    if (!entry.description.loadFromXml(*element)) {
        return std::nullopt;
    }

    restoreState(entry, stateBase64);
    return entry;
}

void PluginChain::loadPersistedChain()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    if (const auto chainXml = settings->getValue(pluginChainPropertyKey); chainXml.isNotEmpty()) {
        const auto element = juce::parseXML(chainXml);
        if (element == nullptr || !element->hasTagName(chainXmlTag)) {
            return;
        }

        for (const auto* slotElement : element->getChildWithTagNameIterator(slotXmlTag)) {
            if (auto entry = parseSlot(*slotElement); entry.has_value()) {
                chain.push_back(std::move(*entry));
            }
        }

        return;
    }

    // Migrate a legacy single-plugin selection into a one-slot chain.
    const auto legacyPluginXml = settings->getValue(legacySelectedPluginIdPropertyKey);
    if (legacyPluginXml.isEmpty()) {
        return;
    }

    auto entry = parseLegacyEntry(legacyPluginXml, settings->getValue(legacySelectedPluginStatePropertyKey));
    if (!entry.has_value()) {
        return;
    }

    chain.push_back(std::move(*entry));
    persistChain();

    settings->removeValue(legacySelectedPluginIdPropertyKey);
    settings->removeValue(legacySelectedPluginStatePropertyKey);
    settings->saveIfNeeded();
}

void PluginChain::persistChain()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    // Include the latest tweaks from any open editors in the persisted state.
    captureOpenEditorStates();

    if (chain.empty()) {
        settings->removeValue(pluginChainPropertyKey);
        settings->saveIfNeeded();
        return;
    }

    juce::XmlElement chainElement(chainXmlTag);

    for (const auto& entry : chain) {
        auto descriptionElement = entry.description.createXml();
        if (descriptionElement == nullptr) {
            continue;
        }

        auto* slotElement = chainElement.createNewChildElement(slotXmlTag);
        slotElement->setAttribute(slotEnabledAttribute, static_cast<int>(entry.enabled));

        if (entry.state.getSize() > 0) {
            slotElement->setAttribute(
                slotStateAttribute, juce::Base64::toBase64(entry.state.getData(), entry.state.getSize())
            );
        }

        slotElement->addChildElement(descriptionElement.release());
    }

    settings->setValue(pluginChainPropertyKey, chainElement.toString());
    settings->saveIfNeeded();
}

void PluginChain::persistKnownPluginList() const
{
    // Failures are logged by saveKnownPluginList, and the in-memory list stays usable.
    [[maybe_unused]] const auto saved = saveKnownPluginList(knownPluginList);
}

void PluginChain::loadKnownPluginList()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    migrateKnownPluginList(*settings);
    ::loadKnownPluginList(*settings, knownPluginList);
}

bool PluginChain::beginPluginScan()
{
    if (!pluginScanLock.tryAcquire()) {
        utils::logWarn("Skipped a plugin scan because another plugin scan is running");
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions::makeOptionsOk(
                juce::MessageBoxIconType::InfoIcon,
                "Plugin Scan Skipped",
                "Another plugin scan is running, for example audiobatch --rescan in a terminal.\n\n"
                "Scan again once it has finished.",
                "OK"
            ),
            nullptr
        );
        return false;
    }

    // Another process may have saved a newer list since this one was loaded.
    loadKnownPluginList();
    return true;
}

void PluginChain::notifyChainChanged()
{
    sendChangeMessage();

    if (chainChangedCallback != nullptr) {
        chainChangedCallback();
    }
}
