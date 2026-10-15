/// Implementation of PluginChain.
/// Covers the popup menu, slot editing operations, and window handling for the per-slot plugin editors,
/// the chain editor, and the plugin scan dialog,
/// including capturing live plugin state from open editors before a run or save.
/// Also implements XML persistence of the chain and the known-plugins list in application settings,
/// with migration of the legacy single-plugin selection into a one-slot chain.

#include "plugins/PluginChain.h"

#include "gui/components/CustomLookAndFeel.h"
#include "gui/components/PluginChainEditor.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>

namespace audiobatch
{
namespace
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
PluginDescriptorRef makeDescriptorRef(const juce::PluginDescription& description, const juce::MemoryBlock& state)
{
    PluginDescriptorRef descriptorRef;
    descriptorRef.pluginFormatName = description.pluginFormatName;
    descriptorRef.identifierString = description.createIdentifierString();
    descriptorRef.name = description.name;
    descriptorRef.manufacturer = description.manufacturerName;
    descriptorRef.state = state;
    return descriptorRef;
}
}  // namespace

/// Formats a plugin description as "Manufacturer Name (Format)" for chain editor rows, menus, and log messages.
juce::String formatPluginDescription(const juce::PluginDescription& description)
{
    return utils::format("{} {} ({})", description.manufacturerName, description.name, description.pluginFormatName);
}

/// Registers the default plugin formats and restores the known-plugins list
/// and the persisted chain from application settings.
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

/// Closes all editor, chain-editor, and scan windows and destroys their plugin instances.
PluginChain::~PluginChain()
{
    utils::logDebug("Application shutdown: closing plugin windows");
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
    utils::logDebug("Application shutdown: releasing plugin chain members");
}

/// Returns the number of slots in the chain, including disabled ones.
int PluginChain::getNumSlots() const noexcept
{
    return static_cast<int>(chain.size());
}

/// Returns the number of enabled slots that identify a valid plugin.
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

/// Returns the plugin description for the given slot, or an empty description when out of range.
juce::PluginDescription PluginChain::getSlotDescription(const int index) const
{
    if (!juce::isPositiveAndBelow(index, getNumSlots())) {
        return {};
    }

    return chain[static_cast<std::size_t>(index)].description;
}

/// Returns true when the given slot exists and is enabled.
bool PluginChain::isSlotEnabled(const int index) const
{
    if (!juce::isPositiveAndBelow(index, getNumSlots())) {
        return false;
    }

    return chain[static_cast<std::size_t>(index)].enabled;
}

/// Returns the enabled plugins in chain order.
/// Captures live state from any open editor windows first, so a processing run always uses the latest tweaks.
std::vector<EnabledChainPlugin> PluginChain::getEnabledPlugins()
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

/// Creates one chain of instances of the enabled plugins per worker, for a processing run.
/// Must run on the message thread.
/// Stops at the first plugin that fails, since a chain missing any plugin must not run.
/// Captures the initial state of each plugin from the first chain.
ChainInstantiation PluginChain::instantiateChains(
    const std::vector<EnabledChainPlugin>& enabledPlugins,
    const int workerCount
) const
{
    // Use a reasonable default sample rate. The chain is prepared again per file with the file's rate.
    constexpr double defaultSampleRate = 48000.0;
    constexpr int defaultBlockSize = 1024;

    ChainInstantiation instantiation;
    instantiation.chains.reserve(static_cast<std::size_t>(workerCount));

    for (int worker = 0; worker < workerCount; ++worker) {
        std::vector<std::unique_ptr<juce::AudioPluginInstance>> chainInstances;
        chainInstances.reserve(enabledPlugins.size());

        for (const auto& [description, descriptorRef] : enabledPlugins) {
            juce::String error;
            auto instance = formatManager.createPluginInstance(description, defaultSampleRate, defaultBlockSize, error);

            if (instance == nullptr) {
                instantiation.errorMessage = error.isNotEmpty() ? error : juce::String("Plugin instantiation failed");
                instantiation.failedPluginName = description.name;
                return instantiation;
            }

            if (worker == 0) {
                instance->getStateInformation(instantiation.initialStates.emplace_back());
            }

            chainInstances.push_back(std::move(instance));
        }

        instantiation.chains.push_back(std::move(chainInstances));
    }

    return instantiation;
}

/// Returns a short human-readable summary of the chain for menu headers,
/// with disabled slots in parentheses, or an empty string when the chain is empty.
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

/// Invoked whenever the chain (order, slots, enabled flags, or persisted state) changes.
void PluginChain::setChainChangedCallback(ChainChangedCallback callback)
{
    chainChangedCallback = std::move(callback);
}

/// Persists the known-plugins list whenever the plugin scanner updates it.
void PluginChain::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &knownPluginList) {
        persistKnownPluginList();
    }
}

/// Polls for editor windows the user has closed, capturing their plugin state and releasing their instances.
/// A window hidden by its close button counts as closed, because the dialog hides itself instead of deleting itself.
/// Stops itself once no editor windows remain open.
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

/// Returns true when the entry's editor window has been closed by the user.
/// The dialog window hides itself when its close button is pressed instead of deleting itself,
/// so a hidden window also counts as closed.
bool PluginChain::isEditorWindowClosed(const PluginChainEntry& entry)
{
    return entry.editorWindow == nullptr || !entry.editorWindow->isVisible();
}

/// Captures plugin state from a closed editor into its entry, then destroys the window and the instance.
/// Returns true when plugin state was captured.
bool PluginChain::reapClosedEditor(PluginChainEntry& entry)
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

