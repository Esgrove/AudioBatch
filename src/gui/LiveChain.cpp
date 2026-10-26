/// Implementation of LiveChain.
/// Covers the popup menu, slot editing operations by slot id, and window handling for the per-slot plugin editors,
/// the chain editor, and the plugin manager,
/// including capturing live plugin state from loaded slots before a run or save.
/// Also implements XML persistence of the chain in application settings,
/// with migration of the legacy single-plugin selection into a one-slot chain.

#include "gui/LiveChain.h"

#include "gui/components/CustomLookAndFeel.h"
#include "gui/components/PluginChainEditor.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>

namespace audiobatch
{
namespace
{
constexpr auto pluginChainPropertyKey {"pluginChain"};
constexpr auto legacySelectedPluginIdPropertyKey {"selectedPluginIdentifier"};
constexpr auto legacySelectedPluginStatePropertyKey {"selectedPluginState"};

constexpr auto chainXmlTag {"PLUGIN_CHAIN"};
constexpr auto slotXmlTag {"SLOT"};
constexpr auto slotEnabledAttribute {"enabled"};
constexpr auto slotStateAttribute {"state"};

constexpr int editChainMenuItemId {1};
constexpr int clearChainMenuItemId {2};
constexpr int scanMenuItemId {3};

/// Initial width of the generic parameter editor shown for plugins without a custom editor.
constexpr int genericEditorWidth {450};

/// Initial height of the generic parameter editor shown for plugins without a custom editor.
constexpr int genericEditorHeight {600};

/// Initial width of the plugin chain editor window.
constexpr int chainEditorWidth {560};

/// Initial height of the plugin chain editor window.
constexpr int chainEditorHeight {420};

/// Initial width of the plugin manager window that lists and scans plugins.
constexpr int pluginManagerWidth {720};

/// Initial height of the plugin manager window that lists and scans plugins.
constexpr int pluginManagerHeight {520};

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

/// Tears the live processor down, so erasing or clearing slots can never skip the teardown order.
LiveSlot::~LiveSlot()
{
    destroyProcessor();
}

/// Takes over the processor and the editor window of the other slot, which is left without either.
/// The editor window has to be taken explicitly, because a copied safe pointer would let both slots delete it.
LiveSlot::LiveSlot(LiveSlot&& other) noexcept :
    id(other.id),
    description(std::move(other.description)),
    state(std::move(other.state)),
    enabled(other.enabled),
    instance(std::move(other.instance)),
    listener(std::exchange(other.listener, nullptr)),
    editorWindow(std::exchange(other.editorWindow, nullptr)),
    preparationError(std::move(other.preparationError)),
    latencySamples(std::exchange(other.latencySamples, std::nullopt)),
    prepared(std::exchange(other.prepared, false)),
    wasEditorOpen(std::exchange(other.wasEditorOpen, false))
{}

/// Tears down the processor this slot holds before taking over the processor and editor window of the other slot.
LiveSlot& LiveSlot::operator=(LiveSlot&& other) noexcept
{
    if (this != &other) {
        destroyProcessor();
        id = other.id;
        description = std::move(other.description);
        state = std::move(other.state);
        enabled = other.enabled;
        instance = std::move(other.instance);
        listener = std::exchange(other.listener, nullptr);
        editorWindow = std::exchange(other.editorWindow, nullptr);
        preparationError = std::move(other.preparationError);
        latencySamples = std::exchange(other.latencySamples, std::nullopt);
        prepared = std::exchange(other.prepared, false);
        wasEditorOpen = std::exchange(other.wasEditorOpen, false);
    }

    return *this;
}

/// Gives the slot its live processor and registers the listener that observes its parameter and state changes.
/// The listener must outlive the slot, since the slot unregisters it again when the processor is destroyed.
void LiveSlot::attachInstance(
    std::unique_ptr<juce::AudioPluginInstance> newInstance,
    juce::AudioProcessorListener& newListener
)
{
    destroyProcessor();
    instance = std::move(newInstance);
    if (instance != nullptr) {
        instance->addListener(&newListener);
        listener = &newListener;
    }
}

/// Deletes only the editor window. The slot continues owning its live processor.
void LiveSlot::closeEditor()
{
    if (editorWindow != nullptr) {
        editorWindow.deleteAndZero();
    }

    wasEditorOpen = false;
}

/// Closes the editor before unregistering the listener, releasing the prepared resources, and destroying the processor.
/// The editor refers to the processor, and the plugin must not call the listener while it is being destroyed.
void LiveSlot::destroyProcessor()
{
    closeEditor();
    if (instance != nullptr) {
        if (listener != nullptr) {
            instance->removeListener(listener);
        }
        if (prepared) {
            instance->releaseResources();
        }
        instance = nullptr;
    }
    listener = nullptr;
    prepared = false;
    latencySamples.reset();
}

/// Restores the persisted chain from application settings.
/// The library must outlive the chain, since the slots create their processors through it.
/// Observes the longer-lived preview device manager to keep the chain editor's rate context accurate.
LiveChain::LiveChain(
    PluginLibrary& pluginLibrary,
    juce::ApplicationProperties& applicationProperties,
    juce::CriticalSection& previewRenderingLock,
    juce::AudioDeviceManager& audioDeviceManager
) :
    library(pluginLibrary),
    appProperties(applicationProperties),
    previewDeviceManager(audioDeviceManager),
    renderingLock(previewRenderingLock)
{
    previewDeviceManager.addChangeListener(this);
    loadPersistedChain();
}

/// Closes all editor, chain-editor, and scan windows and destroys their plugin instances.
LiveChain::~LiveChain()
{
    const juce::ScopedLock lock(renderingLock);
    utils::logDebug("Application shutdown: closing plugin windows");
    previewDeviceManager.removeChangeListener(this);

    persistChain();
    chain.clear();

    if (chainEditorWindow != nullptr) {
        chainEditorWindow.deleteAndZero();
    }

    if (scanWindow != nullptr) {
        scanWindow.deleteAndZero();
    }
    utils::logDebug("Application shutdown: releasing plugin chain members");
}

/// Returns the number of slots in the chain, including disabled ones.
int LiveChain::getNumSlots() const noexcept
{
    return static_cast<int>(chain.size());
}

/// Returns the number of enabled slots that identify a valid plugin.
int LiveChain::getNumEnabledValidSlots() const noexcept
{
    int count = 0;
    for (const auto& entry : chain) {
        if (entry.enabled && entry.description.fileOrIdentifier.isNotEmpty()) {
            ++count;
        }
    }

    return count;
}

/// Returns the ids of the slots in chain order.
std::vector<PluginSlotId> LiveChain::getSlotIds() const
{
    std::vector<PluginSlotId> slotIds;
    slotIds.reserve(chain.size());
    for (const auto& entry : chain) {
        slotIds.push_back(entry.id);
    }

    return slotIds;
}

/// Returns the position of the slot in the chain, or an empty optional when the slot was removed.
std::optional<int> LiveChain::getSlotIndex(const PluginSlotId slotId) const
{
    const auto found = std::ranges::find(chain, slotId, &LiveSlot::id);
    if (found == chain.end()) {
        return std::nullopt;
    }

    return static_cast<int>(found - chain.begin());
}

/// Returns the plugin description of the slot, or an empty description when the slot was removed.
juce::PluginDescription LiveChain::getSlotDescription(const PluginSlotId slotId) const
{
    const auto index = getSlotIndex(slotId);
    return index.has_value() ? chain[static_cast<std::size_t>(*index)].description : juce::PluginDescription {};
}

/// Returns true when the slot exists and is enabled.
bool LiveChain::isSlotEnabled(const PluginSlotId slotId) const
{
    const auto index = getSlotIndex(slotId);
    return index.has_value() && chain[static_cast<std::size_t>(*index)].enabled;
}

/// Returns the enabled plugins in chain order.
/// Captures every instantiated slot first, so a processing run always uses the latest live tweaks.
std::vector<EnabledChainPlugin> LiveChain::getEnabledPlugins()
{
    const juce::ScopedLock lock(renderingLock);
    // Include closed-editor instances because they continue processing live preview.
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

/// Returns a short human-readable summary of the chain for menu headers,
/// with disabled slots in parentheses, or an empty string when the chain is empty.
juce::String LiveChain::getChainSummary() const
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
void LiveChain::setChainChangedCallback(ChainChangedCallback callback)
{
    chainChangedCallback = std::move(callback);
}

/// Refreshes the device context of the chain editor without rebuilding rows or restarting preview.
void LiveChain::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (source == &previewDeviceManager) {
        sendChangeMessage();
    }
}

/// Captures closed editor state without destroying the live processors, and defers plugin latency notifications.
/// A window hidden by its close button counts as closed, because the dialog hides itself instead of deleting itself.
/// The preview's display-synchronized VBlank callback drives this metadata and window housekeeping.
/// Audio parameter changes use the shared processor directly and never wait for this update.
void LiveChain::updatePreviewStatus()
{
    // Runs on every display frame, so it only takes the rendering lock when an editor closed or a latency changed.
    // The slots are only changed on the message thread, so checking them needs no lock.
    const bool editorClosed = std::ranges::any_of(chain, [](const LiveSlot& entry) {
        return entry.wasEditorOpen && isEditorWindowClosed(entry);
    });

    if (!editorClosed && !latencyDirty.load()) {
        return;
    }

    bool stateCaptured = false;
    bool latencyChanged = false;

    {
        const juce::ScopedLock lock(renderingLock);

        // Watch for the user closing editor windows so we can capture plugin state.
        for (auto& entry : chain) {
            if (entry.wasEditorOpen && isEditorWindowClosed(entry) && reapClosedEditor(entry)) {
                stateCaptured = true;
            }
        }

        if (latencyDirty.exchange(false)) {
            for (auto& entry : chain) {
                if (entry.instance != nullptr && entry.prepared) {
                    const auto latency = static_cast<juce::int64>(juce::jmax(0, entry.instance->getLatencySamples()));
                    latencyChanged = latencyChanged || entry.latencySamples != latency;
                    entry.latencySamples = latency;
                }
            }
        }
    }

    // Saving the settings and the listeners can take a while, so they run after the rendering lock is released.
    if (stateCaptured) {
        persistChain();
        sendChangeMessage();
    }

    if (latencyChanged) {
        if (latencyChangedCallback) {
            latencyChangedCallback();
        }
        sendChangeMessage();
    }
}

/// Returns true when the entry's editor window has been closed by the user.
/// The dialog window hides itself when its close button is pressed instead of deleting itself,
/// so a hidden window also counts as closed.
bool LiveChain::isEditorWindowClosed(const LiveSlot& entry)
{
    return entry.editorWindow == nullptr || !entry.editorWindow->isVisible();
}

/// Captures plugin state from a closed editor and deletes only its window, preserving live audio.
/// Returns true when plugin state was captured.
bool LiveChain::reapClosedEditor(LiveSlot& entry)
{
    entry.wasEditorOpen = false;

    bool stateCaptured = false;
    if (entry.instance != nullptr && entry.prepared) {
        juce::MemoryBlock state;
        entry.instance->getStateInformation(state);

        if (state.getSize() > 0) {
            entry.state = std::move(state);
            stateCaptured = true;
        }
    }

    entry.closeEditor();
    return stateCaptured;
}

/// Pops up the plugin menu anchored to the given component.
/// The menu includes the chain summary, Edit Chain, the Add Plugin submenu, Clear Chain, and a Scan entry.
/// Uses desktop space so plugin selection submenus can extend beyond the anchor window.
void LiveChain::showMenu(juce::Component& anchor)
{
    juce::PopupMenu menu;

    const auto summary = getChainSummary();
    menu.addSectionHeader(
        summary.isEmpty() ? juce::String("No plugins in chain") : utils::format("Chain: {}", summary)
    );

    menu.addItem(editChainMenuItemId, "Edit Chain...");
    menu.addSeparator();

    juce::PopupMenu addSubmenu;
    const auto types = library.getKnownTypes();
    juce::KnownPluginList::addToMenu(addSubmenu, types, juce::KnownPluginList::sortByManufacturer);
    menu.addSubMenu("Add Plugin", addSubmenu, !types.isEmpty());

    menu.addItem(clearChainMenuItemId, "Clear Chain", !chain.empty());
    menu.addItem(scanMenuItemId, "Scan for Plugins...");

    const juce::WeakReference safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&anchor), [safeThis, types](const int result) {
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
    });
}

