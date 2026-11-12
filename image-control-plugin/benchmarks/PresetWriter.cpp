/// Writes native AudioBatch chain presets from installed ImageControl formats or one explicit build-tree VST3.
/// It hosts each format the way AudioBatch does, through JUCE's plugin formats,
/// sets the parameters by name, and saves the hosted plugin's state and description into the preset.
///
/// Usage: ImageControlPresetWriter <output directory>

#include "Parameters.h"
#include <fmt/base.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>

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

/// Selects explicit integration artifacts without changing installed plugin discovery or application storage.
struct Options {
    /// Optional exact VST3 bundle, bypassing installed format searches when supplied.
    juce::String bundle;
    /// Destination for the native chain presets.
    juce::String directory;
    /// Saves the plugin's actual initialized neutral state instead of applying comparison controls.
    bool neutral {false};
};

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
    /// Matching Rust controller title, independent of the C++ parameter ids and title scheme.
    const char* rustName {""};
    /// The plain value, a gain in dB, a frequency in Hz, or a pan in percent.
    float value {0.0F};
    /// The kind of value.
    SettingKind kind {SettingKind::gain};
};

/// Settings that keep every part of the processing busy: moved crossovers, a mono bass,
/// different mid and side gains in the other bands, and panned bands.
constexpr std::array SETTINGS {
    Setting {.name = "Low Crossover", .rustName = "Crossover 1", .value = 150.0F, .kind = SettingKind::crossover},
    Setting {.name = "Mid Crossover", .rustName = "Crossover 2", .value = 800.0F, .kind = SettingKind::crossover},
    Setting {.name = "High Crossover", .rustName = "Crossover 3", .value = 6000.0F, .kind = SettingKind::crossover},
    Setting {.name = "Low: Side", .rustName = "Band 1 Side", .value = -60.0F},
    Setting {.name = "Low Mid: Side", .rustName = "Band 2 Side", .value = 3.0F},
    Setting {.name = "High Mid: Mid", .rustName = "Band 3 Mid", .value = 1.0F},
    Setting {.name = "High Mid: Side", .rustName = "Band 3 Side", .value = -3.0F},
    Setting {.name = "High: Side", .rustName = "Band 4 Side", .value = 4.0F},
    Setting {.name = "Low Mid: Pan", .rustName = "Band 2 Pan", .value = -30.0F, .kind = SettingKind::pan},
    Setting {.name = "High: Pan", .rustName = "Band 4 Pan", .value = 20.0F, .kind = SettingKind::pan},
};

/// Parses integration options before creating native windows, directories, or plugin instances.
Options parseArguments(const std::span<char*> arguments)
{
    Options options;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const juce::String argument(arguments[index]);
        if (argument == "-n" || argument == "--neutral") {
            options.neutral = true;
        } else if (argument == "-b" || argument == "--bundle") {
            if (++index >= arguments.size()) {
                throw std::runtime_error("--bundle requires an absolute VST3 path");
            }
            options.bundle = arguments[index];
            if (!juce::File::isAbsolutePath(options.bundle) || !options.bundle.endsWithIgnoreCase(".vst3")) {
                throw std::runtime_error("--bundle requires an absolute VST3 path");
            }
        } else if (argument.startsWithChar('-') || options.directory.isNotEmpty()) {
            throw std::runtime_error("Unknown option or multiple output directories");
        } else {
            options.directory = argument;
        }
    }
    if (options.directory.isEmpty()) {
        throw std::runtime_error("Give an output directory");
    }
    return options;
}

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
std::unique_ptr<juce::PluginDescription> findImageControl(juce::AudioPluginFormat& format, const juce::String& bundle)
{
    if (bundle.isNotEmpty() && format.getName() != "VST3") {
        return nullptr;
    }
    const auto identifiers = bundle.isEmpty()
        ? format.searchPathsForPlugins(format.getDefaultLocationsToSearch(), true, false)
        : juce::StringArray {bundle};
    for (const auto& identifier : identifiers) {
        if (!isImageControl(identifier)) {
            continue;
        }
        juce::OwnedArray<juce::PluginDescription> types;
        format.findAllTypesForFile(types, identifier);
        for (const auto* type : types) {
            if (type->name == "ImageControl" || (bundle.isNotEmpty() && type->name == "ImageControl RS")) {
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
    const bool rust = plugin.getPluginDescription().name == "ImageControl RS";
    for (const auto& setting : SETTINGS) {
        bool found = false;
        for (auto* parameter : plugin.getParameters()) {
            if (parameter->getName(PARAMETER_NAME_LENGTH) == (rust ? setting.rustName : setting.name)) {
                parameter->setValue(
                    rust ? parameter->getValueForText(juce::String(setting.value)) : normalizedValue(setting)
                );
                found = true;
            }
        }
        if (!found) {
            fmt::print(stderr, "Parameter not found: {}\n", setting.name);
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
    if (std::ranges::any_of(
            arguments.subspan(1),
            [](const char* argument) { return juce::String(argument) == "-h" || juce::String(argument) == "--help"; }
        ))
    {
        fmt::print("Usage: ImageControlPresetWriter [-b|--bundle <absolute.vst3>] [-n|--neutral] <output directory>\n");
        return 0;
    }
    esgrove::image_control::benchmark::Options options;
    try {
        options = esgrove::image_control::benchmark::parseArguments(arguments);
    } catch (const std::exception& error) {
        fmt::print(stderr, "{}\n", error.what());
        return 2;
    }
    const juce::ScopedJuceInitialiser_GUI juce;
    const juce::File directory(juce::File::getCurrentWorkingDirectory().getChildFile(options.directory));
    if (!directory.createDirectory()) {
        fmt::print(stderr, "Cannot create {}\n", directory.getFullPathName().toStdString());
        return 1;
    }

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);
    int written = 0;
    for (auto* format : formats.getFormats()) {
        const auto description = esgrove::image_control::benchmark::findImageControl(*format, options.bundle);
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
            fmt::print(stderr, "Cannot load the {}: {}\n", format->getName().toStdString(), error.toStdString());
            return 1;
        }
        if (!options.neutral && !esgrove::image_control::benchmark::applySettings(*plugin)) {
            return 1;
        }
        const auto label = format->getName() == "AudioUnit" ? juce::String("AU") : format->getName();
        const auto file = directory.getChildFile(description->name + " " + label + ".xml");
        if (!esgrove::image_control::benchmark::writePreset(*plugin, file)) {
            fmt::print(stderr, "Cannot write {}\n", file.getFullPathName().toStdString());
            return 1;
        }
        fmt::print("{}\n", file.getFullPathName().toStdString());
        ++written;
    }
    if (written == 0) {
        fmt::print(stderr, "No matching ImageControl format found in the selected bundle or installed locations.\n");
        return 1;
    }
    return 0;
}
