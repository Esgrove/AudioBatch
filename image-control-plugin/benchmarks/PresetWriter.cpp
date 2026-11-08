/// Writes an AudioBatch chain preset for each installed format of ImageControl, the VST3 and the Audio Unit,
/// with the same non-neutral settings, for comparing the two formats in AudioBatch.
/// It hosts each format the way AudioBatch does, through JUCE's plugin formats,
/// sets the parameters by name, and saves the hosted plugin's state and description into the preset.
///
/// Usage: ImageControlPresetWriter <output directory>

#include "Parameters.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
#include <span>

namespace esgrove::image_control::benchmark
{
namespace
{
/// Sample rate the plugins are created at. The preset does not depend on it.
constexpr double SAMPLE_RATE {48'000.0};
/// Block size the plugins are created at.
constexpr int BLOCK_SIZE {512};
/// Format version of AudioBatch's chain presets.
constexpr int PRESET_FORMAT_VERSION {1};
/// Characters of a parameter name to read, more than any ImageControl parameter name has.
constexpr int PARAMETER_NAME_LENGTH {100};

/// The kind of value a setting holds, which selects the range that normalizes it.
enum class SettingKind : std::uint8_t {
    gain,
    crossover,
    pan,
};

/// A parameter of ImageControl set by its name, with its plain value and the range that normalizes it.
struct Setting {
    /// The parameter name the plugin reports.
    const char* name {""};
    /// The plain value, a gain in dB, a frequency in Hz, or a pan in percent.
    float value {0.0F};
    /// The kind of value.
    SettingKind kind {SettingKind::gain};
};

/// Settings that keep every part of the processing busy: moved crossovers, a mono bass,
/// different mid and side gains in the other bands, and panned bands.
constexpr std::array SETTINGS {
    Setting {.name = "Low Crossover", .value = 150.0F, .kind = SettingKind::crossover},
    Setting {.name = "Mid Crossover", .value = 800.0F, .kind = SettingKind::crossover},
    Setting {.name = "High Crossover", .value = 6000.0F, .kind = SettingKind::crossover},
    Setting {.name = "Low: Side", .value = -60.0F},
    Setting {.name = "Low Mid: Side", .value = 3.0F},
    Setting {.name = "High Mid: Mid", .value = 1.0F},
    Setting {.name = "High Mid: Side", .value = -3.0F},
    Setting {.name = "High: Side", .value = 4.0F},
    Setting {.name = "Low Mid: Pan", .value = -30.0F, .kind = SettingKind::pan},
    Setting {.name = "High: Pan", .value = 20.0F, .kind = SettingKind::pan},
};

/// Returns the normalized value of a setting.
float normalizedValue(const Setting& setting)
{
    switch (setting.kind) {
        case SettingKind::gain:
            return gainRange(BAND_GAIN_MIN_DB, BAND_GAIN_MAX_DB).convertTo0to1(setting.value);
        case SettingKind::crossover:
            return crossoverRange().convertTo0to1(setting.value);
        case SettingKind::pan:
            return panRange().convertTo0to1(setting.value);
    }
    return 0.0F;
}

/// Returns whether a plugin identifier or path from a format's search belongs to ImageControl.
bool isImageControl(const juce::String& identifier)
{
    return identifier.contains("ImageControl") || identifier.contains("Egic");
}

/// Finds ImageControl in a format's default plugin locations, or returns nothing when it is not installed.
std::unique_ptr<juce::PluginDescription> findImageControl(juce::AudioPluginFormat& format)
{
    const auto identifiers = format.searchPathsForPlugins(format.getDefaultLocationsToSearch(), true, false);
    for (const auto& identifier : identifiers) {
        if (!isImageControl(identifier)) {
            continue;
        }
        juce::OwnedArray<juce::PluginDescription> types;
        format.findAllTypesForFile(types, identifier);
        for (const auto* type : types) {
            if (type->name == "ImageControl") {
                return std::make_unique<juce::PluginDescription>(*type);
            }
        }
    }
    return nullptr;
}

/// Sets every setting on a hosted plugin by parameter name, and returns false when one is missing.
bool applySettings(const juce::AudioPluginInstance& plugin)
{
    bool allFound = true;
    for (const auto& setting : SETTINGS) {
        bool found = false;
        for (auto* parameter : plugin.getParameters()) {
            if (parameter->getName(PARAMETER_NAME_LENGTH) == setting.name) {
                parameter->setValue(normalizedValue(setting));
                found = true;
            }
        }
        if (!found) {
            std::println(stderr, "Parameter not found: {}", setting.name);
            allFound = false;
        }
    }
    return allFound;
}

/// Writes the AudioBatch chain preset of one hosted plugin: one enabled slot with its description and state.
/// The description comes from the instance, which fills in the channel counts that a scan leaves empty.
bool writePreset(juce::AudioPluginInstance& plugin, const juce::File& file)
{
    const auto description = plugin.getPluginDescription();
    juce::MemoryBlock state;
    plugin.getStateInformation(state);

    juce::XmlElement preset("PLUGIN_CHAIN_PRESET");
    preset.setAttribute("name", file.getFileNameWithoutExtension());
    preset.setAttribute("version", PRESET_FORMAT_VERSION);
    auto* chain = preset.createNewChildElement("PLUGIN_CHAIN");
    auto* slot = chain->createNewChildElement("SLOT");
    slot->setAttribute("enabled", 1);
    slot->setAttribute("state", juce::Base64::toBase64(state.getData(), state.getSize()));
    slot->addChildElement(description.createXml().release());
    return preset.writeTo(file);
}
}  // namespace
}  // namespace esgrove::image_control::benchmark

/// Writes "ImageControl VST3.xml" and, on macOS, "ImageControl AU.xml" into the output directory.
int main(const int argc, char* argv[])
{
    const std::span arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() != 2) {
        std::println(stderr, "Usage: ImageControlPresetWriter <output directory>");
        return 2;
    }
    const juce::ScopedJuceInitialiser_GUI juce;
    const juce::File directory(juce::File::getCurrentWorkingDirectory().getChildFile(arguments[1]));
    if (!directory.createDirectory()) {
        std::println(stderr, "Cannot create {}", directory.getFullPathName().toStdString());
        return 1;
    }

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);
    int written = 0;
    for (auto* format : formats.getFormats()) {
        const auto description = esgrove::image_control::benchmark::findImageControl(*format);
        if (description == nullptr) {
            continue;
        }
        juce::String error;
        const auto plugin = formats.createPluginInstance(
            *description,
            esgrove::image_control::benchmark::SAMPLE_RATE,
            esgrove::image_control::benchmark::BLOCK_SIZE,
            error
        );
        if (plugin == nullptr) {
            std::println(stderr, "Cannot load the {}: {}", format->getName().toStdString(), error.toStdString());
            return 1;
        }
        if (!esgrove::image_control::benchmark::applySettings(*plugin)) {
            return 1;
        }
        const auto label = format->getName() == "AudioUnit" ? juce::String("AU") : format->getName();
        const auto file = directory.getChildFile("ImageControl " + label + ".xml");
        if (!esgrove::image_control::benchmark::writePreset(*plugin, file)) {
            std::println(stderr, "Cannot write {}", file.getFullPathName().toStdString());
            return 1;
        }
        std::println("{}", file.getFullPathName().toStdString());
        ++written;
    }
    if (written == 0) {
        std::println(stderr, "ImageControl is not installed. Run build.sh --install first.");
        return 1;
    }
    return 0;
}