/// Appends the given plugin to the chain as an enabled slot and opens its editor.
void LiveChain::addPlugin(const juce::PluginDescription& description)
{
    PluginSlotId slotId {};
    {
        const juce::ScopedLock lock(renderingLock);
        LiveSlot entry;
        entry.description = description;
        slotId = appendSlot(std::move(entry));
    }

    persistChain();
    notifyChainChanged();

    // Automatically open the editor for a freshly added plugin so the user can dial it in right away.
    openEditorForSlot(slotId);
}

/// Gives the slot a new id and appends it to the chain.
/// The caller holds the rendering lock once the chain can be playing.
PluginSlotId LiveChain::appendSlot(LiveSlot entry)
{
    entry.id = PluginSlotId {++lastSlotId};
    chain.push_back(std::move(entry));
    return chain.back().id;
}

/// Removes the slot from the chain, closing its editor window if open.
void LiveChain::removeSlot(const PluginSlotId slotId)
{
    const auto index = getSlotIndex(slotId);
    if (!index.has_value()) {
        return;
    }

    {
        const juce::ScopedLock lock(renderingLock);
        chain.erase(chain.begin() + *index);

        // The live preview drops the destroyed processor before the audio callback can take the lock again.
        notifyChainChanged();
    }

    persistChain();
}

