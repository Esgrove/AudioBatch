/// The chain presets of LiveChain.
/// Covers saving the chain as a named preset, replacing the chain with a preset, and deleting presets,
/// with the dialogs that ask for a name, confirm a replace or a delete, and report failures,
/// and the presets submenu that the chain menu and the main menu bar share.

#include "gui/LiveChain.h"
#include "utils/AppInfo.h"
#include "utils/Log.h"
#include "utils/StringFormat.h"

namespace audiobatch
{
namespace
{
/// Offsets of the presets submenu items from its first item id.
constexpr int savePresetMenuOffset {0};
constexpr int showPresetFolderMenuOffset {1};
constexpr int firstPresetMenuOffset {2};

/// Name of the text field in the dialog that asks for a preset name.
constexpr auto presetNameFieldName {"presetName"};

/// Tells the user why a preset operation failed.
void showPresetError(const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOk(juce::MessageBoxIconType::WarningIcon, title, message, "OK"), nullptr
    );
}
}  // namespace

/// Returns the folder that holds the chain presets, next to the application settings file.
juce::File LiveChain::getPresetDirectory() const
{
    if (const auto* settings = appProperties.getUserSettings(); settings != nullptr) {
        return plugin_presets::getPresetDirectory(settings->getFile());
    }
    return plugin_presets::getPresetDirectory(utils::createApplicationSettingsOptions().getDefaultFile());
}

/// Returns the saved chain presets, sorted by name.
std::vector<ChainPresetInfo> LiveChain::getPresets() const
{
    return plugin_presets::listPresets(getPresetDirectory());
}

/// Saves the chain with the latest state of every instantiated slot as the named preset,
/// replacing a preset of the same name.
/// Returns the preset file, or the reason it could not be saved.
std::expected<juce::File, juce::String> LiveChain::saveAsPreset(const juce::String& name)
{
    const auto slots = captureSavedSlots();
    auto result = plugin_presets::savePreset(getPresetDirectory(), name, slots);
    if (result.has_value()) {
        presetName = result->getFileNameWithoutExtension();
        presetRevision = revision;
        sendChangeMessage();
    }
    return result;
}

/// Returns the name of the preset the chain was last loaded from or saved as,
/// or an empty string once a slot was added, removed, moved, enabled, or disabled since then.
juce::String LiveChain::getCurrentPresetName() const
{
    return revision == presetRevision ? presetName : juce::String();
}

/// Builds the presets submenu with ids counted from the given first id:
/// saving the chain as a preset, showing the preset folder, and loading each saved preset.
/// handlePresetsMenuItem decodes the ids with the same first id.
juce::PopupMenu LiveChain::createPresetsMenu(const int firstItemId) const
{
    juce::PopupMenu menu;
    menu.addItem(firstItemId + savePresetMenuOffset, "Save Chain as Preset...", !chain.empty());
    menu.addItem(firstItemId + showPresetFolderMenuOffset, "Show Presets Folder");

    const auto presets = getPresets();
    if (!presets.empty()) {
        menu.addSeparator();
        menu.addSectionHeader("Load Preset");
    }

    const auto currentName = getCurrentPresetName();
    presetMenuFiles.clear();
    for (std::size_t index = 0; index < presets.size(); ++index) {
        const auto& preset = presets[index];
        menu.addItem(
            firstItemId + firstPresetMenuOffset + static_cast<int>(index), preset.name, true, preset.name == currentName
        );
        presetMenuFiles.push_back(preset.file);
    }

    return menu;
}

/// Runs the presets submenu item with the given id, and returns false for an id outside the submenu.
/// A preset item loads the file the menu was built with, so presets added or removed since then cannot shift it,
/// and a preset removed since then is not loaded.
bool LiveChain::handlePresetsMenuItem(const int itemId, const int firstItemId)
{
    const auto offset = itemId - firstItemId;
    if (offset < 0) {
        return false;
    }

    if (offset == savePresetMenuOffset) {
        promptSavePreset();
        return true;
    }

    if (offset == showPresetFolderMenuOffset) {
        showPresetFolder();
        return true;
    }

    const auto presetIndex = static_cast<std::size_t>(offset - firstPresetMenuOffset);
    if (presetIndex >= presetMenuFiles.size()) {
        return false;
    }

    if (const auto file = presetMenuFiles[presetIndex]; file.existsAsFile()) {
        loadPresetAndReport(file);
    }
    return true;
}