/// Pops up the plugin menu anchored to the given component.
/// The menu includes the chain summary, Edit Chain, the Add Plugin submenu, Clear Chain, and a Scan entry.
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

    const juce::WeakReference safeThis(this);
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

/// Appends the given plugin to the chain as an enabled slot and opens its editor.
void PluginChain::addPlugin(const juce::PluginDescription& description)
{
    PluginChainEntry entry;
    entry.description = description;
    chain.push_back(std::move(entry));

    persistChain();
    notifyChainChanged();

    // Automatically open the editor for a freshly added plugin so the user can dial it in right away.
    openEditorForSlot(getNumSlots() - 1);
}

/// Removes the given slot from the chain, closing its editor window if open.
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

/// Moves a slot to a new position in the chain.
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

/// Enables or disables the given slot without removing it from the chain.
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

/// Removes all slots from the chain, closing any open editor windows.
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

/// Opens the editor window for the given slot.
/// Multiple slot editors can be open at the same time.
/// The editor instance is prepared before its state is restored and its editor is created,
/// and released in closeEditorForEntry.
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

    // A window the user closed is only hidden, not deleted, and the polling timer may not have reaped it yet.
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

    // Prepare the instance before restoring its state and creating its editor, like a DAW does.
    // JUCE only initializes an Audio Unit in prepareToPlay,
    // and some plugins, such as iZotope Ozone 12, crash when their state is restored
    // or their view is attached to a window while the unit is still uninitialized.
    entry.editorInstance->prepareToPlay(editorSampleRate, editorBlockSize);

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
    launchOptions.resizable = editor->isResizable();
    launchOptions.useBottomRightCornerResizer = false;
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

/// Opens the chain editor window for adding, reordering, enabling, and removing plugins.
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

/// Opens the plugin scan dialog.
/// Reloads the known-plugins list from disk first, so changes from a CLI rescan are kept.
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

/// Captures live plugin state from every open editor into its entry without closing anything.
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

/// Closes the given entry's editor window, releases its instance's resources,
/// and destroys the instance without capturing state.
void PluginChain::closeEditorForEntry(PluginChainEntry& entry)
{
    if (entry.editorWindow != nullptr) {
        entry.editorWindow.deleteAndZero();
    }

    // The editor is deleted with its window above, so the instance has no editor when it is released.
    if (entry.editorInstance != nullptr) {
        entry.editorInstance->releaseResources();
    }

    entry.editorInstance = nullptr;
    entry.wasEditorOpen = false;
}

/// Returns true when any entry still has an editor window open.
bool PluginChain::anyEditorOpen() const noexcept
{
    return std::ranges::any_of(chain, [](const auto& entry) { return entry.wasEditorOpen; });
}

/// Decodes a base64 plugin state onto the entry, leaving the state empty when the text is not valid base64.
void PluginChain::restoreState(PluginChainEntry& entry, const juce::String& stateBase64)
{
    if (stateBase64.isEmpty()) {
        return;
    }

    juce::MemoryOutputStream stream(entry.state, false);
    if (!juce::Base64::convertFromBase64(stream, stateBase64)) {
        entry.state.reset();
    }
}

/// Parses one stored chain slot, or returns an empty optional when its plugin description cannot be loaded.
std::optional<PluginChainEntry> PluginChain::parseSlot(const juce::XmlElement& slotElement)
{
    const auto* descriptionElement = slotElement.getFirstChildElement();
    if (descriptionElement == nullptr) {
        return std::nullopt;
    }

    PluginChainEntry entry;
    if (!entry.description.loadFromXml(*descriptionElement)) {
        return std::nullopt;
    }

    entry.enabled = slotElement.getBoolAttribute(slotEnabledAttribute, true);
    restoreState(entry, slotElement.getStringAttribute(slotStateAttribute));
    return entry;
}

/// Parses a legacy single-plugin selection, or returns an empty optional when its description cannot be loaded.
std::optional<PluginChainEntry> PluginChain::parseLegacyEntry(
    const juce::String& pluginXml,
    const juce::String& stateBase64
)
{
    const auto element = juce::parseXML(pluginXml);
    if (element == nullptr) {
        return std::nullopt;
    }

    PluginChainEntry entry;
    if (!entry.description.loadFromXml(*element)) {
        return std::nullopt;
    }

    restoreState(entry, stateBase64);
    return entry;
}

/// Restores the chain from application settings.
/// When no chain is stored, migrates a legacy single-plugin selection into a one-slot chain
/// and removes the legacy keys.
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

/// Writes the chain to application settings, capturing the latest state from any open editors first.
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

/// Writes the known-plugins list to its KnownPlugins.xml file.
void PluginChain::persistKnownPluginList() const
{
    // Failures are logged by saveKnownPluginList, and the in-memory list stays usable.
    [[maybe_unused]] const auto saved = saveKnownPluginList(knownPluginList);
}

/// Restores the known-plugins list from its KnownPlugins.xml file,
/// first moving a list stored in the settings by older versions into that file.
void PluginChain::loadKnownPluginList()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    migrateKnownPluginList(*settings);
    audiobatch::loadKnownPluginList(*settings, knownPluginList);
}

/// Prepares a scan from the plugin manager before its first plugin is loaded.
/// Takes the plugin scan lock and reloads the known-plugins list, so the scan builds on the latest saved list.
/// Returns false, and tells the user, when another process such as a CLI rescan is scanning.
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

/// Broadcasts a change message and invokes the chain-changed callback, if one is set.
void PluginChain::notifyChainChanged()
{
    sendChangeMessage();

    if (chainChangedCallback != nullptr) {
        chainChangedCallback();
    }
}
}  // namespace audiobatch