/// Moves a slot to a new position in the chain.
void LiveChain::moveSlot(const PluginSlotId slotId, const int toIndex)
{
    const auto index = getSlotIndex(slotId);
    if (!index.has_value() || !juce::isPositiveAndBelow(toIndex, getNumSlots()) || *index == toIndex) {
        return;
    }

    const auto fromIndex = *index;

    {
        const juce::ScopedLock lock(renderingLock);

        // Entries own their editor windows, so reordering carries any open editor along with its slot.
        if (fromIndex < toIndex) {
            std::rotate(chain.begin() + fromIndex, chain.begin() + fromIndex + 1, chain.begin() + toIndex + 1);
        } else {
            std::rotate(chain.begin() + toIndex, chain.begin() + fromIndex, chain.begin() + fromIndex + 1);
        }
    }

    persistChain();
    notifyChainChanged();
}

/// Enables or disables the slot without removing it from the chain.
void LiveChain::setSlotEnabled(const PluginSlotId slotId, const bool enabled)
{
    const auto index = getSlotIndex(slotId);
    if (!index.has_value()) {
        return;
    }

    auto& entry = chain[static_cast<std::size_t>(*index)];
    if (entry.enabled == enabled) {
        return;
    }

    {
        const juce::ScopedLock lock(renderingLock);
        entry.enabled = enabled;
    }

    persistChain();
    notifyChainChanged();
}

