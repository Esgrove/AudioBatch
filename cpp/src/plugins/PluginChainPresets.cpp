/// Implementation of the saved chain XML and the chain preset files.
/// A chain is a PLUGIN_CHAIN element with one SLOT child per slot in chain order.
/// Each slot holds its enabled flag, its plugin state as base64, and the plugin description XML as its child.
/// A preset file wraps that element in a PLUGIN_CHAIN_PRESET element with the preset name and the format version,
/// and is written through a temporary file, so a failed save never leaves a partial preset behind.

#include "plugins/PluginChainPresets.h"

#include "utils/Log.h"
#include "utils/StringFormat.h"

#include <algorithm>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace audiobatch::plugin_presets
{
namespace
{
constexpr auto chainXmlTag {"PLUGIN_CHAIN"};
constexpr auto slotXmlTag {"SLOT"};
constexpr auto slotEnabledAttribute {"enabled"};
constexpr auto slotStateAttribute {"state"};

constexpr auto presetXmlTag {"PLUGIN_CHAIN_PRESET"};
constexpr auto presetNameAttribute {"name"};
constexpr auto presetVersionAttribute {"version"};
/// Version of the preset file format, raised when a change would make older versions misread the file.
constexpr int presetFormatVersion {1};

/// Parses one stored slot, or returns an empty optional when its plugin description cannot be loaded.
std::optional<SavedChainSlot> parseSlot(const juce::XmlElement& slotElement)
{
    const auto* descriptionElement = slotElement.getFirstChildElement();
    if (descriptionElement == nullptr) {
        return std::nullopt;
    }

    SavedChainSlot slot;
    if (!slot.description.loadFromXml(*descriptionElement)) {
        return std::nullopt;
    }

    slot.enabled = slotElement.getBoolAttribute(slotEnabledAttribute, true);
    slot.state = decodeState(slotElement.getStringAttribute(slotStateAttribute));
    return slot;
}
}  // namespace

/// Builds the PLUGIN_CHAIN element of the slots in chain order.
/// Slots whose description cannot be written are left out, and empty plugin states are not stored.
std::unique_ptr<juce::XmlElement> createChainXml(const std::span<const SavedChainSlot> slots)
{
    auto chainElement = std::make_unique<juce::XmlElement>(chainXmlTag);

    for (const auto& slot : slots) {
        auto descriptionElement = slot.description.createXml();
        if (descriptionElement == nullptr) {
            continue;
        }

        auto* slotElement = chainElement->createNewChildElement(slotXmlTag);
        slotElement->setAttribute(slotEnabledAttribute, static_cast<int>(slot.enabled));

        if (slot.state.getSize() > 0) {
            slotElement->setAttribute(
                slotStateAttribute, juce::Base64::toBase64(slot.state.getData(), slot.state.getSize())
            );
        }

        slotElement->addChildElement(descriptionElement.release());
    }

    return chainElement;
}

/// Reads the slots of a PLUGIN_CHAIN element in chain order, or returns an empty optional for any other element.
/// Slots whose plugin description cannot be loaded are skipped, so one damaged slot does not lose the rest.
std::optional<std::vector<SavedChainSlot>> parseChainXml(const juce::XmlElement& element)
{
    if (!element.hasTagName(chainXmlTag)) {
        return std::nullopt;
    }

    std::vector<SavedChainSlot> slots;
    for (const auto* slotElement : element.getChildWithTagNameIterator(slotXmlTag)) {
        if (auto slot = parseSlot(*slotElement); slot.has_value()) {
            slots.push_back(std::move(*slot));
        }
    }

    return slots;
}

/// Decodes a base64 plugin state, returning an empty block when the text is empty or not valid base64.
juce::MemoryBlock decodeState(const juce::String& stateBase64)
{
    juce::MemoryBlock state;
    if (stateBase64.isEmpty()) {
        return state;
    }

    // The stream resizes the block to what it wrote when it is destroyed, so it must be gone before a reset.
    bool decoded = false;
    {
        juce::MemoryOutputStream stream(state, false);
        decoded = juce::Base64::convertFromBase64(stream, stateBase64);
    }
    if (!decoded) {
        state.reset();
    }
    return state;
}

/// Returns the preset folder next to the given application settings file.
/// Tests that point the settings at a temporary folder get a temporary preset folder with it.
juce::File getPresetDirectory(const juce::File& settingsFile)
{
    return settingsFile.getSiblingFile(presetFolderName);
}

/// Turns a preset name into a name that is valid as a file name on every platform.
/// Characters that file systems reject are removed, along with surrounding spaces and dots.
/// Returns an empty string when nothing usable is left.
juce::String sanitizePresetName(const juce::String& name)
{
    return juce::File::createLegalFileName(name.trim()).trimCharactersAtStart(" .").trimCharactersAtEnd(" .");
}

/// Returns the file of the named preset in the folder, or an empty optional when the name has no usable characters.
std::optional<juce::File> getPresetFile(const juce::File& directory, const juce::String& name)
{
    const auto fileName = sanitizePresetName(name);
    if (fileName.isEmpty()) {
        return std::nullopt;
    }

    return directory.getChildFile(fileName + presetFileExtension);
}

/// Lists the preset files in the folder, sorted by name, ignoring case.
/// Returns an empty list when the folder does not exist yet.
/// Hidden files are left out, such as the temporary file of a save that was interrupted.
/// The files are not parsed, so a damaged preset is listed and reports its problem when it is loaded.
std::vector<ChainPresetInfo> listPresets(const juce::File& directory)
{
    std::vector<ChainPresetInfo> presets;
    if (!directory.isDirectory()) {
        return presets;
    }

    for (const auto& entry : juce::RangedDirectoryIterator(
             directory,
             false,
             utils::format("*{}", presetFileExtension),
             juce::File::findFiles | juce::File::ignoreHiddenFiles
         ))
    {
        if (entry.getFile().getFileName().startsWithChar('.')) {
            continue;
        }
        const auto& file = entry.getFile();
        presets.push_back(
            {.name = file.getFileNameWithoutExtension(), .file = file, .modified = entry.getModificationTime()}
        );
    }

    std::ranges::sort(presets, [](const ChainPresetInfo& first, const ChainPresetInfo& second) {
        return first.name.compareNatural(second.name) < 0;
    });
    return presets;
}

/// Saves the slots as the named preset in the folder, creating the folder when needed,
/// and replaces a preset of the same name.
/// Returns the preset file, or the reason it could not be written.
std::expected<juce::File, juce::String> savePreset(
    const juce::File& directory,
    const juce::String& name,
    const std::span<const SavedChainSlot> slots
)
{
    const auto file = getPresetFile(directory, name);
    if (!file.has_value()) {
        return std::unexpected(juce::String("Enter a preset name"));
    }

    if (const auto result = directory.createDirectory(); result.failed()) {
        return std::unexpected(
            utils::format(
                "Could not create the preset folder {}: {}",
                directory.getFullPathName().quoted(),
                result.getErrorMessage()
            )
        );
    }

    juce::XmlElement presetElement(presetXmlTag);
    presetElement.setAttribute(presetNameAttribute, file->getFileNameWithoutExtension());
    presetElement.setAttribute(presetVersionAttribute, presetFormatVersion);
    presetElement.addChildElement(createChainXml(slots).release());

    // The hidden temporary file keeps an interrupted save out of the preset list,
    // which a visible file with the preset extension would join.
    const juce::TemporaryFile temporaryFile(*file, juce::TemporaryFile::useHiddenFile);
    {
        juce::FileOutputStream output(temporaryFile.getFile());
        if (!output.openedOk()) {
            return std::unexpected(utils::format("Could not write the preset {}", file->getFullPathName().quoted()));
        }
        presetElement.writeTo(output);
        output.flush();
        if (output.getStatus().failed()) {
            return std::unexpected(utils::format("Could not write the preset {}", file->getFullPathName().quoted()));
        }
    }
    if (!temporaryFile.overwriteTargetFileWithTemporary()) {
        return std::unexpected(utils::format("Could not replace the preset {}", file->getFullPathName().quoted()));
    }

    utils::logInfo("Saved plugin chain preset {}", file->getFullPathName().quoted());
    return *file;
}

/// Reads the slots of a preset file in chain order, or returns the reason the file cannot be used.
/// A file from a newer format version is refused instead of being read partially.
std::expected<std::vector<SavedChainSlot>, juce::String> loadPreset(const juce::File& file)
{
    if (!file.existsAsFile()) {
        return std::unexpected(utils::format("The preset {} does not exist", file.getFullPathName().quoted()));
    }

    const auto element = juce::parseXML(file);
    if (element == nullptr || !element->hasTagName(presetXmlTag)) {
        return std::unexpected(utils::format("{} is not a plugin chain preset", file.getFileName().quoted()));
    }

    if (element->getIntAttribute(presetVersionAttribute, presetFormatVersion) > presetFormatVersion) {
        return std::unexpected(
            utils::format("{} was saved by a newer version of AudioBatch", file.getFileName().quoted())
        );
    }

    const auto* chainElement = element->getChildByName(chainXmlTag);
    auto slots = chainElement != nullptr ? parseChainXml(*chainElement) : std::nullopt;
    if (!slots.has_value()) {
        return std::unexpected(utils::format("{} has no plugin chain", file.getFileName().quoted()));
    }

    return std::move(*slots);
}

/// Deletes a preset file, returning whether it is gone.
bool deletePreset(const juce::File& file)
{
    return !file.existsAsFile() || file.deleteFile();
}
}  // namespace audiobatch::plugin_presets
