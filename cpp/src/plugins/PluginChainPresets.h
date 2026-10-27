/// Saved plugin chains and named chain presets.
/// Declares SavedChainSlot, the stored form of one chain slot with its plugin, enabled flag, and plugin state,
/// and the functions in the plugin_presets namespace that turn a chain into XML and back.
/// The live chain stores its slots in the application settings with them,
/// and the presets are the same XML in one file per preset in the Chain Presets folder next to the settings file.

#pragma once

#include <JuceHeader.h>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace audiobatch
{
/// The stored form of one chain slot: which plugin it holds, whether it is enabled, and the plugin's saved state.
struct SavedChainSlot {
    juce::PluginDescription description;
    /// The plugin's getStateInformation data, empty when the plugin keeps its defaults.
    juce::MemoryBlock state;
    bool enabled {true};
};

/// A preset file found in the preset folder.
struct ChainPresetInfo {
    /// The name shown to the user, which is the file name without its extension.
    juce::String name;
    juce::File file;
    juce::Time modified;
};

namespace plugin_presets
{
/// File extension of the preset files.
constexpr auto presetFileExtension {".xml"};
/// Name of the preset folder, which sits next to the application settings file.
constexpr auto presetFolderName {"Chain Presets"};

[[nodiscard]] std::unique_ptr<juce::XmlElement> createChainXml(std::span<const SavedChainSlot> slots);
[[nodiscard]] std::optional<std::vector<SavedChainSlot>> parseChainXml(const juce::XmlElement& element);
[[nodiscard]] juce::MemoryBlock decodeState(const juce::String& stateBase64);

[[nodiscard]] juce::File getPresetDirectory(const juce::File& settingsFile);
[[nodiscard]] juce::String sanitizePresetName(const juce::String& name);
[[nodiscard]] std::optional<juce::File> getPresetFile(const juce::File& directory, const juce::String& name);
[[nodiscard]] std::vector<ChainPresetInfo> listPresets(const juce::File& directory);

[[nodiscard]] std::expected<juce::File, juce::String> savePreset(
    const juce::File& directory,
    const juce::String& name,
    std::span<const SavedChainSlot> slots
);
[[nodiscard]] std::expected<std::vector<SavedChainSlot>, juce::String> loadPreset(const juce::File& file);
[[nodiscard]] bool deletePreset(const juce::File& file);
}  // namespace plugin_presets
}  // namespace audiobatch