/// Removes all slots from the chain, closing any open editor windows.
void LiveChain::clearChain()
{
    if (chain.empty()) {
        return;
    }

    {
        const juce::ScopedLock lock(renderingLock);
        chain.clear();

        // The live preview drops the destroyed processors before the audio callback can take the lock again.
        notifyChainChanged();
    }

    persistChain();
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

    bool stateCaptured = false;
    bool prepared = false;

    {
        const juce::ScopedLock lock(renderingLock);
        // A window the user closed is only hidden, not deleted, and the next display update may not have reaped it.
        // Capture its state and destroy it now so a fresh editor can open below.
        stateCaptured = entry.wasEditorOpen && reapClosedEditor(entry);
        prepared = prepareLiveEntry(entry);
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
                utils::format("Could not prepare plugin:\n\n{}", entry.preparationError),
                "OK"
            ),
            nullptr
        );
        return;
    }

    // Creating the editor and its window can take a long time for a large plugin interface,
    // so it runs without the rendering lock. The processor is only destroyed on the message thread.
    auto* editor = entry.instance->createEditorAndMakeActive();

    if (editor == nullptr) {
        // No custom editor. Fall back to the generic parameter editor.
        editor = new juce::GenericAudioProcessorEditor(*entry.instance);
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
    }
    sendChangeMessage();
}