/// Asks for a preset name and saves the chain under it, suggesting the name of the current preset.
/// Asks again before replacing an existing preset of the same name.
void LiveChain::promptSavePreset()
{
    auto window = std::make_unique<juce::AlertWindow>(
        "Save Chain Preset",
        "Saves the plugins of the chain, their order, and the settings of each plugin.",
        juce::MessageBoxIconType::NoIcon
    );
    window->addTextEditor(presetNameFieldName, getCurrentPresetName(), "Preset name:");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    // The window runs the callback before it deletes itself, so the callback can still read the name.
    const juce::Component::SafePointer<juce::AlertWindow> safeWindow(window.get());
    const juce::WeakReference safeThis(this);
    auto* modalWindow = window.release();
    modalWindow->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, safeWindow](const int result) {
            if (safeThis == nullptr || safeWindow == nullptr || result != 1) {
                return;
            }
            safeThis->savePresetConfirmingReplace(safeWindow->getTextEditorContents(presetNameFieldName));
        }),
        true
    );
}

/// Saves the chain as the named preset, first asking whether to replace a preset that already has the name.
void LiveChain::savePresetConfirmingReplace(const juce::String& name)
{
    const auto file = plugin_presets::getPresetFile(getPresetDirectory(), name);
    if (!file.has_value()) {
        showPresetError("Preset Not Saved", "Enter a name for the preset.");
        return;
    }

    if (!file->existsAsFile()) {
        savePresetAndReport(name);
        return;
    }

    const juce::WeakReference safeThis(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::QuestionIcon,
            "Replace Preset",
            utils::format(
                "A preset named {} already exists. Replace it?", file->getFileNameWithoutExtension().quoted()
            ),
            "Replace",
            "Cancel"
        ),
        [safeThis, name](const int result) {
            if (safeThis != nullptr && result == 1) {
                safeThis->savePresetAndReport(name);
            }
        }
    );
}

/// Saves the chain as the named preset and tells the user when it fails.
void LiveChain::savePresetAndReport(const juce::String& name)
{
    if (const auto result = saveAsPreset(name); !result.has_value()) {
        utils::logWarn("Could not save plugin chain preset: {}", result.error());
        showPresetError("Preset Not Saved", result.error());
    }
}

/// Replaces the chain with a preset and tells the user when the preset cannot be read.
void LiveChain::loadPresetAndReport(const juce::File& file)
{
    if (const auto result = loadPreset(file); !result.has_value()) {
        showPresetError("Preset Not Loaded", result.error());
    }
}

/// Asks before deleting a preset file, and tells the user when it cannot be deleted.
void LiveChain::confirmDeletePreset(const ChainPresetInfo& preset)
{
    const juce::WeakReference safeThis(this);
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::WarningIcon,
            "Delete Preset",
            utils::format("Delete the preset {}?", preset.name.quoted()),
            "Delete",
            "Cancel"
        ),
        [safeThis, preset](const int result) {
            if (safeThis == nullptr || result != 1) {
                return;
            }
            if (!safeThis->deletePreset(preset.file)) {
                showPresetError(
                    "Preset Not Deleted", utils::format("Could not delete {}", preset.file.getFullPathName().quoted())
                );
            }
        }
    );
}

/// Opens the preset folder in the platform file manager, creating it first so there is always a folder to show.
void LiveChain::showPresetFolder() const
{
    const auto directory = getPresetDirectory();
    if (const auto result = directory.createDirectory(); result.failed()) {
        showPresetError("Presets Folder", result.getErrorMessage());
        return;
    }
    directory.startAsProcess();
}

/// Replaces the chain with the slots of a preset file, closing the editors of the current slots.
/// The new slots load their processors lazily, the same way a chain restored from the settings does,
/// so a plugin that is not installed shows as unavailable instead of failing the load.
/// Returns the reason when the file cannot be read, and then leaves the chain unchanged.
std::expected<void, juce::String> LiveChain::loadPreset(const juce::File& file)
{
    auto slots = plugin_presets::loadPreset(file);
    if (!slots.has_value()) {
        utils::logWarn("Could not load plugin chain preset: {}", slots.error());
        return std::unexpected(slots.error());
    }

    {
        const juce::ScopedLock lock(renderingLock);
        chain.clear();
        for (auto& savedSlot : *slots) {
            appendSlot(LiveSlot(std::move(savedSlot)));
        }

        // The live preview drops the destroyed processors before the audio callback can take the lock again.
        notifyChainChanged();
        presetName = file.getFileNameWithoutExtension();
        presetRevision = revision;
    }

    persistChain();
    utils::logInfo("Loaded plugin chain preset {}", file.getFullPathName().quoted());
    return {};
}

/// Deletes a preset file, returning whether it is gone.
bool LiveChain::deletePreset(const juce::File& file)
{
    const bool deleted = plugin_presets::deletePreset(file);
    if (deleted) {
        sendChangeMessage();
    }
    return deleted;
}
}  // namespace audiobatch
