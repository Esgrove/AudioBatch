/// Tests for the plugin chain presets and for processing through the repository's test VST3.
/// The preset tests save and load chains of plugins that are not installed, so they need no plugin at all.
/// The VST3 tests load the AudioBatch Test MS plugin that CMake builds from test-plugin/,
/// save its parameters in a preset, load the preset into a new chain, and process the committed fixtures with it.
/// Every output is null tested against the expected file that fixtures/plugin_expected.py computes independently,
/// and must cancel to the last 16-bit step, the same files the Rust tests compare against.

#include "analysis/AudioAnalysis.h"
#include "gui/LiveChain.h"
#include "plugins/PluginChainPresets.h"
#include "plugins/PluginLibrary.h"
#include "plugins/PluginProcessing.h"
#include "TestHelpers.h"
#include "TestSuites.h"
#include "utils/AudioFormatUtils.h"
#include "utils/StringFormat.h"

#include <array>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace audiobatch::tests
{
namespace
{
/// One processing case of the test plugin and the committed file it must produce.
struct PluginExpectation {
    const char* source;
    float gain;
    float width;
    /// Latency in samples, which the host must compensate for the output to cancel against the expected file.
    int latency;
    const char* expected;
};

/// The cases that fixtures/plugin_expected.py writes, which the Rust test-fixtures crate lists too.
constexpr std::array pluginExpectations {
    PluginExpectation {
        .source = "stereo_split_44100_16.wav",
        .gain = 0.5f,
        .width = 0.5f,
        .latency = 0,
        .expected = "plugin_expected/ms_gain_0.5_width_0.5_stereo_split_44100_16.wav",
    },
    PluginExpectation {
        .source = "stereo_split_44100_16.wav",
        .gain = 1.0f,
        .width = 0.0f,
        .latency = 0,
        .expected = "plugin_expected/ms_gain_1_width_0_stereo_split_44100_16.wav",
    },
    PluginExpectation {
        .source = "stereo_split_44100_16.wav",
        .gain = 1.0f,
        .width = 2.0f,
        .latency = 0,
        .expected = "plugin_expected/ms_gain_1_width_2_stereo_split_44100_16.wav",
    },
    PluginExpectation {
        .source = "sine_mono_44100_16.wav",
        .gain = 0.5f,
        .width = 1.0f,
        .latency = 0,
        .expected = "plugin_expected/ms_gain_0.5_width_1_sine_mono_44100_16.wav",
    },
    PluginExpectation {
        .source = "stereo_split_44100_16.wav",
        .gain = 0.5f,
        .width = 0.5f,
        .latency = 256,
        .expected = "plugin_expected/ms_gain_0.5_width_0.5_latency_256_stereo_split_44100_16.wav",
    },
};

/// Index of the gain parameter of the test plugin, and of the width and latency parameters after it.
constexpr int gainParameterIndex {0};
constexpr int widthParameterIndex {1};
constexpr int latencyParameterIndex {2};
/// Largest latency of the test plugin in samples, which maps linearly from zero.
constexpr float testLatencyMaximum {1024.0f};
/// Largest plain value of both test plugin parameters, which map linearly from zero.
constexpr float testParameterMaximum {2.0f};

/// Points the application settings at a folder inside the temporary directory,
/// which also places the preset folder there.
void useTemporarySettings(juce::ApplicationProperties& properties, const TemporaryDirectory& temporary)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "PluginPresetTests";
    options.filenameSuffix = ".xml";
    options.folderName = temporary.getChildFile("properties").getFullPathName();
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    properties.setStorageParameters(options);
}

/// Returns a slot for a plugin that is not installed, with the given name, state, and enabled flag.
SavedChainSlot makeUnavailableSlot(const juce::String& name, const juce::String& state, const bool enabled)
{
    SavedChainSlot slot;
    slot.description.name = name;
    slot.description.pluginFormatName = "Unavailable test format";
    slot.description.fileOrIdentifier = utils::format("not-installed/{}", name);
    slot.state = juce::MemoryBlock(state.toRawUTF8(), state.getNumBytesAsUTF8());
    slot.enabled = enabled;
    return slot;
}

/// Stores the slots as the persisted chain, so a live chain created next starts with them.
void storeChain(juce::PropertiesFile& settings, const std::vector<SavedChainSlot>& slots)
{
    settings.setValue("pluginChain", plugin_presets::createChainXml(slots)->toString());
}

class PluginPresetTests final : public juce::UnitTest
{
public:
    PluginPresetTests() : juce::UnitTest("Plugin chain presets", "AudioBatch") {}

    /// Runs the preset file, live chain preset, and test VST3 null tests.
    void runTest() override
    {
        testChainXml();
        testPresetFiles();
        testLiveChainPresets();
        testNullTest();
        testPluginProcessing();
    }

private:
    /// Checks that the null test cancels identical files, reports a difference,
    /// and finds the delay of a shifted copy through the cross-correlation.
    void testNullTest()
    {
        beginTest("The null test cancels identical files and reports differences and their lag");
        const TemporaryDirectory temporary;
        const auto source = fixtureFile("stereo_split_44100_16.wav");
        expect(nullTestFiles(source, source).cancels());

        const auto different = nullTestFiles(source, fixtureFile(pluginExpectations.front().expected));
        expect(!different.cancels());
        expect(different.differingSamples > 0);
        expect(different.peakDifference > 0);
        expectEquals(different.bestLag, 0);
        expect(different.describe().contains("dBFS"));

        constexpr int delaySamples {10};
        const auto delayed = temporary.getChildFile("delayed.wav");
        expect(writeDelayedCopy(source, delayed, delaySamples));
        const auto shifted = nullTestFiles(source, delayed);
        expect(!shifted.cancels());
        expectEquals(shifted.bestLag, delaySamples);

        const auto unreadable = nullTestFiles(source, temporary.getChildFile("missing.wav"));
        expect(!unreadable.readable);
        expect(!unreadable.cancels());
    }

    /// Writes the source delayed by the given number of samples to a 16-bit WAV file of the same length.
    static bool writeDelayedCopy(const juce::File& source, const juce::File& destination, const int delaySamples)
    {
        auto& formatManager = utils::threadLocalFormatManager();
        const std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(source));
        if (reader == nullptr) {
            return false;
        }

        const auto length = static_cast<int>(reader->lengthInSamples);
        const auto channels = static_cast<int>(reader->numChannels);
        juce::AudioBuffer<float> buffer(channels, length);
        buffer.clear();
        reader->read(&buffer, delaySamples, length - delaySamples, 0, true, true);

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> output = destination.createOutputStream();
        if (output == nullptr) {
            return false;
        }
        const auto writer = wavFormat.createWriterFor(
            output,
            juce::AudioFormatWriterOptions()
                .withSampleRate(reader->sampleRate)
                .withNumChannels(channels)
                .withBitsPerSample(16)
        );
        return writer != nullptr && writer->writeFromAudioSampleBuffer(buffer, 0, length);
    }

    /// Checks that the stored chain keeps the slot order, enabled flags, and states, and skips damaged slots.
    void testChainXml()
    {
        beginTest("Chain XML keeps the slot order, enabled flags, and plugin states");
        {
            const std::vector<SavedChainSlot> slots {
                makeUnavailableSlot("First", "first state", true),
                makeUnavailableSlot("Second", "second state", false),
                makeUnavailableSlot("Third", "", true),
            };
            const auto element = plugin_presets::createChainXml(slots);
            const auto parsed = plugin_presets::parseChainXml(*element);
            expect(parsed.has_value());
            if (!parsed.has_value()) {
                return;
            }

            expectEquals(static_cast<int>(parsed->size()), 3);
            for (std::size_t index = 0; index < juce::jmin(slots.size(), parsed->size()); ++index) {
                expectEquals((*parsed)[index].description.name, slots[index].description.name);
                expectEquals((*parsed)[index].description.fileOrIdentifier, slots[index].description.fileOrIdentifier);
                expect((*parsed)[index].state == slots[index].state);
                expect((*parsed)[index].enabled == slots[index].enabled);
            }
        }

        beginTest("Chain XML skips damaged slots and refuses other elements");
        {
            const std::vector<SavedChainSlot> slots {
                makeUnavailableSlot("Kept", "kept", true),
                makeUnavailableSlot("Also kept", "also kept", true),
            };
            auto element = plugin_presets::createChainXml(slots);
            element->getFirstChildElement()->deleteAllChildElements();
            element->getChildElement(1)->setAttribute("state", "not base64 at all!");

            const auto parsed = plugin_presets::parseChainXml(*element);
            expect(parsed.has_value());
            expectEquals(parsed.has_value() ? static_cast<int>(parsed->size()) : -1, 1);
            if (parsed.has_value() && !parsed->empty()) {
                expectEquals(parsed->front().description.name, juce::String("Also kept"));
                expectEquals(static_cast<int>(parsed->front().state.getSize()), 0);
            }

            expect(!plugin_presets::parseChainXml(juce::XmlElement("SOMETHING_ELSE")).has_value());
            expectEquals(static_cast<int>(plugin_presets::decodeState({}).getSize()), 0);
        }
    }

    /// Checks saving, listing, replacing, loading, and deleting preset files, and the errors for unusable files.
    void testPresetFiles()
    {
        beginTest("Preset names are sanitized into file names");
        {
            expectEquals(plugin_presets::sanitizePresetName("  Mastering  "), juce::String("Mastering"));
            expectEquals(plugin_presets::sanitizePresetName("Bus: A/B"), juce::String("Bus AB"));
            expectEquals(plugin_presets::sanitizePresetName(" .hidden. "), juce::String("hidden"));
            expect(plugin_presets::sanitizePresetName(" ... ").isEmpty());

            const juce::File directory("/presets");
            expect(!plugin_presets::getPresetFile(directory, "  ").has_value());
            expectEquals(
                plugin_presets::getPresetFile(directory, "Master").value_or(juce::File {}).getFileName(),
                juce::String("Master.xml")
            );
        }

        beginTest("Presets are saved, listed by name, replaced, loaded, and deleted");
        {
            const TemporaryDirectory temporary;
            const auto directory = temporary.getChildFile("Chain Presets");
            expect(plugin_presets::listPresets(directory).empty());

            const std::vector<SavedChainSlot> first {makeUnavailableSlot("Gain", "one", true)};
            const std::vector<SavedChainSlot> second {
                makeUnavailableSlot("Width", "two", false),
                makeUnavailableSlot("Gain", "three", true),
            };

            expect(!plugin_presets::savePreset(directory, " ", first).has_value());
            expect(plugin_presets::savePreset(directory, "preset 10", first).has_value());
            expect(plugin_presets::savePreset(directory, "preset 9", first).has_value());
            const auto saved = plugin_presets::savePreset(directory, "Alpha", first);
            expect(saved.has_value());

            const auto listed = plugin_presets::listPresets(directory);
            expectEquals(static_cast<int>(listed.size()), 3);
            if (listed.size() == 3) {
                expectEquals(listed[0].name, juce::String("Alpha"));
                expectEquals(listed[1].name, juce::String("preset 9"));
                expectEquals(listed[2].name, juce::String("preset 10"));
            }

            const auto replaced = plugin_presets::savePreset(directory, "Alpha", second);
            expect(replaced.has_value());
            expect(replaced.value_or(juce::File {}) == saved.value_or(juce::File {}));
            expectEquals(static_cast<int>(plugin_presets::listPresets(directory).size()), 3);

            const auto loaded = plugin_presets::loadPreset(replaced.value_or(juce::File {}));
            expect(loaded.has_value());
            expectEquals(loaded.has_value() ? static_cast<int>(loaded->size()) : -1, 2);
            if (loaded.has_value() && loaded->size() == 2) {
                expectEquals((*loaded)[0].description.name, juce::String("Width"));
                expect(!(*loaded)[0].enabled);
                expect((*loaded)[0].state == second[0].state);
                expectEquals((*loaded)[1].description.name, juce::String("Gain"));
                expect((*loaded)[1].state == second[1].state);
            }

            expect(plugin_presets::deletePreset(replaced.value_or(juce::File {})));
            expect(plugin_presets::deletePreset(replaced.value_or(juce::File {})));
            expectEquals(static_cast<int>(plugin_presets::listPresets(directory).size()), 2);
        }

        beginTest("Missing, malformed, and newer preset files are refused with a reason");
        {
            const TemporaryDirectory temporary;
            expect(!plugin_presets::loadPreset(temporary.getChildFile("missing.xml")).has_value());

            const auto malformed = temporary.getChildFile("malformed.xml");
            expect(malformed.replaceWithText("not xml"));
            const auto malformedResult = plugin_presets::loadPreset(malformed);
            expect(!malformedResult.has_value());
            expect(!malformedResult.has_value() && malformedResult.error().contains("not a plugin chain preset"));

            const auto newer = temporary.getChildFile("newer.xml");
            expect(newer.replaceWithText(
                R"(<PLUGIN_CHAIN_PRESET name="newer" version="99"><PLUGIN_CHAIN/></PLUGIN_CHAIN_PRESET>)"
            ));
            const auto newerResult = plugin_presets::loadPreset(newer);
            expect(!newerResult.has_value() && newerResult.error().contains("newer version"));

            const auto empty = temporary.getChildFile("empty.xml");
            expect(empty.replaceWithText(R"(<PLUGIN_CHAIN_PRESET name="empty" version="1"/>)"));
            const auto emptyResult = plugin_presets::loadPreset(empty);
            expect(!emptyResult.has_value() && emptyResult.error().contains("no plugin chain"));
        }
    }

    /// Checks that a live chain saves its slots as a preset, tracks the loaded preset name, and loads presets back.
    void testLiveChainPresets()
    {
        beginTest("A live chain saves and loads presets next to its settings");
        const TemporaryDirectory temporary;
        juce::ApplicationProperties properties;
        useTemporarySettings(properties, temporary);
        auto* settings = properties.getUserSettings();
        const std::vector<SavedChainSlot> stored {
            makeUnavailableSlot("First", "first state", true),
            makeUnavailableSlot("Second", "second state", false),
        };
        storeChain(*settings, stored);

        juce::CriticalSection renderingLock;
        juce::AudioDeviceManager previewDeviceManager;
        PluginLibrary library(properties);
        LiveChain chain(library, properties, renderingLock, previewDeviceManager);
        expectEquals(chain.getNumSlots(), 2);
        expect(chain.getPresetDirectory().isAChildOf(temporary.getChildFile("properties")));
        expect(chain.getPresets().empty());
        expect(chain.getCurrentPresetName().isEmpty());

        const auto saved = chain.saveAsPreset(" Two Slots ");
        expect(saved.has_value());
        expectEquals(chain.getCurrentPresetName(), juce::String("Two Slots"));
        const auto presets = chain.getPresets();
        expectEquals(static_cast<int>(presets.size()), 1);
        expectEquals(presets.empty() ? juce::String() : presets.front().name, juce::String("Two Slots"));

        chain.clearChain();
        expectEquals(chain.getNumSlots(), 0);
        expect(chain.getCurrentPresetName().isEmpty());

        const auto loaded = chain.loadPreset(saved.value_or(juce::File {}));
        expect(loaded.has_value());
        expectEquals(chain.getNumSlots(), 2);
        expectEquals(chain.getCurrentPresetName(), juce::String("Two Slots"));
        const auto slotIds = chain.getSlotIds();
        if (slotIds.size() == 2) {
            expectEquals(chain.getSlotDescription(slotIds[0]).name, juce::String("First"));
            expect(chain.isSlotEnabled(slotIds[0]));
            expectEquals(chain.getSlotDescription(slotIds[1]).name, juce::String("Second"));
            expect(!chain.isSlotEnabled(slotIds[1]));
        }

        const auto enabled = chain.getEnabledPlugins();
        expectEquals(static_cast<int>(enabled.size()), 1);
        expect(!enabled.empty() && enabled.front().descriptorRef.state == stored.front().state);

        const auto persisted = juce::parseXML(settings->getValue("pluginChain"));
        const auto persistedSlots = persisted != nullptr ? plugin_presets::parseChainXml(*persisted) : std::nullopt;
        expectEquals(persistedSlots.has_value() ? static_cast<int>(persistedSlots->size()) : -1, 2);

        expect(!chain.loadPreset(temporary.getChildFile("missing.xml")).has_value());
        expectEquals(chain.getNumSlots(), 2);

        if (!slotIds.empty()) {
            chain.removeSlot(slotIds.front());
        }
        expect(chain.getCurrentPresetName().isEmpty());

        expect(chain.deletePreset(saved.value_or(juce::File {})));
        expect(chain.getPresets().empty());
    }

    /// Loads the test VST3, checks that its default settings leave audio untouched,
    /// and null tests each expected output after a round trip of its parameters through a preset.
    void testPluginProcessing()
    {
        beginTest("The test VST3 loads from the bundle CMake built");
        const auto bundle = testPluginBundle();
        expect(bundle.isDirectory(), utils::format("Test plugin bundle {} is missing", bundle.getFullPathName()));
        if (!bundle.isDirectory()) {
            return;
        }

        const TemporaryDirectory temporary;
        juce::ApplicationProperties properties;
        useTemporarySettings(properties, temporary);
        PluginLibrary library(properties);

        juce::OwnedArray<juce::PluginDescription> types;
        juce::VST3PluginFormat format;
        format.findAllTypesForFile(types, bundle.getFullPathName());
        expectEquals(types.size(), 1);
        if (types.isEmpty()) {
            return;
        }
        const auto description = *types.getFirst();
        expectEquals(description.name, juce::String("AudioBatch Test MS"));

        beginTest("The test VST3 leaves audio untouched at its default settings");
        {
            storeChain(*properties.getUserSettings(), {{.description = description, .state = {}, .enabled = true}});
            juce::CriticalSection renderingLock;
            juce::AudioDeviceManager previewDeviceManager;
            LiveChain chain(library, properties, renderingLock, previewDeviceManager);
            const auto source = temporary.copyFixture("stereo_split_44100_16.wav", "identity.wav");
            const auto output = processWithChain(library, chain.getEnabledPlugins(), source, 0);
            const auto result = nullTestFiles(fixtureFile("stereo_split_44100_16.wav"), output);
            expect(result.cancels(), result.describe());
        }

        int caseIndex = 0;
        for (const auto& expectation : pluginExpectations) {
            beginTest(
                utils::format(
                    "The test VST3 output for gain {}, width {}, and latency {} on {} cancels against {}",
                    expectation.gain,
                    expectation.width,
                    expectation.latency,
                    expectation.source,
                    expectation.expected
                )
            );

            const auto presetFile = savePresetWithParameters(
                properties, library, description, expectation, utils::format("Case {}", caseIndex)
            );
            expect(presetFile.existsAsFile());

            juce::CriticalSection renderingLock;
            juce::AudioDeviceManager previewDeviceManager;
            LiveChain chain(library, properties, renderingLock, previewDeviceManager);
            chain.clearChain();
            expect(chain.loadPreset(presetFile).has_value());
            expectEquals(chain.getNumSlots(), 1);

            const auto source
                = temporary.copyFixture(expectation.source, utils::format("case_{}_{}", caseIndex, expectation.source));
            const auto output = processWithChain(library, chain.getEnabledPlugins(), source, expectation.latency);
            const auto result = nullTestFiles(fixtureFile(expectation.expected), output);
            expect(result.cancels(), result.describe());
            ++caseIndex;
        }
    }

    /// Creates a live chain with the test plugin, sets its gain and width the way a host does,
    /// and saves the chain as a preset with the given name. Returns the preset file, or an empty file on failure.
    juce::File savePresetWithParameters(
        juce::ApplicationProperties& properties,
        PluginLibrary& library,
        const juce::PluginDescription& description,
        const PluginExpectation& expectation,
        const juce::String& presetName
    )
    {
        storeChain(*properties.getUserSettings(), {{.description = description, .state = {}, .enabled = true}});

        juce::CriticalSection renderingLock;
        juce::AudioDeviceManager previewDeviceManager;
        LiveChain chain(library, properties, renderingLock, previewDeviceManager);
        const auto preview = chain.preparePreview(44'100.0, 512);
        expect(preview.preparation.errorMessage.isEmpty(), preview.preparation.errorMessage);
        expectEquals(static_cast<int>(preview.instances.size()), 1);
        if (preview.instances.empty()) {
            return {};
        }

        const auto& parameters = preview.instances.front()->getParameters();
        expect(parameters.size() > latencyParameterIndex);
        if (parameters.size() <= latencyParameterIndex) {
            return {};
        }
        expectEquals(parameters[gainParameterIndex]->getName(32), juce::String("Gain"));
        expectEquals(parameters[widthParameterIndex]->getName(32), juce::String("Width"));
        expectEquals(parameters[latencyParameterIndex]->getName(32), juce::String("Latency"));
        parameters[gainParameterIndex]->setValueNotifyingHost(expectation.gain / testParameterMaximum);
        parameters[widthParameterIndex]->setValueNotifyingHost(expectation.width / testParameterMaximum);
        parameters[latencyParameterIndex]->setValueNotifyingHost(
            static_cast<float>(expectation.latency) / testLatencyMaximum
        );

        const auto saved = chain.saveAsPreset(presetName);
        expect(saved.has_value(), saved.has_value() ? juce::String() : saved.error());
        return saved.value_or(juce::File {});
    }

    /// Assembles the plugins and their options as a processing run does, processes the source through them,
    /// checks that the first plugin reported the expected latency, which processing compensated,
    /// and returns the output file, which does not exist when processing failed.
    /// Each plugin is restored from its preset state, or from its state right after creation when the preset has none.
    juce::File processWithChain(
        const PluginLibrary& library,
        const std::vector<EnabledChainPlugin>& enabledPlugins,
        const juce::File& source,
        const int expectedLatencySamples
    )
    {
        PluginProcessingOptions settings;
        settings.disposeFile = [](const juce::File& file) { return file.deleteFile(); };
        auto run = library.createProcessingRun(enabledPlugins, std::move(settings), 1);
        expect(run.has_value(), run.has_value() ? juce::String() : run.error());
        if (!run.has_value() || run->chains.empty()) {
            return {};
        }

        std::vector<juce::AudioPluginInstance*> chain;
        for (const auto& instance : run->chains.front()) {
            chain.push_back(instance.get());
        }

        const auto result = processFileWithPlugins(analyzeFile(source), run->options, chain);
        expect(!result.hasError(), utils::format("Processing {} failed", source.getFileName()));
        expectEquals(chain.front()->getLatencySamples(), expectedLatencySamples);
        return getProcessingOutputFile(source);
    }
};
}  // namespace

std::unique_ptr<juce::UnitTest> createPluginPresetTests()
{
    return std::make_unique<PluginPresetTests>();
}
}  // namespace audiobatch::tests