/// Opens the chain editor window for adding, reordering, enabling, and removing plugins.
void LiveChain::showChainEditor()
{
    const auto inspectedChain = preparePreview(previewSampleRate, previewBlockSize, previewChannels, true);
    if (inspectedChain.preparation.errorMessage.isNotEmpty()) {
        utils::logWarn("Plugin latency inspection: {}", inspectedChain.preparation.errorMessage);
    }
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

/// Opens the plugin manager window that lists the known plugins and scans for new ones.
void LiveChain::showScanWindow()
{
    if (scanWindow != nullptr) {
        // The window hides itself when closed, so reopening only needs to make it visible again.
        scanWindow->setVisible(true);
        scanWindow->toFront(true);
        return;
    }

    auto listComponent = library.createScanComponent();
    listComponent->setSize(pluginManagerWidth, pluginManagerHeight);

    juce::DialogWindow::LaunchOptions launchOptions;
    launchOptions.dialogTitle = "Plugin Manager";
    launchOptions.content.setOwned(listComponent.release());
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

/// Captures all instantiated slots, including processors whose editor windows have closed.
void LiveChain::captureOpenEditorStates()
{
    for (auto& entry : chain) {
        if (entry.instance != nullptr && entry.prepared) {
            juce::MemoryBlock state;
            entry.instance->getStateInformation(state);

            if (state.getSize() > 0) {
                entry.state = std::move(state);
            }
        }
    }
}

/// Decodes a base64 plugin state onto the entry, leaving the state empty when the text is not valid base64.
void LiveChain::restoreState(LiveSlot& entry, const juce::String& stateBase64)
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
std::optional<LiveSlot> LiveChain::parseSlot(const juce::XmlElement& slotElement)
{
    const auto* descriptionElement = slotElement.getFirstChildElement();
    if (descriptionElement == nullptr) {
        return std::nullopt;
    }

    LiveSlot entry;
    if (!entry.description.loadFromXml(*descriptionElement)) {
        return std::nullopt;
    }

    entry.enabled = slotElement.getBoolAttribute(slotEnabledAttribute, true);
    restoreState(entry, slotElement.getStringAttribute(slotStateAttribute));
    return entry;
}

/// Parses a legacy single-plugin selection, or returns an empty optional when its description cannot be loaded.
std::optional<LiveSlot> LiveChain::parseLegacyEntry(const juce::String& pluginXml, const juce::String& stateBase64)
{
    const auto element = juce::parseXML(pluginXml);
    if (element == nullptr) {
        return std::nullopt;
    }

    LiveSlot entry;
    if (!entry.description.loadFromXml(*element)) {
        return std::nullopt;
    }

    restoreState(entry, stateBase64);
    return entry;
}

/// Restores the chain from application settings.
/// When no chain is stored, migrates a legacy single-plugin selection into a one-slot chain
/// and removes the legacy keys.
void LiveChain::loadPersistedChain()
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
                appendSlot(std::move(*entry));
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

    appendSlot(std::move(*entry));
    persistChain();

    settings->removeValue(legacySelectedPluginIdPropertyKey);
    settings->removeValue(legacySelectedPluginStatePropertyKey);
    settings->saveIfNeeded();
}

/// Writes the chain to application settings, capturing the latest state from any open editors first.
void LiveChain::persistChain()
{
    auto* settings = appProperties.getUserSettings();
    if (settings == nullptr) {
        return;
    }

    {
        // Include the latest tweaks from any open editors in the persisted state.
        // Capturing calls into the live processors, which is serialized with the audio callback,
        // while building and saving the settings below only reads the slots and runs without the lock.
        const juce::ScopedLock lock(renderingLock);
        captureOpenEditorStates();
    }

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

/// Broadcasts a change message and invokes the chain-changed callback, if one is set.
void LiveChain::notifyChainChanged()
{
    ++revision;
    sendChangeMessage();

    if (chainChangedCallback != nullptr) {
        chainChangedCallback();
    }
}
}  // namespace audiobatch
